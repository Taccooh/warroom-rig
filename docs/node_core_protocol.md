# Node ↔ Core wire protocol (reverse-engineered)

Datum: 2026-05-06
Quelle: reverse-engineered aus `ESP32DualBandWardriver` (Just Call Me Koko, MIT), Stand v2.2.0.
Ziel: Vollstaendige Spezifikation des Wire-Protocols, damit Marauder v7 als CORE die unmodifizierten C5-Wardriver-Nodes bedienen kann.

Alle File:Line-Referenzen beziehen sich auf das Wardriver-Repo, sofern nicht anders genannt.

---

## 1. Transport Layer

**Transport: ESP-NOW.** Definitiv und ausschliesslich. Keine UDP-Sockets, kein BLE-GATT, kein TCP, kein SoftAP-Pairing zwischen Node und Core. Der SoftAP `c5wardriver` (`WiFiOps.h:104-105`) existiert ausschliesslich fuer das Web-Admin-UI im Initial-Setup-Window und ist beim Wardriving abgeschaltet (`begin()` ruft `deinitWiFi()` und danach `initWiFi()` neu, `WiFiOps.cpp:2287/2315`).

Beleg:

- `WiFiOps.h:13` `#include <esp_now.h>`
- `WiFiOps.cpp:1082` `if (esp_now_init() != ESP_OK)`
- `WiFiOps.cpp:1091` `esp_now_register_recv_cb(OnDataRecv);`
- Kein RX-Send-Callback registriert (kein `esp_now_register_send_cb`) → Send-Status wird ausschliesslich aus dem `esp_err_t` Return von `esp_now_send` bewertet, nicht aus dem ACK-Callback.

**WiFi-Channel: Fixed Channel 6.** Hardcoded.

- `WiFiOps.cpp:6` `static constexpr uint8_t ESPNOW_CHANNEL = 6;`
- `WiFiOps.cpp:1079` `this->setFixedChannel(ESPNOW_CHANNEL);` direkt am Anfang von `startESPNow()`.
- `setFixedChannel` (`WiFiOps.cpp:573-593`) ruft `esp_wifi_set_ps(WIFI_PS_NONE)`, kurz `esp_wifi_set_promiscuous(true)`, dann `esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE)`, dann promiscuous wieder aus. PowerSave aus, weil sonst Channel-Drift.
- **Wichtig**: Nodes scannen aktiv andere Channels durch (`scan_channels` mit 2.4 + 5 GHz, `WiFiOps.cpp:44-53`, 40 Channels total — 14×2.4 + 26×5 GHz). Das heisst die Node verlaesst Channel 6 staendig fuer den Scan und kommt nur fuer das ESP-NOW-Send-Window zurueck. `sendBroadcastStringPlain` und `sendEncryptedStringToCore` rufen jeweils zuerst `setFixedChannel(ESPNOW_CHANNEL)` (`WiFiOps.cpp:752, 783`) — d.h. JEDE einzelne Wardrive-Daten-Uebermittlung wechselt vorher zurueck auf Channel 6. Das ist teuer, funktioniert aber.

**MAC-Adressen:**

- Broadcast: `BROADCAST_MAC = FF:FF:FF:FF:FF:FF` (`WiFiOps.cpp:3`).
- Unicast Node→Core: `g_core_mac` (gelernt aus dem `MSG_CORE_REPLY`, `WiFiOps.cpp:1026`).
- Unicast Core→Node: aus `info->src_addr` der eingehenden Pakete bzw. aus `node_table[].mac_suffix` plus tatsaechlicher MAC bei Admin-Send (`WiFiOps.cpp:614-654`).
- Identifikation in der Node-Tabelle erfolgt nur ueber die letzten 2 Bytes der MAC: `mac_suffix = (mac[4]<<8)|mac[5]` (`WiFiOps.cpp:397-398`). Findet sich kein Match, wird ein neuer Slot allokiert (`allocateNodeSlot`, `WiFiOps.cpp:419-436`). **Achtung Kollision**: zwei Nodes mit identischen letzten 2 Bytes wuerden im Suffix-Lookup verschmelzen. Der dest-MAC fuer den Reply nutzt aber die volle MAC aus `info->src_addr`, der Slot-Lookup aber nur das Suffix.

