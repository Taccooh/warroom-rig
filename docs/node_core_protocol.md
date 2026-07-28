# Node ↔ Core wire protocol (reverse-engineered)

Date: 2026-05-06
Source: reverse-engineered from `ESP32DualBandWardriver` (Just Call Me Koko, MIT), as of v2.2.0.
Goal: Complete specification of the wire protocol so that Marauder v7 as CORE can serve the unmodified C5 wardriver nodes.

All file:line references refer to the wardriver repo unless otherwise noted.

---

## 1. Transport Layer

**Transport: ESP-NOW.** Definitively and exclusively. No UDP sockets, no BLE-GATT, no TCP, no SoftAP pairing between node and core. The SoftAP `c5wardriver` (`WiFiOps.h:104-105`) exists exclusively for the web admin UI in the initial setup window and is switched off during wardriving (`begin()` calls `deinitWiFi()` and then `initWiFi()` again, `WiFiOps.cpp:2287/2315`).

Evidence:

- `WiFiOps.h:13` `#include <esp_now.h>`
- `WiFiOps.cpp:1082` `if (esp_now_init() != ESP_OK)`
- `WiFiOps.cpp:1091` `esp_now_register_recv_cb(OnDataRecv);`
- No RX send callback registered (no `esp_now_register_send_cb`) → send status is evaluated exclusively from the `esp_err_t` return of `esp_now_send`, not from the ACK callback.

**WiFi channel: Fixed Channel 6.** Hardcoded.

- `WiFiOps.cpp:6` `static constexpr uint8_t ESPNOW_CHANNEL = 6;`
- `WiFiOps.cpp:1079` `this->setFixedChannel(ESPNOW_CHANNEL);` right at the start of `startESPNow()`.
- `setFixedChannel` (`WiFiOps.cpp:573-593`) calls `esp_wifi_set_ps(WIFI_PS_NONE)`, briefly `esp_wifi_set_promiscuous(true)`, then `esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE)`, then promiscuous off again. PowerSave off, otherwise channel drift.
- **Important**: Nodes actively scan through other channels (`scan_channels` with 2.4 + 5 GHz, `WiFiOps.cpp:44-53`, 40 channels total — 14×2.4 + 26×5 GHz). This means the node constantly leaves Channel 6 for the scan and only returns for the ESP-NOW send window. `sendBroadcastStringPlain` and `sendEncryptedStringToCore` each first call `setFixedChannel(ESPNOW_CHANNEL)` (`WiFiOps.cpp:752, 783`) — i.e. EVERY single wardrive data transmission switches back to Channel 6 beforehand. This is expensive but works.

**MAC addresses:**

- Broadcast: `BROADCAST_MAC = FF:FF:FF:FF:FF:FF` (`WiFiOps.cpp:3`).
- Unicast Node→Core: `g_core_mac` (learned from the `MSG_CORE_REPLY`, `WiFiOps.cpp:1026`).
- Unicast Core→Node: from `info->src_addr` of the incoming packets, or from `node_table[].mac_suffix` plus actual MAC on admin send (`WiFiOps.cpp:614-654`).
- Identification in the node table happens only via the last 2 bytes of the MAC: `mac_suffix = (mac[4]<<8)|mac[5]` (`WiFiOps.cpp:397-398`). If no match is found, a new slot is allocated (`allocateNodeSlot`, `WiFiOps.cpp:419-436`). **Collision warning**: two nodes with identical last 2 bytes would merge in the suffix lookup. The dest MAC for the reply, however, uses the full MAC from `info->src_addr`, but the slot lookup only the suffix.

**ESP-specific API calls the Marauder CORE needs:**

