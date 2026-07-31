// WardriveCore.cpp
//
// Marauder v7 Core Mode — Implementation.
// Empfaengt Wardrive-Records von C5-Wardriver-Nodes via ESP-NOW auf Channel 6,
// reichert mit GPS an, schreibt Wigle-CSV-Lines auf SD via Marauders Buffer.
//
// Adapted from JCMK ESP32DualBandWardriver (src/WiFiOps.cpp), MIT License,
// Copyright (c) 2025 Just Call Me Koko.
//
// Marauder integration: Phase 3, 2026-05-06.

#include "WardriveCore.h"

#ifdef MARAUDER_CORE_MODE

#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "mbedtls/sha256.h"

#include "WiFiScan.h"
#include "Buffer.h"

#ifdef HAS_GPS
  #include "GpsInterface.h"
#endif

#ifdef HAS_SD
  #include "SDInterface.h"
  #include "SD.h"
#endif

#ifdef HAS_SCREEN
  #include "Display.h"
  #include "BatteryInterface.h"   // battery percentage in the cluster header
#endif

#include "settings.h"

// Marauder-Globals (definiert in esp32_marauder.ino):
extern WiFiScan wifi_scan_obj;
extern Buffer   buffer_obj;
#ifdef HAS_GPS
  extern GpsInterface gps_obj;
#endif
#ifdef HAS_SD
  extern SDInterface sd_obj;
#endif
#ifdef HAS_SCREEN
  extern Display display_obj;
  extern BatteryInterface battery_obj;
#endif
extern Settings settings_obj;

// Center-Button: Marauder-Switches-Wrapper.
#ifdef HAS_BUTTONS
  #if (C_BTN >= 0) && !defined(MARAUDER_CARDPUTER) && !defined(MARAUDER_CARDPUTER_ADV)
    #include "Switches.h"
    extern Switches c_btn;
  #endif
#endif

static const uint8_t BROADCAST_MAC_CORE[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// Singleton-Pointer fuer Static-Callback. ESP-NOW-Recv-Callback ist eine
// freie Funktion — wir routen ueber diesen Pointer zur Instanz. Set in init(),
// cleared in deinit().
//
// Warum nicht `extern WardriveCore wardrive_core_obj;` direkt im Callback
// nutzen? Geht auch — aber dieser Indirection-Pointer macht clear, dass der
// Callback nur dann etwas tut wenn init() lief und deinit() noch nicht.
static WardriveCore* g_active_core = nullptr;

// ============================================================
// Konstruktor
// ============================================================

WardriveCore::WardriveCore() {
    rx_queue = nullptr;
    is_running = false;
    use_encryption = false;
    memset(pmk, 0, sizeof(pmk));
    memset(lmk, 0, sizeof(lmk));
    user_key = "";
    assignment_version = 1;
    total_rx_lines = 0;
    total_rx_wifi = 0;
    total_rx_ble = 0;
    total_rx_bad = 0;
    total_rx_drops = 0;
    low_heap_events = 0;
    buffer_overruns = 0;
    last_rx_node_idx = -1;
    last_rx_ms = 0;
    last_rx_rssi = 0;
    rate_window_ms = 0;
    rate_prev_total = 0;
    rate_lines_per_min = 0;
    drawn_row_count = 0xFF;   // forces a full node-table repaint
    drawn_pitch = 0;
    memset(drawn_sig, 0xFF, sizeof(drawn_sig));
    session_start_ms = 0;
    last_display_refresh_ms = 0;
    last_stale_check_ms = 0;
    last_heap_check_ms = 0;
    last_sd_check_ms = 0;
    center_press_start_ms = 0;
    center_was_pressed = false;
    sd_healthy = true;
    memset(node_table, 0, sizeof(node_table));
}

// ============================================================
// Helpers
// ============================================================

uint16_t WardriveCore::macToSuffix(const uint8_t* mac) {
    return ((uint16_t)mac[4] << 8) | mac[5];
}

// Adapted from JCMK ESP32DualBandWardriver (WiFiOps.cpp:1108-1118), MIT License
void WardriveCore::derive_key_16(const String& s, uint8_t out16[16]) {
    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const unsigned char*)s.c_str(), s.length());
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    memcpy(out16, hash, 16);
}

void WardriveCore::computeKeysFromUserKey() {
    // PMK = SHA256(key + "_pmk")[0..15], LMK = SHA256(key + "_lmk")[0..15].
    // Byte-exakt portiert aus Wardriver `WiFiOps.cpp:1120-1125`. Beide Keys
    // muessen auf Node und Core IDENTISCH sein, sonst dropt IDF die Pakete
    // still ohne Decrypt-Error-Callback (Failure-Mode 6.8).
    derive_key_16(user_key + "_pmk", pmk);
    derive_key_16(user_key + "_lmk", lmk);
}

// Adapted from JCMK ESP32DualBandWardriver (WiFiOps.cpp:573-593), MIT License
void WardriveCore::setFixedChannel(uint8_t ch) {
    // Workaround: esp_wifi_set_channel() funktioniert auf Classic ESP32 nur
    // wenn das Radio gerade NICHT in einem normalen STA-RX-Cycle ist. Promisc-
    // on/off-Toggle umgeht das. Power-Save off, sonst Channel-Drift.
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_promiscuous(true);
    esp_err_t e = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    if (e != ESP_OK) {
        Serial.printf("CORE: esp_wifi_set_channel failed: %d\n", (int)e);
    }
    esp_wifi_set_promiscuous(false);
}

// ============================================================
// Node-Tabelle
// ============================================================

int WardriveCore::findNodeByMacSuffix(uint16_t suffix) {
    // Kept for any external callers; not used in the hot path anymore.
    for (int i = 0; i < WARDRIVE_CORE_MAX_NODES; i++) {
        if ((node_table[i].flags & NODE_FLAG_ACTIVE) &&
            node_table[i].mac_suffix == suffix) {
            return i;
        }
    }
    return -1;
}

// Full 6-byte MAC equality lookup. Replaces the upstream wardriver's
// 2-byte-suffix lookup because two NodeMCU-32 boards from the same batch
// can legitimately share the last two MAC bytes — observed on TJ's test
// rig 2026-05-15, only one of two nodes ever showed up in the slot table
// when both were powered together.
int WardriveCore::findNodeByMac(const uint8_t* mac) {
    for (int i = 0; i < WARDRIVE_CORE_MAX_NODES; i++) {
        if ((node_table[i].flags & NODE_FLAG_ACTIVE) &&
            memcmp(node_table[i].mac, mac, 6) == 0) {
            return i;
        }
    }
    return -1;
}

int WardriveCore::allocateNodeSlot(const uint8_t* mac) {
    uint16_t suffix = macToSuffix(mac);
    for (int i = 0; i < WARDRIVE_CORE_MAX_NODES; i++) {
        if (!(node_table[i].flags & NODE_FLAG_ACTIVE)) {
            memset(&node_table[i], 0, sizeof(NodeRecord));
            memcpy(node_table[i].mac, mac, 6);
            node_table[i].mac_suffix = suffix;
            node_table[i].last_seen_ms = millis();
            node_table[i].assigned_index = 0;
            node_table[i].start_channel_idx = 0;
            node_table[i].end_channel_idx = NUM_SCAN_CHANNELS - 1;
            node_table[i].last_admin_version_sent = 0;
            node_table[i].flags = NODE_FLAG_ACTIVE | NODE_FLAG_ADMIN_DIRTY;
            return i;
        }
    }
    // Hard-Reject: alle MAX_NODES Slots belegt. Caller wertet -1 als
    // "ignore packet, do not send reply" — siehe integration_design.md 2.4.
    return -1;
}

