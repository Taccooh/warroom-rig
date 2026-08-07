# Releasing

warroom-rig ships **two firmwares for two device classes**, released on two
independent tag tracks in this one repo:

| Track | Runs on | Tag scheme | Internal version |
|---|---|---|---|
| **Core (hub)** | Marauder v7 (ESP32) · Marauder v8 (ESP32-C5, touch) | `core-vX.Y` | `WARROOM_RIG_VERSION` |
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
  --fqbn "esp32:esp32:d32" \
  $LIB_ARGS \
  --build-property "build.partitions=huge_app" \
  --build-property "upload.maximum_size=3145728" \
  --build-property "compiler.cpp.extra_flags=-DMARAUDER_V7 -DMARAUDER_CORE_MODE -DMARAUDER_WDGWARS_UPLOAD -DMARAUDER_FILE_SERVER_AP" \
  --output-dir ./out \
  ESP32Marauder/esp32_marauder/esp32_marauder.ino
```

`huge_app` (3 MB single app slot, no OTA) is passed as a build property because
the d32 board does not list it in its partition menu. OTA was dropped
deliberately; `SDInterface::runUpdate()` refuses to run when there is no second
app partition, rather than overwriting the running one.

**Assets:** `warroom-rig-core-vX.Y-merged.bin` (full 4 MB image, flash at `0x0`)
and `warroom-rig-core-vX.Y-app.bin` (app partition only).

**Flash** (merged image, most universal):

```bash
esptool.py --chip esp32 -p <PORT> write_flash 0x0 warroom-rig-core-vX.Y-merged.bin
```

or `arduino-cli upload -p <PORT> --fqbn esp32:esp32:d32 ...`.

### Marauder v8 variant (ESP32-C5, touch)

Same hub firmware, built for the touch-only ESP32-C5. Selected by `-DMARAUDER_V8`
instead of `-DMARAUDER_V7`; the touch UIs (home console, Rig Mode session
buttons, Upload file picker) are all `#ifdef HAS_TOUCH` and only exist here.

Key differences from the v7 build:
- **Chip/FQBN:** `esp32:esp32:esp32c5`. `CDCOnBoot=cdc` is mandatory (else the
  USB-CDC console stays silent), same as the C5 node.
- **Partition:** `huge_app` (3 MB app / 1 MB SPIFFS, **no OTA**). The image is
  ~2.17 MB and does not fit `min_spiffs`. All three targets are on `huge_app`
  now; OTA is dropped because the rig is USB-flashed.
- **Bootloader offset is `0x2000`, not `0x1000`** — a C5 quirk. The merged image
  already places it correctly, so flashing `merged.bin` at `0x0` is safe.
- Needs a TFT_eSPI that knows the C5: this repo's vendored copy is patched (route
  the C5 through the generic ESP32 processor path + the C3-style RISC-V register
  fixes; see the `[warroom-rig]` notes in `libs/CustomTFT_eSPI/Processors/TFT_eSPI_ESP32.h`).

```bash
$ARDUINO_CLI compile \
  --fqbn "esp32:esp32:esp32c5:CDCOnBoot=cdc,PartitionScheme=huge_app,FlashSize=4M" \
  $LIB_ARGS \
  --build-property "compiler.cpp.extra_flags=-DMARAUDER_V8 -DMARAUDER_CORE_MODE -DMARAUDER_WDGWARS_UPLOAD -DMARAUDER_FILE_SERVER_AP" \
  --output-dir ./out-v8 \
  ESP32Marauder/esp32_marauder/esp32_marauder.ino
```

**Flash** (merged image; handles the 0x2000 bootloader offset internally):

```bash
python -m esptool --chip esp32c5 -p <PORT> -b 921600 \
  --before default-reset --after hard-reset \
  write-flash 0x0 out-v8/esp32_marauder.ino.merged.bin
```

### M5Stack Cardputer ADV variant (ESP32-S3, keyboard)

Same hub firmware for the Cardputer ADV. **Not tested on hardware** — see the
Hardware section of `README.md` before shipping this to anyone.