```
esp_now_init()                                 // once after WiFi init
esp_now_set_pmk(pmk)                           // set 16-byte PMK
esp_now_register_recv_cb(OnDataRecv)
esp_now_add_peer(&peerInfo)                    // per node MAC (or broadcast)
esp_now_del_peer(mac)                          // on mode switch (encrypt vs plain)
esp_now_is_peer_exist(mac)
esp_now_send(dest_mac, buf, len)
esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE)
esp_wifi_set_ps(WIFI_PS_NONE)
esp_wifi_set_promiscuous(true/false)           // only as a workaround to set the channel
```

Library: `esp_now.h`, `esp_wifi.h`, `WiFi.h` (Arduino), `mbedtls/sha256.h`. All available on classic ESP32 — this is the standard IDF stack.

---

## 2. Discovery & Pairing

### Discovery strategy

There is **no** beacon/SSID discovery. The node sends a `MSG_CORE_REQUEST` as ESP-NOW broadcast, the core replies with `MSG_CORE_REPLY` as unicast — and thereby the node learns the core MAC. Pre-shared MAC NO, fixed SSID NO, BLE beacon NO. Pre-shared is only the shared **`esp_now_key` string** (set per device in the web admin, `settings.cpp` key `"ek"`, loaded `WiFiOps.cpp:2296`), from which PMK and LMK are derived via SHA-256 — see Section 4.

Note: `sendCoreRequest()` is only called periodically when `use_encryption == true` AND `!g_have_core` (`WiFiOps.cpp:2342-2352`). In plain mode the active CORE_REQUEST loop is omitted entirely; the node simply sends data and heartbeats via broadcast. Consequence: in plain mode a node never has a concrete core MAC and sends **all** TEXT/HEARTBEAT packets as broadcast (`WiFiOps.cpp:741, 811`).

There is a one-time `runAdminWindowAfterScanCycle()` right in the node's `begin()` (`WiFiOps.cpp:2329`) that sends a heartbeat and waits 300ms (`ADMIN_WAIT_MS`) for an admin reply — even without CORE_REQUEST. This works in plain mode as an implicit discovery strategy: the heartbeat broadcast reaches the core, the core learns the node MAC from `info->src_addr` and sends an admin packet back.

### Pairing sequence

**Variant A: Encryption ENABLED (Node-driven)**

```
NODE                                CORE
  | startESPNow()                    | startESPNow()
  | g_last_req_ms = millis()         | waits for RX
  |
  | --- main() loop tick: ----------|
  | sendCoreRequest()                |
  |   - esp_now_add_peer(BCAST,plain)|
  |   - esp_now_send(BCAST,enow_text)|
  |       magic="ENOW",type=1,counter=0
  | =========BROADCAST===========>   |
  |                                  | OnDataRecv: MSG_CORE_REQUEST
  |                                  |   touchNode(src) -> slot, isNewNode
  |                                  |   sendCoreReply(src):
  |                                  |     addPeerWithMode(src,plain)
  |                                  |     esp_now_send(src, type=2)
  |   <======UNICAST plain===========|
  | OnDataRecv: MSG_CORE_REPLY       |
  |   memcpy(g_core_mac,src,6)       |
  |   g_have_core=true               |
  |   addPeerWithMode(g_core_mac,    |
  |       encrypt=true, lmk16)       |
  |   g_secure_ready=true            |
  |                                  | parallel: addPeerWithMode(src,
  |                                  |   encrypt=true, lmk16)
  |                                  | set flag NODE_FLAG_ENCRYPTED
  |                                  |
  |                                  | if isNewNode:
  |                                  |   handleNodeTopologyChange()
  |                                  |   -> assignment_version++
  |                                  |      recalculateChannelAssignments()
  |                                  |      markAllActiveNodesAdminDirty()
  |                                  |
  |                                  | if ADMIN_DIRTY:
  |                                  |   sendAdminToNodeSlot(slot,src)
  |                                  |   (always as plaintext peer!)
  |   <======UNICAST plain===========|
  | OnDataRecv: MSG_ADMIN            |
  |   if version != local:           |
  |     adopt                        |
  |     assigned_start_idx,          |
  |     assigned_end_idx,            |
  |     assigned_node_index,         |
  |     assigned_node_count          |
  |
  | now: getNodeReady()=true         |
  | scan loop sends each wardrive    |
  | record as MSG_TEXT encrypted     |
  | unicast to g_core_mac.           |
  |                                  |
  | heartbeat at the end of every    |
  | scan cycle, Channel 6:           |
  | sendHeartbeat() encrypted        |
  | =========UNICAST enc==========>  |
  |                                  | OnDataRecv: MSG_HEARTBEAT
  |                                  |   touchNode(src)
  |                                  |   if ADMIN_DIRTY:
  |                                  |     sendAdminToNodeSlot()
```