// Adapted from JCMK ESP32DualBandWardriver (WiFiOps.cpp:438-459), MIT License,
// erweitert um Hard-Reject bei MAX_NODES.
int WardriveCore::touchNode(const uint8_t* mac, bool& isNewNode) {
    isNewNode = false;

    int slot = findNodeByMac(mac);
    if (slot >= 0) {
        node_table[slot].last_seen_ms = millis();
        return slot;
    }

    // Lookup failed — versuchen zu allokieren.
    slot = allocateNodeSlot(mac);
    if (slot >= 0) {
        isNewNode = true;
        Serial.printf("CORE: Node added slot %d MAC %02X:%02X:%02X:%02X:%02X:%02X (count=%u)\n",
                      slot, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                      getActiveNodeCount());
    } else {
        // Hard-Reject. Counter koennte in zukuenftiger Stats-Erweiterung
        // erhoeht werden. Aktuell: nur Log.
        Serial.printf("CORE: Node REJECT (table full, MAX_NODES=%d)\n",
                      WARDRIVE_CORE_MAX_NODES);
    }
    return slot;
}

// Adapted from JCMK ESP32DualBandWardriver (WiFiOps.cpp:461-475), MIT License
bool WardriveCore::removeStaleNodes() {
    bool changed = false;
    uint32_t now = millis();
    for (int i = 0; i < WARDRIVE_CORE_MAX_NODES; i++) {
        if (node_table[i].flags & NODE_FLAG_ACTIVE) {
            if ((uint32_t)(now - node_table[i].last_seen_ms) > WARDRIVE_CORE_NODE_TIMEOUT_MS) {
                // Peer auch aus ESP-NOW-Liste loeschen (best effort).
                if (esp_now_is_peer_exist(node_table[i].mac)) {
                    esp_now_del_peer(node_table[i].mac);
                }
                Serial.printf("CORE: Slot %d stale, dropping (suffix %04X)\n",
                              i, node_table[i].mac_suffix);
                memset(&node_table[i], 0, sizeof(NodeRecord));
                changed = true;
            }
        }
    }
    return changed;
}

// Adapted from JCMK ESP32DualBandWardriver (WiFiOps.cpp:477-502), MIT License
void WardriveCore::recalculateChannelAssignments() {
    uint8_t active_slots[WARDRIVE_CORE_MAX_NODES];
    uint8_t active_count = 0;

    for (uint8_t i = 0; i < WARDRIVE_CORE_MAX_NODES; i++) {
        if (node_table[i].flags & NODE_FLAG_ACTIVE) {
            active_slots[active_count++] = i;
        }
    }
    if (active_count == 0) return;

#ifdef WARDRIVE_2_4_ONLY
    // 2.4-GHz-only Fleet: nur Index 0..13 verteilen, 5-GHz-Slots bleiben unbesetzt.
    // Sanity: die ersten 14 Eintraege in scan_channels[] sind die 2.4-GHz-Channels.
    static_assert(WARDRIVE_2_4_CHANNEL_COUNT <= NUM_SCAN_CHANNELS,
                  "WARDRIVE_2_4_CHANNEL_COUNT exceeds scan_channels[] length");
    const uint8_t pool_size = WARDRIVE_2_4_CHANNEL_COUNT;
#else
    const uint8_t pool_size = NUM_SCAN_CHANNELS;
#endif

    for (uint8_t node_num = 0; node_num < active_count; node_num++) {
        uint8_t slot = active_slots[node_num];
        // Even split des Pools auf alle aktiven Nodes.
        uint8_t start_idx = (node_num * pool_size) / active_count;
        uint8_t end_idx   = (((node_num + 1) * pool_size) / active_count) - 1;

        node_table[slot].assigned_index = node_num;
        node_table[slot].start_channel_idx = start_idx;
        node_table[slot].end_channel_idx = end_idx;
        node_table[slot].flags |= NODE_FLAG_ADMIN_DIRTY;
    }
}

// Adapted from JCMK ESP32DualBandWardriver (WiFiOps.cpp:524-530), MIT License
void WardriveCore::markAllActiveNodesAdminDirty() {
    for (uint8_t i = 0; i < WARDRIVE_CORE_MAX_NODES; i++) {
        if (node_table[i].flags & NODE_FLAG_ACTIVE) {
            node_table[i].flags |= NODE_FLAG_ADMIN_DIRTY;
        }
    }
}

// Adapted from JCMK ESP32DualBandWardriver (WiFiOps.cpp:532-540), MIT License
void WardriveCore::handleNodeTopologyChange() {
    assignment_version++;
    if (assignment_version == 0) assignment_version = 1; // skip 0
    recalculateChannelAssignments();
    markAllActiveNodesAdminDirty();
}

uint8_t WardriveCore::getActiveNodeCount() {
    uint8_t count = 0;
    for (uint8_t i = 0; i < WARDRIVE_CORE_MAX_NODES; i++) {
        if (node_table[i].flags & NODE_FLAG_ACTIVE) count++;
    }
    return count;
}

uint8_t WardriveCore::getNodeCount() {
    return getActiveNodeCount();
}

// ============================================================
// Barrier-Session-Steuerung
// ============================================================

// MSG_SESSION an alle Nodes broadcasten (FF:FF:...). ESP-NOW-Broadcast ist
// unbestaetigt, daher senden wir das Kommando mehrfach. Eine Node, die es
// trotzdem verpasst, bleibt idle bis zum naechsten resyncSession().
bool WardriveCore::broadcastSession(uint8_t command) {
    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    if (!bcast_peer_ready) {
        if (!esp_now_is_peer_exist(bcast)) {
            esp_now_peer_info_t p = {};
            memcpy(p.peer_addr, bcast, 6);
            p.channel = 0;      // follow home channel (=6)
            p.encrypt = false;  // Session-Control ist immer plaintext
            if (esp_now_add_peer(&p) != ESP_OK) {
                Serial.println("CORE: session broadcast peer add failed");
                return false;
            }
        }
        bcast_peer_ready = true;
    }

    enow_session_msg_t msg = {};
    memcpy(msg.magic, ENOW_MAGIC, 4);
    msg.type = MSG_SESSION;
    msg.command = command;

    bool ok = false;
    for (int i = 0; i < 5; i++) {                 // 5x fuer Broadcast-Reliability
        if (esp_now_send(bcast, (uint8_t*)&msg, sizeof(msg)) == ESP_OK) ok = true;
        delay(15);
    }
    return ok;
}

void WardriveCore::startSession() {
    if (!is_running) return;
    collecting = true;
    Serial.printf("CORE: SESSION START (%u nodes)\n", getActiveNodeCount());
    broadcastSession(SESSION_CMD_START);
}

void WardriveCore::stopSession() {
    if (!is_running) return;
    collecting = false;
    Serial.println("CORE: SESSION STOP -> lobby");
    broadcastSession(SESSION_CMD_STOP);
}

void WardriveCore::resyncSession() {
    if (!is_running) return;
    // Alle aktuell registrierten Nodes (inkl. Spaet-Joiner) frisch
    // partitionieren; das neue Admin geht per Check-in raus, START breit.
    handleNodeTopologyChange();
    collecting = true;
    Serial.printf("CORE: SESSION RESYNC + START (%u nodes)\n", getActiveNodeCount());
    broadcastSession(SESSION_CMD_START);
}

