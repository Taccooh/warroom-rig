# warroom-rig

**The field firmware for [warroom](https://warroom.mechanics-toolbox.org) — a
purpose-built passive wardriving rig for the [wdgwars](https://wdgwars.pl) game.**

warroom-rig is a hardware companion to the warroom PWA: a handheld ESP32 hub that
aggregates a fleet of small wardrive nodes, enriches every record with a GPS fix,
writes WiGLE CSV to SD, and uploads to the game. It is **not** a general-purpose
security tool — it does one thing, for the game, on purpose.

It is derived from two MIT-licensed projects by
[Just Call Me Koko](https://github.com/justcallmekoko):
[ESP32Marauder](https://github.com/justcallmekoko/ESP32Marauder) (the handheld
hub) and
[ESP32DualBandWardriver](https://github.com/justcallmekoko/ESP32DualBandWardriver)
(the C5 / NodeMCU nodes). This repository is **not** affiliated with or endorsed
by Koko. Full attribution and licenses: [CREDITS.md](CREDITS.md).

## Passive by construction

warroom-rig is a Marauder fork with the **offensive tooling removed**. The
active-attack code paths — deauth, beacon/SSID spam, BLE spam, Evil Portal,
port/network recon, MAC spoofing, the serial CommandLine — are gone, and the
wardrive / EAPOL / probe scans have had their active-transmit calls stripped out.
The offensive modules and the menu / dispatch paths that launched them are
**removed** — the rig only collects.

What that leaves is all passive:

- Wardriving: logging the metadata WiFi access points broadcast anyway (BSSID,
  SSID, channel, RSSI) with a GPS position, for mapping, the wdgwars game, and
  feeding coverage back into warroom.
- Passive analysis views (signal/channel analyzer, packet monitor) and
  receive-only detectors are kept as harmless, flash-cheap extras.

Wardriving law varies — logging broadcast beacons is legal in most places;
connecting to, probing, or interfering with networks you do not own is not, and
is not what this rig does. Wardriving maps infrastructure, not individuals.

## The three modules

warroom-rig is built around three passive modules on top of the wardrive scanner:

| Module | What it does |
|---|---|
| **Rig Mode** (Wardrive Core) | Turns the handheld into an ESP-NOW aggregator hub for ESP32-C5 or NodeMCU-32 wardrive nodes. Enriches every node record with the hub's GPS fix and writes WiGLE CSV to SD. Wire-protocol byte-compatible with `ESP32DualBandWardriver`. |
| **Upload** | Uploads `wardrive_*.log` files from SD to the game API over HTTPS (`X-API-Key` read from SD, embedded CA bundle). On-device file picker, newest-first. |
| **File Server** | WPA2 SoftAP + AsyncWebServer over the SD card: list / download / delete files and set SSID / pass / API-key via a `/wdgcfg` form, in a browser at `http://192.168.4.1/`. |

The handheld boots to a bespoke rig home console (gorilla splash → Rig Mode /
Upload / File Server up front, the inherited Marauder scanners tucked behind one
"Tools" door) with an honest live GPS / SD / battery header.

## Nodes

The node firmware in `ESP32DualBandWardriver/` is **Koko's ESP32DualBandWardriver
v2.2.0** — his scanner, his GPS/SD/WiGLE logging, and his complete ESP-NOW
CORE / NODE / SOLO swarm. Our changes there are **board ports + fleet
adaptations**: the headless **C5-Zero** (ESP32-C5) and **NodeMCU-32** targets,
headless compile-time role selection, session gating, a broadcast-peer fix,
single-node BLE-host election, and 2.4-GHz-only-node channel handling. For the
stock node experience, use [Koko's upstream](https://github.com/justcallmekoko/ESP32DualBandWardriver)
directly.

## Hardware

| Board | Screen | Input | Status |
|---|---|---|---|
| **Marauder V7 / V7.1** | 240×320 portrait | 5 buttons | daily driver, tested |
| **Marauder V8** | 240×320 portrait | touch | builds, lightly used |
| **M5Stack Cardputer ADV** | 240×135 landscape | keyboard (TCA8418) | **builds, never run on hardware** |

That is the list. `configs.h` used to carry ESP32Marauder's whole 27-board
matrix; the boards this project does not build for have been removed from it,
so finding your board there now means something.

The Cardputer ADV is a port, not a tested target — nobody working on this repo
owns one. It compiles, its geometry is checked arithmetically, and every input
and layout path was written deliberately for it, but *no one has watched it
boot*. Treat the first run as debugging, not as using. In particular the ST7789
panel offsets in `tft_setup.h` are the generic TFT_eSPI ones for a 135×240
display; if the image is shifted by a few pixels, that is the knob.

Porting to a fourth board needs two things: an input backend in `RigInput`
(`up/down/left/right/select/back`, plus "is it still held", which is the gesture
that leaves a screen) and a panel entry in `tft_setup.h`. Layout follows from
`SCREEN_WIDTH` / `SCREEN_HEIGHT` via `RigTheme`, which has a compact profile for
short screens. Scanning, logging and upload are board-agnostic. Building for an
unlisted board stops with a compile error; define
`WARROOM_RIG_ALLOW_UNTESTED_BOARD` to proceed anyway — the gate is there to stop
accidents, not to stop you.

### One trap worth knowing about

TFT_eSPI's driver headers define `TFT_WIDTH` / `TFT_HEIGHT` **without an
`#ifndef` guard**, and arduino-cli compiles with `-w`, so the redefinition
warning never prints. Any translation unit that reaches the library before
`configs.h` therefore silently gets the driver's default panel size instead of
the board's — which on the Cardputer meant two `.cpp` files laid out a 240×135
screen as though it were 240×320, in a build that was green from end to end.

Panel selection now lives in `esp32_marauder/tft_setup.h`, which TFT_eSPI reads
before its own setup (`libs/CustomTFT_eSPI/User_Setup.h` is consequently dead —
editing it does nothing). `RigTheme.h` carries a `static_assert` on the expected
geometry so the failure can never be silent again.

## Build

Prerequisites:

- arduino-cli 1.4.1
- ESP32 core **3.3.4** (3.3.8 has multi-definition errors; local `platform.txt`
  carries the `-Wl,-zmuldefs` patch)

Hub one-liner from this directory:

```bash
ARDUINO_CLI=/path/to/arduino-cli
LIBS=$(pwd)/libs
SRC=$(pwd)/ESP32Marauder/esp32_marauder/esp32_marauder.ino

LIB_ARGS=""
for d in $LIBS/Custom*/; do LIB_ARGS="$LIB_ARGS --library $d"; done

MODULES="-DMARAUDER_CORE_MODE -DMARAUDER_WDGWARS_UPLOAD -DMARAUDER_FILE_SERVER_AP"

# Marauder V7 (LOLIN D32). huge_app is passed as a build property because the
# d32 board does not offer it in its partition menu.
$ARDUINO_CLI compile \
    --fqbn "esp32:esp32:d32" \
    $LIB_ARGS \
    --build-property "build.partitions=huge_app" \
    --build-property "upload.maximum_size=3145728" \
    --build-property "compiler.cpp.extra_flags=-DMARAUDER_V7 $MODULES" \
    "$SRC"
```

The other two targets differ only in FQBN and board define:

| Target | FQBN | Define |
|---|---|---|
| Marauder V8 | `esp32:esp32:esp32c5:CDCOnBoot=cdc,PartitionScheme=huge_app,FlashSize=4M` | `-DMARAUDER_V8` |
| Cardputer ADV | `esp32:esp32:m5stack_cardputer:PartitionScheme=huge_app,PSRAM=enabled` | `-DMARAUDER_CARDPUTER_ADV` |

The ESP32 core has no separate Cardputer ADV entry; the plain Cardputer board is
the same ESP32-S3 family and the differences (flash, PSRAM) are menu options.

Append `--upload -p COM<N>` to flash. The build flags select the three modules;
they are inherited from the Marauder-fork layout. The node build (C5-Zero /
NodeMCU-32) is documented in `RELEASING.md`.

## Releases

Prebuilt, checksummed firmware ships on two independent tag tracks — see
[Releases](../../releases) and [RELEASING.md](RELEASING.md):

- **`core-vX.Y`** — the handheld hub firmware (this repo's work).
- **`node-v<koko>-warroom.N`** — the node firmware; Koko's v2.2.0 base is carried
  in the version, and the notes credit him as the author.

## Repo layout

```
ESP32Marauder/           # Hub source (MIT, upstream by justcallmekoko) + our edits
ESP32DualBandWardriver/  # Node firmware (MIT, upstream by justcallmekoko), our
                         # C5-Zero / NodeMCU-32 headless targets + fleet fixes
libs/                    # Vendored dependencies (see CREDITS.md)
docs/                    # Wire spec etc.
```

## License

warroom-rig's own code is **MIT** — the same license as upstream Marauder (see
[LICENSE](LICENSE)). Bundled upstream projects and vendored libraries keep their
own licenses; full map in [CREDITS.md](CREDITS.md). No GPL or AGPL code in this
tree. (The warroom **PWA** is a separate, AGPL-3.0 project — different repo,
different license; the firmware does not include it.)

## Thanks

To [Just Call Me Koko](https://github.com/justcallmekoko) for ESP32Marauder and
ESP32DualBandWardriver — none of this exists without that groundwork.