**Variant B: Encryption DISABLED**

Substantially simpler. Node never sends `MSG_CORE_REQUEST` (see `WiFiOps.cpp:1103-1104`, the call is under `if (this->use_encryption)`). Instead:

```
NODE                                CORE
  | begin() -> runAdminWindowAfter   |
  |   ScanCycle() -> sendHeartbeat() |
  | type=3 as BROADCAST plain        |
  | =========BROADCAST===========>   |
  |                                  | OnDataRecv: MSG_HEARTBEAT
  |                                  |   touchNode(src), isNewNode=true
  |                                  |   handleNodeTopologyChange()
  |                                  |   sendAdminToNodeSlot(slot,src)
  |   <======UNICAST plain===========|
  | OnDataRecv: MSG_ADMIN -> adopt
  |
  | wardriving data:                 |
  | sendBroadcastStringPlain(line)   |
  | type=4 as BROADCAST plain        |
  | =========BROADCAST===========>   |
```

In plain mode `g_have_core=false` stays (there is no MSG_CORE_REPLY trigger), `g_secure_ready=true` (forced in `getNodeReady()` via `if (!use_encryption) return true`, `WiFiOps.cpp:1140-1141`). So the node never has a concrete CORE MAC in plain mode — everything goes broadcast.

### Number of nodes

- Hard limit in code: **`MAX_NODES = 24`** (`WiFiOps.h:44`). This is the size of `node_table[MAX_NODES]` (`WiFiOps.cpp:66`). Slots are managed as occupied/free via `flags & NODE_FLAG_ACTIVE`.
- Encrypt limit "max 6": **NOT explicit in code as a constant**. The only hint is the restriction of ESP-NOW itself: Espressif limits encrypted ESP-NOW peers in the IDF to 6, plaintext to 20 (doc claim, not reflected in the wardriver code). The wardriver allocates up to `MAX_NODES = 24` slots in its own table, but `addPeerWithMode(..., encrypt=true)` is rejected by ESP-NOW itself at the 7th encrypted peer with `ESP_ERR_ESPNOW_FULL` or similar → the README statement is indirect here, not coded. **Status: README claim, in code an ESP-NOW library limit, not a dedicated `#define` constant.**

### Role of the state variables

