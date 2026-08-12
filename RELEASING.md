# Releasing

warroom-rig ships **two firmwares for two device classes**, released on two
independent tag tracks in this one repo:

| Track | Runs on | Tag scheme | Internal version |
|---|---|---|---|
| **Core (hub)** | Marauder v7 (ESP32, tested) · Cardputer ADV (boots and runs, see below) · Marauder v7.1 (builds, never run) · Marauder v8 (ESP32-C5 — unfinished, gated off) | `core-vX.Y` | `WARROOM_RIG_VERSION` |
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

`--chip` follows the board, not the firmware: `esp32` for the Marauder v7/v7.1,
**`esp32s3` for the Cardputer ADV**, `esp32c5` for the V8. Copying the v7 line
onto a Cardputer is the first thing people get wrong. If esptool cannot pull the
board into download mode on its own, hold BOOT/G0 while plugging the cable in.

### Marauder v7.1 variant

Same board family, same FQBN, same everything — swap `-DMARAUDER_V7` for
`-DMARAUDER_V7_1` in the recipe above and nothing else changes.

It compiles green, but it has never been run on a v7.1 board: the pin map in
`configs.h` is inherited from the v7 and unconfirmed. Do not put a v7.1 image in
a release without someone booting one first. (For a long time it did not even
build — its config was missing `HAS_NIMBLE_2` / `HAS_IDF_3`, so it fell into
legacy IDF branches this tree does not carry. That is fixed.)

### Marauder v8 variant (ESP32-C5, touch) — UNFINISHED, DO NOT SHIP

This target does **not** currently produce a working device, and the build
refuses it unless you also define `WARROOM_RIG_ALLOW_BROKEN_V8`. It is documented
here so whoever has the hardware can finish the port, not so it can be released.
Do **not** cut a `core-` tag that carries a V8 image until the problems below are
actually fixed on hardware. What is wrong — all confirmed against the shipped
binary, all because the config still describes a classic-ESP32 Marauder instead
of the C5:

- **Panel pins land on the C5's flash bus.** The build passes no panel flags, so
  TFT_eSPI takes `libs/CustomTFT_eSPI/User_Setup_dual_nrf24.h` — the OG Marauder
  ILI9341 config (CS 17, DC 26, MOSI 23, SCLK 18, BL 32). On the ESP32-C5 those
  are flash/MSPI pins, and GPIO32 does not exist at all (29 GPIOs). The C5 needs
  its own panel config in build flags, the way the Cardputer does — but nobody
  here has the V8 schematic, so the pins are unknown. Do not guess them.
- **GPS UART on the USB pins.** `GPS_TX 14 / GPS_RX 13` are the C5's `USB_DP` /
  `USB_DM`; with `CDCOnBoot=cdc` the console loses its own pins mid-setup.
- **Touch input is dead.** `HAS_TOUCH` is the only input path, but the active TFT
  setup has `TOUCH_CS -1`, so `getTouch()` never returns true — the UI accepts
  nothing.
