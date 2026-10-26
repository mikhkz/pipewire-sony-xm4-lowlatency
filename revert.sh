#!/usr/bin/env bash
set -euo pipefail
state="$HOME/.local/state/sony-audio/2026-10-26-custom-driver"
config="$HOME/.config/wireplumber/wireplumber.conf.d/90-sony-wh1000xm4-latency.conf"
dropin="$HOME/.config/systemd/user/wireplumber.service.d/90-sony-xm4-driver.conf"
target_dir="$HOME/.local/lib/sony-wh1000xm4/pipewire-1.6.8/spa-0.2/bluez5"

if [[ -f "$state/latency.conf.before-driver" ]]; then
    cp -- "$state/latency.conf.before-driver" "$config"
else
    rm -f -- "$config"
fi

if [[ -f "$state/dropin.before-driver" ]]; then
    cp -- "$state/dropin.before-driver" "$dropin"
else
    rm -f -- "$dropin"
fi

rm -f -- "$target_dir/libspa-bluez5.so" "$target_dir/libspa-codec-bluez5-sbc.so"

systemctl --user daemon-reload
systemctl --user restart wireplumber