// ============================================================
// ESP-NOW-Send
// ============================================================

// Adapted from JCMK ESP32DualBandWardriver (WiFiOps.cpp:595-612), MIT License
bool WardriveCore::addPeerWithMode(const uint8_t* mac, bool encrypt,
                                   const uint8_t lmk16[16]) {
    if (esp_now_is_peer_exist(mac)) {
        esp_now_del_peer(mac);
        delay(10);
    }
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = 0;        // 0 = follow home channel (=6 bei uns)
    peerInfo.encrypt = encrypt;
    if (encrypt && lmk16) {
        memcpy(peerInfo.lmk, lmk16, 16);
    }
    return (esp_now_add_peer(&peerInfo) == ESP_OK);
}

// Adapted from JCMK ESP32DualBandWardriver (WiFiOps.cpp:671-689), MIT License
bool WardriveCore::sendCoreReply(const uint8_t* destMac) {
    enow_text_msg_t msg = {};
    memcpy(msg.magic, ENOW_MAGIC, 4);
    msg.type = MSG_CORE_REPLY;
    msg.counter = 0;

    // Plaintext-Peer fuer Reply — die Node lernt erst aus diesem Reply unsere
    // CORE-MAC und legt DANACH einen encrypted Peer fuer uns an.
    if (!addPeerWithMode(destMac, false, nullptr)) {
        Serial.println("CORE: addPeerWithMode (plain reply) failed");
        return false;
    }
    esp_err_t res = esp_now_send(destMac, (uint8_t*)&msg, sizeof(msg));
    if (res != ESP_OK) {
        Serial.printf("CORE: sendCoreReply esp_now_send failed: %d\n", (int)res);
        return false;
    }
    return true;
}

// Adapted from JCMK ESP32DualBandWardriver (WiFiOps.cpp:614-654), MIT License.
// Admin-Pakete werden IMMER plaintext gesendet — auch wenn der Peer regulaer
// encrypted ist. Wardriver-Konvention. Begruendung: bei Topologie-Wechsel
// (encryption-Mode-flip) muss Admin auch durchgehen wenn der Encrypted-Peer
// gerade neu aufgebaut wird.
bool WardriveCore::sendAdminToNodeSlot(uint8_t slot, const uint8_t* dest_mac) {
    if (slot >= WARDRIVE_CORE_MAX_NODES) return false;
    if (!(node_table[slot].flags & NODE_FLAG_ACTIVE)) return false;

    enow_admin_msg_t msg = {};
    memcpy(msg.magic, ENOW_MAGIC, 4);
    msg.type = MSG_ADMIN;
    msg.assignment_version = assignment_version;
    msg.node_index = node_table[slot].assigned_index;
    msg.node_count = getActiveNodeCount();
    msg.start_channel_idx = node_table[slot].start_channel_idx;
    msg.end_channel_idx = node_table[slot].end_channel_idx;

    // Temporary plaintext peer. Wir loeschen ihn sofort danach, damit der
    // regulaere encrypted Peer (falls vorhanden) wieder aktiv ist.
    bool had_peer_before = esp_now_is_peer_exist(dest_mac);
    if (had_peer_before) {
        esp_now_del_peer(dest_mac);
    }
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, dest_mac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("CORE: admin temp-peer add failed");
        return false;
    }

    esp_err_t res = esp_now_send(dest_mac, (uint8_t*)&msg, sizeof(msg));
    esp_now_del_peer(dest_mac);

    if (res != ESP_OK) {
        Serial.printf("CORE: sendAdmin esp_now_send failed: %d\n", (int)res);
        return false;
    }

    // Wenn der Node-Slot vorher encrypted-Peer hatte, encrypted-Peer
    // wiederherstellen (nur wenn wir use_encryption nutzen UND der Slot
    // als ENCRYPTED markiert war).
    if (use_encryption && (node_table[slot].flags & NODE_FLAG_ENCRYPTED)) {
        addPeerWithMode(dest_mac, true, lmk);
    }

    node_table[slot].last_admin_version_sent = assignment_version;
    node_table[slot].flags &= ~NODE_FLAG_ADMIN_DIRTY;
    return true;
}

// ============================================================
// Wigle-Line-Compose
// ============================================================

// Adapted from JCMK ESP32DualBandWardriver (WiFiOps.cpp:821-980), MIT License.
// Node-Payload-Format: "BSSID,ESSID,SECURITY,CHANNEL,RSSI,W|B".
// Marauder-Wigle-Output: "BSSID,SSID,SECURITY,DATETIME,CHANNEL,RSSI,LAT,LON,ALT,ACC,WIFI|BLE".
// Wir ergaenzen DATETIME (zwischen SECURITY und CHANNEL) und LAT/LON/ALT/ACC/Type.
String WardriveCore::composeWigleLineFromNodeText(const enow_text_msg_t& msg) {
    const char* line = msg.text;
    int start = 0;
    int fieldIndex = 0;
    String fields[6];

    // Inline-Parser, byte-exakt aus Wardriver `parseWardriveLine`.
    while (fieldIndex < 6) {
        const char* comma = strchr(line + start, ',');
        if (!comma) {
            fields[fieldIndex++] = String(line + start);
            break;
        }
        fields[fieldIndex++] = String(line + start).substring(0, comma - (line + start));
        start = (comma - line) + 1;
    }
    if (fieldIndex != 6) {
        // Malformed — Caller waehlt was zu tun.
        return String();
    }

    String bssid    = fields[0];
    String essid    = fields[1];
    String security = fields[2];
    String channel  = fields[3];
    String rssi     = fields[4];
    String typeflag = fields[5]; // "W" or "B"

    String type = "WIFI";
    if (typeflag == "B") type = "BLE";

    // GPS-Anreicherung. Bei no-fix nutzen wir lat=0,lon=0,alt=0,acc=0
    // (Wardriver-Konvention, integration_design.md 6.1).
    String dt, lat, lon;
    float alt = 0.0f, acc = 0.0f;
    #ifdef HAS_GPS
        dt  = gps_obj.getDatetime();
        if (gps_obj.getFixStatus()) {
            lat = gps_obj.getLat();
            lon = gps_obj.getLon();
            alt = gps_obj.getAlt();
            acc = gps_obj.getAccuracy();
        } else {
            lat = "0";
            lon = "0";
        }
    #else
        dt  = "";
        lat = "0";
        lon = "0";
    #endif

    String wigle =
        bssid + "," +
        essid + "," +
        security + "," +
        dt + "," +
        channel + "," +
        rssi + "," +
        lat + "," +
        lon + "," +
        String(alt) + "," +
        String(acc) + "," +
        type + "\n";

    return wigle;
}

// ============================================================
// ESP-NOW-Recv-Callback (static, im WiFi-Task-Context)
// ============================================================