- **No real battery gauge.** V8 names an I2C bus but no gauge. It used to inherit
  the auto-detect fallback (now fixed in `configs.h` so it no longer blind-writes
  an AXP192 on the C5's flash pins), but it still has no gauge of its own.

`configs.h` documents the same four at the V8 gate. Selected by `-DMARAUDER_V8`
instead of `-DMARAUDER_V7`; the touch UIs (home console, Rig Mode session
buttons, Upload file picker) are all `#ifdef HAS_TOUCH` and only exist here — but
per the touch note above, none of them can be driven until `TOUCH_CS` is real.
The recipe below is kept for that work; it will not ship a usable image as-is.

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
  --build-property "compiler.cpp.extra_flags=-DMARAUDER_V8 -DWARROOM_RIG_ALLOW_BROKEN_V8 -DMARAUDER_CORE_MODE -DMARAUDER_WDGWARS_UPLOAD -DMARAUDER_FILE_SERVER_AP" \
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

Same hub firmware for the Cardputer ADV. It builds, flashes, boots and runs:
display, keyboard, SD and Rig Mode with a four-node fleet have all been exercised
on the hardware. Treat it as newer and less travelled than the v7 rather than as
unproven.

Key differences from the v7 build:
- **Chip/FQBN:** `esp32:esp32:m5stack_cardputer`. The core has no ADV entry; the
  plain Cardputer is the same ESP32-S3 family and the differences are menu
  options.
- **Screen:** 240×135 landscape instead of 240×320 portrait. `RigTheme` switches
  to a compact profile below 200 px of height — smaller rows, one font step
  down, card subtitles dropped.
- **Panel config comes from build flags** (`-DUSER_SETUP_LOADED -DST7789_DRIVER
  …`), not from a `tft_setup.h` in the sketch. TFT_eSPI reads such a file before
  its own setup and then treats the configuration as finished **for every
  target**, so a file added for the Cardputer silently reconfigures the Marauder
  boards too — that was tried, and it blanked the V7's display while changing
  none of its values. Flags are per-build, land before any header is parsed, and
  still reach the library: `platform.txt` has one `recipe.cpp.o.pattern` and it
  carries `compiler.cpp.extra_flags`. Build with `--clean` when changing them,
  or a cached TFT_eSPI object compiled under the old configuration survives.
  `-DTFT_RGB_ORDER=1` is not optional: a 135×240 panel gets `CGRAM_OFFSET`
  defined for it automatically, and TFT_eSPI then defaults the colour order to
  **BGR**. Upstream's `User_Setup_marauder_m5cardputer_adv.h` sets RGB; leaving
  the flag off swaps red and blue on everything.
- **Needs a TFT_eSPI patched for the S3 on modern IDF.** `SPI_PORT` was `FSPI`,
  which is Arduino's bus enum (0 on the S3), not the peripheral number the IDF
  register macros want. `REG_SPI_BASE(0)` returns 0 on IDF 5.5, so every SPI
  register write went to its bare offset — `*(0x10) = SPI_USR_MOSI` panics on
  the first command `tft.init()` sends. TFT_eSPI's own `REG_SPI_BASE` fallback
  is `#ifndef`-guarded and no longer fires because IDF defines the macro now.
  Vendored copy sets `SPI_PORT 2`; see the `[warroom-rig]` note in
  `libs/CustomTFT_eSPI/Processors/TFT_eSPI_ESP32_S3.h`. Only the S3 target
  reaches that file, so the V7 and V8 are untouched.
- **PSRAM off.** The ADV config defines no `HAS_PSRAM`, so the firmware never
  allocates from it. Enabling it only adds an early-boot init that hangs if the
  module's line mode (QSPI vs OPI) does not match the build — risk with no
  return.
- **Input:** the 56-key keyboard behind a TCA8418 I²C controller, not buttons.
  `RigInput` maps the printed arrow cluster (`; . , /`), ENTER and ESC/BACKSPACE
  onto the same six logical keys the button boards produce. The board's
  `U/D/L/R_BTN` are all `-1`; that is expected and no longer means "no input".
- **Toolchain flags:** the ADV config sets `HAS_NIMBLE_2` and `HAS_IDF_3` like
  the other targets. Upstream builds the Cardputer against an older core and
  leaves them off; without them the build falls into the legacy IDF/NimBLE
  branches and fails.

```bash
PANEL="-DUSER_SETUP_LOADED -DST7789_DRIVER -DTFT_WIDTH=135 -DTFT_HEIGHT=240 \
-DTFT_RGB_ORDER=1 \
-DTFT_MOSI=35 -DTFT_SCLK=36 -DTFT_CS=37 -DTFT_DC=34 -DTFT_RST=33 -DTFT_BL=38 \
-DTFT_BACKLIGHT_ON=HIGH -DLOAD_GLCD -DLOAD_FONT2 -DLOAD_FONT4 -DLOAD_FONT6 \
-DLOAD_FONT7 -DLOAD_FONT8 -DLOAD_GFXFF -DSMOOTH_FONT \
-DSPI_FREQUENCY=40000000 -DSPI_READ_FREQUENCY=20000000"

$ARDUINO_CLI compile --clean \
  --fqbn "esp32:esp32:m5stack_cardputer:PartitionScheme=huge_app,PSRAM=disabled" \
  $LIB_ARGS \
  --build-property "compiler.c.extra_flags=$PANEL" \
  --build-property "compiler.cpp.extra_flags=-DMARAUDER_CARDPUTER_ADV -DMARAUDER_CORE_MODE -DMARAUDER_WDGWARS_UPLOAD -DMARAUDER_FILE_SERVER_AP $PANEL" \
  --output-dir ./out-adv \
  ESP32Marauder/esp32_marauder/esp32_marauder.ino
```

Image is ~1.68 MB (53 % of the 3 MB app slot).

**Flash it** — note the chip, this is an S3 and not the v7's plain ESP32:

```bash
python -m esptool --chip esp32s3 -p <PORT> write_flash 0x0 \
  warroom-rig-core-vX.Y-cardputer-adv-merged.bin
```

Nothing has to be built to get there: the release track ships the ADV image
alongside the v7 one, so `...-cardputer-adv-merged.bin` off the Releases page is
the short path, and the recipe above is for changing something.

**Panel flags.** The recipe drives the panel with `-DST7789_DRIVER` and
`-DSPI_FREQUENCY=40000000`. These are now confirmed on hardware: the ADV boots,
`tft.init()` returns, and the UI is legible with the right colours. The vendored
upstream setup for this board
(`libs/CustomTFT_eSPI/User_Setup_marauder_m5cardputer_adv.h`) uses
`ST7789_2_DRIVER` and `20000000` instead; that pair is untested here and is the
first knob to turn if a different ADV unit shows a blank or garbled panel. The
two drivers send different init command lists, and the higher clock is more than
some panels of this class tolerate, so it is a real fallback rather than a
formality.

**What actually blocked the first bring-up was neither of those**, and it is
worth knowing before chasing the panel: on the S3, TFT_eSPI's `spi` is the global
Arduino `SPI`, and `SPIClass::begin()` returns `true` without doing anything when
the object already holds a bus handle — which it can while the bus is stopped and
`SPI_CLK_GATE` is zero. The master clock stays gated, every command sets its busy
bit, nothing clears it, and the boot dies inside `tft_Write_8()` with the
backlight on. `Display::RunSetup()` now calls `SPI.end()` before `SPI.begin()` for
this target, which is correct from either state. A blank ADV that is *silent on
the console too* is this, not the panel flags.

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
