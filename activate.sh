#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
[[ "$(pkg-config --modversion libpipewire-0.3)" == 1.6.8 ]]

if [[ ! -f build/bluez5/libspa-bluez5.so || ! -f build/bluez5/libspa-codec-bluez5-sbc.so ]]; then
    ./build.sh
    ./build/test-transport-recovery
fi

state="$HOME/.local/state/sony-audio/2026-10-26-custom-driver"
config="$HOME/.config/wireplumber/wireplumber.conf.d/90-sony-wh1000xm4-latency.conf"
dropin="$HOME/.config/systemd/user/wireplumber.service.d/90-sony-xm4-driver.conf"
target_dir="$HOME/.local/lib/sony-wh1000xm4/pipewire-1.6.8/spa-0.2/bluez5"

mkdir -p "$state"
if [[ ! -e "$state/config-checked" ]]; then
    if [[ -f "$config" ]]; then
        cp -- "$config" "$state/latency.conf.before-driver"
    fi
    touch "$state/config-checked"
fi
if [[ ! -e "$state/dropin-checked" ]]; then
    if [[ -e "$dropin" ]]; then
        cp -- "$dropin" "$state/dropin.before-driver"
    fi
    touch "$state/dropin-checked"
fi

install -Dm755 build/bluez5/libspa-bluez5.so "$target_dir/libspa-bluez5.so"
install -Dm755 build/bluez5/libspa-codec-bluez5-sbc.so "$target_dir/libspa-codec-bluez5-sbc.so"
install -Dm755 wireplumber-xm4 "$HOME/.local/libexec/wireplumber-xm4"

mkdir -p -- "$(dirname -- "$dropin")"
cat > "$dropin" <<'EOF'
[Service]
ExecStart=
ExecStart=%h/.local/libexec/wireplumber-xm4
EOF

mkdir -p -- "$(dirname -- "$config")"
cat > "$config" <<'EOF'
# Sony WH-1000XM4 low-latency & anti-lag configuration
wireplumber.profiles = {
  main = {
    monitor.bluez-midi = disabled
  }
}

monitor.bluez.rules = [
  {
    matches = [ { device.name = "~bluez_card.*" } ]
    actions = {
      update-props = {
        bluez5.a2dp.sbc.packet-frames = 2
      }
    }
  }
]
EOF

systemctl --user daemon-reload
systemctl --user restart wireplumber