**ESP-spezifische API-Calls die der Marauder-CORE braucht:**

```
esp_now_init()                                 // einmal nach WiFi-init
esp_now_set_pmk(pmk)                           // 16-byte PMK setzen
esp_now_register_recv_cb(OnDataRecv)
esp_now_add_peer(&peerInfo)                    // pro Node-MAC (oder Broadcast)
esp_now_del_peer(mac)                          // bei Modus-Wechsel (encrypt vs plain)
esp_now_is_peer_exist(mac)
esp_now_send(dest_mac, buf, len)
esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE)
esp_wifi_set_ps(WIFI_PS_NONE)
esp_wifi_set_promiscuous(true/false)           // nur als Workaround zum Channel-Setzen
```

Library: `esp_now.h`, `esp_wifi.h`, `WiFi.h` (Arduino), `mbedtls/sha256.h`. Alle auf classic ESP32 verfuegbar — das ist der Standard-IDF-Stack.

---

## 2. Discovery & Pairing

### Discovery-Strategie

Es gibt **keine** Beacon-/SSID-Discovery. Die Node sendet einen `MSG_CORE_REQUEST` als ESP-NOW-Broadcast, der Core antwortet mit `MSG_CORE_REPLY` als Unicast — und damit lernt die Node die Core-MAC. Pre-shared MAC NICHT, fixed SSID NICHT, BLE-Beacon NICHT. Pre-shared ist nur der gemeinsame **`esp_now_key`-String** (im Web-Admin pro Geraet eingestellt, `settings.cpp` Key `"ek"`, geladen `WiFiOps.cpp:2296`), aus dem PMK und LMK per SHA-256 abgeleitet werden — siehe Sektion 4.

Hinweis: `sendCoreRequest()` wird nur dann periodisch aufgerufen, wenn `use_encryption == true` UND `!g_have_core` (`WiFiOps.cpp:2342-2352`). Im Plain-Mode unterbleibt die aktive CORE_REQUEST-Schleife komplett; die Node sendet einfach Daten und Heartbeats per Broadcast. Folge: Im Plain-Mode hat eine Node nie eine konkrete Core-MAC und sendet **alle** TEXT/HEARTBEAT-Pakete als Broadcast (`WiFiOps.cpp:741, 811`).

Es gibt einen einmaligen `runAdminWindowAfterScanCycle()` direkt im `begin()` der Node (`WiFiOps.cpp:2329`), der einen Heartbeat schickt und 300ms (`ADMIN_WAIT_MS`) auf Admin-Reply wartet — auch ohne CORE_REQUEST. Das funktioniert im Plain-Mode als implizite Discovery-Strategie: der Heartbeat-Broadcast erreicht den Core, der Core lernt die Node-MAC aus `info->src_addr` und sendet ein Admin-Paket zurueck.

### Pairing-Sequenz

**Variante A: Encryption ENABLED (Node-driven)**

```
NODE                                CORE
  | startESPNow()                    | startESPNow()
  | g_last_req_ms = millis()         | wartet auf RX
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
  |                                  | flag NODE_FLAG_ENCRYPTED setzen
  |                                  |
  |                                  | wenn isNewNode:
  |                                  |   handleNodeTopologyChange()
  |                                  |   -> assignment_version++
  |                                  |      recalculateChannelAssignments()
  |                                  |      markAllActiveNodesAdminDirty()
  |                                  |
  |                                  | wenn ADMIN_DIRTY:
  |                                  |   sendAdminToNodeSlot(slot,src)
  |                                  |   (immer als plaintext-peer!)
  |   <======UNICAST plain===========|
  | OnDataRecv: MSG_ADMIN            |
  |   wenn version != local:         |
  |     uebernehmen                  |
  |     assigned_start_idx,          |
  |     assigned_end_idx,            |
  |     assigned_node_index,         |
  |     assigned_node_count          |
  |
  | jetzt: getNodeReady()=true       |
  | scanloop sendet jeden Wardrive-  |
  | Record als MSG_TEXT encrypted    |
  | unicast an g_core_mac.           |
  |                                  |
  | Heartbeat alle Scan-Zyklen am    |
  | Ende, Channel 6:                 |
  | sendHeartbeat() encrypted        |
  | =========UNICAST enc==========>  |
  |                                  | OnDataRecv: MSG_HEARTBEAT
  |                                  |   touchNode(src)
  |                                  |   wenn ADMIN_DIRTY:
  |                                  |     sendAdminToNodeSlot()
```