// Adapted from JCMK ESP32DualBandWardriver (WiFiOps.cpp:852-1019, CORE-Pfad),
// MIT License. Reduziert: hier wird NICHT direkt verarbeitet, sondern auf
// die Queue gelegt. Worker-Tick (runTick()) drain't und macht die teuere
// Logik (parse, GPS-Lookup, Buffer-Append) im Loop-Context.
//
// Begruendung: SD-I/O im RX-Callback ist Anti-Pattern — Latenz, Stack-Tiefe,
// Mutex-contention.
void WardriveCore::onDataRecv_static(const esp_now_recv_info_t* info,
                                     const uint8_t* data, int len) {
    if (!g_active_core) return;
    if (!info || !data) return;
    // Min-Length: Magic[4] + Type[1] = 5 Bytes.
    if (len < 5) {
        g_active_core->total_rx_bad++;
        return;
    }
    if (memcmp(data, ENOW_MAGIC, 4) != 0) {
        // Kein Marauder-CORE-Paket. Im normalen Marauder-Stack hat ESP-NOW
        // nichts zu suchen, daher als bad-packet zaehlen.
        g_active_core->total_rx_bad++;
        return;
    }
    const uint8_t msgType = data[4];

    // ADMIN ist Core->Node only. Wenn wir ADMIN empfangen, ist das ein
    // anderer Core in Reichweite — ignorieren statt enqueuen.
    if (msgType == MSG_ADMIN) {
        g_active_core->total_rx_bad++;
        return;
    }

    // Nur TEXT/HEARTBEAT/CORE_REQUEST gehen in die Queue.
    if (msgType != MSG_TEXT && msgType != MSG_HEARTBEAT && msgType != MSG_CORE_REQUEST) {
        g_active_core->total_rx_bad++;
        return;
    }

    // Volle Struct-Groesse muss vorhanden sein (Wardriver-Konvention:
    // immer sizeof(enow_text_msg_t) gesendet).
    if (len < (int)sizeof(enow_text_msg_t)) {
        g_active_core->total_rx_bad++;
        return;
    }

    WardriveCoreQueueMsg qmsg;
    memcpy(qmsg.src_mac, info->src_addr, 6);
    qmsg.rssi = (info->rx_ctrl) ? info->rx_ctrl->rssi : 0;
    qmsg.msg_type = msgType;
    memcpy(&qmsg.payload, data, sizeof(enow_text_msg_t));

    // Snapshot the queue handle once: deinit() may null rx_queue concurrently
    // from another task. Guard against a torn-down (or not-yet-created) queue so
    // we never call xQueueSend on an invalid handle.
    QueueHandle_t q = g_active_core->rx_queue;
    if (!q) return;
    // RX-Callback laeuft im WiFi-Task — wir nutzen FromISR-API NICHT direkt
    // (kein ISR), aber `xQueueSend` ist threadsafe und nicht-blocking mit
    // ticks_to_wait=0. Bei voll: drop. Counter wird erhoeht.
    if (xQueueSend(q, &qmsg, 0) != pdTRUE) {
        g_active_core->total_rx_drops++;
    }
}

void WardriveCore::updateLastRx(int slot, int8_t rssi) {
    last_rx_node_idx = slot;
    last_rx_ms = millis();
    last_rx_rssi = rssi;
    // Keep it per node too — the display shows a signal bar per row, not just
    // for whoever transmitted last.
    if (slot >= 0 && slot < WARDRIVE_CORE_MAX_NODES)
        node_table[slot].last_rssi = rssi;
}

// Roll the lines/min counters. Called from runTick; scales the delta since the
// last window up to a per-minute figure (window is 15 s by default).
void WardriveCore::updateRates(uint32_t now) {
    if (rate_window_ms == 0) {            // first call — establish the baseline
        rate_window_ms = now;
        rate_prev_total = total_rx_lines;
        for (uint8_t i = 0; i < WARDRIVE_CORE_MAX_NODES; i++)
            node_table[i].rate_prev_lines = node_table[i].rx_text_count;
        return;
    }
    uint32_t elapsed = now - rate_window_ms;
    if (elapsed < WARDRIVE_CORE_RATE_WINDOW_MS) return;

    const uint32_t scale = 60000UL / (elapsed ? elapsed : 1);
    uint32_t d = total_rx_lines - rate_prev_total;
    uint32_t per_min = d * scale;
    rate_lines_per_min = (per_min > 0xFFFF) ? 0xFFFF : (uint16_t)per_min;
    rate_prev_total = total_rx_lines;

    for (uint8_t i = 0; i < WARDRIVE_CORE_MAX_NODES; i++) {
        uint16_t cur = node_table[i].rx_text_count;
        uint16_t dn  = cur - node_table[i].rate_prev_lines;   // wraps safely
        uint32_t pm  = (uint32_t)dn * scale;
        node_table[i].rate_per_min = (pm > 255) ? 255 : (uint8_t)pm;
        node_table[i].rate_prev_lines = cur;
    }
    rate_window_ms = now;
}

// ============================================================
// Init / Deinit
// ============================================================

void WardriveCore::init() {
    if (is_running) {
        Serial.println("CORE: init() called but already running");
        return;
    }
    Serial.println("CORE: init() begin");

    // Step 1: WiFi-State.
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_promiscuous(false);

    // Step 2: Channel 6 fix.
    setFixedChannel(WARDRIVE_CORE_CHANNEL);

    // Step 3: PMK/LMK.
    // User-Key aus Marauder-Settings. Falls leer / Setting nicht vorhanden:
    // Plain-Mode (use_encryption=false). Wardriver-Konvention.
    user_key = "";
    // TODO: verify exact Settings-API. Marauder hat Settings::loadSetting<String>
    // mit fallback. Bei Fehlen wird leerer String erwartet. Wenn die API hier
    // anders ist (ggf. throws), Try/Catch oder default ergaenzen.
    user_key = settings_obj.loadSetting<String>("WardriveCoreKey");
    use_encryption = (user_key.length() > 0);
    if (use_encryption) {
        computeKeysFromUserKey();
    }

    // Step 4: ESP-NOW init.
    if (esp_now_init() != ESP_OK) {
        Serial.println("CORE: esp_now_init failed");
        WiFi.mode(WIFI_OFF);
        wifi_scan_obj.currentScanMode = WIFI_SCAN_OFF;
        return;
    }
    if (use_encryption) {
        if (esp_now_set_pmk(pmk) != ESP_OK) {
            Serial.println("CORE: esp_now_set_pmk failed (continuing)");
        }
    }
    // NOTE: the receive callback is registered LAST (Step 9b), only after the rx
    // queue and every counter it touches are initialized. Registering it here —
    // before the queue exists — let a node that was already broadcasting fire the
    // callback into a not-yet-created queue and crash the Marauder. That was the
    // "start nodes before Core Mode => crash" bug.

    // Step 5: GPS-Status check (no-fix ist OK, no-module ist Fehler — aber
    // `RunWardriveCore` checkt das schon vorher; hier nur defensive log).
    #ifdef HAS_GPS
        if (!gps_obj.getGpsModuleStatus()) {
            Serial.println("CORE: WARN — GPS module not detected, logging will use lat=0,lon=0");
        }
    #endif

    // Step 6: SD-File oeffnen via Marauder-Buffer.
    #ifdef HAS_SD
        if (sd_obj.supported) {
            wifi_scan_obj.startLog("wardrive_core");  // startLog takes const char* since v1.12.2
            buffer_obj.append(wifi_scan_obj.header_line);
        } else {
            Serial.println("CORE: WARN — SD not supported, logging disabled");
            sd_healthy = false;
        }
    #endif

    // Step 7: Queue erstellen.
    rx_queue = xQueueCreate(WARDRIVE_CORE_QUEUE_LEN, sizeof(WardriveCoreQueueMsg));
    if (!rx_queue) {
        Serial.println("CORE: queue create failed");
        // recv callback is not registered yet (Step 9b), so nothing to unregister.
        esp_now_deinit();
        WiFi.mode(WIFI_OFF);
        wifi_scan_obj.currentScanMode = WIFI_SCAN_OFF;
        return;
    }

    // Step 8: State init.
    memset(node_table, 0, sizeof(node_table));
    // Barrier: der CORE startet in der LOBBY (registrieren + zuweisen, aber
    // nicht sammeln). Erst startSession() schaltet auf Collecting.
    collecting = false;
    bcast_peer_ready = false;
    total_rx_lines = 0;
    total_rx_wifi = 0;
    total_rx_ble = 0;
    total_rx_bad = 0;
    total_rx_drops = 0;
    low_heap_events = 0;
    buffer_overruns = 0;
    last_rx_node_idx = -1;
    last_rx_ms = 0;
    last_rx_rssi = 0;
    rate_window_ms = 0;
    rate_prev_total = 0;
    rate_lines_per_min = 0;
    drawn_row_count = 0xFF;   // forces a full node-table repaint
    drawn_pitch = 0;
    memset(drawn_sig, 0xFF, sizeof(drawn_sig));
    assignment_version = 1;
    session_start_ms = millis();
    last_display_refresh_ms = 0;
    last_stale_check_ms = 0;
    last_heap_check_ms = 0;
    last_sd_check_ms = 0;
    center_press_start_ms = 0;
    center_was_pressed = false;

    // Step 9: Display init.
    drawCoreModeFrame();

    // Step 9b: Arm the ESP-NOW receive path LAST — only now that rx_queue exists
    // and every counter the callback touches has been zeroed. If a node is already
    // broadcasting when Core Mode starts, an incoming packet would otherwise hit
    // the callback before the queue exists and crash the Marauder. Registering
    // last is what makes "start nodes, then activate Core Mode" safe.
    g_active_core = this;
    if (esp_now_register_recv_cb(WardriveCore::onDataRecv_static) != ESP_OK) {
        Serial.println("CORE: esp_now_register_recv_cb failed");
        g_active_core = nullptr;
        vQueueDelete(rx_queue);
        rx_queue = nullptr;
        esp_now_deinit();
        WiFi.mode(WIFI_OFF);
        wifi_scan_obj.currentScanMode = WIFI_SCAN_OFF;
        return;
    }

    // Step 10: Running flag.
    is_running = true;

    Serial.printf("CORE: init() complete | encryption=%s | free_heap=%u\n",
                  use_encryption ? "ON" : "OFF",
                  ESP.getFreeHeap());
}