| Variable | Where | Meaning |
|---|---|---|
| `g_have_core` | static, `WiFiOps.cpp:13` | Node-only. True after receiving a `MSG_CORE_REPLY`. Only set in the encrypted path. Controls the `sendCoreRequest` backoff loop in `main()` (`WiFiOps.cpp:2342`). |
| `g_secure_ready` | static, `WiFiOps.cpp:14` | Node-only. True when the encrypted peer for the core is successfully created (or immediately true in plain mode). Gating for `sendEncryptedStringToCore` (`WiFiOps.cpp:754`) and `sendHeartbeat` (`WiFiOps.cpp:729`). |
| `g_core_mac[6]` | static | Node-only. Copied from the first `MSG_CORE_REPLY` `info->src_addr` (`WiFiOps.cpp:1026`). |
| `assigned_start_idx`, `assigned_end_idx` | global, file-scope `WiFiOps.cpp:57-58` | Node-only. Index in `scan_channels[]`. Determines which channels the node should scan. Default before the first admin: `0..NUM_SCAN_CHANNELS-1` (= all 40 channels). After admin: only a slice. |
| `assigned_node_index`, `assigned_node_count` | global, file-scope | Node-only. Which index the node has in the core cluster (0..node_count-1) and how many nodes the core knows. Pure info, not used for logic except for logging. |
| `assignment_version` | global | Node-only. Compared against `admin->assignment_version`, the mapping is adopted only on change (`WiFiOps.cpp:1053`). |
| `current_assigned_scan_idx` | public member of `WiFiOps`, `WiFiOps.h:167` | Node-only. Cursor in `scan_channels[]` during the active wardrive loop. Starts at `assigned_start_idx`, increments per scan, wraps at `assigned_end_idx`. |
| `current_assignment_version` | public member of `WiFiOps`, `WiFiOps.h:166` | Core-only. Incremented in `handleNodeTopologyChange` (`WiFiOps.cpp:533-534`), 0 is skipped. |
| `node_table[MAX_NODES]` | global, `WiFiOps.cpp:66` | Core-only. NodeRecord per active node slot. |
| `pmk[16]`, `lmk[16]` | global, `WiFiOps.cpp:63-64` | Both, derived from `esp_now_key` via SHA-256. |

---

## 3. Packet Format(s)

A total of **5 message types**, all sharing the same 5-byte header prefix (`magic[4] + type[1]`). Each packet is a `__attribute__((packed))` C struct, no CBOR/JSON, **raw little-endian struct bytes on the wire**. ESP32 is little-endian, all `uint16_t`/`uint32_t` are sent as-is.

### Magic + type enum

```
static const char MAGIC[4] = {'E','N','O','W'};   // WiFiOps.cpp:4

enum MsgType : uint8_t {                          // WiFiOps.cpp:68-74
  MSG_CORE_REQUEST   = 1,
  MSG_CORE_REPLY     = 2,
  MSG_HEARTBEAT      = 3,
  MSG_TEXT           = 4,
  MSG_ADMIN          = 5
};
```

### Struct A: `enow_text_msg_t` (used by types 1, 2, 3, 4)

`WiFiOps.h:53-59`:

```c
typedef struct __attribute__((packed)) {
  char     magic[4];                  // "ENOW"
  uint8_t  type;                      // MSG_CORE_REQUEST=1, _REPLY=2, _HEARTBEAT=3, _TEXT=4
  uint32_t counter;                   // little-endian; heartbeat counter only for MSG_HEARTBEAT
  uint16_t len;                       // little-endian; valid only for MSG_TEXT, else 0
  char     text[ENOW_TEXT_MAX + 1];   // ENOW_TEXT_MAX=200, +1 NUL = 201 bytes
} enow_text_msg_t;
```

- Total: 4 + 1 + 4 + 2 + 201 = **212 bytes**.
- ESP-NOW max payload is 250 bytes → fits.
- The full struct is ALWAYS sent (also for CORE_REQUEST/REPLY/HEARTBEAT, where `text` and `len` are unused), not just the first 11 bytes. Code: `esp_now_send(..., (uint8_t*)&msg, sizeof(msg))` — `sizeof(enow_text_msg_t)` = 212. RX checks the minimum with `if (len < (int)sizeof(enow_text_msg_t)) return;` (`WiFiOps.cpp:873, 914, 939, 1024`). **Marauder must mandatorily send 212 bytes, not just the header.**
- `text[len]` is NUL-terminated by the sender (`msg.text[n] = '\0'`, `WiFiOps.cpp:768, 808`).
- Maximum wardrive line: 200 characters. In code this is capped at `n > ENOW_TEXT_MAX` (`WiFiOps.cpp:765, 805`). Wardrive lines from `processWardrive` with long SSIDs can exceed this — they are truncated, not rejected.

### Struct B: `enow_admin_msg_t` (only type 5)