**Variante B: Encryption DISABLED**

Wesentlich simpler. Node sendet nie `MSG_CORE_REQUEST` (siehe `WiFiOps.cpp:1103-1104`, der Aufruf steht unter `if (this->use_encryption)`). Stattdessen:

```
NODE                                CORE
  | begin() -> runAdminWindowAfter   |
  |   ScanCycle() -> sendHeartbeat() |
  | type=3 als BROADCAST plain       |
  | =========BROADCAST===========>   |
  |                                  | OnDataRecv: MSG_HEARTBEAT
  |                                  |   touchNode(src), isNewNode=true
  |                                  |   handleNodeTopologyChange()
  |                                  |   sendAdminToNodeSlot(slot,src)
  |   <======UNICAST plain===========|
  | OnDataRecv: MSG_ADMIN -> uebernehmen
  |
  | Wardriving-Daten:                |
  | sendBroadcastStringPlain(line)   |
  | type=4 als BROADCAST plain       |
  | =========BROADCAST===========>   |
```

Im Plain-Mode bleibt `g_have_core=false` (es gibt keinen MSG_CORE_REPLY-Trigger), `g_secure_ready=true` (forced bei `getNodeReady()` via `if (!use_encryption) return true`, `WiFiOps.cpp:1140-1141`). Die Node hat also nie eine konkrete CORE-MAC im Plain-Mode — alles geht broadcast.

### Anzahl Nodes

- Hard-Limit im Code: **`MAX_NODES = 24`** (`WiFiOps.h:44`). Das ist die Groesse von `node_table[MAX_NODES]` (`WiFiOps.cpp:66`). Slots werden ueber `flags & NODE_FLAG_ACTIVE` belegt/frei verwaltet.
- Encrypt-Limit "max 6": **NICHT explizit im Code als Konstante**. Der einzige Hinweis ist die Beschraenkung von ESP-NOW selbst: Espressif beschraenkt encrypted ESP-NOW-Peers im IDF auf 6, plaintext auf 20 (Doku-Behauptung, nicht im Wardriver-Code reflektiert). Der Wardriver allokiert bis zu `MAX_NODES = 24` Slots in der eigenen Table, aber `addPeerWithMode(..., encrypt=true)` wird beim 7. Encrypted-Peer von ESP-NOW selbst mit `ESP_ERR_ESPNOW_FULL` o.ae. zurueckgewiesen → die README-Aussage ist hier indirekt, nicht codiert. **Status: README-Behauptung, im Code als ESP-NOW-Library-Limit, nicht als eigene `#define`-Konstante.**

### Rolle der State-Variablen

| Variable | Wo | Bedeutung |
|---|---|---|
| `g_have_core` | static, `WiFiOps.cpp:13` | Node-only. True nach Empfang eines `MSG_CORE_REPLY`. Nur gesetzt im Encrypted-Pfad. Steuert die `sendCoreRequest`-Backoff-Schleife in `main()` (`WiFiOps.cpp:2342`). |
| `g_secure_ready` | static, `WiFiOps.cpp:14` | Node-only. True wenn der encrypted Peer fuer den Core erfolgreich angelegt ist (oder sofort true im Plain-Mode). Gating fuer `sendEncryptedStringToCore` (`WiFiOps.cpp:754`) und `sendHeartbeat` (`WiFiOps.cpp:729`). |
| `g_core_mac[6]` | static | Node-only. Aus dem ersten `MSG_CORE_REPLY` `info->src_addr` kopiert (`WiFiOps.cpp:1026`). |
| `assigned_start_idx`, `assigned_end_idx` | global, file-scope `WiFiOps.cpp:57-58` | Node-only. Index in `scan_channels[]`. Bestimmt welche Channels die Node scannen soll. Default vor dem ersten Admin: `0..NUM_SCAN_CHANNELS-1` (= alle 40 Channels). Nach Admin: nur ein Slice. |
| `assigned_node_index`, `assigned_node_count` | global, file-scope | Node-only. Welcher Index die Node im Core-Cluster hat (0..node_count-1) und wieviele Nodes der Core kennt. Reine Info, nicht zur Logik herangezogen ausser fuer Logging. |
| `assignment_version` | global | Node-only. Wird vergleichen mit `admin->assignment_version`, nur bei Aenderung wird das Mapping uebernommen (`WiFiOps.cpp:1053`). |
| `current_assigned_scan_idx` | public Member von `WiFiOps`, `WiFiOps.h:167` | Node-only. Cursor in `scan_channels[]` waehrend des aktiven Wardrive-Loops. Faengt bei `assigned_start_idx` an, inkrementiert pro Scan, wrappt bei `assigned_end_idx`. |
| `current_assignment_version` | public Member von `WiFiOps`, `WiFiOps.h:166` | Core-only. Wird bei `handleNodeTopologyChange` inkrementiert (`WiFiOps.cpp:533-534`), 0 wird uebersprungen. |
| `node_table[MAX_NODES]` | global, `WiFiOps.cpp:66` | Core-only. NodeRecord pro aktivem Node-Slot. |
| `pmk[16]`, `lmk[16]` | global, `WiFiOps.cpp:63-64` | Beide, abgeleitet aus `esp_now_key` per SHA-256. |