void WardriveCore::deinit() {
    if (!is_running) return;
    Serial.println("CORE: deinit() begin");

    // Disarm the receive path FIRST — stop new callbacks and make the instance
    // unreachable BEFORE tearing down the queue the callback writes into. Mirror
    // of init()'s "arm last": otherwise a node still transmitting during shutdown
    // could fire the callback into a freed queue.
    esp_now_unregister_recv_cb();
    g_active_core = nullptr;

    // Drain remaining Queue + flush.
    if (rx_queue) {
        WardriveCoreQueueMsg qmsg;
        while (xQueueReceive(rx_queue, &qmsg, 0) == pdTRUE) {
            if (qmsg.msg_type == MSG_TEXT) {
                String line = composeWigleLineFromNodeText(qmsg.payload);
                if (line.length() > 0) {
                    #ifdef HAS_SD
                        if (sd_healthy) buffer_obj.append(line);
                    #endif
                }
            }
        }
        vQueueDelete(rx_queue);
        rx_queue = nullptr;
    }
    // Force-Flush. Marauders loop() ruft buffer_obj.save() periodisch — wir
    // koennen aber nicht garantieren, dass bis zum naechsten Tick noch
    // alle Daten dranhaengen, daher hier nochmal explizit.
    buffer_obj.save();

    // ESP-NOW deinit. Recv callback was already unregistered and g_active_core
    // cleared at the top of deinit(), before the queue was freed.
    if (esp_now_is_peer_exist(BROADCAST_MAC_CORE)) {
        esp_now_del_peer(BROADCAST_MAC_CORE);
    }
    for (int i = 0; i < WARDRIVE_CORE_MAX_NODES; i++) {
        if (node_table[i].flags & NODE_FLAG_ACTIVE) {
            if (esp_now_is_peer_exist(node_table[i].mac)) {
                esp_now_del_peer(node_table[i].mac);
            }
        }
    }
    esp_now_deinit();

    // WiFi off.
    WiFi.mode(WIFI_OFF);

    // State reset.
    memset(node_table, 0, sizeof(node_table));
    is_running = false;

    // Display zurueck — Marauder-Menu zeichnet beim naechsten Tick.
    #ifdef HAS_SCREEN
        display_obj.clearScreen();
    #endif

    wifi_scan_obj.currentScanMode = WIFI_SCAN_OFF;

    Serial.println("CORE: deinit() complete");
}

// ============================================================
// runTick() — Main-Worker
// ============================================================

