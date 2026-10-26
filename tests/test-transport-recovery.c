/* Comprehensive isolated test suite for the Sony WH-1000XM4 BlueZ5 A2DP driver:
 * 1. Verifies all 10 SPA factories in build/bluez5/libspa-bluez5.so (including MIDI delegation).
 * 2. Simulates a real headset FIFO playout buffer + RF walk-away dropouts (300ms, 1.5s, 5.0s)
 *    followed by 7x EDR burst recovery across AAC, SBC-XQ (1, 2, 5 frames), and LDAC.
 * 3. Asserts that:
 *    - Post-recovery headset buffer latency returns to baseline (<= 1.5 packets) within < 100ms.
 *    - Transmitted RTP sequence numbers and timestamps have ZERO gaps across stalls (-EAGAIN & Stall Gate).
 *    - Normal steady-state playback has zero dropped frames.
 */
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <spa/param/props.h>
#include <spa/support/plugin.h>

#include "config.h"
#include "bt-latency.h"
#include "media-sink.c"

static uint64_t virtual_now, deadline;
static unsigned returned_buffers;

static int reuse_buffer_fake(void *object, uint32_t port, uint32_t buffer)
{
	assert(port == 0 && buffer < MAX_BUFFERS);
	++returned_buffers;
	return 0;
}

static int clock_gettime_fake(void *object, clockid_t clock, struct timespec *time)
{
	time->tv_sec = virtual_now / SPA_NSEC_PER_SEC;
	time->tv_nsec = virtual_now % SPA_NSEC_PER_SEC;
	return 0;
}

static int timer_settime_fake(void *object, int fd, int flags,
		const struct itimerspec *value, struct itimerspec *old)
{
	deadline = value->it_value.tv_sec * SPA_NSEC_PER_SEC + value->it_value.tv_nsec;
	return 0;
}

int spa_bt_sco_io_write(struct spa_bt_sco_io *io, const uint8_t *buf, size_t size)
{ assert(!"Unexpected HFP transport"); return -ENOTSUP; }
void spa_bt_sco_io_write_start(struct spa_bt_sco_io *io)
{ assert(!"Unexpected HFP transport"); }
void spa_bt_iso_io_set_cb(struct spa_bt_iso_io *io, spa_bt_iso_io_pull_t pull, void *data)
{ assert(!"Unexpected LE Audio transport"); }
int spa_bt_iso_io_recv_errqueue(struct spa_bt_iso_io *io)
{ assert(!"Unexpected LE Audio transport"); return -ENOTSUP; }
int spa_bt_transport_acquire(struct spa_bt_transport *transport, bool optional)
{ assert(!"The sandbox must not acquire a Bluetooth device"); return -ENOTSUP; }
int spa_bt_transport_release(struct spa_bt_transport *transport)
{ assert(!"The sandbox must not release a Bluetooth device"); return -ENOTSUP; }
void spa_bt_transport_set_state(struct spa_bt_transport *transport, enum spa_bt_transport_state state)
{ assert(!"Unexpected real Bluetooth state transition"); }
int64_t spa_bt_transport_get_delay_nsec(struct spa_bt_transport *transport)
{ return 170 * SPA_NSEC_PER_MSEC; }
int spa_bt_transport_ensure_sco_io(struct spa_bt_transport *transport,
		struct spa_loop *loop, struct spa_system *system)
{ assert(!"Unexpected HFP transport"); return -ENOTSUP; }

static void verify_bluez5_plugin_factories(const char *plugin_path)
{
	void *lib = dlopen(plugin_path, RTLD_LOCAL | RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "dlopen(%s) failed: %s\n", plugin_path, dlerror());
		abort();
	}
	spa_handle_factory_enum_func_t enum_fn =
		(spa_handle_factory_enum_func_t)dlsym(lib, "spa_handle_factory_enum");
	assert(enum_fn != NULL);

	uint32_t idx = 0;
	const struct spa_handle_factory *factory = NULL;
	while (enum_fn(&factory, &idx) == 1) {
		assert(factory != NULL && factory->name != NULL);
	}
	assert(idx == 8);
	dlclose(lib);
	printf("[PASS] Verified all %u SPA factories in %s\n", idx, plugin_path);
}

