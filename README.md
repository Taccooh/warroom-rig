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
(the C5/NodeMCU nodes). This repository is **not** affiliated with or endorsed by
Koko. Full attribution and licenses: [CREDITS.md](CREDITS.md).

> [!WARNING]
> **Status: freshly forked, carve-down in progress.**
> This tree still contains the **complete upstream Marauder offensive tooling**
> (deauth, beacon/BLE spam, Evil Portal, EAPOL capture, port/net recon). The
> whole point of warroom-rig is to **remove** that and ship a rig that can *only*
> wardrive — passive collection, nothing offensive. Until the strip lands, treat
> this exactly like upstream Marauder and read the authorized-use notes below.
> See [the carve-down plan](#carve-down-plan).

## What it is for

Wardriving: passively logging the metadata WiFi access points broadcast anyway
(BSSID, SSID, channel, signal strength) together with a GPS position, for mapping,
the wdgwars game, and feeding coverage back into warroom. That is the entire
intended scope. Everything warroom-rig keeps is **passive data collection**.

Until the offensive upstream modules are removed, the usual rules apply to them:

- **Passive collection is what this is for.** Logging broadcast beacons is legal
  in most places; connecting to, probing, deauthenticating, or interfering with
  networks or devices you do not own is not, and is not what this rig is for.
- **The inherited offensive tools** (deauth, spam, portal, …) are for use **only
  on networks and devices you own or have explicit written permission to test**,
  and they are on their way out of this tree entirely.
- **Know your local law.** Radio, privacy, and wiretapping rules vary widely.
- **Respect people.** Wardriving maps infrastructure, not individuals.

## The three kept features

These are the modules warroom-rig is built around — all **passive**, all staying:

| Module | What it does |
|---|---|
| **Wardrive Core** | Turns the handheld into an ESP-NOW aggregator hub for ESP32-C5 or NodeMCU-32 wardrive nodes. Enriches every node record with the hub's GPS fix and writes WiGLE CSV to SD. Wire-protocol byte-compatible with `ESP32DualBandWardriver`. |
| **wdgwars / warroom upload** | Uploads `wardrive_*.log` files from SD to the game API over HTTPS (`X-API-Key`, embedded CA bundle). On-device file picker, newest-first. |
| **SD web config + browser** | WPA2 SoftAP + AsyncWebServer over the SD card: list/download/delete files and set SSID/pass/API-key via a `/wdgcfg` form, all in a browser at `http://192.168.4.1/`. |

Node side (`ESP32DualBandWardriver/`) is already a clean passive wardriver and
carries the C5-Zero / NodeMCU-32 headless-node build targets in its `configs.h`.
Nothing to strip there — it comes across as-is.

## Carve-down plan

warroom-rig starts as a full Marauder and gets reduced to a wardriving rig. The
target: remove all active-attack code paths, keep the passive scan + wardrive +
GPS + SD + the three modules above.

- **Out:** deauth, beacon/SSID spam, BLE spam, Evil Portal, EAPOL/PMKID capture,
  pwnagotchi, SAE/CSA attacks, port/net recon, MAC spoofing — and the files that
  exist only to serve them (`EvilPortal.*`, `Keyboard.*`/`TouchKeyboard.*`, most
  of `CommandLine.cpp`), plus their menu entries.
- **Stays:** wardrive AP/BLE scan, GPS, WiGLE logging, the ESP-NOW Core, the
  uploader, the SD web-config — and the passive analysis views (signal/channel
  analyzer, packet monitor) as harmless, flash-cheap keepers.
- **Frees the flash** the three modules need to all fit at once (today > 92 %).

The coupling is small: the three modules touch only `currentScanMode`,
`header_line`, and `startLog()` on the scan class — no attack symbol — so the cut
is scoped, not a rewrite.

## Build

Prerequisites:

- arduino-cli 1.4.1
- ESP32 core **3.3.4** (3.3.8 has multi-definition errors; local `platform.txt`
  carries the `-Wl,-zmuldefs` patch)
- Handheld hub (LOLIN D32 / Marauder v7): FQBN
  `esp32:esp32:d32:PartitionScheme=min_spiffs`

Hub one-liner from this directory (build flags select the modules):

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

Append `--upload -p COM<N>` to flash. The node build (C5-Zero / NodeMCU-32) is
documented in `ESP32DualBandWardriver/`.

> Build flags are how the modules are selected **today**, inherited from the
> Marauder-fork layout. Once the carve-down lands and the offensive code is gone,
> the passive rig becomes the default build and these become plain defaults.

## Repo layout

```
ESP32Marauder/           # Hub source (MIT, upstream by justcallmekoko) + our edits
ESP32DualBandWardriver/  # Node firmware (MIT, upstream by justcallmekoko), our
                         # C5-Zero / NodeMCU-32 headless targets in configs.h
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