void WardriveCore::runTick(uint32_t currentTime) {
    if (!is_running) return;

    // 1) Drain Queue (max N pro Tick).
    if (rx_queue) {
        for (int i = 0; i < WARDRIVE_CORE_MAX_DRAIN_PER_TICK; i++) {
            WardriveCoreQueueMsg qmsg;
            if (xQueueReceive(rx_queue, &qmsg, 0) != pdTRUE) break;

            bool isNewNode = false;
            int slot = touchNode(qmsg.src_mac, isNewNode);

            if (slot < 0) {
                // Hard-Reject: Tabelle voll. Trotzdem wenn CORE_REQUEST,
                // KEIN Reply senden (sonst denkt Node sie ist gepaired,
                // bekommt aber nie Admin). Drop and move on.
                if (qmsg.msg_type == MSG_TEXT) {
                    // Wir koennten die Line trotzdem verarbeiten — sie ist
                    // ja inhaltlich nuetzlich. Wardriver-Verhalten: drop.
                    // Hier: drop-und-zaehlen.
                    total_rx_bad++;
                }
                continue;
            }

            // Re-Partition NUR in der Lobby. Waehrend COLLECTING ist die
            // Partition eingefroren — ein Spaet-Joiner registriert sich, bleibt
            // aber ohne Slice (idle) bis zum naechsten resyncSession().
            if (isNewNode && !collecting) {
                handleNodeTopologyChange();
            }

            switch (qmsg.msg_type) {
                case MSG_CORE_REQUEST: {
                    // Adapted from W:WiFiOps.cpp:872-911.
                    sendCoreReply(qmsg.src_mac);
                    if (use_encryption) {
                        if (addPeerWithMode(qmsg.src_mac, true, lmk)) {
                            node_table[slot].flags |= NODE_FLAG_ENCRYPTED;
                        }
                    }
                    if (node_table[slot].flags & NODE_FLAG_ADMIN_DIRTY) {
                        sendAdminToNodeSlot(slot, qmsg.src_mac);
                    }
                    break;
                }
                case MSG_HEARTBEAT: {
                    // Adapted from W:WiFiOps.cpp:913-936.
                    node_table[slot].hb_counter = qmsg.payload.counter;
                    if (node_table[slot].flags & NODE_FLAG_ADMIN_DIRTY) {
                        sendAdminToNodeSlot(slot, qmsg.src_mac);
                    }
                    break;
                }
                case MSG_TEXT: {
                    // Records nur im Collecting-State verarbeiten; in der Lobby
                    // senden Nodes ohnehin nichts (defensiv gegen verirrte Pakete).
                    if (!collecting) break;
                    // Adapted from W:WiFiOps.cpp:938-1013.
                    if (qmsg.payload.len > ENOW_TEXT_MAX) {
                        total_rx_bad++;
                        node_table[slot].rx_bad_count++;
                        break;
                    }
                    String wigle = composeWigleLineFromNodeText(qmsg.payload);
                    if (wigle.length() == 0) {
                        total_rx_bad++;
                        node_table[slot].rx_bad_count++;
                        break;
                    }
                    #ifdef HAS_SD
                        if (sd_healthy) {
                            buffer_obj.append(wigle);
                        }
                    #endif
                    total_rx_lines++;
                    node_table[slot].rx_text_count++;
                    // Type-Counter: parse-out aus Payload (letztes Feld nach ',').
                    // Bei voller Wigle-Line am ende: ",WIFI" oder ",BLE".
                    if (wigle.indexOf(",BLE") >= 0) {
                        total_rx_ble++;
                    } else {
                        total_rx_wifi++;
                    }
                    updateLastRx(slot, qmsg.rssi);
                    break;
                }
                default:
                    total_rx_bad++;
                    break;
            }
        }
    }

    // 2) Periodischer Stale-Cleanup (alle 1s).
    if (currentTime - last_stale_check_ms > WARDRIVE_CORE_STALE_CHECK_MS) {
        last_stale_check_ms = currentTime;
        // Tote Nodes immer aus der Tabelle raeumen; re-partitionieren aber nur
        // in der Lobby. Waehrend COLLECTING bleibt der Slice einer toten Node
        // dunkel bis zum naechsten resyncSession() (bewusste Entscheidung).
        if (removeStaleNodes() && !collecting) {
            handleNodeTopologyChange();
        }
    }

    // 3) Periodischer Heap-Check (Failure-Mode 6.10).
    if (currentTime - last_heap_check_ms > WARDRIVE_CORE_HEAP_CHECK_MS) {
        last_heap_check_ms = currentTime;
        if (ESP.getFreeHeap() < WARDRIVE_CORE_HEAP_MIN_RUNTIME) {
            low_heap_events++;
            Serial.printf("CORE: LOW HEAP %u\n", ESP.getFreeHeap());
        }
    }

    // 4) Periodischer SD-Check (Failure-Modes 6.3 + 6.4).
    #ifdef HAS_SD
        if (currentTime - last_sd_check_ms > WARDRIVE_CORE_SD_CHECK_MS) {
            last_sd_check_ms = currentTime;
            if (sd_obj.supported) {
                // Frei-Space-Check. Default-Marauder-API exposed kein
                // direktes "totalBytes/usedBytes"-Wrapper — fall-back via
                // SD-Lib direkt.
                uint64_t total = SD.totalBytes();
                uint64_t used  = SD.usedBytes();
                if (total > 0 && (total - used) < (1ULL * 1024 * 1024)) {
                    sd_healthy = false;
                    Serial.println("CORE: SD < 1MB free, stop logging");
                }
            }
        }
    #endif

    // 5) Periodischer Display-Refresh (Raten vorher rollen, damit die Anzeige
    //    frische lines/min sieht).
    updateRates(currentTime);
    if (currentTime - last_display_refresh_ms > WARDRIVE_CORE_DISPLAY_REFRESH_MS) {
        last_display_refresh_ms = currentTime;
        refreshCoreDisplay();
    }

    // 6) Center-Long-Press-Detection fuer Exit.
    #ifdef HAS_BUTTONS
      #if (C_BTN >= 0) && !defined(MARAUDER_CARDPUTER) && !defined(MARAUDER_CARDPUTER_ADV)
        // Marauders Switches-Wrapper hat keinen direkten "isHeld(ms)"-Check,
        // wir tracken pressed/released selbst. `c_btn.justPressed()` ist
        // edge-trigger; fuer hold-Detection koennten wir auch `c_btn.justReleased()`
        // nutzen, aber wir wollen WAEHREND des Holds erkennen. Fallback:
        // direkter `digitalRead(C_BTN)` mit invertierter Pull-Up-Logik.
        // TODO: verify Switches-API — ob es eine "isPressed"-Polling-Methode
        // gibt. Bis dahin: digitalRead-Fallback.
        bool pressed_now = (digitalRead(C_BTN) == LOW); // active-low PULLUP
        if (pressed_now) {
            if (!center_was_pressed) {
                center_was_pressed = true;
                center_press_start_ms = currentTime;
            } else {
                if ((currentTime - center_press_start_ms) >= WARDRIVE_CORE_EXIT_HOLD_MS) {
                    Serial.println("CORE: Center long-press -> exit");
                    deinit();
                    return;
                }
            }
        } else {
            center_was_pressed = false;
            center_press_start_ms = 0;
        }
      #endif
    #endif

    // 7) Buffer::save() ruft Marauder-loop() ohnehin (esp32_marauder.ino).
    //    Kein eigener Aufruf hier.
}

// ============================================================
// Display
// ============================================================

// ============================================================
// Display — warroom-rig cluster view
// ============================================================
//
// Continues the home console's visual language (bronze header bar, clan gold,
// dark panels) instead of a flat text dump: a hero tile carries the two numbers
// that matter (WiFi / BLE), each node gets a panel row with a status stripe and
// a signal-bar RSSI. Row density adapts: roomy for a small fleet, compact once
// more than six nodes are registered (up to WARDRIVE_CORE_MAX_NODES).

static const uint16_t WC_GOLD   = 0xEDA9;  // clan gold
static const uint16_t WC_INK    = 0xEF3B;  // warm off-white
static const uint16_t WC_DIM    = 0x8C0E;  // muted label
static const uint16_t WC_DIM2   = 0x5AC9;  // faint hint
static const uint16_t WC_PANEL  = 0x1081;  // panel fill
static const uint16_t WC_PANEL3 = 0x2902;  // panel outline
static const uint16_t WC_GREEN  = 0x6E6D;
static const uint16_t WC_AMBER  = 0xFD20;

// Layout, 240x320 portrait.
static const int WC_HERO_Y   = 25;
static const int WC_HERO_H   = 48;
static const int WC_ROWS_Y   = 92;
static const int WC_ROWS_END = 272;   // rows must stop before the touch bar (274)

// Column origins, shared by the header labels and the rows so they can't drift.
static const int WC_X_NODE = 12, WC_X_SLICE = 58, WC_X_LINES = 128;
static const int WC_X_RATE = 168, WC_X_SIG = 200, WC_X_RSSI = 218;

// Compact number so a column can never overflow into its neighbour.
static void wcFmt(char* out, size_t n, uint32_t v) {
    if (v < 10000) snprintf(out, n, "%lu", (unsigned long)v);
    else           snprintf(out, n, "%luk", (unsigned long)(v / 1000));
}

// Four rising signal bars, lit according to RSSI. 0 = never heard -> all dim.
static void wcBars(int x, int y, int8_t rssi, uint16_t col, uint16_t bg) {
    int lvl = 0;
    if (rssi != 0) {
        if      (rssi >= -55) lvl = 4;
        else if (rssi >= -68) lvl = 3;
        else if (rssi >= -78) lvl = 2;
        else                  lvl = 1;
    }
    display_obj.tft.fillRect(x, y, 15, 10, bg);
    for (int i = 0; i < 4; i++) {
        int bh = 3 + i * 2;
        display_obj.tft.fillRect(x + i * 4, y + 10 - bh, 3, bh, (i < lvl) ? col : WC_DIM2);
    }
}