`WiFiOps.h:61-69`:

```c
typedef struct __attribute__((packed)) {
  char    magic[4];
  uint8_t type;                  // MSG_ADMIN = 5
  uint8_t assignment_version;
  uint8_t node_index;
  uint8_t node_count;
  uint8_t start_channel_idx;     // Index in scan_channels[]
  uint8_t end_channel_idx;
} enow_admin_msg_t;
```

- Total: 4 + 1 + 5 = **10 bytes**.
- `start_channel_idx` and `end_channel_idx` are **indices** into `scan_channels[]` (`WiFiOps.cpp:44-53`), NOT channel numbers directly. `scan_channels[0]=1, scan_channels[13]=14, scan_channels[14]=36, ...`. The array has **NUM_SCAN_CHANNELS = 40** entries (14 × 2.4 GHz + 26 × 5 GHz). The core must have the identical channel table, otherwise indices do not match.

**Byte-exact array** (from `WiFiOps.cpp:44-53`):

```c
static const uint8_t scan_channels[] = {
  // 2.4 GHz (14)
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
  // 5 GHz (26):
  36, 40, 44, 48,
  52, 56, 60, 64,
  100, 112, 116, 120, 124, 128, 132, 136, 140, 144,
  149, 153, 157, 161, 165, 169, 173, 177
};
#define NUM_SCAN_CHANNELS (sizeof(scan_channels) / sizeof(scan_channels[0]))  // = 40
```

Notable gaps in the 5GHz range: between 100 and 112, 104+108 are missing. Does not stop at 165 but goes up to 177.

### Encoding note

- Endianness: little-endian (ESP32 native).
- Padding: `__attribute__((packed))` → no padding.
- There is **no** sequence number scheme for TEXT packets, no CRC outside the ESP-NOW layer CRC, no per-message auth.
- The `counter` in `enow_text_msg_t` is used exclusively by heartbeats (monotonically increasing, `g_hb_counter++`), is only logged by the core, not checked against replay (`WiFiOps.cpp:919`).

### TEXT payload format (MSG_TEXT)

The wardrive line is a **comma-separated string**, 6 fields, mixed for WiFi and BLE. `parseWardriveLine` (`WiFiOps.cpp:821-850`) expects exactly 6 comma-separated fields:

```
BSSID,ESSID,SECURITY,CHANNEL,RSSI,TYPE
```

- `TYPE = "W"` for WiFi, `"B"` for BLE (`WiFiOps.cpp:131, 1340`).
- For BLE: `SECURITY = "[BLE]"`, `CHANNEL = "0"`.
- Example plaintext from `processWardrive`:
  - WiFi: `AA:BB:CC:DD:EE:FF,MyHomeAP,WPA2,11,-67,W`
  - BLE:  `12:34:56:78:9A:BC,,[BLE],0,-89,B`
- Comma in SSID is replaced with underscore by the sender beforehand (`ssid.replace(",", "_")`, `WiFiOps.cpp:1297, 1339`) — no CSV quoting.

The core enriches it with GPS and a timestamp and writes an **11-field** Wigle line with its own `Datetime/Lat/Lon/Alt/Accuracy/Type` (`WiFiOps.cpp:969-980`).

---

## 4. Encryption

### Mechanism: ESP-NOW built-in AES-128-CCMP encryption

ESP-NOW natively offers AES-128 between peers (CCMP-like, IDF internal). Wardriver uses this via:

- `esp_now_set_pmk(pmk)` globally once (`WiFiOps.cpp:1087`). PMK = Primary Master Key, 16 bytes.
- `peerInfo.encrypt = true; memcpy(peerInfo.lmk, lmk16, 16);` per peer (`WiFiOps.cpp:603-609`). LMK = Local Master Key, 16 bytes.

**No custom AES is set up via mbedtls.** mbedtls/sha256 is used exclusively for key derivation from the user string, not for data encryption.

