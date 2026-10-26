# pipewire-sony-xm4-lowlatency

A custom low-latency PipeWire BlueZ5 A2DP SPA driver (`libspa-bluez5.so` + `libspa-codec-bluez5-sbc.so`) that eliminates **progressive post-dropout audio lag** on **Sony WH-1000XM4** (and similar MediaTek/Sony A2DP headsets) on Linux.

> **Companion Omarchy Plugin**: Pair this driver with [`omarchy-sony-xm4`](https://github.com/mikhkz/omarchy-sony-xm4) for bar battery telemetry, ANC / Wind / Ambient 0–20 / Focus on Voice control, Multipoint switching, and seamless A2DP handoff between PC and phone.

---

## Why Does Audio Lag Accumulate After Bluetooth Micro-Dropouts on Linux?

When using Sony WH-1000XM4 on Linux (especially in low-latency workloads, gaming, or over 2.4 GHz Wi-Fi interference), brief RF interference stalls the Bluetooth L2CAP transmit queue. In stock PipeWire (`spa/plugins/bluez5/media-sink.c`), three interacting behaviors turn a 100 ms radio stall into **500 ms – 2+ seconds of permanent audio delay**:

1. **Unrewound RTP `seqnum` and `timestamp` on `-EAGAIN`**:
   - Stock `flush_packets()` increments `this->seqnum` and `this->sample_count` **before** calling `write()` on the L2CAP socket.
   - When the kernel socket buffer fills up (`-EAGAIN`) and packets are dropped via `flush_Ready()`, stock PipeWire **does not rewind** `seqnum` or `sample_count`.
   - When transmission resumes, the headset's internal DSP sees a jump in the RTP sequence number and RTP timestamp (`seq_gap > 0`, `ts_gap > 0`). Interpreting this as severe network jitter, the Sony WH-1000XM4 firmware **permanently expands its internal jitter buffer** for the remainder of the A2DP session.
2. **Stale Audio Queued in Kernel `sk_wmem_alloc` + `&port->ready`**:
   - Stock PipeWire only starts dropping `&port->ready` after the kernel socket buffer is 100% full (`-EAGAIN`), leaving ~10–14 KB of stale audio queued in the kernel socket (`sk_wmem_alloc`) that must play out in slow motion after the radio recovers.
   - *Linux Kernel Quirk (`net/bluetooth/af_bluetooth.c:bt_sock_ioctl`)*: Unlike `AF_UNIX`/`AF_INET` where `ioctl(fd, TIOCOUTQ)` returns queued bytes, `AF_BLUETOOTH` returns **free space** (`sk_sndbuf - sk_wmem_alloc`). Our stall gate converts `used = fd_buffer_size - value` and proactively trims `&port->ready` to 1 fresh packet whenever `outq_bytes >= 4608` for 2+ consecutive ticks.
3. **Coarse 6-Frame SBC-XQ Packetization (~16 ms per packet)**:
   - Stock `a2dp-codec-sbc.c` packs up to 6 SBC-XQ frames into a single RTP packet (~895 bytes, 768 samples = 16 ms). Our `libspa-codec-bluez5-sbc.so` honors `bluez5.a2dp.sbc.packet-frames = 2` (256 samples = **5.33 ms** per RTP packet, 315 bytes), cutting packetizer latency by 3×.

---

## Stock PipeWire vs. `pipewire-sony-xm4-lowlatency`

| Metric / Behavior | Stock PipeWire (`1.6.8`) | `pipewire-sony-xm4-lowlatency` |
| :--- | :--- | :--- |
| **SBC-XQ packet size (`packet-frames`)** | 6 frames (~895 B, **16.0 ms** per packet) | **2 frames** (315 B, **5.33 ms** per packet) |
| **Behavior on L2CAP stall (`-EAGAIN` / `TIOCOUTQ`)** | Increments RTP `seqnum` & `timestamp` on dropped packets (`seq_gap > 0`, `ts_gap > 0`) | **Zero-gap RTP rewind** (`seqnum--`, `sample_count -= block_samples` via `discard_current_packet()`) |
| **Sony WH-1000XM4 internal DSP buffer after stall** | Expands permanently (0.5 s – 2+ s cumulative audio lag) | **Never expands** (`seq_gaps = 0`, `ts_gaps = 0`) |
| **Pre-stall queue trimming (`&port->ready`)** | Waits until socket is 100% full (`-EAGAIN`) | Proactively trims `&port->ready` to 1 fresh packet when `outq_bytes >= 4608` for 2 ticks |
| **Transport state drop (`< ACTIVE`)** | Leaves stale packets in ring buffer | Immediately calls `do_stop(this)` to reset transport cleanly |
| **System files modified (`/usr/lib`)** | N/A | **None** — installed strictly in `~/.local/lib/...` with version-guarded `SPA_PLUGIN_DIR` |

---

## Quick Start (Build, Test & Activate)

### 1. Build and run the 22 transport-recovery unit tests

```sh
chmod +x build.sh activate.sh revert.sh wireplumber-xm4
./build.sh
./build/test-transport-recovery
```

### 2. Activate in User Space (Safe, Non-Root)

```sh
./activate.sh
```

`activate.sh` installs the compiled modules into `~/.local/lib/sony-wh1000xm4/pipewire-1.6.8/spa-0.2/bluez5/`, configures `~/.config/wireplumber/wireplumber.conf.d/90-sony-wh1000xm4-latency.conf` (`bluez5.a2dp.sbc.packet-frames = 2`), and restarts `wireplumber`.

If you upgrade PipeWire via `pacman -Syu` in the future, `wireplumber-xm4` automatically checks `pkg-config --modversion libpipewire-0.3 == 1.6.8` and falls back to stock `/usr/lib/spa-0.2` if the version changes.

### 3. Revert to Stock PipeWire Anytime

```sh
./revert.sh
```

---

## Standalone Unified Patch

If you are building PipeWire from source or packaging for another distribution/version, apply [`patches/pipewire-1.6.8-sony-xm4-zero-lag.patch`](patches/pipewire-1.6.8-sony-xm4-zero-lag.patch) directly to `spa/plugins/bluez5/`.

---

## License

[MIT](LICENSE) (PipeWire upstream files in `vendor/` and `src/` are licensed under the MIT License by Wim Taymans and contributors).