---

## 3. Packet Format(s)

Insgesamt **5 Message-Types**, alle teilen die gleiche 5-Byte-Header-Praefix (`magic[4] + type[1]`). Jedes Paket ist eine `__attribute__((packed))` C-Struktur, kein CBOR/JSON, **rohe little-endian Struct-Bytes auf der Leitung**. ESP32 ist little-endian, alle `uint16_t`/`uint32_t` werden als-is gesendet.

### Magic + Type-Enum

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
  uint32_t counter;                   // little-endian; heartbeat counter only fuer MSG_HEARTBEAT
  uint16_t len;                       // little-endian; gueltig nur fuer MSG_TEXT, sonst 0
  char     text[ENOW_TEXT_MAX + 1];   // ENOW_TEXT_MAX=200, +1 NUL = 201 bytes
} enow_text_msg_t;
```

- Total: 4 + 1 + 4 + 2 + 201 = **212 bytes**.
- ESP-NOW max payload ist 250 bytes → passt.
- Das volle Struct wird IMMER gesendet (auch bei CORE_REQUEST/REPLY/HEARTBEAT, wo `text` und `len` ungenutzt sind), nicht nur die ersten 11 Bytes. Code: `esp_now_send(..., (uint8_t*)&msg, sizeof(msg))` — `sizeof(enow_text_msg_t)` = 212. RX prueft das Minimum mit `if (len < (int)sizeof(enow_text_msg_t)) return;` (`WiFiOps.cpp:873, 914, 939, 1024`). **Marauder muss zwingend 212 Bytes senden, nicht nur den header.**
- `text[len]` wird vom Sender NUL-terminiert (`msg.text[n] = '\0'`, `WiFiOps.cpp:768, 808`).
- Maximale Wardrive-Line: 200 Zeichen. Im Code wird das bei `n > ENOW_TEXT_MAX` gecappt (`WiFiOps.cpp:765, 805`). Wardrive-Lines aus `processWardrive` mit langen SSIDs koennen das ueberschreiten — werden truncated, nicht abgelehnt.

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
- `start_channel_idx` und `end_channel_idx` sind **Indizes** in `scan_channels[]` (`WiFiOps.cpp:44-53`), NICHT Channel-Nummern direkt. `scan_channels[0]=1, scan_channels[13]=14, scan_channels[14]=36, ...`. Das Array hat **NUM_SCAN_CHANNELS = 40** Eintraege (14 × 2.4 GHz + 26 × 5 GHz). Der Core muss die identische Channel-Tabelle haben, sonst stimmen Indizes nicht.

**Byte-genaues Array** (aus `WiFiOps.cpp:44-53`):

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

Auffaellige Luecken im 5GHz-Bereich: zwischen 100 und 112 fehlen 104+108. Stoppt nicht bei 165 sondern geht bis 177.

### Encoding-Bemerkung

- Endianness: little-endian (ESP32 native).
- Padding: `__attribute__((packed))` → kein Padding.
- Es gibt **kein** Sequence-Number-Schema fuer TEXT-Pakete, kein CRC ausserhalb der ESP-NOW-Layer-CRC, kein per-Message-Auth.
- Der `counter` in `enow_text_msg_t` wird ausschliesslich von Heartbeats genutzt (monoton ansteigend, `g_hb_counter++`), wird vom Core nur geloggt, nicht gegen Replay geprueft (`WiFiOps.cpp:919`).

### TEXT-Payload-Format (MSG_TEXT)

Die Wardrive-Line ist ein **Comma-Separated-String**, 6 Felder, gemischt fuer WiFi und BLE. `parseWardriveLine` (`WiFiOps.cpp:821-850`) erwartet exakt 6 kommasgetrennte Felder:

```
BSSID,ESSID,SECURITY,CHANNEL,RSSI,TYPE
```

- `TYPE = "W"` fuer WiFi, `"B"` fuer BLE (`WiFiOps.cpp:131, 1340`).
- Bei BLE: `SECURITY = "[BLE]"`, `CHANNEL = "0"`.
- Beispiel-Plaintext aus `processWardrive`:
  - WiFi: `AA:BB:CC:DD:EE:FF,MyHomeAP,WPA2,11,-67,W`
  - BLE:  `12:34:56:78:9A:BC,,[BLE],0,-89,B`
- Komma in SSID wird vom Sender vorab durch Underscore ersetzt (`ssid.replace(",", "_")`, `WiFiOps.cpp:1297, 1339`) — keine CSV-Quotierung.

Der Core zeitlich-anreichert das mit GPS und schreibt eine **11-Feld**-Wigle-Line mit eigener `Datetime/Lat/Lon/Alt/Accuracy/Type` (`WiFiOps.cpp:969-980`).

---

## 4. Encryption

### Mechanismus: ESP-NOW eingebaute AES-128-CCMP-Verschluesselung

ESP-NOW bietet nativ AES-128 zwischen Peers (CCMP-aehnlich, IDF intern). Wardriver nutzt das via:

- `esp_now_set_pmk(pmk)` global einmal (`WiFiOps.cpp:1087`). PMK = Primary Master Key, 16 Bytes.
- `peerInfo.encrypt = true; memcpy(peerInfo.lmk, lmk16, 16);` pro Peer (`WiFiOps.cpp:603-609`). LMK = Local Master Key, 16 Bytes.

**Es wird kein eigenes AES via mbedtls aufgesetzt.** mbedtls/sha256 wird ausschliesslich fuer die Key-Derivation aus dem User-String genutzt, nicht fuer Daten-Verschluesselung.

### Key-Derivation

`derive_key_16` (`WiFiOps.cpp:1108-1118`):

```
SHA256(esp_now_key)[0..15]  -> nicht direkt, sondern:
SHA256(esp_now_key + "_pmk")[0..15] -> pmk[16]
SHA256(esp_now_key + "_lmk")[0..15] -> lmk[16]
```

`computeKeysFromEnowKey` (`WiFiOps.cpp:1120-1125`):

```
derive_key_16(esp_now_key + "_pmk", pmk);
derive_key_16(esp_now_key + "_lmk", lmk);
```

Beide Geraete (Node und Core) muessen den exakt gleichen `esp_now_key`-String haben — pre-shared via Web-Admin, persistiert in SPIFFS unter Key `"ek"` (`WiFiOps.cpp:2292, 2296`).

### Schluessel-Storage

- Persistiert in SPIFFS via `settings.cpp` (Key `"ek"`).
- Nicht hardcoded.
- Nicht ueber den Pairing-Flow ausgehandelt — pre-shared.

### "max 6 Nodes" Begruendung

Espressif-Doku begrenzt encrypted ESP-NOW-Peers auf 6 (default). Plaintext bis zu 20. Im Wardriver-Code ist das nicht selbst limitiert; `MAX_NODES=24` ist die Slot-Tabelle. **Behauptung im README, im Wardriver-Code nur indirekt durch den IDF-Layer durchgesetzt.** → Bei einer Marauder-Implementation auf classic ESP32 muss man das ESP-NOW-IDF-Limit beachten oder anheben (CONFIG_ESPNOW_MAX_ENCRYPT_PEER_NUM ist build-time tweakbar).

### Off-Mode (Encryption disabled)

Komplett plain. Es gibt **keinen** simplen Auth-Mechanismus, keinen MAC-Filter, keinen Shared-Token im Plaintext-Pfad. Jede beliebige ESP32 in Reichweite, die das ENOW-Magic kennt und auf Channel 6 sendet, wird vom Core akzeptiert und in `node_table` aufgenommen. Spoofing eines Wardrive-Records ist trivial. Im README als "theoretisch unbegrenzt" verkauft, faktisch aber auch ungeschuetzt.

---

## 5. Heartbeat / Keepalive

### Heartbeat-Send

- Trigger: `runAdminWindowAfterScanCycle()` (`WiFiOps.cpp:691-701`), aufgerufen von Node nach jedem vollstaendigen Scan-Zyklus durch alle assigned channels (`WiFiOps.cpp:1251-1252`).
- Eine Node sendet einen Heartbeat **nicht in einem festen Zeit-Intervall**, sondern abhaengig vom Scan-Zyklus. Mit `CHANNEL_TIMER=80`ms (`configs.h:140`) pro Channel und z.B. 5 zugewiesenen Channels: ~400ms zwischen Heartbeats. Bei 32 Channels (kein Admin empfangen): ~2.5s.
- Heartbeat enthaelt monotonen `g_hb_counter`, plain im Encrypted-Mode an `g_core_mac`, sonst Broadcast.

### Stale-Detection

- `NODE_TIMEOUT_MS = 60000` (`WiFiOps.h:45`). 60 Sekunden ohne Heartbeat → Node entfernt.
- `removeStaleNodes()` (`WiFiOps.cpp:461-475`) wird in `main()` jeden Tick aufgerufen, wenn `run_mode==CORE_MODE` (`WiFiOps.cpp:2360`). Geloeschter Slot triggert `handleNodeTopologyChange()` → `assignment_version++`, channel-Reassignment fuer verbliebene Nodes, `markAllActiveNodesAdminDirty()`. Naechster Heartbeat einer noch aktiven Node bekommt das neue Admin-Paket.
- Der Core printet alle `DEBUG_OUTPUT_DELAY = 30000`ms (30 Sekunden) die Node-Tabelle.

### Node-side Core-Loss Detection

**Fehlt fast vollstaendig.** Es gibt keine Logik in der Node, die `g_have_core` auf `false` zuruecksetzt, wenn der Core stumm wird. Folge: Eine Node, die Encryption aktiviert hat und ihren Core verliert, wird unendlich viele encrypted TEXT-Pakete an die alte CORE-MAC schicken, die ins Leere laufen. Sie sendet keine neuen `MSG_CORE_REQUEST` mehr (Backoff-Schleife in `main()` ist durch `g_have_core==true` blockiert).

Das ist eine bekannte Schwachstelle im Wardriver-Design. Fuer Marauder-Implementierung als CORE ist das nicht direkt relevant — der Core kuemmert sich nicht um seinen eigenen Verlust. Aber wenn ein Marauder-CORE neu gestartet wird, koennen alte C5-Nodes "kaputt" bleiben bis sie selbst rebooten.

---

## 6. Error Handling & Retransmission

### ESP-NOW ACK-Auswertung

- **Kein** `esp_now_register_send_cb` registriert. Die asynchrone Send-Status-Callback wird nicht genutzt.
- Stattdessen wird der **synchrone Return** `esp_err_t` aus `esp_now_send` ausgewertet. Das ist nur die "Hat ESP-NOW das Paket angenommen" — nicht "Empfaenger hat ACKt". Beleg: `WiFiOps.cpp:640-647, 665-668, 743-748, 772-775, 811-815`.
- Bei Failure: Logging via `Logger::log(WARN_MSG, ...)`. **Keine Retransmission**, kein Buffer-Save, kein Retry. Der Wardrive-Record wird verworfen.

### Buffer-Strategie auf Node-Seite

- Es gibt keinen Outbound-Buffer fuer Wardrive-Records. `processWardrive` ruft direkt `sendEncryptedStringToCore` oder `sendBroadcastStringPlain` und ignoriert Failures.
- `Buffer::append(String log)` ist ein lokaler PCAP-/Log-Buffer fuer SD-Schreiben (`Buffer.cpp:119-124`), wird im Node-Mode nicht fuer Network-Traffic genutzt. Im Node-Mode ist die SD typischerweise nicht supported (`begin()` fuer SD passiert trotzdem, aber `sd_obj.supported` ist false).
- Fazit: Out-of-Range Nodes verlieren Daten dauerhaft.

### Retry-Logik (nur fuer CORE_REQUEST)

`WiFiOps.cpp:9-22, 2342-2352`:

- Initial 300ms, exponential backoff (×2), capped bei 5000ms.
- Reset bei `g_have_core=true`.
- Gilt **nur** fuer den encrypted Pairing-Flow.

### Sequence-Numbers / Dedup

- Keine Sequence-Numbers im Protocol Header.
- Heartbeat-Counter wird nicht zur Replay-Detection genutzt.
- Wardrive-Records werden auf der CORE-Seite per `seen_mac()` deduplikiert (`WiFiOps.cpp:962`) — gleiche BSSID nur einmal pro `mac_history_len=200` (`configs.h:139`) — das ist eine **inhaltliche** Dedup, nicht protokollbasiert.

### Verhalten bei malformed packets

- `OnDataRecv` prueft minimum `len >= 5` (`WiFiOps.cpp:859`).
- Magic-Check: `if (memcmp(data, MAGIC, 4) != 0) return;` (`WiFiOps.cpp:862`).
- Pro Type wird eine zusaetzliche Mindest-Laenge geprueft (z.B. `len < sizeof(enow_text_msg_t)` → return, `WiFiOps.cpp:873`).
- Unknown Type → Log `RX unknown type N`, kein Crash (`WiFiOps.cpp:1016-1017, 1072-1073`).
- TEXT mit `t->len > ENOW_TEXT_MAX` wird verworfen (Log only, `WiFiOps.cpp:1003-1004`).

Robust ist das nicht — eine boese 4 Bytes "ENOW" + falsche len-Field-Werte koennten eine `parseWardriveLine` mit unkontrollierten Strings versorgen, aber es gibt keinen Out-of-bounds-Read weil das gesamte Struct fixed-size ist.

---

## 7. Marauder-Implementations-Anforderungen

### Was muss ein Marauder-v7-CORE_MODE replizieren?

1. **WiFi-Mode `WIFI_STA` ohne Connect** — `esp_now_init` erfordert eine WiFi-Init mit Mode STA (oder AP, oder STA_AP). Marauder ruft i.d.R. `WiFi.mode(WIFI_AP_STA)` o.ae. — kompatibel.
2. **Channel auf 6 fixiert** — KOLLISION mit Marauders eigenem Wardrive-/Sniff-/Beacon-Spam, der staendig Channels wechselt. Im CORE-Mode auf Marauder MUSS der Channel-Hop deaktiviert sein. Loesung: `setFixedChannel(6)` einmal beim CORE-Mode-Enter, alle anderen Marauder-WiFi-Funktionen blockieren.
3. **PMK setzen** — `esp_now_set_pmk(pmk)` mit den 16 Bytes aus SHA256(`<userKey>_pmk`).
4. **Empty Recv-Callback registrieren** der die exakt gleiche `OnDataRecv`-Logik implementiert: Magic-Check, Type-Switch, `touchNode` mit MAX_NODES-Slots und 60s Timeout, Channel-Idx-Assignment, `sendCoreReply`/`sendAdminToNodeSlot`/`sendHeartbeat`-RX-Trigger.
5. **`scan_channels[]` Array exakt identisch** (40 Eintraege, 14×2.4 + 26×5 GHz, siehe Sektion 3 fuer das byte-genaue Array). Wichtig: classic ESP32 (Marauder v7 ohne C5) hat nur 2.4 GHz, kann selbst nicht auf Channel 36+ scannen. Aber als CORE muss er nur die Indizes-Bedeutung verstehen, da er nicht selbst scannt.
6. **Keine eigene Wardrive-Aktivitaet** — der Core scannt nicht. Nur RX, GPS, SD, Wigle-Format-Compose.
7. **GPS + SD-Funktionalitaet** — Marauder hat beides bereits. GpsInterface beider Repos ist vermutlich aehnlich (in 1.4 zu pruefen).
8. **Wigle-Output-Format identisch** — 11-Feld-Line wie in `WiFiOps.cpp:969-980`.

### Kritische API-Calls (Includes)

```cpp
#include <esp_now.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include <WiFi.h>
#include "mbedtls/sha256.h"
```

Alle Standard-IDF — auf Arduino-ESP32-Core 2.x und 3.x verfuegbar. Marauder nutzt Arduino-Core 2.x → kein Problem.

### Bekannte Inkompatibilitaeten

1. **Channel-Konflikt** mit anderen Scan-Modes des Marauder (z.B. Probe-Scan, Packet-Monitor). CORE-Mode muss exklusiv-blockierend sein.
2. **PMK ist global** in ESP-NOW. Wenn Marauder ESP-NOW fuer andere Features nutzt (Beacon-Spam nicht, aber evtl. fuer andere Tools), wuerden die Schluessel kollidieren. **In 1.5 zu pruefen** ob Marauder ESP-NOW aktuell nutzt.
3. **Encrypted-Peer-Limit von 6** im IDF — fuer Marauder als CORE kompatibel zur C5-Wardriver-Doku, aber harte Untergrenze; build-time-Tweak moeglich.
4. **NimBLE-Coexistence** — Wardriver nutzt NimBLE parallel zu ESP-NOW auf classic ESP32 (im Encrypted-Mode am Anfang nicht aktiv, aber generell). Marauder hat NimBLE-Submodul, daher kein Konflikt erwartet.
5. **Promiscuous-Mode** wird kurzzeitig aktiviert nur als Workaround zum `esp_wifi_set_channel`-Call. Das stoert keinen Marauder-State, ist aber zeitlich kollisionsempfindlich, wenn Marauder selbst gerade promiscuous Sniffing macht. Im CORE-Mode duerfte das nicht passieren.
6. **`ieee80211_raw_frame_sanity_check`** Override (`WiFiOps.cpp:79-84`) — das ist ein Wardriver-spezifischer Hack der erlaubt, raw frames zu senden. Fuer CORE nicht noetig, fuer NODE-Mode aber wichtig (Marauder braucht das im Wardrive-Mode evtl. selbst — pruefen in 1.4).

---

## Zusammenfassung der Sicherheits-Level

| Befund | Status |
|---|---|
| Transport ist ESP-NOW | **Belegt durch Code (`#include`, `esp_now_send` etc.)** |
| Channel = 6 hardcoded | **Belegt** (`WiFiOps.cpp:6`) |
| 5 Message-Types: REQUEST/REPLY/HEARTBEAT/TEXT/ADMIN | **Belegt** (`WiFiOps.cpp:68-74`) |
| Encryption per ESP-NOW eingebautem AES-128 mit PMK/LMK | **Belegt** (`esp_now_set_pmk`, `peerInfo.lmk`) |
| Key-Derivation per SHA-256 vom User-String | **Belegt** (`derive_key_16`) |
| MAX_NODES=24, NODE_TIMEOUT=60s | **Belegt** (`WiFiOps.h:44-45`) |
| "max 6 Nodes mit Encryption" | **Behauptet im README, im Code als IDF-Library-Limit, nicht als eigene Konstante** |
| Plain-Mode hat keinerlei Auth | **Belegt durch Abwesenheit** |
| Keine Retransmission, keine Send-Callbacks | **Belegt durch Abwesenheit von `esp_now_register_send_cb`** |
| Node-side Core-Loss Detection fehlt | **Belegt durch Abwesenheit jeglicher `g_have_core=false` Reset-Logik im Encrypted-Pfad** |
| Wardrive-Line ist 6-Feld CSV "BSSID,ESSID,SEC,CH,RSSI,W/B" | **Belegt** (`parseWardriveLine`) |
| Struct-Pakete werden full-size gesendet (212 Bytes auch fuer Heartbeat) | **Belegt** (`sizeof(enow_text_msg_t)` in `esp_now_send`) |

Hypothesen, NICHT direkt belegt:
- Die Annahme dass Marauder ESP-NOW aktuell nicht selbst nutzt — **muss in 1.5 verifiziert werden**.
- Die Annahme dass classic ESP32 die 5GHz-Indizes-Bedeutung verstehen kann ohne selbst scannen zu muessen — sehe ich als trivial weil der CORE im Wardriver eh nichts scannt.
- "Encrypted-Peer-Limit 6 ist tweakbar via build-flag" — kommt aus IDF-Doku, im Wardriver-Code keinerlei Tweak gemacht.