// Dynamic part of the bronze header: satellites + battery. The wordmark itself
// is static and painted once by drawCoreModeFrame.
void WardriveCore::drawRigBar() {
    #ifdef HAS_SCREEN
        auto &tft = display_obj.tft;
        tft.setTextSize(1);

        bool mod = false, fix = false; uint8_t sats = 0;
        #ifdef HAS_GPS
            mod  = gps_obj.getGpsModuleStatus();
            fix  = gps_obj.getFixStatus();
            sats = gps_obj.getNumSats();
        #endif
        // Honest GPS: module presence is NOT a fix.
        uint16_t scol = (!mod) ? WC_DIM2 : (fix ? WC_GREEN : WC_AMBER);
        char sb[16];
        if      (!mod) snprintf(sb, sizeof(sb), "NO GPS");
        else if (fix)  snprintf(sb, sizeof(sb), "FIX %-2u", (unsigned)sats);
        else           snprintf(sb, sizeof(sb), "ACQ %-2u", (unsigned)sats);
        tft.setTextColor(scol, STATUSBAR_COLOR);
        tft.setCursor(132, 7);
        tft.print(sb);

        uint8_t bl = battery_obj.battery_level;
        uint16_t bcol = (bl >= 40) ? WC_GREEN : ((bl >= 20) ? WC_AMBER : TFT_RED);
        char bb[8];
        snprintf(bb, sizeof(bb), "%3u%%", (unsigned)bl);
        tft.setTextColor(bcol, STATUSBAR_COLOR);
        tft.setCursor(204, 7);
        tft.print(bb);
    #endif
}

void WardriveCore::drawCoreModeFrame() {
    #ifdef HAS_SCREEN
        auto &tft = display_obj.tft;
        display_obj.clearScreen();

        // Bronze header + wordmark (static; sats/battery are refreshed).
        tft.fillRect(0, 0, TFT_WIDTH, 22, STATUSBAR_COLOR);
        tft.setTextSize(1);
        tft.setTextColor(WC_INK, STATUSBAR_COLOR);
        tft.setCursor(6, 7);
        tft.print("WARROOM RIG");

        // Hero tile with a gold spine + the static metric labels.
        tft.fillRoundRect(4, WC_HERO_Y, TFT_WIDTH - 8, WC_HERO_H, 4, WC_PANEL);
        tft.drawRoundRect(4, WC_HERO_Y, TFT_WIDTH - 8, WC_HERO_H, 4, WC_PANEL3);
        tft.fillRect(4, WC_HERO_Y + 2, 3, WC_HERO_H - 4, WC_GOLD);
        tft.setTextColor(WC_DIM, WC_PANEL);
        tft.setCursor(14, WC_HERO_Y + 4);  tft.print("WIFI");
        tft.setCursor(96, WC_HERO_Y + 4);  tft.print("BLE");
        tft.setCursor(166, WC_HERO_Y + 4); tft.print("LINES");

        // Column header for the node table.
        tft.setTextColor(WC_DIM2, TFT_BLACK);
        tft.setCursor(10,         78); tft.print("NODE");
        tft.setCursor(WC_X_SLICE, 78); tft.print("SLICE");
        tft.setCursor(WC_X_LINES, 78); tft.print("LINES");
        tft.setCursor(WC_X_RATE,  78); tft.print("RATE");
        tft.setCursor(WC_X_SIG+2, 78); tft.print("SIG");
        tft.drawFastHLine(4, 88, TFT_WIDTH - 8, WC_PANEL3);

        drawn_row_count = 0xFF;   // force a full row repaint on the next refresh

        #ifdef HAS_TOUCH
            this->drawTouchControls();
        #else
            tft.setTextColor(WC_DIM2, TFT_BLACK);
            tft.setCursor(6, 306);
            tft.print("R: session   C hold: exit");
        #endif
    #endif
}

void WardriveCore::refreshCoreDisplay() {
    #ifdef HAS_SCREEN
        auto &tft = display_obj.tft;
        char buf[24];

        drawRigBar();

        // ---- hero: WiFi / BLE are the headline pair, lines the third value ----
        tft.setTextSize(3);
        tft.setTextColor(collecting ? WC_GOLD : WC_DIM2, WC_PANEL);
        wcFmt(buf, sizeof(buf), total_rx_wifi);
        tft.setCursor(14, WC_HERO_Y + 12); tft.printf("%-4s", buf);
        wcFmt(buf, sizeof(buf), total_rx_ble);
        tft.setCursor(96, WC_HERO_Y + 12); tft.printf("%-3s", buf);

        tft.setTextSize(2);
        tft.setTextColor(collecting ? WC_INK : WC_DIM2, WC_PANEL);
        wcFmt(buf, sizeof(buf), total_rx_lines);
        tft.setCursor(166, WC_HERO_Y + 16); tft.printf("%-4s", buf);

        // Status strip along the bottom of the hero tile.
        tft.setTextSize(1);
        tft.setTextColor(collecting ? WC_GREEN : WC_AMBER, WC_PANEL);
        tft.setCursor(14, WC_HERO_Y + 37);
        tft.print(collecting ? "LIVE " : "LOBBY");
        tft.setTextColor(WC_DIM, WC_PANEL);
        tft.setCursor(52, WC_HERO_Y + 37);
        if (collecting) snprintf(buf, sizeof(buf), "%u/min   ", (unsigned)rate_lines_per_min);
        else            snprintf(buf, sizeof(buf), "idle     ");
        tft.print(buf);
        tft.setCursor(116, WC_HERO_Y + 37);
        snprintf(buf, sizeof(buf), "n %u/%u  ch %u   ",
                 (unsigned)getActiveNodeCount(),
                 (unsigned)WARDRIVE_CORE_MAX_NODES,
                 (unsigned)WARDRIVE_CORE_CHANNEL);
        tft.print(buf);

        // ---- node rows ----
        // Compact the active slots so the table shows no gaps after a dropout.
        uint8_t slots[WARDRIVE_CORE_MAX_NODES];
        uint8_t n = 0;
        for (uint8_t i = 0; i < WARDRIVE_CORE_MAX_NODES; i++)
            if (node_table[i].flags & NODE_FLAG_ACTIVE) slots[n++] = i;

        const int pitch = (n <= 6) ? 24 : 15;
        const int rh    = pitch - 3;

        // Relayout only when the fleet size (and thus the density) changes.
        if (n != drawn_row_count || pitch != drawn_pitch) {
            tft.fillRect(0, WC_ROWS_Y, TFT_WIDTH, WC_ROWS_END - WC_ROWS_Y, TFT_BLACK);
            drawn_row_count = n;
            drawn_pitch     = pitch;
            for (uint8_t k = 0; k < WARDRIVE_CORE_MAX_NODES; k++) drawn_sig[k] = 0xFFFF;
            if (n == 0) {
                tft.setTextSize(1);
                tft.setTextColor(WC_DIM2, TFT_BLACK);
                tft.setCursor(12, WC_ROWS_Y + 6);
                tft.print("waiting for nodes...");
            }
        }

        for (uint8_t k = 0; k < n; k++) {
            NodeRecord &nr = node_table[slots[k]];
            const int y  = WC_ROWS_Y + k * pitch;
            const int ty = y + (rh - 8) / 2;
            // Amber once a node is more than half its timeout quiet.
            const bool stale = ((millis() - nr.last_seen_ms) > (WARDRIVE_CORE_NODE_TIMEOUT_MS / 2));
            const uint16_t col   = stale ? WC_AMBER : WC_GREEN;
            const uint16_t rowbg = (k % 2 == 0) ? WC_PANEL : TFT_BLACK;

            // Row chrome (zebra panel, status stripe, node id) only when the row's
            // identity changes — repainting it every 500 ms would flicker.
            const uint16_t sig = (uint16_t)(nr.mac_suffix ^ (stale ? 0x8000 : 0x0000));
            if (drawn_sig[k] != sig) {
                drawn_sig[k] = sig;
                if (rowbg != TFT_BLACK) tft.fillRoundRect(4, y, TFT_WIDTH - 8, rh, 3, rowbg);
                else                    tft.fillRect(4, y, TFT_WIDTH - 8, rh, TFT_BLACK);
                tft.fillRect(4, y, 3, rh, col);
                tft.setTextSize(1);
                tft.setTextColor(WC_INK, rowbg);
                tft.setCursor(WC_X_NODE, ty);
                tft.printf("%04X", nr.mac_suffix);
            }

            tft.setTextSize(1);

            // Assigned channel slice, in gold, plus a BLE marker on the node that
            // also runs the BLE scanner. No protocol field needed: the node derives
            // its BLE role from the admin packet WE send —
            //   ble_host = (node_count <= 1) || (node_index == node_count - 1)
            // (WiFiOps.cpp) — so mirroring that predicate here is exact. It can lag
            // by one admin round right after a topology change, until the new
            // assignment reaches the node.
            const bool ble_host = (n <= 1) || (nr.assigned_index == n - 1);
            tft.setTextColor(WC_GOLD, rowbg);
            tft.setCursor(WC_X_SLICE, ty);
            if (nr.start_channel_idx < NUM_SCAN_CHANNELS &&
                nr.end_channel_idx   < NUM_SCAN_CHANNELS)
                snprintf(buf, sizeof(buf), "%u-%u%s",
                         (unsigned)scan_channels[nr.start_channel_idx],
                         (unsigned)scan_channels[nr.end_channel_idx],
                         ble_host ? " BLE" : "");
            else
                snprintf(buf, sizeof(buf), "%s", ble_host ? "BLE" : "-");
            // Pad to the column width so a shorter value overwrites the old tail.
            for (size_t p = strlen(buf); p < 10 && p < sizeof(buf) - 1; p++) buf[p] = ' ';
            buf[10] = '\0';
            tft.print(buf);

            // Wigle lines contributed by this node.
            tft.setTextColor(WC_INK, rowbg);
            tft.setCursor(WC_X_LINES, ty);
            wcFmt(buf, sizeof(buf), nr.rx_text_count);
            tft.printf("%-5s", buf);

            // Rolling throughput.
            if (nr.rate_per_min) {
                tft.setTextColor(WC_GREEN, rowbg);
                snprintf(buf, sizeof(buf), "%u/m ", (unsigned)nr.rate_per_min);
            } else {
                tft.setTextColor(WC_DIM2, rowbg);
                snprintf(buf, sizeof(buf), "-    ");
            }
            tft.setCursor(WC_X_RATE, ty);
            tft.print(buf);

            // Link quality: bars + the raw value.
            wcBars(WC_X_SIG, y + (rh - 10) / 2, nr.last_rssi, col, rowbg);
            tft.setTextColor(WC_DIM, rowbg);
            tft.setCursor(WC_X_RSSI, ty);
            if (nr.last_rssi) tft.printf("%-3d", (int)nr.last_rssi);
            else              tft.print("   ");
        }
    #endif
}