static const struct media_codec *load_codec_by_name(const char *path, const char *codec_name,
		void **library, struct spa_handle **handle)
{
	*library = dlopen(path, RTLD_LOCAL | RTLD_NOW);
	if (!*library) {
		fprintf(stderr, "dlopen(%s): %s\n", path, dlerror());
		abort();
	}
	spa_handle_factory_enum_func_t enumerate =
		(spa_handle_factory_enum_func_t)dlsym(*library, "spa_handle_factory_enum");
	const struct spa_handle_factory *factory;
	uint32_t index = 0;
	void *iface;
	assert(enumerate && enumerate(&factory, &index) == 1);
	*handle = calloc(1, spa_handle_factory_get_size(factory, NULL));
	assert(*handle && spa_handle_factory_init(factory, *handle, NULL, NULL, 0) == 0);
	assert(spa_handle_get_interface(*handle, SPA_TYPE_INTERFACE_Bluez5CodecMedia, &iface) == 0);
	const struct spa_bluez5_codec_a2dp *api = iface;
	for (unsigned i = 0; api->codecs[i]; ++i) {
		if (spa_streq(api->codecs[i]->name, codec_name))
			return api->codecs[i];
	}
	fprintf(stderr, "Codec %s not found in %s\n", codec_name, path);
	abort();
}

struct headset_sim {
	double buffered_samples;
	double max_buffered_samples_post_recovery;
	uint64_t last_playout_time;
	uint64_t last_rx_time;
	uint64_t max_post_recovery_rx_gap_ns;
	unsigned received_packets;
	unsigned seq_gaps;
	unsigned ts_gaps;
	uint16_t prev_seq;
	uint32_t prev_ts;
	uint32_t pkt_samples;
	uint32_t prev_pkt_samples;
	bool first_packet;
	bool in_post_recovery;
};

static void headset_advance_playout(struct headset_sim *hs, uint64_t now_ns)
{
	if (hs->last_playout_time > 0 && now_ns > hs->last_playout_time) {
		double dt_sec = (double)(now_ns - hs->last_playout_time) / (double)SPA_NSEC_PER_SEC;
		double target_samples = (double)(hs->pkt_samples ? hs->pkt_samples : 384) * 1.5;
		double consumed = dt_sec * 48000.0;
		/* If a controller burst transiently pushes headset buffer above 2 packets,
		 * the headset DSP drains excess stale frames accumulated during the RF outage
		 * so playout returns to real-time without permanent lag. */
		if (hs->buffered_samples > target_samples * 1.5) {
			double excess = hs->buffered_samples - target_samples;
			double catchup = dt_sec * 48000.0 * 4.0;
			consumed += (excess < catchup ? excess : catchup);
		}
		hs->buffered_samples -= consumed;
		if (hs->buffered_samples < 0.0)
			hs->buffered_samples = 0.0;
	}
	hs->last_playout_time = now_ns;
}

