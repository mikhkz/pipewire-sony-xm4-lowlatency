#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sbc/sbc.h>
#include "media-codecs.h"
#include "rtp.h"
#include <spa/param/props.h>

#define N_FRAMES 1000
#define CAPACITY (1024 * 1024)

struct plugin {
    void *library;
    struct spa_handle *handle;
    const struct spa_bluez5_codec_a2dp *iface;
};

static struct plugin load_plugin(const char *path)
{
    struct plugin p = {0};
    const struct spa_handle_factory *factory;
    uint32_t index = 0;
    void *iface;
    p.library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!p.library) { fprintf(stderr, "%s\n", dlerror()); exit(1); }
    int (*enumerate)(const struct spa_handle_factory **, uint32_t *) =
        dlsym(p.library, "spa_handle_factory_enum");
    assert(enumerate && enumerate(&factory, &index) == 1);
    if (strstr(path, "xm4-")) assert(strstr(factory->name, "xm4-isolated-sbc"));
    p.handle = calloc(1, spa_handle_factory_get_size(factory, NULL));
    assert(p.handle && spa_handle_factory_init(factory, p.handle, NULL, NULL, 0) == 0);
    assert(spa_handle_get_interface(p.handle, SPA_TYPE_INTERFACE_Bluez5CodecMedia, &iface) == 0);
    p.iface = iface;
    return p;
}

static const struct media_codec *get_codec(struct plugin *p, const char *name)
{
    for (size_t i = 0; p->iface->codecs[i]; ++i)
        if (strcmp(p->iface->codecs[i]->name, name) == 0) return p->iface->codecs[i];
    abort();
}

static size_t encode(const struct media_codec *codec, const char *frames,
                     uint8_t *output, unsigned *packet_count)
{
    uint8_t caps[A2DP_MAX_CAPS_SIZE], config[A2DP_MAX_CAPS_SIZE];
    /* SBC and SBC-XQ share their advertised SBC endpoint capabilities. */
    a2dp_sbc_t capability = {
        .frequency = SBC_SAMPLING_FREQ_48000,
        .channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL | SBC_CHANNEL_MODE_JOINT_STEREO,
        .block_length = SBC_BLOCK_LENGTH_16, .subbands = SBC_SUBBANDS_8,
        .allocation_method = SBC_ALLOCATION_LOUDNESS,
        .min_bitpool = 2, .max_bitpool = 53,
    };
    memcpy(caps, &capability, sizeof(capability));
    struct media_codec_audio_info requested = { .rate = 48000, .channels = 2 };
    int length = codec->select_config(codec, 0, caps, sizeof(capability), &requested,
                                      NULL, config, NULL);
    assert(length == sizeof(capability));
    struct spa_audio_info audio;
    assert(codec->validate_config(codec, 0, config, length, &audio) == 0);
    struct spa_dict_item setting = SPA_DICT_ITEM_INIT("bluez5.a2dp.sbc.packet-frames", frames);
    struct spa_dict settings = SPA_DICT_INIT(&setting, 1);
    void *props = codec->init_props ? codec->init_props(codec, 0, frames ? &settings : NULL) : NULL;
    void *encoder = codec->init(codec, 0, config, length, &audio, props, 1008);
    assert(encoder);
    int codesize = codec->get_block_size(encoder);
    assert(codesize == 512);
    sbc_t decoder;
    assert(sbc_init_a2dp(&decoder, 0, config, length) == 0);
    size_t total = 0;
    unsigned done = 0, packets = 0;
    uint32_t rng = 17;
    while (done < N_FRAMES) {
        uint8_t packet[2048], decoded[512];
        int header = codec->start_encode(encoder, packet, sizeof(packet),
                                         (uint16_t)(65530u + packets), 0xffff0000u + done * 128);
        assert(header > 0);
        size_t position = header;
        unsigned start = done;
        int flush = NEED_FLUSH_NO;
        do {
            int16_t pcm[256];
            for (unsigned i = 0; i < 256; ++i) {
                rng = rng * 1664525u + 1013904223u;
                pcm[i] = (int16_t)(rng >> 16);
            }
            size_t written, decoded_bytes;
            int consumed = codec->encode(encoder, pcm, sizeof(pcm), packet + position,
                                          sizeof(packet) - position, &written, &flush);
            assert(consumed == codesize && written > 0 && total + written <= CAPACITY);
            ssize_t read = sbc_decode(&decoder, packet + position, written,
                                      decoded, sizeof(decoded), &decoded_bytes);
            assert(read == (ssize_t)written && decoded_bytes == sizeof(pcm));
            memcpy(output + total, packet + position, written);
            total += written;
            position += written;
            ++done;
        } while (flush == NEED_FLUSH_NO && done < N_FRAMES);
        struct rtp_header *rtp = (void *)packet;
        struct rtp_payload *payload = (void *)(packet + sizeof(*rtp));
        assert(rtp->v == 2 && ntohs(rtp->sequence_number) == (uint16_t)(65530u + packets));
        assert(ntohl(rtp->timestamp) == 0xffff0000u + start * 128);
        assert(payload->frame_count == done - start && position <= 1008);
        if (frames && strcmp(frames, "1") == 0) assert(payload->frame_count == 1);
        ++packets;
    }
    /* Exercise the new packetizer's small-buffer error paths. */
    if (codec->init_props) {
        uint8_t tiny[2], packet[2048];
        int16_t pcm[256] = {0};
        size_t written = 123;
        int flush = -1;
        assert(codec->start_encode(encoder, tiny, sizeof(tiny), 0, 0) == -ENOSPC);
        assert(codec->start_encode(encoder, packet, sizeof(packet), 0, 0) > 0);
        assert(codec->encode(encoder, pcm, 1, packet + 13, sizeof(packet) - 13,
                              &written, &flush) == -EINVAL && written == 0);
        assert(codec->encode(encoder, pcm, sizeof(pcm), tiny, sizeof(tiny),
                              &written, &flush) == -ENOSPC && written == 0);
        errno = 0;
        assert(codec->init(codec, 0, config, length, &audio, props, 1) == NULL && errno == EINVAL);
    }
    codec->deinit(encoder);
    if (codec->clear_props) codec->clear_props(props);
    sbc_finish(&decoder);
    *packet_count = packets;
    return total;
}

