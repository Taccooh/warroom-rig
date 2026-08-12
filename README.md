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
| **Marauder V7** | 240×320 portrait | 5 buttons | daily driver, tested |
| **Marauder V7.1** | 240×320 portrait | 5 buttons | same board family as V7; builds green, never run on hardware |
| **Marauder V8** (ESP32-C5) | 240×320 portrait | touch | **unfinished port — does not work, gated off in `configs.h`** |
| **M5Stack Cardputer ADV** | 240×135 landscape | keyboard (TCA8418) | boots and runs; display, keyboard, SD and Rig Mode exercised on hardware |

That is the list. `configs.h` used to carry ESP32Marauder's whole 27-board
matrix; the boards this project does not build for have been removed from it,
so finding your board there now means something.

Marauder V7.1 shares the V7 pin map and is meant to be a first-class target, but
it was not actually building. Its config was missing the two toolchain-selector
macros (`HAS_NIMBLE_2`, `HAS_IDF_3`) that point the compile at the modern IDF and
NimBLE paths this tree pins, so it dropped into legacy branches that do not exist
here and the build died. Those macros are now set to match V7 and the target
compiles. Nobody has run the result on a V7.1 board, so the pin map is still only
inherited, not confirmed — but "does not build" is no longer true of it.

Marauder V8 (ESP32-C5) is in the table because its name is wired through the
whole config, but it is **not a working target**, and the build refuses it unless
you define `WARROOM_RIG_ALLOW_BROKEN_V8`. The port was never finished, and the
gaps were confirmed against the shipped binary rather than merely suspected: the
config still describes a classic-ESP32 Marauder, so the default panel pins land
on the C5's flash bus, the GPS UART sits on top of the USB-CDC console pins, and
touch is declared as the only input while `TOUCH_CS` is `-1`, so nothing on
screen responds. `configs.h` spells out all four problems at the V8 gate. Nobody
here has the V8 schematic, so the pin map cannot be guessed from this end —
finishing it needs the board in hand and the two things any port needs (below).

The Cardputer ADV is a port, not a tested target — nobody working on this repo
owns one. It compiles, its geometry is checked arithmetically, and every input
and layout path was written deliberately for it, but *no one has watched it
boot*. Treat the first run as debugging, not as using. In particular the ST7789
panel offsets are TFT_eSPI's generic ones for a 135×240 display; if the image is
shifted by a few pixels, that is the knob.

Porting to a fourth board needs two things: an input backend in `RigInput`
(`up/down/left/right/select/back`, plus "is it still held", which is the gesture
that leaves a screen) and a set of panel macros in that target's build flags.
Layout follows from `SCREEN_WIDTH` / `SCREEN_HEIGHT` via `RigTheme`, which has a
compact profile for short screens. Scanning, logging and upload are
board-agnostic. Building for an unlisted board stops with a compile error;
define `WARROOM_RIG_ALLOW_UNTESTED_BOARD` to proceed anyway — the gate is there
to stop accidents, not to stop you.

### One trap worth knowing about

TFT_eSPI's driver headers define `TFT_WIDTH` / `TFT_HEIGHT` **without an
`#ifndef` guard**, and arduino-cli compiles with `-w`, so the redefinition
warning never prints. Any translation unit that reaches the library before
`configs.h` therefore silently gets the driver's default panel size instead of
the board's — which on the Cardputer meant two `.cpp` files laid out a 240×135
screen as though it were 240×320, in a build that was green from end to end.

The Marauder targets get their panel from
`libs/CustomTFT_eSPI/User_Setup_Select.h`, whose single uncommented include is
`User_Setup_dual_nrf24.h` — the OG Marauder ILI9341 config (CS 17, DC 26, MOSI 23,
SCLK 18, BL 32). That is the file to open when you are auditing a Marauder
target's pins. `User_Setup.h` is **not** included by anything: it sits in the
library looking like the answer, and reading it tells you nothing about what the
build actually uses. That gap is exactly how the V8's pin problem stayed
invisible — the real config was one `#include` away from where anyone would look.
The classic-ESP32 boards (V7/V7.1) want that ILI9341 config and it is left alone.
The Cardputer needs a different panel, and it takes it from **build flags** (see
RELEASING.md) rather
than from a `tft_setup.h` in the sketch: TFT_eSPI reads such a file before its
own setup and then considers the configuration finished *for every target*, so a
file added for one board silently reconfigures the others. That is not
hypothetical — it was tried here and it blanked the V7's display while changing
nothing about its values. Build flags are per-target, land before any header is
parsed, and reach the library's own translation unit (`platform.txt` has a
single `recipe.cpp.o.pattern`, and it carries `compiler.cpp.extra_flags`).

`RigTheme.h` carries a `static_assert` on the expected geometry, so a panel size
that gets overridden fails the build instead of shipping a wrong-looking screen.

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

There is also a V8 (ESP32-C5) target, but it does **not** currently produce a
working device, and the build refuses it unless you also pass
`-DWARROOM_RIG_ALLOW_BROKEN_V8`. See the Hardware section above and the V8 gate
in `configs.h` for exactly what is wrong. The recipe below differs from V7 only
in FQBN and board define, and is here for whoever picks the port up — not to ship:

```
--fqbn "esp32:esp32:esp32c5:CDCOnBoot=cdc,PartitionScheme=huge_app,FlashSize=4M"
--build-property "compiler.cpp.extra_flags=-DMARAUDER_V8 -DWARROOM_RIG_ALLOW_BROKEN_V8 $MODULES"
```

The Cardputer ADV additionally carries its panel configuration in the flags,
because its screen is not the one `User_Setup_dual_nrf24.h` describes (the ILI9341
panel the classic-ESP32 Marauder targets use) — see
[RELEASING.md](RELEASING.md) for the full command. The ESP32 core has no
separate ADV entry; the plain Cardputer board is the same ESP32-S3 family and
the differences are menu options.

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