#ifdef HAS_TOUCH
// ============================================================
// Touch session controls (Marauder V8)
// ============================================================
//
// Geometry of the on-screen button bar. It occupies the bottom band that the
// "Slots:" telemetry line uses on button boards (suppressed under HAS_TOUCH in
// refreshCoreDisplay). Two states:
//   LOBBY -> [   START   ] [EXIT]
//   LIVE  -> [SYNC][STOP] [EXIT]
static const int TC_BAR_Y    = 274;   // button bar top
static const int TC_BAR_H    = 40;    // button height (274..314)
static const int TC_EXIT_X   = 182;   // Exit: 182..234
static const int TC_EXIT_W   = 52;
static const int TC_START_X  = 6;     // Lobby Start: 6..176 (wide)
static const int TC_START_W  = 170;
static const int TC_SYNC_X   = 6;     // Live Re-Sync: 6..88
static const int TC_SYNC_W   = 82;
static const int TC_STOP_X   = 92;    // Live Stop: 92..176
static const int TC_STOP_W   = 84;

static void tcDrawButton(int x, int y, int w, int h, const char* label,
                         uint16_t fill, uint16_t border) {
    display_obj.tft.fillRoundRect(x, y, w, h, 5, fill);
    display_obj.tft.drawRoundRect(x, y, w, h, 5, border);
    display_obj.tft.setTextSize(2);
    display_obj.tft.setTextColor(border, fill);
    int tw = (int)strlen(label) * 12;               // 6px glyph * size 2
    int tx = x + (w - tw) / 2; if (tx < x + 3) tx = x + 3;
    int ty = y + (h - 16) / 2;                       // 8px glyph * size 2
    display_obj.tft.setCursor(tx, ty);
    display_obj.tft.print(label);
}

void WardriveCore::drawTouchControls() {
    #ifdef HAS_SCREEN
        // Clear the whole band first so a Live<->Lobby switch leaves no stale glyphs.
        display_obj.tft.fillRect(0, TC_BAR_Y - 2, TFT_WIDTH, TC_BAR_H + 6, TFT_BLACK);
        tcDrawButton(TC_EXIT_X, TC_BAR_Y, TC_EXIT_W, TC_BAR_H, "EXIT",
                     TFT_DARKGREY, TFT_WHITE);
        if (collecting) {
            tcDrawButton(TC_SYNC_X, TC_BAR_Y, TC_SYNC_W, TC_BAR_H, "SYNC",
                         TFT_NAVY, TFT_CYAN);
            tcDrawButton(TC_STOP_X, TC_BAR_Y, TC_STOP_W, TC_BAR_H, "STOP",
                         TFT_MAROON, TFT_RED);
        } else {
            tcDrawButton(TC_START_X, TC_BAR_Y, TC_START_W, TC_BAR_H, "START",
                         TFT_DARKGREEN, TFT_GREEN);
        }
    #endif
}

bool WardriveCore::handleTouch(uint16_t x, uint16_t y) {
    // Only taps inside the button band count; anything else is ignored (returns
    // false, tap consumed by caller without exiting).
    if ((int)y < TC_BAR_Y - 4 || (int)y > TC_BAR_Y + TC_BAR_H + 4) return false;

    #define TC_IN_X(bx, bw) ((int)x >= (bx) && (int)x <= (bx) + (bw))
    if (TC_IN_X(TC_EXIT_X, TC_EXIT_W)) return true;   // caller stops the scan

    if (collecting) {
        if      (TC_IN_X(TC_SYNC_X, TC_SYNC_W)) resyncSession();
        else if (TC_IN_X(TC_STOP_X, TC_STOP_W)) stopSession();
    } else {
        if      (TC_IN_X(TC_START_X, TC_START_W)) startSession();
    }
    #undef TC_IN_X

    // Session state may have flipped -> redraw the button set. The STATE line
    // updates on the next refreshCoreDisplay tick (<=500ms).
    drawTouchControls();
    return false;
}
#endif // HAS_TOUCH

#endif // MARAUDER_CORE_MODE