### Key derivation

`derive_key_16` (`WiFiOps.cpp:1108-1118`):

```
SHA256(esp_now_key)[0..15]  -> not directly, but:
SHA256(esp_now_key + "_pmk")[0..15] -> pmk[16]
SHA256(esp_now_key + "_lmk")[0..15] -> lmk[16]
```

`computeKeysFromEnowKey` (`WiFiOps.cpp:1120-1125`):

```
derive_key_16(esp_now_key + "_pmk", pmk);
derive_key_16(esp_now_key + "_lmk", lmk);
```

Both devices (node and core) must have the exact same `esp_now_key` string — pre-shared via web admin, persisted in SPIFFS under key `"ek"` (`WiFiOps.cpp:2292, 2296`).

### Key storage

- Persisted in SPIFFS via `settings.cpp` (key `"ek"`).
- Not hardcoded.
- Not negotiated over the pairing flow — pre-shared.

### "max 6 nodes" rationale

Espressif docs limit encrypted ESP-NOW peers to 6 (default). Plaintext up to 20. In the wardriver code this is not limited itself; `MAX_NODES=24` is the slot table. **Claim in the README, in the wardriver code only enforced indirectly through the IDF layer.** → For a Marauder implementation on classic ESP32 one must observe or raise the ESP-NOW IDF limit (CONFIG_ESPNOW_MAX_ENCRYPT_PEER_NUM is build-time tweakable).

### Off-Mode (Encryption disabled)

Completely plain. There is **no** simple auth mechanism, no MAC filter, no shared token in the plaintext path. Any arbitrary ESP32 in range that knows the ENOW magic and sends on Channel 6 is accepted by the core and added to `node_table`. Spoofing a wardrive record is trivial. Sold in the README as "theoretically unlimited", but in fact also unprotected.

---

## 5. Heartbeat / Keepalive

### Heartbeat send

- Trigger: `runAdminWindowAfterScanCycle()` (`WiFiOps.cpp:691-701`), called by the node after every complete scan cycle through all assigned channels (`WiFiOps.cpp:1251-1252`).
- A node sends a heartbeat **not on a fixed time interval**, but depending on the scan cycle. With `CHANNEL_TIMER=80`ms (`configs.h:140`) per channel and e.g. 5 assigned channels: ~400ms between heartbeats. With 32 channels (no admin received): ~2.5s.
- Heartbeat contains monotonic `g_hb_counter`, plain in encrypted mode to `g_core_mac`, otherwise broadcast.

### Stale detection

- `NODE_TIMEOUT_MS = 60000` (`WiFiOps.h:45`). 60 seconds without heartbeat → node removed.
- `removeStaleNodes()` (`WiFiOps.cpp:461-475`) is called in `main()` every tick when `run_mode==CORE_MODE` (`WiFiOps.cpp:2360`). A deleted slot triggers `handleNodeTopologyChange()` → `assignment_version++`, channel reassignment for the remaining nodes, `markAllActiveNodesAdminDirty()`. The next heartbeat of a still-active node gets the new admin packet.
- The core prints the node table every `DEBUG_OUTPUT_DELAY = 30000`ms (30 seconds).

### Node-side Core-Loss Detection

**Almost entirely missing.** There is no logic in the node that resets `g_have_core` to `false` when the core goes silent. Consequence: a node that has encryption enabled and loses its core will send infinitely many encrypted TEXT packets to the old CORE MAC that go nowhere. It no longer sends new `MSG_CORE_REQUEST` (the backoff loop in `main()` is blocked by `g_have_core==true`).

This is a known weakness in the wardriver design. For a Marauder implementation as CORE it is not directly relevant — the core does not care about its own loss. But when a Marauder CORE is restarted, old C5 nodes can stay "broken" until they reboot themselves.

---

## 6. Error Handling & Retransmission

### ESP-NOW ACK evaluation

