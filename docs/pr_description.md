# Add Wardrive Core Mode for v7

## Summary

Adds a "Wardrive Core" mode to the Marauder v7 firmware that turns the device into
an ESP-NOW data-aggregator hub for one or more ESP32-C5-DevKitC-1 boards running
[`ESP32DualBandWardriver`](https://github.com/justcallmekoko/ESP32DualBandWardriver)
in Node Mode. The Marauder receives Wardrive records (WiFi + BLE) from up to 4
nodes simultaneously, enriches them with its own GPS fix, and writes standard
Wigle 1.4 CSV to its SD card.

This is a feature-flagged addition. Without the build flag, the firmware is
byte-identical to upstream.

## Why

The ESP32-C5 DevKitC-1 can scan 2.4 GHz and 5 GHz WiFi simultaneously, but the
board itself lacks a display, GPS, SD slot, and battery. The standard
`ESP32DualBandWardriver` workflow already supports a "Core" role to which one or
more "Node" boards forward their records — but the existing Core implementation
also runs on a bare C5 with the same hardware limitations.

The Marauder v7 has exactly the peripherals a wardriving aggregator needs:
2.4'' TFT for live status, GPS (UART2 with auto-baud), SD card on the shared
SPI bus, MAX17048 fuel-gauge battery, and a 5-way tactile switch for control.
By acting as Core, the Marauder turns into a turnkey aggregator that any
unmodified C5-Node firmware can talk to. Solo wardriving on the Marauder
(`WIFI_SCAN_WAR_DRIVE`) remains untouched.

## What's new

- New menu entry: **WiFi → Sniffers → Wardrive Core** (cyan, parallel to the existing green "Wardrive" entry).
- New scan mode constant: `WIFI_SCAN_WAR_DRIVE_CORE = 84` in `WiFiScan.h` (free slot between `WIFI_SCAN_DISPLAY_AP_INFO=83` and `WIFI_ATTACK_FUNNY_BEACON=99`).
- Output format: standard Wigle CSV under `wardrive_core_<n>.log` on SD, identical 11-field schema to the existing native Wardrive.
- Compatible with **unmodified** ESP32DualBandWardriver Node firmware (HEAD `b674bd8` or newer). Wire-protocol byte-exactly replicated from upstream.
- Transport: ESP-NOW on Channel 6, optional AES-128-CCMP encryption (PMK/LMK derived via SHA-256 from a user-supplied key shared with the nodes).
- Up to 4 simultaneous nodes (configurable via `WARDRIVE_CORE_MAX_NODES`).
- Display: live counters for nodes, total Wigle lines, WiFi/BLE split, malformed packets, last-RX RSSI/MAC, GPS state, encryption state, file name, free SD, session uptime. Refresh 500 ms.
- Exit via Center-button long-press (>2 s) — Left/Right reserved for menu navigation.

## Build flag

Disabled by default. Enable by uncommenting:

```c
//#define MARAUDER_CORE_MODE
```

in `esp32_marauder/configs.h` (orthogonal to `MARAUDER_V7` / `MARAUDER_V7_1`
hardware targets — Core Mode is a feature flag, not a board target).

Without the flag set, no Core-Mode code is compiled in: all new code is wrapped
in `#ifdef MARAUDER_CORE_MODE` guards, no globals are allocated, and the
default build is byte-identical to upstream.

## Files changed

### New files (3, ~960 LOC total)

| File | LOC | Purpose |
|---|---|---|
| `esp32_marauder/WardriveCoreProtocol.h` | ~99 | Wire protocol: `ENOW_MAGIC`, `WardriveCoreMsgType` enum (5 types), `enow_text_msg_t` (212 B), `enow_admin_msg_t` (10 B), `scan_channels[40]` table, `NodeRecord` flags. Header-only. `static_assert` on struct sizes. |
| `esp32_marauder/WardriveCore.h` | ~197 | `WardriveCore` class API: `init()`/`runTick()`/`deinit()`, state container (`NodeRecord node_table[]`, FreeRTOS queue handle, counters), default constants. |
| `esp32_marauder/WardriveCore.cpp` | ~963 | Full implementation: ESP-NOW init+recv-callback (queue-enqueue only, no SD I/O in callback), `touchNode`/`removeStaleNodes`/`handleNodeTopologyChange`, `sendCoreReply`/`sendAdminToNodeSlot`, SHA-256 key derivation, Channel-6 fixed-channel workaround, Wigle-line composer, display render, exit cleanup. |

### Modified existing files (5, ~58 LOC delta)

| File | Delta | Change |
|---|---|---|
| `esp32_marauder/configs.h` | +12 | `//#define MARAUDER_CORE_MODE` toggle (commented out by default) and 7 tunable constants (`WARDRIVE_CORE_CHANNEL=6`, `_MAX_NODES=4`, `_QUEUE_LEN=12`, `_DISPLAY_REFRESH_MS=500`, `_NODE_TIMEOUT_MS=60000`, `_HEAP_MIN_INIT=30000`, `_HEAP_MIN_RUNTIME=15000`). |
| `esp32_marauder/WiFiScan.h` | +5 | `WIFI_SCAN_WAR_DRIVE_CORE = 84` constant + `RunWardriveCore`/`shutdownWardriveCore` method declarations under `MARAUDER_CORE_MODE`. |
| `esp32_marauder/WiFiScan.cpp` | ~+50 | Top-of-`main()` dispatch branch, `StartScan` branch, `RunWardriveCore`/`shutdownWardriveCore` bodies, cleanup hook in `shutdownWiFi()` for OTA-safety. |
| `esp32_marauder/MenuFunctions.cpp` | +12 | Menu entry "Wardrive Core" (line 1837) + stop-scan handler updates (line 286 + line 387) so touch/button-tap during Core Mode exits cleanly. |
| `esp32_marauder/esp32_marauder.ino` | +5 | `#include "WardriveCore.h"` + global `WardriveCore wardrive_core_obj;` instance under `MARAUDER_CORE_MODE`. |

Full LOC count: ~1018 lines added/modified.

## Compatibility

- **ESP-NOW protocol byte-exact replicated** from upstream `ESP32DualBandWardriver` (MIT License, same author). Magic `"ENOW"`, 5 message types (CORE_REQUEST/REPLY/HEARTBEAT/TEXT/ADMIN), 212-byte and 10-byte packed structs, 40-entry `scan_channels[]` table identical to `W:WiFiOps.cpp:44-53`. SHA-256 key-derivation identical (`<userKey>_pmk` → 16 B, `<userKey>_lmk` → 16 B).
- **Wigle 1.4 CSV format matches** existing Marauder Wardrive output (same 11-field schema, same header line). A single user can post-process `wardrive_<n>.log` and `wardrive_core_<n>.log` files together.
- **No conflicts with existing Marauder modes**: Mode-exclusivity is enforced by `currentScanMode` (only one mode active at a time). All Core-Mode init/cleanup is gated behind `WIFI_SCAN_WAR_DRIVE_CORE`.
- **No conflicts with existing ESP-NOW usage**: Marauder does not use ESP-NOW today (verified via repo-wide grep — 0 `esp_now_*` calls, 0 `<esp_now.h>` includes). No PMK collision risk.
- **Cleanup-on-mode-switch is enforced**: `shutdownWiFi()` calls `wardrive_core_obj.deinit()` if Core Mode is active, ensuring `esp_now_deinit()` runs before any subsequent mode (Beacon Spam, EvilPortal, OTA) re-touches the WiFi stack.

## Limitations

- **Max 4 simultaneous nodes** by default. Configurable up to 6 (ESP-NOW IDF encrypted-peer hard limit). Hard-rejects pairing attempt #5 with a counter-tracked log line, no crash.
- **Plain mode is unauthenticated**: Same behavior as upstream Wardriver — any ESP32 in range with the `"ENOW"` magic on Channel 6 is accepted. Use encrypted mode (shared key) for any deployment that cares about spoofing.
- **No retry / no buffering**: Wardrive records dropped at the ESP-NOW layer (e.g. queue full at 50+ records/s aggregate burst) are counted via `total_rx_drops` but not retransmitted. Matches upstream behavior.
- **Plain-mode without nodes nearby cannot self-distinguish from "wrong key" failure**: A 60-second silence detection prints a hint, but the user must manually verify key vs range vs nodes-off.
- **No multi-Marauder cluster support**: Two Cores in the same RF range will race on `MSG_CORE_REPLY`. Out-of-scope v1.
- **Display strings are English-only**, matching the rest of Marauder.

## Testing

See [`docs/test_plan.md`](docs/test_plan.md) for the full plan (4 levels:
Unit / Integration / Stress / Regression, 26+ test cases, hardware
inventory and reproduction steps).

The most important smoke tests:

- **Compile clean**: Toggle ON and OFF both build without warnings on
  Arduino-IDE 2.x with ESP32-Core 2.x and all 8 submodules.
- **R1 (Compile-Diff)**: Toggle-OFF binary is byte-identical to pre-edit
  upstream. (Recommended as CI gate.)
- **R2 (Existing Wardrive)**: With Toggle ON, the regular Wardrive entry
  still scans + writes `wardrive_<n>.log` exactly as before.
- **I1 (Plain-Mode Pairing)**: 1 Marauder + 1 C5 Node, both with empty
  encryption key. Within 30 s, `Nodes: 1 / 4` shown, lines flow to SD.
- **I2 (Encrypted-Mode Pairing)**: Same with matching SHA-256-derived key,
  display shows `[ENC: ON]`.
- **R7 (Mode-Switch)**: Core Mode → Center-Long-Press exit → Beacon Spam
  works cleanly. No PMK leak.

**Disclosure**: All hardware tests in the test plan are documented as
reproduction steps. The author has no full lab setup; the test plan
specifies exact hardware, expected outputs, and pass/fail criteria so a
reviewer with access can validate.

## Credits / License

Code adapted from `ESP32DualBandWardriver/src/WiFiOps.cpp` (MIT License,
Just Call Me Koko, 2025). All ported blocks marked with inline comments
referencing the upstream file:line. New code under same MIT License as
upstream Marauder. No external dependencies added — `mbedtls/sha256.h`
and `esp_now.h` are part of the standard ESP-IDF / Arduino-ESP32 stack
already included in Marauder builds.