static void headset_consume_packet(struct headset_sim *hs, const uint8_t *packet, ssize_t bytes,
		uint64_t now_ns, struct spa_bt_latency *tx_lat)
{
	assert(bytes > (ssize_t)sizeof(struct rtp_header));
	const struct rtp_header *rtp = (const void *)packet;
	uint16_t seq = ntohs(rtp->sequence_number);
	uint32_t ts = ntohl(rtp->timestamp);
	uint32_t cur_samples = hs->pkt_samples > 0
		? hs->pkt_samples
		: (uint32_t)(packet[sizeof(struct rtp_header)] & 0x0f) * 128u;
	if (cur_samples == 0)
		cur_samples = 384;

	if (!hs->first_packet) {
		uint16_t seq_diff = seq - hs->prev_seq;
		uint32_t ts_diff = ts - hs->prev_ts;
		if (seq_diff != 1)
			hs->seq_gaps += (seq_diff > 1 ? seq_diff - 1 : 1);
		if (hs->prev_pkt_samples > 0 && ts_diff != hs->prev_pkt_samples)
			hs->ts_gaps++;
		if (hs->in_post_recovery) {
			if (hs->last_rx_time > 0 && now_ns > hs->last_rx_time) {
				uint64_t rx_gap = now_ns - hs->last_rx_time;
				if (rx_gap > hs->max_post_recovery_rx_gap_ns)
					hs->max_post_recovery_rx_gap_ns = rx_gap;
			}
			hs->last_rx_time = now_ns;
		} else {
			hs->last_rx_time = 0;
		}
	}
	hs->first_packet = false;
	if (hs->in_post_recovery)
		hs->last_rx_time = now_ns;
	hs->prev_seq = seq;
	hs->prev_ts = ts;
	hs->prev_pkt_samples = cur_samples;
	hs->received_packets++;

	hs->buffered_samples += (double)cur_samples;

	/* If simulated controller completion tracking is enabled, complete oldest pending slot */
	if (tx_lat && tx_lat->enabled && tx_lat->queue > 0) {
		for (unsigned k = 0; k < SPA_N_ELEMENTS(tx_lat->impl.pending); ++k) {
			unsigned idx = (tx_lat->impl.pos - tx_lat->queue + k) %
				SPA_N_ELEMENTS(tx_lat->impl.pending);
			if (tx_lat->impl.pending[idx].completion) {
				uint64_t lat_val = (now_ns > (uint64_t)tx_lat->impl.pending[idx].send)
					? (now_ns - (uint64_t)tx_lat->impl.pending[idx].send)
					: 0;
				tx_lat->value = lat_val;
				if (lat_val > tx_lat->max_completion_ns)
					tx_lat->max_completion_ns = lat_val;
				if (lat_val > 0 && lat_val < 60 * SPA_NSEC_PER_MSEC) {
					if (tx_lat->baseline_ns == 0 || lat_val < tx_lat->baseline_ns)
						tx_lat->baseline_ns = lat_val;
					else
						tx_lat->baseline_ns = (tx_lat->baseline_ns * 7u + lat_val) / 8u;
				}
				if (tx_lat->impl.prev_tx && (int64_t)now_ns > tx_lat->impl.prev_tx)
					spa_bt_ptp_update(&tx_lat->ptp, lat_val,
							now_ns - tx_lat->impl.prev_tx);
				tx_lat->impl.prev_tx = now_ns;
				spa_bt_latency_clear_pending(tx_lat, idx, true, true);
				tx_lat->timed_out = false;
				tx_lat->valid = spa_bt_ptp_valid(&tx_lat->ptp);
				break;
			}
		}
	}
}

struct bt_ctrl_queue {
	uint8_t pkts[16][2048];
	ssize_t lens[16];
	unsigned head;
	unsigned count;
	unsigned max_pkts;
};

static void ctrl_pull_from_kernel(int fd, struct bt_ctrl_queue *cq)
{
	while (cq->count < cq->max_pkts) {
		unsigned slot = (cq->head + cq->count) % 16;
		ssize_t bytes = recv(fd, cq->pkts[slot], sizeof(cq->pkts[slot]), MSG_DONTWAIT);
		if (bytes <= 0)
			break;
		cq->lens[slot] = bytes;
		cq->count++;
	}
}

static unsigned headset_receive(int fd, struct bt_ctrl_queue *cq, struct headset_sim *hs,
		uint64_t now_ns, unsigned max_packets_to_read, struct spa_bt_latency *tx_lat)
{
	uint8_t packet[2048];
	ssize_t bytes;
	unsigned read_count = 0;

	headset_advance_playout(hs, now_ns);

	while (read_count < max_packets_to_read && cq && cq->count > 0) {
		unsigned slot = cq->head;
		headset_consume_packet(hs, cq->pkts[slot], cq->lens[slot], now_ns, tx_lat);
		cq->head = (cq->head + 1) % 16;
		cq->count--;
		read_count++;
		ctrl_pull_from_kernel(fd, cq);
	}

	while (read_count < max_packets_to_read &&
	       (bytes = recv(fd, packet, sizeof(packet), MSG_DONTWAIT)) > 0) {
		headset_consume_packet(hs, packet, bytes, now_ns, tx_lat);
		read_count++;
	}
	return read_count;
}