- **No** `esp_now_register_send_cb` registered. The asynchronous send-status callback is not used.
- Instead the **synchronous return** `esp_err_t` from `esp_now_send` is evaluated. This is only the "did ESP-NOW accept the packet" — not "receiver has ACKed". Evidence: `WiFiOps.cpp:640-647, 665-668, 743-748, 772-775, 811-815`.
- On failure: logging via `Logger::log(WARN_MSG, ...)`. **No retransmission**, no buffer save, no retry. The wardrive record is discarded.

### Buffer strategy on the node side

- There is no outbound buffer for wardrive records. `processWardrive` calls `sendEncryptedStringToCore` or `sendBroadcastStringPlain` directly and ignores failures.
- `Buffer::append(String log)` is a local PCAP/log buffer for SD writing (`Buffer.cpp:119-124`), not used for network traffic in node mode. In node mode the SD is typically not supported (`begin()` for SD happens anyway, but `sd_obj.supported` is false).
- Conclusion: out-of-range nodes lose data permanently.

### Retry logic (only for CORE_REQUEST)

`WiFiOps.cpp:9-22, 2342-2352`:

- Initial 300ms, exponential backoff (×2), capped at 5000ms.
- Reset on `g_have_core=true`.
- Applies **only** to the encrypted pairing flow.

### Sequence numbers / dedup

- No sequence numbers in the protocol header.
- Heartbeat counter is not used for replay detection.
- Wardrive records are deduplicated on the CORE side via `seen_mac()` (`WiFiOps.cpp:962`) — same BSSID only once per `mac_history_len=200` (`configs.h:139`) — this is a **content-based** dedup, not protocol-based.

### Behavior on malformed packets

- `OnDataRecv` checks minimum `len >= 5` (`WiFiOps.cpp:859`).
- Magic check: `if (memcmp(data, MAGIC, 4) != 0) return;` (`WiFiOps.cpp:862`).
- Per type an additional minimum length is checked (e.g. `len < sizeof(enow_text_msg_t)` → return, `WiFiOps.cpp:873`).
- Unknown type → log `RX unknown type N`, no crash (`WiFiOps.cpp:1016-1017, 1072-1073`).
- TEXT with `t->len > ENOW_TEXT_MAX` is discarded (log only, `WiFiOps.cpp:1003-1004`).

This is not robust — a malicious 4 bytes "ENOW" + wrong len field values could feed a `parseWardriveLine` with uncontrolled strings, but there is no out-of-bounds read because the entire struct is fixed-size.

---

## 7. Marauder implementation requirements

### What must a Marauder v7 CORE_MODE replicate?

1. **WiFi mode `WIFI_STA` without connect** — `esp_now_init` requires a WiFi init with mode STA (or AP, or STA_AP). Marauder usually calls `WiFi.mode(WIFI_AP_STA)` or similar — compatible.
2. **Channel fixed to 6** — COLLISION with Marauder's own wardrive/sniff/beacon spam that constantly switches channels. In CORE mode on Marauder the channel hop MUST be disabled. Solution: `setFixedChannel(6)` once at CORE mode enter, block all other Marauder WiFi functions.
3. **Set PMK** — `esp_now_set_pmk(pmk)` with the 16 bytes from SHA256(`<userKey>_pmk`).
4. **Register an empty recv callback** that implements the exact same `OnDataRecv` logic: magic check, type switch, `touchNode` with MAX_NODES slots and 60s timeout, channel-idx assignment, `sendCoreReply`/`sendAdminToNodeSlot`/`sendHeartbeat` RX trigger.
5. **`scan_channels[]` array exactly identical** (40 entries, 14×2.4 + 26×5 GHz, see Section 3 for the byte-exact array). Important: classic ESP32 (Marauder v7 without C5) has only 2.4 GHz, cannot itself scan on Channel 36+. But as CORE it only needs to understand the meaning of the indices, since it does not scan itself.
6. **No own wardrive activity** — the core does not scan. Only RX, GPS, SD, Wigle format compose.
7. **GPS + SD functionality** — Marauder already has both. GpsInterface of both repos is presumably similar (to be checked in 1.4).
8. **Wigle output format identical** — 11-field line as in `WiFiOps.cpp:969-980`.

