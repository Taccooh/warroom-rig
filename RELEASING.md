# Releasing

warroom-rig ships **two firmwares for two device classes**, released on two
independent tag tracks in this one repo:

| Track | Runs on | Tag scheme | Internal version |
|---|---|---|---|
| **Core (hub)** | Marauder v7 / LOLIN D32 (ESP32) | `core-vX.Y` | `WARROOM_RIG_VERSION` |
| **Node** | Waveshare C5-Zero (ESP32-C5), NodeMCU-32 | `node-v<koko>-warroom.N` | `FIRMWARE_VERSION` |

They version independently: a node fix must not force a core release, and vice
versa. The node tag deliberately carries the upstream Koko base version
(e.g. `node-v2.2.0-warroom.1` = Koko's ESP32DualBandWardriver **v2.2.0** + our
Nth warroom node revision) so the provenance is visible in the tag itself.

Mark `v0.x` releases as **pre-release** on GitHub.

---

## Core (hub) — `core-vX.Y`

The carved-down Marauder v7 firmware: Rig Mode (Wardrive Core aggregator),
WDGWars Upload, File Server AP. This is the flagship and is mostly our own work.

**Build** (from repo root; needs arduino-cli 1.4.1 + esp32 core 3.3.4, see
`README.md` for the `-Wl,-zmuldefs` platform.txt patch):

```bash
ARDUINO_CLI=/path/to/arduino-cli
LIBS=$(pwd)/libs
LIB_ARGS=""; for d in $LIBS/Custom*/; do LIB_ARGS="$LIB_ARGS --library $d"; done
$ARDUINO_CLI compile \
  --fqbn "esp32:esp32:d32:PartitionScheme=min_spiffs" \
  $LIB_ARGS \
  --build-property "compiler.cpp.extra_flags=-DMARAUDER_V7 -DMARAUDER_CORE_MODE -DMARAUDER_WDGWARS_UPLOAD -DMARAUDER_FILE_SERVER_AP" \
  --output-dir ./out \
  ESP32Marauder/esp32_marauder/esp32_marauder.ino
```

**Assets:** `warroom-rig-core-vX.Y-merged.bin` (full 4 MB image, flash at `0x0`)
and `warroom-rig-core-vX.Y-app.bin` (app partition only).

**Flash** (merged image, most universal):

```bash
esptool.py --chip esp32 -p <PORT> write_flash 0x0 warroom-rig-core-vX.Y-merged.bin
```

or `arduino-cli upload -p <PORT> --fqbn esp32:esp32:d32 ...`.

---

## Node — `node-v<koko>-warroom.N`

**Koko's ESP32DualBandWardriver v2.2.0 — his firmware, including the full ESP-NOW
CORE/NODE/SOLO swarm** (roles, heartbeats, session handling, multi-node
channel-slicing, receive protocol). Our changes on top are **board ports + fleet
adaptations, not new architecture**: the C5-Zero / NodeMCU-32 targets, headless
compile-time role selection, session START/STOP gating, a broadcast-peer setup
fix, single-node BLE-host election, and 2.4-GHz-only-node channel handling. For
the stock firmware, use Koko's upstream directly. Two node targets share
`ESP32DualBandWardriver/src/src.ino`:

- **C5-Zero** (`-DC5_ZERO_NODE`) — ESP32-C5, headless. **CDCOnBoot=cdc is
  mandatory** or the serial console stays silent. Prebuilt, field-tested bins +
  a Python flasher live in `ESP32DualBandWardriver/C5_Py_Flasher_c5zero/`.
  <!-- TODO: pin the exact ESP32-C5 arduino-cli FQBN here once confirmed. -->
- **NodeMCU-32** (`-DNODEMCU32_NODE`) — standard ESP32, FQBN
  `esp32:esp32:nodemcu-32s`. Not prebuilt; build from source if needed.

**Assets:** `warroom-rig-node-c5zero-v<...>.bin` (C5-Zero app image). Flash via
the bundled `c5_flasher.py` (handles bootloader + partitions + app for the C5).

**Attribution (required + fair):** the node is ~93% Koko's MIT code. Every node
release MUST credit **Just Call Me Koko** and link the upstream
`https://github.com/justcallmekoko/ESP32DualBandWardriver` prominently in the
notes. See `CREDITS.md`.

---

## Pre-release checklist

1. Working tree **clean** and on the intended commit (`git status`).
2. Build the asset bins **fresh from that commit** — never ship a stale binary.
   For the C5-Zero, verify the bundled bin matches current node source (it is
   built + field-tested on the node setup, not by the hub toolchain).
3. Record **SHA-256** of every asset in the release notes.
4. Node notes: Koko credit + upstream link present.
5. Tag the correct commit; mark `v0.x` as pre-release.
6. Attach bins; publish.
