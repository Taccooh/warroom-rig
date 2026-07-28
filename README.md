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

## Build

Prerequisites:

- arduino-cli 1.4.1
- ESP32 core **3.3.4** (3.3.8 has multi-definition errors; local `platform.txt`
  carries the `-Wl,-zmuldefs` patch)
- Handheld hub (LOLIN D32 / Marauder v7): FQBN
  `esp32:esp32:d32:PartitionScheme=min_spiffs`

Hub one-liner from this directory:

```bash
ARDUINO_CLI=/path/to/arduino-cli
LIBS=$(pwd)/libs
SRC=$(pwd)/ESP32Marauder/esp32_marauder/esp32_marauder.ino

LIB_ARGS=""
for d in $LIBS/Custom*/; do LIB_ARGS="$LIB_ARGS --library $d"; done

$ARDUINO_CLI compile \
    --fqbn "esp32:esp32:d32:PartitionScheme=min_spiffs" \
    $LIB_ARGS \
    --build-property "compiler.cpp.extra_flags=-DMARAUDER_V7 -DMARAUDER_CORE_MODE -DMARAUDER_WDGWARS_UPLOAD -DMARAUDER_FILE_SERVER_AP" \
    "$SRC"
```

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
