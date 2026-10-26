#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
version=$(pkg-config --modversion libpipewire-0.3)
if [[ "$version" != 1.6.8 ]]; then
  printf 'This build targets PipeWire 1.6.8; installed: %s\n' "$version" >&2
  exit 1
fi
vendor=vendor/pipewire-1.6.8/spa/plugins/bluez5
mkdir -p build/bluez5

read -r -a codec_cflags <<< "$(pkg-config --cflags libspa-0.2 sbc)"
read -r -a codec_libs <<< "$(pkg-config --libs sbc)"
gcc -std=gnu11 -O2 -g -D_GNU_SOURCE -DSPA_AUDIO_MAX_CHANNELS=128u -DCODEC_PLUGIN -fPIC \
  -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter \
  -Werror=implicit-function-declaration "${codec_cflags[@]}" -Isrc -I"$vendor" \
  -shared -Wl,-z,defs,-z,relro,-z,now \
  src/a2dp-codec-sbc.c "$vendor/media-codecs.c" "${codec_libs[@]}" \
  -o build/bluez5/libspa-codec-bluez5-sbc.so

read -r -a bluez_cflags <<< "$(pkg-config --cflags libspa-0.2 dbus-1 bluez glib-2.0 gio-2.0 gio-unix-2.0 gobject-2.0 sbc libusb-1.0)"
read -r -a bluez_libs <<< "$(pkg-config --libs dbus-1 bluez glib-2.0 gio-2.0 gio-unix-2.0 gobject-2.0 libusb-1.0)"
gcc -std=gnu11 -O2 -g -D_GNU_SOURCE -DSPA_AUDIO_MAX_CHANNELS=128u -fPIC \
  -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
  -Werror=implicit-function-declaration \
  -include src/config.h \
  "${bluez_cflags[@]}" -Isrc -I"$vendor" -Ivendor/pipewire-1.6.8/spa/include-private \
  -shared -Wl,-z,defs,-z,relro,-z,now,-z,nodelete \
  src/plugin.c \
  src/media-sink.c \
  "$vendor/codec-loader.c" \
  "$vendor/media-codecs.c" \
  "$vendor/media-source.c" \
  "$vendor/sco-io.c" \
  "$vendor/iso-io.c" \
  "$vendor/quirks.c" \
  "$vendor/player.c" \
  "$vendor/bluez5-device.c" \
  "$vendor/bluez5-dbus.c" \
  "$vendor/hci.c" \
  "$vendor/dbus-monitor.c" \
  "$vendor/backend-native.c" \
  "$vendor/upower.c" \
  "$vendor/telephony.c" \
  "$vendor/backend-ofono.c" \
  "$vendor/backend-hsphfpd.c" \
  "${bluez_libs[@]}" -lm -ldl \
  -o build/bluez5/libspa-bluez5.so

gcc -std=gnu11 -O1 -g -fsanitize=address,undefined -D_GNU_SOURCE -DSPA_AUDIO_MAX_CHANNELS=128u \
  -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
  "${bluez_cflags[@]}" -Isrc -I"$vendor" -Ivendor/pipewire-1.6.8/spa/include-private \
  tests/test-transport-recovery.c "${codec_libs[@]}" "${bluez_libs[@]}" -lm -ldl \
  -o build/test-transport-recovery