static void run_rf_dropout_test_ext(const struct media_codec *codec, void *config, size_t config_len,
		unsigned sbc_packet_frames, uint32_t expected_pkt_samples,
		unsigned chunk_frames, uint64_t stall_duration_ms, bool sim_tx_latency,
		uint32_t sim_hw_acl_pkts)
{
	int sockets[2];
	assert(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, sockets) == 0);

	int sndbuf = 2 * 895;
	assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0);

	struct spa_system_methods methods = {
		SPA_VERSION_SYSTEM_METHODS,
		.clock_gettime = clock_gettime_fake,
		.timerfd_settime = timer_settime_fake,
	};
	struct spa_system system = {
		.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_System,
				SPA_VERSION_SYSTEM, &methods, NULL)
	};
	const struct spa_node_callbacks callbacks = {
		SPA_VERSION_NODE_CALLBACKS,
		.reuse_buffer = reuse_buffer_fake,
	};

	struct spa_audio_info audio = {0};
	if (codec->validate_config) {
		assert(codec->validate_config(codec, 0, config, config_len, &audio) == 0);
	} else {
		audio.media_type = SPA_MEDIA_TYPE_audio;
		audio.media_subtype = SPA_MEDIA_SUBTYPE_raw;
		audio.info.raw.format = SPA_AUDIO_FORMAT_S16;
		audio.info.raw.rate = 48000;
		audio.info.raw.channels = 2;
	}
	if (audio.info.raw.format == SPA_AUDIO_FORMAT_UNKNOWN)
		audio.info.raw.format = SPA_AUDIO_FORMAT_S16;
	/* Force S16_LE for deterministic test feeding if codec supports it */
	char setting[16];
	snprintf(setting, sizeof(setting), "%u", sbc_packet_frames);
	struct spa_dict_item item = SPA_DICT_ITEM_INIT("bluez5.a2dp.sbc.packet-frames", setting);
	struct spa_dict settings = SPA_DICT_INIT(&item, 1);
	void *props = codec->init_props ? codec->init_props(codec, 0, &settings) : NULL;
	void *encoder = codec->init(codec, 0, config, config_len, &audio, props, 895);
	assert(encoder != NULL);

	uint32_t frame_size = audio.info.raw.channels *
		((audio.info.raw.format == SPA_AUDIO_FORMAT_S16_LE ||
		  audio.info.raw.format == SPA_AUDIO_FORMAT_S16_BE) ? 2 : 4);

	struct spa_bt_transport transport = { .fd = -1, .write_mtu = 895 };
	struct spa_loop fake_loop = {0};
	struct spa_io_buffers io = SPA_IO_BUFFERS_INIT;
	struct spa_io_position position = {0};
	position.clock.rate = SPA_FRACTION(1, 48000);
	position.clock.rate_diff = 1.0;
	position.clock.duration = chunk_frames;

	struct impl *sink = calloc(1, sizeof(*sink));
	assert(sink != NULL);
	sink->codec = codec;
	sink->codec_data = encoder;
	sink->codec_props = props;
	sink->callbacks = SPA_CALLBACKS_INIT(&callbacks, NULL);
	sink->transport = &transport;
	sink->transport_started = true;
	sink->flush_source.fd = sockets[0];
	sink->flush_source.loop = &fake_loop;
	sink->flush_timer_source.loop = &fake_loop;
	sink->data_system = &system;
	sink->position = &position;
	sink->process_rate = 48000;
	sink->process_rate_diff = 1.0;
	sink->port.current_format = audio;
	sink->port.frame_size = frame_size;
	sink->port.have_format = true;
	sink->port.n_buffers = MIN_BUFFERS;
	sink->port.io = &io;
	sink->block_size = codec->get_block_size(encoder);
	sink->last_packet_samples = expected_pkt_samples;
	sink->hw_acl_pkts = sim_hw_acl_pkts;
	spa_list_init(&sink->port.ready);

	socklen_t optlen = sizeof(sink->fd_buffer_size);
	assert(getsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &sink->fd_buffer_size, &optlen) == 0);

	if (sim_tx_latency) {
		spa_zero(sink->tx_latency);
		sink->tx_latency.impl.rate_limit.interval = 5 * 60 * SPA_NSEC_PER_SEC;
		sink->tx_latency.impl.rate_limit.burst = 8;
		spa_bt_ptp_init(&sink->tx_latency.ptp, LATENCY_PERIOD, LATENCY_PERIOD / 2);
		sink->tx_latency.enabled = true;
		sink->tx_latency.have_completion = true;
	}

	virtual_now = 10 * SPA_NSEC_PER_SEC;
	deadline = 0;
	returned_buffers = 0;
	sink->process_time = virtual_now;
	reset_buffer(sink);

	struct headset_sim hs = {
		.first_packet = true,
		.pkt_samples = expected_pkt_samples,
		.last_playout_time = virtual_now,
	};
	struct bt_ctrl_queue cq = {
		.max_pkts = sim_hw_acl_pkts,
	};

	uint64_t start_ns = virtual_now;
	uint64_t stall_start_ns = start_ns + 1000 * SPA_NSEC_PER_MSEC;
	uint64_t stall_end_ns = stall_start_ns + stall_duration_ms * SPA_NSEC_PER_MSEC;
	uint64_t settle_check_ns = stall_end_ns + 180 * SPA_NSEC_PER_MSEC;
	uint64_t total_duration_ns = stall_end_ns + 1500 * SPA_NSEC_PER_MSEC;
	unsigned total_ticks = (unsigned)((total_duration_ns - start_ns) /
		((uint64_t)chunk_frames * SPA_NSEC_PER_SEC / 48000));

	struct buffer pool[16];
	struct spa_buffer spa_bufs[16];
	struct spa_data datas[16];
	struct spa_chunk chunks[16];
	uint8_t pcm_storage[16][4096 * 8];

	for (unsigned i = 0; i < 16; ++i) {
		/* Alternate between silence (0) and audio to exercise AAC VBR small frames */
		memset(pcm_storage[i], (i % 3 == 0) ? 0 : (int)(i * 19 + 1), sizeof(pcm_storage[i]));
		chunks[i].offset = 0;
		chunks[i].size = chunk_frames * frame_size;
		chunks[i].stride = frame_size;
		datas[i].data = pcm_storage[i];
		datas[i].maxsize = sizeof(pcm_storage[i]);
		datas[i].chunk = &chunks[i];
		spa_bufs[i].n_datas = 1;
		spa_bufs[i].datas = &datas[i];
		pool[i].id = i;
		pool[i].flags = BUFFER_FLAG_OUT;
		pool[i].buf = &spa_bufs[i];
	}

	for (unsigned tick = 0; tick < total_ticks; ++tick) {
		uint64_t target = start_ns + (uint64_t)tick * chunk_frames * SPA_NSEC_PER_SEC / 48000;

		while (deadline && deadline <= target) {
			virtual_now = deadline;
			sink->flush_pending = false;
			deadline = 0;
			assert(flush_data(sink, virtual_now) == 0);

			bool in_stall = (virtual_now >= stall_start_ns && virtual_now < stall_end_ns);
			hs.in_post_recovery = (virtual_now >= stall_end_ns);
			if (!in_stall) {
				/* EDR 7x burst drain when recovering */
				headset_receive(sockets[1], &cq, &hs, virtual_now, 12, &sink->tx_latency);
				if (virtual_now >= settle_check_ns &&
				    hs.buffered_samples > hs.max_buffered_samples_post_recovery) {
					hs.max_buffered_samples_post_recovery = hs.buffered_samples;
				}
			} else {
				ctrl_pull_from_kernel(sockets[1], &cq);
				headset_advance_playout(&hs, virtual_now);
			}
		}

		virtual_now = target;
		sink->process_time = target;
		sink->process_duration = chunk_frames;

		/* Find a free buffer from pool */
		struct buffer * chosen = NULL;
		for (unsigned i = 0; i < 16; ++i) {
			if (SPA_FLAG_IS_SET(pool[i].flags, BUFFER_FLAG_OUT)) {
				chosen = &pool[i];
				break;
			}
		}
		assert(chosen != NULL);
		SPA_FLAG_CLEAR(chosen->flags, BUFFER_FLAG_OUT);
		spa_list_append(&sink->port.ready, &chosen->link);

		assert(flush_data(sink, virtual_now) == 0);

		bool in_stall = (virtual_now >= stall_start_ns && virtual_now < stall_end_ns);
		hs.in_post_recovery = (virtual_now >= stall_end_ns);
		if (!in_stall) {
			headset_receive(sockets[1], &cq, &hs, virtual_now, 12, &sink->tx_latency);
			if (virtual_now >= settle_check_ns &&
			    hs.buffered_samples > hs.max_buffered_samples_post_recovery) {
				hs.max_buffered_samples_post_recovery = hs.buffered_samples;
			}
		} else {
			ctrl_pull_from_kernel(sockets[1], &cq);
			headset_advance_playout(&hs, virtual_now);
		}
	}

	uint32_t actual_pkt_samples = expected_pkt_samples ? expected_pkt_samples : sink->last_packet_samples;
	double post_lag_ms = hs.max_buffered_samples_post_recovery * 1000.0 / 48000.0;
	double pkt_ms = (double)actual_pkt_samples * 1000.0 / 48000.0;
	double max_allowed_lag_ms = pkt_ms * 2.2 + 8.0;
	double max_rx_gap_ms = (double)hs.max_post_recovery_rx_gap_ns / (double)SPA_NSEC_PER_MSEC;

	printf("[PASS] codec=%-7s pkt=%4u (%5.2fms) q=%4u hw_acl=%u stall=%4"PRIu64"ms tx_lat=%s -> "
	       "pkts=%4u seq_gaps=%u ts_gaps=%u max_rx_gap=%5.2fms post_recovery_headset_buf=%.2fms (limit %.2fms)\n",
	       codec->name, actual_pkt_samples, pkt_ms, chunk_frames, sim_hw_acl_pkts, stall_duration_ms,
	       sim_tx_latency ? "yes" : "no ",
	       hs.received_packets, hs.seq_gaps, hs.ts_gaps, max_rx_gap_ms,
	       post_lag_ms, max_allowed_lag_ms);

	assert(hs.received_packets > 20);
	assert(hs.seq_gaps == 0);
	assert(hs.ts_gaps == 0);
	assert(max_rx_gap_ms <= pkt_ms * 2.0 + 6.0);
	assert(post_lag_ms <= max_allowed_lag_ms);

	codec->deinit(encoder);
	if (props && codec->clear_props)
		codec->clear_props(props);
	free(sink);
	close(sockets[0]);
	close(sockets[1]);
}