Key differences from the v7 build:
- **Chip/FQBN:** `esp32:esp32:m5stack_cardputer`. The core has no ADV entry; the
  plain Cardputer is the same ESP32-S3 family and the differences are menu
  options (`PSRAM=enabled`).
- **Screen:** 240×135 landscape instead of 240×320 portrait. `RigTheme` switches
  to a compact profile below 200 px of height — smaller rows, one font step
  down, card subtitles dropped. Panel driver and pins come from
  `esp32_marauder/tft_setup.h`.
- **Input:** the 56-key keyboard behind a TCA8418 I²C controller, not buttons.
  `RigInput` maps the printed arrow cluster (`; . , /`), ENTER and ESC/BACKSPACE
  onto the same six logical keys the button boards produce. The board's
  `U/D/L/R_BTN` are all `-1`; that is expected and no longer means "no input".
- **Toolchain flags:** the ADV config sets `HAS_NIMBLE_2` and `HAS_IDF_3` like
  the other targets. Upstream builds the Cardputer against an older core and
  leaves them off; without them the build falls into the legacy IDF/NimBLE
  branches and fails.

```bash
$ARDUINO_CLI compile \
  --fqbn "esp32:esp32:m5stack_cardputer:PartitionScheme=huge_app,PSRAM=enabled" \
  $LIB_ARGS \
  --build-property "compiler.cpp.extra_flags=-DMARAUDER_CARDPUTER_ADV -DMARAUDER_CORE_MODE -DMARAUDER_WDGWARS_UPLOAD -DMARAUDER_FILE_SERVER_AP" \
  --output-dir ./out-adv \
  ESP32Marauder/esp32_marauder/esp32_marauder.ino
```

Image is ~1.68 MB (53 % of the 3 MB app slot).

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

### Node assets and how they get flashed

Ship the same pair the core track ships, plus checksums:

| Asset | What it is |
|---|---|
| `warroom-rig-node-c5zero-v<...>-merged.bin` | **the one to hand people** — bootloader + partition table + app in one file, flashed at `0x0` |
| `warroom-rig-node-c5zero-v<...>-app.bin` | app only, `0x10000`, for re-flashing a node that already runs this firmware |
| `SHA256SUMS.txt` | both of the above |

The app image on its own is **not bootable on a fresh C5**. It needs a
bootloader and a partition table that the release did not carry, at offsets
nobody guesses:

```
0x2000   bootloader.bin      <- 0x2000, not 0x1000. A C5 quirk.
0x8000   partitions.bin
0x10000  <app>.bin
```

Those two live in `ESP32DualBandWardriver/C5_Py_Flasher_c5zero/bins/`. Build the
merged asset from them rather than from a fresh compile, so the release ships
the binaries that were actually tested in the field:

```bash
cd ESP32DualBandWardriver/C5_Py_Flasher_c5zero/bins
python -m esptool --chip esp32c5 merge-bin \
  -o warroom-rig-node-c5zero-v<...>-merged.bin \
  0x2000 bootloader.bin 0x8000 partitions.bin 0x10000 <app>.bin
```

Three ways to get it onto a node, in the order to try them:

1. **Merged image, any esptool.** One file, one offset, no assumptions about
   what is already on the chip.
   ```bash
   python -m esptool --chip esp32c5 -p <PORT> write-flash 0x0 <...>-merged.bin
   ```
2. **Browser** — esptool-js or esp.huhn.me, same file at `0x0`. Needs Chrome or
   Edge (WebSerial), and needs the tool's bundled esptool-js to know the
   ESP32-C5; the chip is new enough that older builds fail at detection, before
   offsets matter. You find out at "Connect".
3. **`c5_flasher.py`** in the repo — the field-proven path, and the fallback
   when the browser route cannot see the chip.

The C5-Zero's USB is the chip's own USB-JTAG on GPIO13/14 with no bridge in
between, so a vanished serial port is a firmware symptom, not a cable one: hold
BOOT while plugging in to force ROM download mode, which always accepts a flash.

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