static int update_policy(const struct media_codec *codec, void *props, int frames, int time_us)
{
    uint8_t buffer[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct spa_pod_frame f[2];
    spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&b, SPA_PROP_params, 0);
    spa_pod_builder_push_struct(&b, &f[1]);
    spa_pod_builder_string(&b, "bluez5.a2dp.sbc.packet-frames");
    spa_pod_builder_int(&b, frames);
    spa_pod_builder_string(&b, "bluez5.a2dp.sbc.packet-time-us");
    spa_pod_builder_int(&b, time_us);
    spa_pod_builder_pop(&b, &f[1]);
    return codec->set_props(props, spa_pod_builder_pop(&b, &f[0]));
}

static void test_live_update(const struct media_codec *codec, unsigned frequency)
{
    a2dp_sbc_t config = {
        .frequency = frequency, .channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL,
        .block_length = SBC_BLOCK_LENGTH_16, .subbands = SBC_SUBBANDS_8,
        .allocation_method = SBC_ALLOCATION_LOUDNESS, .min_bitpool = 2, .max_bitpool = 39,
    };
    struct spa_audio_info audio;
    assert(codec->validate_config(codec, 0, &config, sizeof(config), &audio) == 0);
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        void *props = codec->init_props(codec, 0, NULL);
        assert(props && update_policy(codec, props, 3, 0) == 1);
        /* The adapter sends unrelated boolean/float/string parameters too. */
        uint8_t parameters[512];
        struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(parameters, sizeof(parameters));
        struct spa_pod_frame param_frames[2];
        spa_pod_builder_push_object(&builder, &param_frames[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
        spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
        spa_pod_builder_push_struct(&builder, &param_frames[1]);
        spa_pod_builder_string(&builder, "channelmix.normalize");
        spa_pod_builder_bool(&builder, true);
        spa_pod_builder_string(&builder, "channelmix.min-volume");
        spa_pod_builder_float(&builder, 0.0f);
        spa_pod_builder_string(&builder, "dither.method");
        spa_pod_builder_string(&builder, "none");
        spa_pod_builder_pop(&builder, &param_frames[1]);
        assert(codec->set_props(props, spa_pod_builder_pop(&builder, &param_frames[0])) == 0);
        void *encoder = codec->init(codec, 0, &config, sizeof(config), &audio, props, 1008);
        assert(encoder);
        uint8_t packet[2048];
        int16_t pcm[256] = {0};
        size_t written;
        int flush;
        int offset = codec->start_encode(encoder, packet, sizeof(packet), 1, 0);
        assert(offset > 0);
        assert(codec->encode(encoder, pcm, sizeof(pcm), packet + offset,
                             sizeof(packet) - offset, &written, &flush) == sizeof(pcm));
        offset += written;
        assert(flush == NEED_FLUSH_NO);
        assert(update_policy(codec, props, 1, 0) == 1);
        assert(codec->update_props(encoder, props) == 0);
        /* Shrinking the policy must not truncate the already-open packet. */
        for (unsigned frame = 1; frame < 3; ++frame) {
            assert(codec->encode(encoder, pcm, sizeof(pcm), packet + offset,
                                 sizeof(packet) - offset, &written, &flush) == sizeof(pcm));
            offset += written;
            assert(flush == (frame == 2 ? NEED_FLUSH_ALL : NEED_FLUSH_NO));
        }
        offset = codec->start_encode(encoder, packet, sizeof(packet), 2, 384);
        assert(codec->encode(encoder, pcm, sizeof(pcm), packet + offset,
                             sizeof(packet) - offset, &written, &flush) == sizeof(pcm));
        assert(flush == NEED_FLUSH_ALL);
        assert(update_policy(codec, props, 0, 6000) == 1);
        assert(codec->update_props(encoder, props) == 0);
        assert(update_policy(codec, props, 15, -1) == -EINVAL);
        /* Invalid mixed updates are transactional: 6 ms still means 2 frames. */
        assert(codec->update_props(encoder, props) == 0);
        offset = codec->start_encode(encoder, packet, sizeof(packet), 3, 512);
        for (unsigned frame = 0; frame < 2; ++frame) {
            assert(codec->encode(encoder, pcm, sizeof(pcm), packet + offset,
                                 sizeof(packet) - offset, &written, &flush) == sizeof(pcm));
            offset += written;
            assert(flush == (frame == 1 ? NEED_FLUSH_ALL : NEED_FLUSH_NO));
        }
        assert(codec->set_props(props, NULL) == 1);
        assert(codec->update_props(encoder, props) == 0);
        offset = codec->start_encode(encoder, packet, sizeof(packet), 4, 768);
        unsigned frames = 0;
        do {
            assert(codec->encode(encoder, pcm, sizeof(pcm), packet + offset,
                                 sizeof(packet) - offset, &written, &flush) == sizeof(pcm));
            offset += written;
            ++frames;
        } while (flush == NEED_FLUSH_NO);
        assert(frames == 5);
        codec->deinit(encoder);
        codec->clear_props(props);
    }
    printf("%s %u Hz: 100 reopen cycles, mid-packet updates, time budget and atomic rejection passed\n",
           codec->name, audio.info.raw.rate);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    struct plugin custom = load_plugin(argv[1]), stock = load_plugin(argv[2]);
    uint8_t *expected = malloc(CAPACITY), *actual = malloc(CAPACITY);
    assert(expected && actual);
    const char *names[] = { "sbc", "sbc_xq" };
    for (unsigned i = 0; i < 2; ++i) {
        test_live_update(get_codec(&custom, names[i]), SBC_SAMPLING_FREQ_48000);
        test_live_update(get_codec(&custom, names[i]), SBC_SAMPLING_FREQ_44100);
        unsigned stock_packets, packets;
        size_t expected_size = encode(get_codec(&stock, names[i]), NULL, expected, &stock_packets);
        const char *settings[] = { NULL, "1", "2", "15", "0", "16", "-1", "garbage" };
        for (unsigned j = 0; j < sizeof(settings)/sizeof(*settings); ++j) {
            size_t size = encode(get_codec(&custom, names[i]), settings[j], actual, &packets);
            assert(size == expected_size && memcmp(expected, actual, size) == 0);
            if (settings[j] && strcmp(settings[j], "1") == 0) assert(packets == N_FRAMES);
            if (!settings[j] || j >= 4) assert(packets == stock_packets);
            printf("%s frames=%s: %u RTP packets, %d decodable SBC frames, payload identical to stock\n",
                   names[i], settings[j] ? settings[j] : "default", packets, N_FRAMES);
        }
    }
    free(expected); free(actual);
    spa_handle_clear(custom.handle); free(custom.handle); dlclose(custom.library);
    spa_handle_clear(stock.handle); free(stock.handle); dlclose(stock.library);
    return 0;
}