### Critical API calls (includes)

```cpp
#include <esp_now.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include <WiFi.h>
#include "mbedtls/sha256.h"
```

All standard IDF — available on Arduino-ESP32 core 2.x and 3.x. Marauder uses Arduino core 2.x → no problem.

### Known incompatibilities

1. **Channel conflict** with other scan modes of the Marauder (e.g. probe scan, packet monitor). CORE mode must be exclusive-blocking.
2. **PMK is global** in ESP-NOW. If Marauder uses ESP-NOW for other features (not beacon spam, but possibly for other tools), the keys would collide. **To be checked in 1.5** whether Marauder currently uses ESP-NOW.
3. **Encrypted peer limit of 6** in the IDF — for Marauder as CORE compatible with the C5 wardriver docs, but a hard lower bound; build-time tweak possible.
4. **NimBLE coexistence** — Wardriver uses NimBLE in parallel with ESP-NOW on classic ESP32 (not active at the start in encrypted mode, but generally). Marauder has a NimBLE submodule, so no conflict expected.
5. **Promiscuous mode** is briefly activated only as a workaround for the `esp_wifi_set_channel` call. This does not disturb any Marauder state, but is timing-collision-sensitive if Marauder itself is doing promiscuous sniffing at that moment. In CORE mode this should not happen.
6. **`ieee80211_raw_frame_sanity_check`** override (`WiFiOps.cpp:79-84`) — this is a wardriver-specific hack that allows sending raw frames. Not needed for CORE, but important for NODE mode (Marauder may need this itself in wardrive mode — check in 1.4).

---

## Summary of security levels

| Finding | Status |
|---|---|
| Transport is ESP-NOW | **Confirmed by code (`#include`, `esp_now_send` etc.)** |
| Channel = 6 hardcoded | **Confirmed** (`WiFiOps.cpp:6`) |
| 5 message types: REQUEST/REPLY/HEARTBEAT/TEXT/ADMIN | **Confirmed** (`WiFiOps.cpp:68-74`) |
| Encryption via ESP-NOW built-in AES-128 with PMK/LMK | **Confirmed** (`esp_now_set_pmk`, `peerInfo.lmk`) |
| Key derivation via SHA-256 from the user string | **Confirmed** (`derive_key_16`) |
| MAX_NODES=24, NODE_TIMEOUT=60s | **Confirmed** (`WiFiOps.h:44-45`) |
| "max 6 nodes with encryption" | **Claimed in the README, in code an IDF library limit, not a dedicated constant** |
| Plain mode has no auth whatsoever | **Confirmed by absence** |
| No retransmission, no send callbacks | **Confirmed by absence of `esp_now_register_send_cb`** |
| Node-side core-loss detection missing | **Confirmed by absence of any `g_have_core=false` reset logic in the encrypted path** |
| Wardrive line is 6-field CSV "BSSID,ESSID,SEC,CH,RSSI,W/B" | **Confirmed** (`parseWardriveLine`) |
| Struct packets are sent full-size (212 bytes also for heartbeat) | **Confirmed** (`sizeof(enow_text_msg_t)` in `esp_now_send`) |

Hypotheses, NOT directly confirmed:
- The assumption that Marauder currently does not use ESP-NOW itself — **must be verified in 1.5**.
- The assumption that classic ESP32 can understand the meaning of the 5GHz indices without having to scan itself — I consider trivial because the CORE in the wardriver scans nothing anyway.
- "Encrypted peer limit 6 is tweakable via build flag" — comes from IDF docs, no tweak made in the wardriver code whatsoever.