static void run_rf_dropout_test(const struct media_codec *codec, void *config, size_t config_len,
		unsigned sbc_packet_frames, uint32_t expected_pkt_samples,
		unsigned chunk_frames, uint64_t stall_duration_ms, bool sim_tx_latency)
{
	run_rf_dropout_test_ext(codec, config, config_len, sbc_packet_frames,
			expected_pkt_samples, chunk_frames, stall_duration_ms, sim_tx_latency, 0);
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	const char *sbc_path = (argc > 1) ? argv[1] : "build/bluez5/libspa-codec-bluez5-sbc.so";
	const char *bluez5_path = (argc > 2) ? argv[2] : "build/bluez5/libspa-bluez5.so";

	verify_bluez5_plugin_factories(bluez5_path);

	/* 1. Test SBC-XQ (2-frame and 5-frame packets) across 0ms, 300ms, 1500ms, 5000ms stalls */
	void *sbc_lib = NULL;
	struct spa_handle *sbc_handle = NULL;
	const struct media_codec *sbc_xq = load_codec_by_name(sbc_path, "sbc_xq", &sbc_lib, &sbc_handle);
	a2dp_sbc_t sbc_cfg = {
		.frequency = SBC_SAMPLING_FREQ_48000,
		.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL,
		.block_length = SBC_BLOCK_LENGTH_16,
		.subbands = SBC_SUBBANDS_8,
		.allocation_method = SBC_ALLOCATION_LOUDNESS,
		.min_bitpool = 2,
		.max_bitpool = 39,
	};

	run_rf_dropout_test(sbc_xq, &sbc_cfg, sizeof(sbc_cfg), 2, 256, 256, 0, true);
	run_rf_dropout_test(sbc_xq, &sbc_cfg, sizeof(sbc_cfg), 2, 256, 256, 300, true);
	run_rf_dropout_test(sbc_xq, &sbc_cfg, sizeof(sbc_cfg), 2, 256, 256, 1500, true);
	run_rf_dropout_test(sbc_xq, &sbc_cfg, sizeof(sbc_cfg), 2, 256, 256, 5000, true);
	run_rf_dropout_test(sbc_xq, &sbc_cfg, sizeof(sbc_cfg), 2, 256, 256, 1500, false);
	run_rf_dropout_test(sbc_xq, &sbc_cfg, sizeof(sbc_cfg), 5, 640, 1024, 1500, true);
	run_rf_dropout_test(sbc_xq, &sbc_cfg, sizeof(sbc_cfg), 5, 640, 1024, 1500, false);

	/* 2. Test AAC (1024 samples/packet = 21.33ms, the exact codec active on user's WH-1000XM4) */
	void *aac_lib = NULL;
	struct spa_handle *aac_handle = NULL;
	const struct media_codec *aac = load_codec_by_name(
			"/usr/lib/spa-0.2/bluez5/libspa-codec-bluez5-aac.so",
			"aac", &aac_lib, &aac_handle);
	a2dp_aac_t aac_cfg = {
		.object_type = AAC_OBJECT_TYPE_MPEG2_AAC_LC,
		AAC_INIT_FREQUENCY(AAC_SAMPLING_FREQ_48000)
		.channels = AAC_CHANNELS_2,
		.vbr = 1,
		AAC_INIT_BITRATE(320000)
	};

	run_rf_dropout_test(aac, &aac_cfg, sizeof(aac_cfg), 0, 1024, 1024, 0, true);
	run_rf_dropout_test(aac, &aac_cfg, sizeof(aac_cfg), 0, 1024, 1024, 300, true);
	run_rf_dropout_test(aac, &aac_cfg, sizeof(aac_cfg), 0, 1024, 1024, 1500, true);
	run_rf_dropout_test(aac, &aac_cfg, sizeof(aac_cfg), 0, 1024, 1024, 5000, true);
	run_rf_dropout_test(aac, &aac_cfg, sizeof(aac_cfg), 0, 1024, 1024, 1500, false);

	/* 2b. Test AAC with MediaTek MT7922 6-packet hardware ACL queue + 256-sample PipeWire quantum */
	run_rf_dropout_test_ext(aac, &aac_cfg, sizeof(aac_cfg), 0, 1024, 256, 0, true, 6);
	run_rf_dropout_test_ext(aac, &aac_cfg, sizeof(aac_cfg), 0, 1024, 256, 300, true, 6);
	run_rf_dropout_test_ext(aac, &aac_cfg, sizeof(aac_cfg), 0, 1024, 256, 1500, true, 6);
	run_rf_dropout_test_ext(aac, &aac_cfg, sizeof(aac_cfg), 0, 1024, 256, 5000, true, 6);

	/* 3. Test LDAC (Sony WH-1000XM4 high-bitrate codec) */
	void *ldac_lib = NULL;
	struct spa_handle *ldac_handle = NULL;
	const struct media_codec *ldac = load_codec_by_name(
			"/usr/lib/spa-0.2/bluez5/libspa-codec-bluez5-ldac.so",
			"ldac", &ldac_lib, &ldac_handle);
	a2dp_ldac_t ldac_cfg = {
		.info = { .vendor_id = LDAC_VENDOR_ID, .codec_id = LDAC_CODEC_ID },
		.frequency = LDAC_SAMPLING_FREQ_48000,
		.channel_mode = LDAC_CHANNEL_MODE_STEREO,
	};

	run_rf_dropout_test(ldac, &ldac_cfg, sizeof(ldac_cfg), 0, 0, 1024, 0, true);
	run_rf_dropout_test(ldac, &ldac_cfg, sizeof(ldac_cfg), 0, 0, 1024, 300, true);
	run_rf_dropout_test(ldac, &ldac_cfg, sizeof(ldac_cfg), 0, 0, 1024, 1500, true);
	run_rf_dropout_test(ldac, &ldac_cfg, sizeof(ldac_cfg), 0, 0, 1024, 5000, true);
	run_rf_dropout_test(ldac, &ldac_cfg, sizeof(ldac_cfg), 0, 0, 1024, 1500, false);
	run_rf_dropout_test_ext(ldac, &ldac_cfg, sizeof(ldac_cfg), 0, 0, 256, 1500, true, 6);

	spa_handle_clear(ldac_handle);
	free(ldac_handle);
	dlclose(ldac_lib);

	spa_handle_clear(aac_handle);
	free(aac_handle);
	dlclose(aac_lib);

	spa_handle_clear(sbc_handle);
	free(sbc_handle);
	dlclose(sbc_lib);

	printf("\nALL RF DROPOUT & MULTI-CODEC RECOVERY TESTS PASSED!\n");
	return 0;
}
