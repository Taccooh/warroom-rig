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
#include "RigInput.h"
#include "RigTheme.h"

#ifdef MARAUDER_CORE_MODE

#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_random.h"
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
  #ifdef RIG_HAS_NAV
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

// Callbacks currently inside onDataRecv_static(). Clearing g_active_core does
// NOT stop a callback that has already passed the null check and is midway
// through touching the instance and its queue -- and ESP-NOW gives no promise
// that esp_now_unregister_recv_cb() waits for one either. deinit() spins on
// this before it frees anything the callback can still reach.
//
// Single writer: ESP-NOW dispatches receive callbacks from one task, so the
// increment/decrement pair is never interleaved with itself. deinit() only
// reads it.
static volatile uint32_t g_rx_cb_depth = 0;

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
    partition_node_count = 0;   // no partition until nodes register
    ble_host_slot = 0xFF;       // nobody elected yet
    session_epoch = 0;          // no session has started
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
    last_session_beacon_ms = 0;
    center_press_start_ms = 0;
    center_was_pressed = false;
    back_was_pressed = false;
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
//
// Reserved slots match too: a node that went quiet during a session keeps its
// place in the frozen partition, and this is what hands it back.
int WardriveCore::findNodeByMac(const uint8_t* mac) {
    for (int i = 0; i < WARDRIVE_CORE_MAX_NODES; i++) {
        if ((node_table[i].flags & (NODE_FLAG_ACTIVE | NODE_FLAG_RESERVED)) &&
            memcmp(node_table[i].mac, mac, 6) == 0) {
            return i;
        }
    }
    return -1;
}

int WardriveCore::allocateNodeSlot(const uint8_t* mac) {
    uint16_t suffix = macToSuffix(mac);
    int reclaim = -1;
    uint32_t reclaim_age = 0;

    for (int i = 0; i < WARDRIVE_CORE_MAX_NODES; i++) {
        if (node_table[i].flags & (NODE_FLAG_ACTIVE | NODE_FLAG_RESERVED)) {
            // A reserved slot is held for a node that may still come back, but
            // an unknown node in front of us is here NOW. If nothing is free,
            // the one that has been away longest gives up its place.
            if (!(node_table[i].flags & NODE_FLAG_ACTIVE)) {
                uint32_t age = millis() - node_table[i].last_seen_ms;
                if (reclaim < 0 || age > reclaim_age) { reclaim = i; reclaim_age = age; }
            }
            continue;
        }
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

    if (reclaim >= 0) {
        Serial.printf("CORE: reclaiming reserved slot %d (suffix %04X, away %lus)\n",
                      reclaim, node_table[reclaim].mac_suffix,
                      (unsigned long)(reclaim_age / 1000));
        memset(&node_table[reclaim], 0, sizeof(NodeRecord));
        memcpy(node_table[reclaim].mac, mac, 6);
        node_table[reclaim].mac_suffix = suffix;
        node_table[reclaim].last_seen_ms = millis();
        node_table[reclaim].end_channel_idx = NUM_SCAN_CHANNELS - 1;
        node_table[reclaim].flags = NODE_FLAG_ACTIVE | NODE_FLAG_ADMIN_DIRTY;
        return reclaim;
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
        if (!(node_table[slot].flags & NODE_FLAG_ACTIVE)) {
            // Back from a dropout, into the slice it already had. It is not a
            // new node -- the partition never lost it -- but everything we knew
            // about its state is stale: it may have rebooted, and it certainly
            // missed whatever we broadcast while it was away.
            node_table[slot].flags &= ~(NODE_FLAG_RESERVED | NODE_FLAG_ENCRYPTED |
                                        NODE_FLAG_REPORTS_STATUS);
            node_table[slot].flags |= NODE_FLAG_ACTIVE | NODE_FLAG_ADMIN_DIRTY;
            node_table[slot].admin_confirmed_version = 0;
            node_table[slot].admin_ok_streak = 0;
            node_table[slot].admin_last_send_ms = 0;   // re-arm immediately
            Serial.printf("CORE: Slot %d back (suffix %04X), re-arming\n",
                          slot, node_table[slot].mac_suffix);
        }
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
                if (collecting && (node_table[i].flags & NODE_FLAG_PARTITIONED)) {
                    // Hold the place instead of freeing it. The partition is
                    // frozen during a session, so a node whose slot is wiped
                    // comes back as a late joiner with no slice at all and
                    // contributes nothing for the rest of the drive -- for a
                    // reboot or a minute in a radio shadow. Reserved keeps the
                    // MAC and the slice; touchNode() hands both straight back.
                    node_table[i].flags &= ~(NODE_FLAG_ACTIVE | NODE_FLAG_ENCRYPTED |
                                             NODE_FLAG_REPORTS_STATUS);
                    node_table[i].flags |= NODE_FLAG_RESERVED;
                    Serial.printf("CORE: Slot %d quiet, holding its slice (suffix %04X)\n",
                                  i, node_table[i].mac_suffix);
                } else {
                    Serial.printf("CORE: Slot %d stale, dropping (suffix %04X)\n",
                                  i, node_table[i].mac_suffix);
                    memset(&node_table[i], 0, sizeof(NodeRecord));
                    changed = true;
                }
            }
        }
    }
    return changed;
}

// Adapted from JCMK ESP32DualBandWardriver (WiFiOps.cpp:477-502), MIT License
void WardriveCore::recalculateChannelAssignments() {
    uint8_t active_slots[WARDRIVE_CORE_MAX_NODES];
    uint8_t active_count = 0;
    uint8_t narrow_count = 0;   // nodes whose radio cannot tune 5 GHz

    // 2.4-GHz-only nodes come first, so their slices land at the bottom of
    // scan_channels[] where the channels they can actually reach live. This
    // also keeps the BLE host (highest index) on a dual-band node, which is
    // where it belongs -- see the BLE comment in the node's scan loop.
    for (uint8_t i = 0; i < WARDRIVE_CORE_MAX_NODES; i++) {
        if ((node_table[i].flags & NODE_FLAG_ACTIVE) &&
            (node_table[i].flags & NODE_FLAG_2G4_ONLY)) {
            active_slots[active_count++] = i;
            narrow_count++;
        }
    }
    for (uint8_t i = 0; i < WARDRIVE_CORE_MAX_NODES; i++) {
        if ((node_table[i].flags & NODE_FLAG_ACTIVE) &&
            !(node_table[i].flags & NODE_FLAG_2G4_ONLY)) {
            active_slots[active_count++] = i;
        }
    }
    if (active_count == 0) return;

    // Freeze the fleet size with the indices it belongs to. Everything that
    // needs "how many nodes is this partition for" must read this, not a live
    // count -- see the member's declaration.
    partition_node_count = active_count;

#ifdef WARDRIVE_2_4_ONLY
    // 2.4-GHz-only Fleet: nur Index 0..13 verteilen, 5-GHz-Slots bleiben unbesetzt.
    // Sanity: die ersten 14 Eintraege in scan_channels[] sind die 2.4-GHz-Channels.
    static_assert(WARDRIVE_2_4_CHANNEL_COUNT <= NUM_SCAN_CHANNELS,
                  "WARDRIVE_2_4_CHANNEL_COUNT exceeds scan_channels[] length");
    const uint8_t homogeneous_pool = WARDRIVE_2_4_CHANNEL_COUNT;
    const bool    mixed_fleet = false;
#else
    // A slice was an even cut of 0..39 with no idea what the receiving radio
    // can do. Hand an all-5-GHz cut to a classic ESP32 and it scans nothing,
    // completes no cycle and -- because the heartbeat used to ride the end of a
    // cycle -- checks in never again. Splitting per band means every node gets
    // channels it can tune, and the split is still a contiguous range, which is
    // all the wire format can express.
    const bool    mixed_fleet = (narrow_count > 0) && (narrow_count < active_count);
    const uint8_t homogeneous_pool = (narrow_count == 0) ? NUM_SCAN_CHANNELS
                                                         : (WARDRIVE_CORE_2G4_END_IDX + 1);
#endif

    for (uint8_t node_num = 0; node_num < active_count; node_num++) {
        uint8_t slot = active_slots[node_num];

        int pool_first, pool_len, k, k_count;
        if (mixed_fleet && node_num < narrow_count) {
            pool_first = 0;
            pool_len   = WARDRIVE_CORE_2G4_END_IDX + 1;
            k          = node_num;
            k_count    = narrow_count;
        } else if (mixed_fleet) {
            pool_first = WARDRIVE_CORE_2G4_END_IDX + 1;
            pool_len   = NUM_SCAN_CHANNELS - (WARDRIVE_CORE_2G4_END_IDX + 1);
            k          = node_num - narrow_count;
            k_count    = active_count - narrow_count;
        } else {
            pool_first = 0;
            pool_len   = homogeneous_pool;
            k          = node_num;
            k_count    = active_count;
        }

        int start_idx = pool_first + (k * pool_len) / k_count;
        int end_idx   = pool_first + ((k + 1) * pool_len) / k_count - 1;
        // More nodes in a group than channels in its band: overlap rather than
        // hand out an empty range, which the node would read as start > end and
        // fall back to scanning everything.
        if (end_idx < start_idx) end_idx = start_idx;

        node_table[slot].assigned_index = node_num;
        node_table[slot].start_channel_idx = (uint8_t)start_idx;
        node_table[slot].end_channel_idx = (uint8_t)end_idx;
        node_table[slot].flags |= NODE_FLAG_ADMIN_DIRTY | NODE_FLAG_PARTITIONED;
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
    // Reserved slots belong to the partition that is about to be replaced.
    // Holding them past that point would keep a dead node's index alive in a
    // fleet it is no longer counted in.
    for (uint8_t i = 0; i < WARDRIVE_CORE_MAX_NODES; i++) {
        if (!(node_table[i].flags & NODE_FLAG_ACTIVE) &&
            (node_table[i].flags & NODE_FLAG_RESERVED)) {
            memset(&node_table[i], 0, sizeof(NodeRecord));
        }
    }
    recalculateChannelAssignments();
    markAllActiveNodesAdminDirty();
    // Indices just moved, so the host may have too. Everyone is dirty already;
    // this only keeps ble_host_slot from lagging a partition behind.
    refreshBleHostElection();
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
bool WardriveCore::broadcastSession(uint8_t command, uint8_t repeats) {
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
    for (uint8_t i = 0; i < repeats; i++) {       // Nx fuer Broadcast-Reliability
        if (esp_now_send(bcast, (uint8_t*)&msg, sizeof(msg)) == ESP_OK) ok = true;
        if (i + 1 < repeats) delay(15);
    }
    return ok;
}

// Open the log on first demand — i.e. when a session actually starts.
//
// One Rig Mode visit yields at most one file, not one per start/stop cycle:
// stopping does not close it. That is deliberate. Lines sit in the Marauder
// buffer until buffer_obj.save() runs in the main loop, so swapping the target
// file at stop time would file the tail of a session under the next one. A
// single file per visit costs nothing on upload and cannot mis-attribute rows.
void WardriveCore::ensureLogOpen() {
    #ifdef HAS_SD
        if (log_open || !sd_healthy || !sd_obj.supported) return;
        wifi_scan_obj.startLog("wardrive_core");   // const char* since v1.12.2
        buffer_obj.append(wifi_scan_obj.header_line);
        log_open = true;
        Serial.println("CORE: log opened for this session");
    #endif
}

// After a session edge every node's known state is a guess again: the broadcast
// is unacknowledged, and a node hopping channels is unlikely to be listening
// when it goes out. Clearing the send timestamps means the very next check-in
// from each node carries the new session command in its admin packet, instead
// of waiting out the keepalive interval.
void WardriveCore::rearmAllNodesOnSessionChange() {
    for (uint8_t i = 0; i < WARDRIVE_CORE_MAX_NODES; i++) {
        if (node_table[i].flags & NODE_FLAG_ACTIVE)
            node_table[i].admin_last_send_ms = 0;
    }
}

// One drive. Bumping the epoch is what tells every node to empty its dedup ring
// -- the session command cannot, because the keepalive admin packets repeat
// START for the whole session and a ring cleared every few seconds is no ring at
// all. Skipping 0 keeps it distinct from a node's "no CORE has told me yet".
void WardriveCore::bumpSessionEpoch() {
    session_epoch++;
    if (session_epoch == 0) session_epoch = 1;
}

void WardriveCore::startSession() {
    if (!is_running) return;
    ensureLogOpen();
    const bool was_collecting = collecting;
    collecting = true;
    // A drive begins where collecting begins, and nowhere else. Pressing Start
    // on a session that is already running must not wipe the fleet's dedup
    // rings: every AP still in range would be reported again.
    if (!was_collecting) bumpSessionEpoch();
    refreshBleHostElection();
    Serial.printf("CORE: SESSION START (%u nodes, epoch %u)\n",
                  getActiveNodeCount(), (unsigned)session_epoch);
    // No broadcast START, deliberately. A broadcast says "collect" to every node
    // in earshot, including one still holding a stale assignment from an earlier
    // partition: it sweeps the wrong slice, and if its old index happened to be
    // the last one it runs BLE as a second host. STOP is safe to shout at
    // everyone, START never is -- it only means anything next to the slice it
    // applies to, so it travels in the unicast admin packet instead. The rearm
    // below puts that on the wire at the node's next check-in, which is at most
    // LOBBY_HB_INTERVAL_MS (1.5 s) away.
    rearmAllNodesOnSessionChange();
    last_session_beacon_ms = millis();
}

void WardriveCore::stopSession() {
    if (!is_running) return;
    collecting = false;
    Serial.println("CORE: SESSION STOP -> lobby");
    broadcastSession(SESSION_CMD_STOP);
    rearmAllNodesOnSessionChange();
    last_session_beacon_ms = millis();
}

void WardriveCore::resyncSession() {
    if (!is_running) return;
    // Alle aktuell registrierten Nodes (inkl. Spaet-Joiner) frisch
    // partitionieren; das neue Admin geht per Check-in raus, START breit.
    handleNodeTopologyChange();
    ensureLogOpen();   // resync can be the first thing that starts collecting
    const bool was_collecting = collecting;
    collecting = true;
    // Same rule as startSession(): only a re-sync that is itself the start of
    // the drive counts as a new session. A mid-drive re-sync re-cuts the
    // channels and must leave what the fleet has already filed alone.
    if (!was_collecting) bumpSessionEpoch();
    refreshBleHostElection();
    Serial.printf("CORE: SESSION RESYNC + START (%u nodes, epoch %u)\n",
                  getActiveNodeCount(), (unsigned)session_epoch);
    // Per-node only -- see startSession() for why START is never broadcast.
    // Doubly true here: a re-sync exists precisely because the partition just
    // changed, so every node's held assignment is suspect until it checks in.
    rearmAllNodesOnSessionChange();
    last_session_beacon_ms = millis();
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

// What a node should be doing, given the partition as it stands. A node that
// is registered but has no slice is told to stay in the lobby even in the
// middle of a session: there is nothing for it to sweep, and letting it run on
// its allocation defaults means it sweeps everyone else's channels.
uint8_t WardriveCore::desiredSessionFor(uint8_t slot) const {
    if (slot >= WARDRIVE_CORE_MAX_NODES) return SESSION_CMD_STOP;
    if (!collecting) return SESSION_CMD_STOP;
    if (!(node_table[slot].flags & NODE_FLAG_PARTITIONED)) return SESSION_CMD_STOP;
    return SESSION_CMD_START;
}

// Adapted from JCMK ESP32DualBandWardriver (WiFiOps.cpp:614-654), MIT License.
// Admin-Pakete werden IMMER plaintext gesendet — auch wenn der Peer regulaer
// encrypted ist. Wardriver-Konvention. Begruendung: bei Topologie-Wechsel
// (encryption-Mode-flip) muss Admin auch durchgehen wenn der Encrypted-Peer
// gerade neu aufgebaut wird.
bool WardriveCore::sendAdminToNodeSlot(uint8_t slot, const uint8_t* dest_mac) {
    if (slot >= WARDRIVE_CORE_MAX_NODES) return false;
    if (!(node_table[slot].flags & NODE_FLAG_ACTIVE)) return false;

    // The stock 10 bytes plus our two tails. A stock node bounds its MSG_ADMIN
    // handler with `len < sizeof(enow_admin_msg_t)` and then casts, so a longer
    // frame is read as the 10 bytes it knows and the rest is ignored; a node
    // that knows only the first tail reads 14 and ignores the second. Ours reads
    // both and learns the session state, the session identity and its BLE role
    // at the same time, which is what re-arms a node that rebooted or missed a
    // broadcast.
    enow_admin_ext2_msg_t ext2 = {};
    enow_admin_ext_msg_t& ext = ext2.ext1;
    enow_admin_msg_t& msg = ext.base;
    memcpy(msg.magic, ENOW_MAGIC, 4);
    msg.type = MSG_ADMIN;
    msg.assignment_version = assignment_version;
    msg.node_index = node_table[slot].assigned_index;
    // The count this node's index was assigned under -- not today's headcount.
    // These two travel together or the node's BLE-host election is meaningless.
    msg.node_count = partition_node_count;
    msg.start_channel_idx = node_table[slot].start_channel_idx;
    msg.end_channel_idx = node_table[slot].end_channel_idx;
    ext.tag[0] = ENOW_EXT_TAG0;
    ext.tag[1] = ENOW_EXT_TAG1;
    ext.struct_version = ENOW_ADMIN_EXT_VER;
    ext.session = desiredSessionFor(slot);

    // Second tail: who scans BLE, and which session this is. A node built
    // before this existed reads the 14 bytes it knows and ignores these two.
    ext2.flags = slotIsBleHost(slot) ? ADMIN_EXT2_FLAG_BLE_HOST : 0;
    ext2.session_epoch = session_epoch;

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

    esp_err_t res = esp_now_send(dest_mac, (uint8_t*)&ext2, sizeof(ext2));
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

    // Evidence gathered under a DIFFERENT assignment says nothing about this
    // one -- but a retransmit of the same assignment does not invalidate
    // anything, and clearing the streak on every send made confirmation
    // unreachable: a resend fires on roughly every heartbeat, while the node
    // deduplicates per BSSID and therefore has almost no fresh in-slice records
    // left to offer after the first sweep. Compare what we just sent with what
    // we sent last time and only start over when it actually changed.
    const bool content_changed =
        node_table[slot].admin_sent_index != msg.node_index ||
        node_table[slot].admin_sent_count != msg.node_count ||
        node_table[slot].admin_sent_start != msg.start_channel_idx ||
        node_table[slot].admin_sent_end   != msg.end_channel_idx;
    if (content_changed) {
        node_table[slot].admin_ok_streak = 0;
        node_table[slot].admin_confirmed_version = 0;
        node_table[slot].admin_sent_index = msg.node_index;
        node_table[slot].admin_sent_count = msg.node_count;
        node_table[slot].admin_sent_start = msg.start_channel_idx;
        node_table[slot].admin_sent_end   = msg.end_channel_idx;
    }

    node_table[slot].last_admin_version_sent = assignment_version;
    node_table[slot].admin_last_send_ms = millis();
    if (node_table[slot].admin_resend_count < 255) node_table[slot].admin_resend_count++;
    // ADMIN_DIRTY deliberately stays set. esp_now_send() returning OK means the
    // packet was queued, not that it was heard -- see the NodeRecord comment. It
    // clears when the node itself shows the assignment: in its heartbeat status
    // echo, or (for a stock node, which sends none) in observeAssignmentEvidence.
    return true;
}

// Unicast session command. Only for a node we must NOT send an admin packet to
// -- one that is registered but outside the frozen partition, whose slice
// fields are still the allocation defaults of index 0 and the whole pool.
bool WardriveCore::sendSessionToNode(uint8_t slot, const uint8_t* dest_mac,
                                     uint8_t command) {
    if (slot >= WARDRIVE_CORE_MAX_NODES) return false;

    enow_session_msg_t msg = {};
    memcpy(msg.magic, ENOW_MAGIC, 4);
    msg.type = MSG_SESSION;
    msg.command = command;

    bool had_peer_before = esp_now_is_peer_exist(dest_mac);
    if (had_peer_before) esp_now_del_peer(dest_mac);
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, dest_mac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) return false;

    esp_err_t res = esp_now_send(dest_mac, (uint8_t*)&msg, sizeof(msg));
    esp_now_del_peer(dest_mac);

    if (use_encryption && (node_table[slot].flags & NODE_FLAG_ENCRYPTED)) {
        addPeerWithMode(dest_mac, true, lmk);
    }
    node_table[slot].admin_last_send_ms = millis();
    return (res == ESP_OK);
}

// Send the admin packet at a node check-in. Called from the heartbeat and
// core-request paths: the node transmits from channel 6 and the core answers
// straight away, which is the only window in the cycle where it is reliably
// listening on the right channel.
//
// Two reasons to send. The obvious one is that the node still owes us evidence
// it adopted the assignment. The other is the keepalive: even a confirmed node
// gets one every WARDRIVE_CORE_ADMIN_KEEPALIVE_MS, because the packet also
// carries the session command, and a node that missed a START or STOP broadcast
// has no other way to find out. It doubles as the core's sign of life, which is
// what lets a node notice the core is gone instead of scanning into dead air.
void WardriveCore::maybeResendAdmin(uint8_t slot, const uint8_t* dest_mac) {
    if (slot >= WARDRIVE_CORE_MAX_NODES) return;

    const uint32_t now = millis();
    const bool partitioned = (node_table[slot].flags & NODE_FLAG_PARTITIONED) != 0;
    const bool dirty       = (node_table[slot].flags & NODE_FLAG_ADMIN_DIRTY) != 0;
    // Only a node that owes us evidence for a slice it actually has gets the
    // fast cadence. There is nothing to hurry for the others.
    const uint32_t min_gap = (dirty && partitioned) ? WARDRIVE_CORE_ADMIN_RESEND_MS
                                                    : WARDRIVE_CORE_ADMIN_KEEPALIVE_MS;

    // A fresh slot has admin_last_send_ms == 0 and goes out immediately.
    if (node_table[slot].admin_last_send_ms != 0 &&
        (now - node_table[slot].admin_last_send_ms) < min_gap)
        return;

    // Never hand out the allocation defaults. A slot that is not in the current
    // partition still carries index 0 / channels 0..39, and a node that adopts
    // that sweeps the whole pool across everyone else's slices. It still needs
    // telling to stay idle, though -- it may have been collecting for a
    // different core, or for this one before it dropped out.
    if (!partitioned) {
        sendSessionToNode(slot, dest_mac, SESSION_CMD_STOP);
        return;
    }

    sendAdminToNodeSlot(slot, dest_mac);
}

uint8_t WardriveCore::scanIndexOfChannel(uint8_t channel) {
    for (uint8_t i = 0; i < NUM_SCAN_CHANNELS; i++) {
        if (scan_channels[i] == channel) return i;
    }
    return 0xFF;
}

// The highest assigned index among nodes that are ACTUALLY HERE.
//
// This used to be `assigned_index == partition_node_count - 1`, which is the
// same answer right up until the node holding the top index goes quiet. Its
// slot is then reserved rather than freed -- deliberately, so it keeps its slice
// across a reboot or a minute behind a hill -- and the partition is frozen, so
// the count keeps counting it. The predicate then names an index nobody
// occupies: not one node in the fleet believes it is the host, BLE stops
// entirely, and the only cure was the operator noticing and pressing Re-Sync.
// The 5-GHz slice at the top of the pool goes unswept for the same stretch, so
// the WiFi count drops at the same time -- just by less, because that is one
// node's share of the channels rather than all of the BLE. [warroom-rig]
uint8_t WardriveCore::bleHostSlot() const {
    uint8_t best_slot = 0xFF;
    uint8_t best_idx  = 0;
    for (uint8_t i = 0; i < WARDRIVE_CORE_MAX_NODES; i++) {
        if (!(node_table[i].flags & NODE_FLAG_ACTIVE)) continue;
        if (!(node_table[i].flags & NODE_FLAG_PARTITIONED)) continue;
        if (best_slot == 0xFF || node_table[i].assigned_index > best_idx) {
            best_slot = i;
            best_idx  = node_table[i].assigned_index;
        }
    }
    return best_slot;
}

bool WardriveCore::slotIsBleHost(uint8_t slot) const {
    if (slot >= WARDRIVE_CORE_MAX_NODES) return false;
    if (partition_node_count == 0) return false;
    return bleHostSlot() == slot;
}

// Called wherever the fleet's membership can have changed. A host that has gone
// away cannot be told anything, but marking it dirty is free and puts it right
// if it comes back; the incoming host is the one that matters, and it gets the
// fast resend cadence rather than waiting out a keepalive with the fleet's BLE
// switched off.
void WardriveCore::refreshBleHostElection() {
    const uint8_t now_host = bleHostSlot();
    if (now_host == ble_host_slot) return;

    const uint8_t was_host = ble_host_slot;
    ble_host_slot = now_host;

    if (was_host < WARDRIVE_CORE_MAX_NODES &&
        (node_table[was_host].flags & NODE_FLAG_ACTIVE)) {
        node_table[was_host].flags |= NODE_FLAG_ADMIN_DIRTY;
        node_table[was_host].admin_last_send_ms = 0;
    }
    if (now_host < WARDRIVE_CORE_MAX_NODES) {
        node_table[now_host].flags |= NODE_FLAG_ADMIN_DIRTY;
        node_table[now_host].admin_last_send_ms = 0;
        Serial.printf("CORE: BLE host -> slot %u (suffix %04X)\n",
                      now_host, node_table[now_host].mac_suffix);
    } else {
        Serial.println("CORE: BLE host -> none (no partitioned node present)");
    }
}

// Read a node's live assignment back out of the records it sends.
//
// The wardrive line is the 6-field CSV `BSSID,ESSID,SEC,CHANNEL,RSSI,TYPE`. Two
// of those fields say what the node believes it was told: the channel it just
// swept, and whether it is doing BLE. Both are on the wire for every record
// already, so this costs no traffic and needs no protocol change.
//
// Contradiction is decisive, agreement is not. A stale node sweeping the whole
// pool also produces in-slice records -- it just produces out-of-slice ones too,
// within one sweep. So one contradiction resets, and adoption needs a clean run.
//
// This is the fallback for a node that does not send the heartbeat status echo,
// i.e. a stock wardriver. It can only work while records are flowing, which is
// never the case in the lobby -- a node that reports its own state confirms in
// one heartbeat instead, and does it before the session starts.
void WardriveCore::observeAssignmentEvidence(uint8_t slot, const enow_text_msg_t& msg) {
    if (slot >= WARDRIVE_CORE_MAX_NODES) return;
    if (partition_node_count == 0) return;
    if (msg.len == 0 || msg.len > ENOW_TEXT_MAX) return;

    // Walk to fields 4 (channel) and 6 (type) without copying the line. Every
    // bound below is msg.len, never a NUL: the payload is attacker-controlled
    // in plaintext mode and nothing on the wire makes it terminated.
    const char* field[6] = {0};
    uint8_t nf = 0;
    field[nf++] = msg.text;
    for (uint16_t i = 0; i < msg.len && nf < 6; i++) {
        if (msg.text[i] == ',') field[nf++] = &msg.text[i + 1];
    }
    if (nf < 6) return;   // malformed; the compose path already counts it as bad
    if (field[5] >= msg.text + msg.len) return;   // trailing comma, no type field

    const bool is_ble = (field[5][0] == 'B');
    bool contradicts = false;

    if (is_ble) {
        // A node collecting BLE while it is not the elected host is running an
        // (index, count) pair we never gave it. This is the exact shape of the
        // field failure: several nodes on BLE, channels underneath uncovered.
        if (!slotIsBleHost(slot)) contradicts = true;
    } else {
        // Digits read by hand rather than with atoi(), which would run off the
        // end of an unterminated payload.
        uint16_t p = (uint16_t)(field[3] - msg.text);
        uint16_t ch = 0;
        while (p < msg.len && msg.text[p] >= '0' && msg.text[p] <= '9' && ch < 1000) {
            ch = (uint16_t)(ch * 10 + (msg.text[p] - '0'));
            p++;
        }
        const uint8_t idx = (ch <= 255) ? scanIndexOfChannel((uint8_t)ch) : 0xFF;
        if (idx != 0xFF) {
            if (idx < node_table[slot].start_channel_idx ||
                idx > node_table[slot].end_channel_idx)
                contradicts = true;
        }
        // A channel number that is not in the table proves nothing either way.
    }

    if (contradicts) {
        node_table[slot].admin_ok_streak = 0;
        node_table[slot].admin_confirmed_version = 0;
        node_table[slot].flags |= NODE_FLAG_ADMIN_DIRTY;
        return;
    }

    if (node_table[slot].flags & NODE_FLAG_ADMIN_DIRTY) {
        if (node_table[slot].admin_ok_streak < 255) node_table[slot].admin_ok_streak++;
        if (node_table[slot].admin_ok_streak >= WARDRIVE_CORE_ADMIN_CONFIRM_HITS) {
            node_table[slot].flags &= ~NODE_FLAG_ADMIN_DIRTY;
            node_table[slot].admin_confirmed_version = assignment_version;
            node_table[slot].admin_resend_count = 0;
        }
    }
}

// The status echo a warroom-rig node writes into the otherwise unused text
// payload of its heartbeat. A stock node sends that payload zeroed with len 0,
// so both the length and the tag have to match before a single byte is trusted.
const enow_node_status_t* WardriveCore::nodeStatusFromHeartbeat(const enow_text_msg_t& hb) {
    if (hb.len != sizeof(enow_node_status_t)) return nullptr;
    const enow_node_status_t* st = (const enow_node_status_t*)hb.text;
    if (st->tag[0] != ENOW_EXT_TAG0 || st->tag[1] != ENOW_EXT_TAG1) return nullptr;
    if (st->struct_version != ENOW_NODE_STATUS_VER) return nullptr;
    return st;
}

// Everything that happens when a node checks in. This is the one moment the
// node is provably awake on channel 6, so it is where the core finds out what
// the node is actually running and puts it right -- rather than assuming the
// last packet it queued was heard.
void WardriveCore::serviceNodeCheckin(uint8_t slot, const uint8_t* src_mac,
                                      const enow_text_msg_t& hb) {
    if (slot >= WARDRIVE_CORE_MAX_NODES) return;

    const enow_node_status_t* st = nodeStatusFromHeartbeat(hb);
    if (st) {
        node_table[slot].flags |= NODE_FLAG_REPORTS_STATUS;

        // Band capability. A classic ESP32 cannot tune 5 GHz at all, and a
        // slice made entirely of 5-GHz indices leaves it with nothing to scan.
        // Learning this from the node is the only way the core can know --
        // there is no other field on the wire that says what a radio can do.
        const bool only24 = (st->flags & NODE_STATUS_FLAG_2G4_ONLY) != 0;
        const bool knew24 = (node_table[slot].flags & NODE_FLAG_2G4_ONLY) != 0;
        if (only24 != knew24) {
            if (only24) node_table[slot].flags |= NODE_FLAG_2G4_ONLY;
            else        node_table[slot].flags &= ~NODE_FLAG_2G4_ONLY;
            // Re-cut the pool now that we know which bands it has to respect.
            // Only in the lobby: during a session the partition is frozen, and
            // the node's own scan loop skips what it cannot tune until the
            // operator re-syncs.
            if (!collecting) handleNodeTopologyChange();
        }

        // Does the node's own account of its assignment match what we handed
        // out? This is the confirmation the record-sniffing fallback cannot
        // give in the lobby, which is exactly when the operator is looking at
        // the fleet and deciding whether to start.
        const bool matches =
            (st->flags & NODE_STATUS_FLAG_ASSIGNED) &&
            (node_table[slot].flags & NODE_FLAG_PARTITIONED) &&
            st->node_index        == node_table[slot].assigned_index &&
            st->node_count        == partition_node_count &&
            st->start_channel_idx == node_table[slot].start_channel_idx &&
            st->end_channel_idx   == node_table[slot].end_channel_idx;
        if (matches) {
            node_table[slot].flags &= ~NODE_FLAG_ADMIN_DIRTY;
            node_table[slot].admin_confirmed_version = st->assignment_version;
            node_table[slot].admin_resend_count = 0;
        } else {
            node_table[slot].flags |= NODE_FLAG_ADMIN_DIRTY;
            node_table[slot].admin_confirmed_version = 0;
        }

        // Session state. A node that rebooted comes back saying it is idle
        // while the drive is running; one that missed a STOP says the opposite.
        // Either way it needs correcting now, not at the next Re-Sync.
        const bool node_collecting = (st->flags & NODE_STATUS_FLAG_COLLECTING) != 0;
        if (node_collecting != (desiredSessionFor(slot) == SESSION_CMD_START)) {
            node_table[slot].admin_last_send_ms = 0;   // bypass the keepalive gap
        }
    }

    maybeResendAdmin(slot, src_mac);
}

// ============================================================
// Wigle-Line-Compose
// ============================================================

// Adapted from JCMK ESP32DualBandWardriver (WiFiOps.cpp:821-980), MIT License.
// Node-Payload-Format: "BSSID,ESSID,SECURITY,CHANNEL,RSSI,W|B".
// Marauder-Wigle-Output: "BSSID,SSID,SECURITY,DATETIME,CHANNEL,RSSI,LAT,LON,ALT,ACC,WIFI|BLE".
// Wir ergaenzen DATETIME (zwischen SECURITY und CHANNEL) und LAT/LON/ALT/ACC/Type.
String WardriveCore::composeWigleLineFromNodeText(const enow_text_msg_t& msg) {
    // The sender is supposed to NUL-terminate `text`, and nothing on the wire
    // makes it. The plaintext path has no authentication whatsoever -- any
    // ESP32 in range on channel 6 can send 212 bytes of commas with no
    // terminator -- and the strchr/String parse below would then walk straight
    // off the end of the queue message. Take exactly the declared length and
    // terminate it here, so the parser cannot read past the payload no matter
    // what arrived.
    if (msg.len == 0 || msg.len > ENOW_TEXT_MAX) return String();
    char line_buf[ENOW_TEXT_MAX + 1];
    memcpy(line_buf, msg.text, msg.len);
    line_buf[msg.len] = '\0';

    const char* line = line_buf;
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
    // Announce that a callback is inside the instance BEFORE reading the
    // pointer, and snapshot the pointer once. Re-reading g_active_core on every
    // use, which is what this did, meant deinit() could null it between two
    // dereferences and free the queue under a callback that had already passed
    // the check. The depth counter is what deinit() waits on; without it there
    // is no ordering between the two tasks at all.
    g_rx_cb_depth++;
    WardriveCore* core = g_active_core;
    if (!core || !info || !data) {
        g_rx_cb_depth--;
        return;
    }

    // Min-Length: Magic[4] + Type[1] = 5 Bytes.
    bool bad = false;
    do {
        if (len < 5) { bad = true; break; }
        if (memcmp(data, ENOW_MAGIC, 4) != 0) {
            // Kein Marauder-CORE-Paket. Im normalen Marauder-Stack hat ESP-NOW
            // nichts zu suchen, daher als bad-packet zaehlen.
            bad = true; break;
        }
        const uint8_t msgType = data[4];

        // ADMIN ist Core->Node only. Wenn wir ADMIN empfangen, ist das ein
        // anderer Core in Reichweite — ignorieren statt enqueuen.
        if (msgType == MSG_ADMIN) { bad = true; break; }

        // Nur TEXT/HEARTBEAT/CORE_REQUEST gehen in die Queue.
        if (msgType != MSG_TEXT && msgType != MSG_HEARTBEAT && msgType != MSG_CORE_REQUEST) {
            bad = true; break;
        }

        // Volle Struct-Groesse muss vorhanden sein (Wardriver-Konvention:
        // immer sizeof(enow_text_msg_t) gesendet).
        if (len < (int)sizeof(enow_text_msg_t)) { bad = true; break; }

        WardriveCoreQueueMsg qmsg;
        memcpy(qmsg.src_mac, info->src_addr, 6);
        qmsg.rssi = (info->rx_ctrl) ? info->rx_ctrl->rssi : 0;
        qmsg.msg_type = msgType;
        memcpy(&qmsg.payload, data, sizeof(enow_text_msg_t));

        // Snapshot the queue handle once, for the same reason as the instance
        // pointer above.
        QueueHandle_t q = core->rx_queue;
        if (!q) break;
        // RX-Callback laeuft im WiFi-Task — wir nutzen FromISR-API NICHT direkt
        // (kein ISR), aber `xQueueSend` ist threadsafe und nicht-blocking mit
        // ticks_to_wait=0. Bei voll: drop. Counter wird erhoeht.
        if (xQueueSend(q, &qmsg, 0) != pdTRUE) {
            core->total_rx_drops++;
        }
    } while (false);

    if (bad) core->total_rx_bad++;
    g_rx_cb_depth--;
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

    // Step 6: SD pruefen — aber noch KEINE Logdatei anlegen.
    //
    // Das Log wird erst beim Session-Start geoeffnet (ensureLogOpen()). Vorher
    // legte init() es hier an und schrieb sofort die Wigle-Kopfzeile: damit
    // hinterliess jeder Blick in Rig Mode — nur mal nachsehen, ob die Nodes da
    // sind — eine wardrive_core_N.log mit genau einer Zeile. Die SD lief davon
    // voll, und die Upload-Auswahl war voll leerer Dateien, zwischen denen die
    // echten Fahrten nicht mehr zu finden waren.
    //
    // sd_healthy is re-armed here, not merely cleared. It used to be set true
    // once in the constructor and false on the first bad trip, and nothing ever
    // set it back: one full card or one visit without a card silenced logging
    // for the whole power cycle, while the console happily went on registering
    // nodes and counting received lines. Entering Rig Mode is the natural place
    // to look at the card again.
    #ifdef HAS_SD
        sd_healthy = sd_obj.supported;
        if (!sd_healthy) {
            Serial.println("CORE: WARN — SD not supported, logging disabled");
        } else {
            uint64_t total = SD.totalBytes();
            uint64_t used  = SD.usedBytes();
            if (total > 0 && (total - used) < (1ULL * 1024 * 1024)) {
                sd_healthy = false;
                Serial.println("CORE: WARN — SD < 1MB free, logging disabled");
            }
        }
    #endif
    log_open = false;

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
    // Seed the assignment version from the hardware RNG instead of restarting
    // at 1 on every Core Mode entry.
    //
    // The node holds its copy in RAM for as long as it is powered, and it used
    // to adopt an admin packet only when the version differed. A core that
    // restarts and counts 1,2,3... again therefore hands out numbers a node may
    // already be holding -- with a completely different slice underneath. The
    // node discards the packet as "nothing new" and keeps sweeping the previous
    // session's band: one band unswept, another swept twice, and two nodes both
    // passing the BLE-host test. Our nodes now compare the payload, which fixes
    // it outright; a random start makes the collision unlikely for stock ones
    // too, which is the best a one-byte counter allows.
    assignment_version = (uint8_t)(esp_random() % 255) + 1;   // 1..255, never 0
    partition_node_count = 0;   // no partition until nodes register
    ble_host_slot = 0xFF;       // nobody elected yet
    // Random for the same reason as assignment_version: a node that stayed
    // powered through a CORE restart must not mistake the new CORE's first
    // session for the one it is already in, or it keeps a whole drive's worth of
    // MACs in its dedup ring and reports almost nothing on the next run.
    session_epoch = (uint8_t)(esp_random() % 255) + 1;        // 1..255, never 0
    session_start_ms = millis();
    last_display_refresh_ms = 0;
    last_stale_check_ms = 0;
    last_heap_check_ms = 0;
    last_sd_check_ms = 0;
    last_session_beacon_ms = 0;
    center_press_start_ms = 0;
    center_was_pressed = false;
    // Adopt a BACK key that is already down as "already seen". This runs a
    // moment after the menu entry that opened Rig Mode, and ESC is exactly what
    // someone may still be holding from backing around the tool tree. Seeding it
    // false makes the very first poll read that held key as a rising edge, and
    // the mode closes on the frame it opened -- the comment at the detector says
    // edge-triggering prevents that, and on its own it does not.
    #if defined(RIG_HAS_NAV) && defined(RIG_HAS_BACK)
      back_was_pressed = RigInput::down(RigInput::BACK);
    #else
      back_was_pressed = false;
    #endif

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

    // Tell the fleet the drive is over BEFORE the radio goes away. Leaving Rig
    // Mode used to broadcast nothing at all -- broadcastSession() was only ever
    // called by the operator's Start/Stop/Re-Sync -- so every node kept
    // collecting into a core that no longer existed. Everything it saw in that
    // window went into its 200-entry dedup ring, and those APs were then missing
    // from the START of the next session, which is the one part of a drive an
    // operator cannot repeat.
    const bool was_collecting = collecting;
    if (was_collecting) {
        broadcastSession(SESSION_CMD_STOP);
        collecting = false;
    }

    // Disarm the receive path — stop new callbacks and make the instance
    // unreachable — and then WAIT for any callback that is already inside it.
    // Neither unregistering nor nulling the pointer ejects a callback that has
    // already passed the null check and is on its way to rx_queue; ESP-NOW makes
    // no promise about in-flight dispatches either. Without this wait the
    // vQueueDelete below can free the queue under it.
    esp_now_unregister_recv_cb();
    g_active_core = nullptr;
    for (uint32_t spin = 0; g_rx_cb_depth != 0 && spin < 200; spin++) delay(1);
    if (g_rx_cb_depth != 0) {
        // 200 ms is far longer than the callback's own work (a memcmp and a
        // queue push). If it has not left by now something else is wrong, and
        // leaking one queue is a great deal better than freeing it underneath.
        Serial.println("CORE: RX callback still active at teardown, leaking rx_queue");
        rx_queue = nullptr;
    }

    // Drain remaining Queue + flush.
    if (rx_queue) {
        WardriveCoreQueueMsg qmsg;
        while (xQueueReceive(rx_queue, &qmsg, 0) == pdTRUE) {
            // Same guards runTick applies. Without them, leaving Rig Mode after
            // only looking at the lobby appended Wigle rows to whatever file the
            // PREVIOUS mode had left open in Buffer -- a .pcap, or an already
            // uploaded log that got recreated without its header. `log_open`
            // is the one that says the target file is ours.
            if (qmsg.msg_type == MSG_TEXT && was_collecting && log_open &&
                qmsg.payload.len <= ENOW_TEXT_MAX) {
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

    // ESP-NOW deinit. The recv callback was unregistered, g_active_core cleared
    // and any in-flight callback waited out above, before the queue was freed.
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
                    serviceNodeCheckin(slot, qmsg.src_mac, qmsg.payload);
                    break;
                }
                case MSG_HEARTBEAT: {
                    // Adapted from W:WiFiOps.cpp:913-936.
                    node_table[slot].hb_counter = qmsg.payload.counter;
                    serviceNodeCheckin(slot, qmsg.src_mac, qmsg.payload);
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
                        // log_open says the file Buffer is pointing at is the
                        // one this session created. Without it a failed
                        // startLog would send Wigle rows into whatever another
                        // mode had open.
                        if (sd_healthy && log_open) {
                            buffer_obj.append(wigle);
                        }
                    #endif
                    // Before the counters: what this record says about whether the
                    // node is running the assignment we last sent it. Only worth
                    // anything for a node that sends no status echo -- one that
                    // does has already told us, in its heartbeat, and told us in
                    // the lobby where no records exist to judge by.
                    if (!(node_table[slot].flags & NODE_FLAG_REPORTS_STATUS))
                        observeAssignmentEvidence(slot, qmsg.payload);

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
        // Membership can have changed either way here -- a node timed out into a
        // reserved slot above, or one came back and touchNode() reclaimed it --
        // and during a session neither path re-partitions. The BLE host is the
        // one thing that must not be left pointing at a node that is not there,
        // so it is re-elected on the same tick rather than at the next Re-Sync.
        refreshBleHostElection();
    }

    // 3) Periodischer Heap-Check (Failure-Mode 6.10).
    if (currentTime - last_heap_check_ms > WARDRIVE_CORE_HEAP_CHECK_MS) {
        last_heap_check_ms = currentTime;
        if (ESP.getFreeHeap() < WARDRIVE_CORE_HEAP_MIN_RUNTIME) {
            low_heap_events++;
            Serial.printf("CORE: LOW HEAP %u\n", ESP.getFreeHeap());
        }
    }

    // 4) Periodischer SD-Check (Failure-Modes 6.3 + 6.4). Symmetrisch: der
    //    Check darf sd_healthy auch wieder SETZEN. Als Einbahn-Latch blieb das
    //    Logging nach einem einzigen vollen Moment fuer den Rest des
    //    Power-Cycles aus, ohne dass irgendetwas auf dem Schirm es sagte.
    #ifdef HAS_SD
        if (currentTime - last_sd_check_ms > WARDRIVE_CORE_SD_CHECK_MS) {
            last_sd_check_ms = currentTime;
            bool healthy_now = sd_obj.supported;
            if (healthy_now) {
                // Frei-Space-Check. Default-Marauder-API exposed kein
                // direktes "totalBytes/usedBytes"-Wrapper — fall-back via
                // SD-Lib direkt.
                uint64_t total = SD.totalBytes();
                uint64_t used  = SD.usedBytes();
                if (total > 0 && (total - used) < (1ULL * 1024 * 1024)) {
                    healthy_now = false;
                }
            }
            if (healthy_now != sd_healthy) {
                sd_healthy = healthy_now;
                Serial.println(healthy_now ? "CORE: SD usable again, logging resumed"
                                           : "CORE: SD unusable (<1MB free), stop logging");
            }
            // A session that started while the card was unusable never opened a
            // log; do it now that it is writable again rather than dropping the
            // rest of the drive.
            if (sd_healthy && collecting) ensureLogOpen();
        }
    #endif

    // 4b) Fleet-wide idle beacon -- STOP only, and only while we are idle.
    //     It parks a node that missed a STOP edge or is still following a core
    //     that went away, and it cannot mislead anyone: "stay idle" is true for
    //     every listener whenever no session is running here. The START
    //     direction is per-node by construction; see startSession().
    if (!collecting &&
        currentTime - last_session_beacon_ms > WARDRIVE_CORE_SESSION_BEACON_MS) {
        last_session_beacon_ms = currentTime;
        broadcastSession(SESSION_CMD_STOP, 1);
    }

    // 5) Periodischer Display-Refresh (Raten vorher rollen, damit die Anzeige
    //    frische lines/min sieht).
    updateRates(currentTime);
    if (currentTime - last_display_refresh_ms > WARDRIVE_CORE_DISPLAY_REFRESH_MS) {
        last_display_refresh_ms = currentTime;
        refreshCoreDisplay();
    }

    // 6) Center-Long-Press-Detection fuer Exit.
    #ifdef HAS_BUTTONS
      #ifdef RIG_HAS_NAV
        // Marauders Switches-Wrapper ist edge-triggered (justPressed /
        // justReleased) und kann "wird gerade gehalten" nicht beantworten —
        // genau das brauchen wir aber, um WAEHREND des Holds zu reagieren
        // statt erst beim Loslassen. Deshalb fragen wir den Zustand direkt ab.
        // RigInput liefert ihn boardunabhaengig: Pin-Pegel auf den
        // Tastenboards, gehaltene Taste auf der ADV-Tastatur.
        bool pressed_now = RigInput::down(RigInput::SELECT);
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

        #ifdef RIG_HAS_BACK
          // The key hint at the bottom of this screen says "ESC: exit", and on
          // the ADV it did nothing -- the held-SELECT gesture above was the only
          // way out. A board with a dedicated back key should not have to learn
          // a hold. Edge-triggered on purpose: the same still-held ESC that left
          // a menu must not immediately tear down the mode it just opened.
          const bool back_now = RigInput::down(RigInput::BACK);
          if (back_now && !back_was_pressed) {
              back_was_pressed = true;
              Serial.println("CORE: BACK -> exit");
              deinit();
              return;
          }
          if (!back_now) back_was_pressed = false;
        #endif
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

// Palette shared with the rest of the rig UI -- see RigTheme.h. These were
// local copies until the greys had to be fixed for daylight and the fix had
// to be made twice; aliasing keeps the next change to one place.
static const uint16_t WC_GOLD   = RigTheme::GOLD;
static const uint16_t WC_INK    = RigTheme::INK;
static const uint16_t WC_DIM    = RigTheme::DIM;
static const uint16_t WC_DIM2   = RigTheme::DIM2;
static const uint16_t WC_PANEL  = RigTheme::PANEL;
static const uint16_t WC_PANEL3 = 0x2902;  // panel outline
static const uint16_t WC_GREEN  = 0x6E6D;
static const uint16_t WC_AMBER  = 0xFD20;

// Layout. Two screen shapes: 240x320 portrait (Marauder V7/V8) and 240x135
// landscape (Cardputer ADV). Same width, so the columns below are shared; only
// the vertical rhythm compresses, and the hero's big numbers step down from
// text size 3 to 2 because 24 px of digits does not fit in a 34 px tile.
static const int WC_BAR_H    = RigTheme::COMPACT ? 12 : 22;   // bronze header
static const int WC_HERO_Y   = RigTheme::COMPACT ? 14 : 25;
static const int WC_HERO_H   = RigTheme::COMPACT ? 34 : 48;
// The three hero bands stack inside WC_HERO_H with no slack on the short
// screen: 8 px of label, 16 px of value, 8 px of status in a 34 px tile.
static const int WC_LBL_DY   = RigTheme::COMPACT ?  1 :  4;   // label y in hero
static const int WC_VAL_DY   = RigTheme::COMPACT ?  9 : 12;   // big value y in hero
static const int WC_SUB_DY   = RigTheme::COMPACT ? 25 : 37;   // status strip y
static const int WC_HDR_Y    = RigTheme::COMPACT ? 52 : 78;   // column header text
static const int WC_DIV_Y    = RigTheme::COMPACT ? 60 : 88;   // rule under it
static const int WC_ROWS_Y   = RigTheme::COMPACT ? 63 : 92;
// Rows must stop before the touch bar (274) on the tall screen, or before the
// key hint on the short one.
static const int WC_ROWS_END = RigTheme::COMPACT ? (SCREEN_HEIGHT - 12) : 272;
static const int WC_HINT_Y   = RigTheme::COMPACT ? (SCREEN_HEIGHT -  9) : 306;
static const uint8_t WC_BIG  = RigTheme::COMPACT ?  2 :  3;   // hero number size
static const uint8_t WC_MID  = RigTheme::COMPACT ?  1 :  2;   // hero LINES size

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

        // SD health, in the one place that is on screen the whole time. A card
        // that is missing or full stops every line from being written while the
        // node table and the hero counters carry on as if the drive were being
        // recorded -- which is the worst way to find out, hours later, at the
        // upload. Blank when all is well; the field is padded so it clears.
        #ifdef HAS_SD
            tft.setTextColor(sd_healthy ? STATUSBAR_COLOR : TFT_RED, STATUSBAR_COLOR);
            tft.setCursor(84, 7);
            tft.print(sd_healthy ? "     " : "NO SD");
        #endif

        uint8_t bl = battery_obj.battery_level;
        uint16_t bcol = (bl >= 40) ? WC_GREEN : ((bl >= 20) ? WC_AMBER : TFT_RED);
        char bb[8];
        snprintf(bb, sizeof(bb), "%3u%%", (unsigned)bl);
        tft.setTextColor(bcol, STATUSBAR_COLOR);
        tft.setCursor(204, 7);
        tft.print(bb);
    #endif
}

void WardriveCore::redrawScreen() {
    #ifdef HAS_SCREEN
        // drawCoreModeFrame() clears the screen itself and sets drawn_row_count
        // to 0xFF, which makes the following refresh repaint every node row
        // rather than only the ones whose values happen to have changed.
        drawCoreModeFrame();
        refreshCoreDisplay();
    #endif
}

void WardriveCore::drawCoreModeFrame() {
    #ifdef HAS_SCREEN
        auto &tft = display_obj.tft;
        display_obj.clearScreen();

        // Bronze header + wordmark (static; sats/battery are refreshed).
        tft.fillRect(0, 0, SCREEN_WIDTH, WC_BAR_H, STATUSBAR_COLOR);
        tft.setTextSize(1);
        tft.setTextColor(WC_INK, STATUSBAR_COLOR);
        tft.setCursor(6, (WC_BAR_H - 8) / 2);
        tft.print("WARROOM RIG");

        // Hero tile with a gold spine + the static metric labels.
        tft.fillRoundRect(4, WC_HERO_Y, SCREEN_WIDTH - 8, WC_HERO_H, 4, WC_PANEL);
        tft.drawRoundRect(4, WC_HERO_Y, SCREEN_WIDTH - 8, WC_HERO_H, 4, WC_PANEL3);
        tft.fillRect(4, WC_HERO_Y + 2, 3, WC_HERO_H - 4, WC_GOLD);
        tft.setTextColor(WC_DIM, WC_PANEL);
        tft.setCursor(14,  WC_HERO_Y + WC_LBL_DY); tft.print("WIFI");
        tft.setCursor(96,  WC_HERO_Y + WC_LBL_DY); tft.print("BLE");
        tft.setCursor(166, WC_HERO_Y + WC_LBL_DY); tft.print("LINES");

        // Column header for the node table.
        tft.setTextColor(WC_DIM2, TFT_BLACK);
        tft.setCursor(10,         WC_HDR_Y); tft.print("NODE");
        tft.setCursor(WC_X_SLICE, WC_HDR_Y); tft.print("SLICE");
        tft.setCursor(WC_X_LINES, WC_HDR_Y); tft.print("LINES");
        tft.setCursor(WC_X_RATE,  WC_HDR_Y); tft.print("RATE");
        tft.setCursor(WC_X_SIG+2, WC_HDR_Y); tft.print("SIG");
        tft.drawFastHLine(4, WC_DIV_Y, SCREEN_WIDTH - 8, WC_PANEL3);

        drawn_row_count = 0xFF;   // force a full row repaint on the next refresh

        #ifdef HAS_TOUCH
            this->drawTouchControls();
        #else
            tft.setTextColor(WC_DIM2, TFT_BLACK);
            tft.setCursor(6, WC_HINT_Y);
            tft.print(RIG_HINT_SESSION);
        #endif
    #endif
}

void WardriveCore::refreshCoreDisplay() {
    #ifdef HAS_SCREEN
        auto &tft = display_obj.tft;
        char buf[24];

        drawRigBar();

        // ---- hero: WiFi / BLE are the headline pair, lines the third value ----
        tft.setTextSize(WC_BIG);
        tft.setTextColor(collecting ? WC_GOLD : WC_DIM2, WC_PANEL);
        wcFmt(buf, sizeof(buf), total_rx_wifi);
        tft.setCursor(14, WC_HERO_Y + WC_VAL_DY); tft.printf("%-4s", buf);
        wcFmt(buf, sizeof(buf), total_rx_ble);
        tft.setCursor(96, WC_HERO_Y + WC_VAL_DY); tft.printf("%-3s", buf);

        tft.setTextSize(WC_MID);
        tft.setTextColor(collecting ? WC_INK : WC_DIM2, WC_PANEL);
        wcFmt(buf, sizeof(buf), total_rx_lines);
        tft.setCursor(166, WC_HERO_Y + WC_VAL_DY + 4); tft.printf("%-4s", buf);

        // Status strip along the bottom of the hero tile.
        tft.setTextSize(1);
        tft.setTextColor(collecting ? WC_GREEN : WC_AMBER, WC_PANEL);
        tft.setCursor(14, WC_HERO_Y + WC_SUB_DY);
        tft.print(collecting ? "LIVE " : "LOBBY");
        tft.setTextColor(WC_DIM, WC_PANEL);
        tft.setCursor(52, WC_HERO_Y + WC_SUB_DY);
        if (collecting) snprintf(buf, sizeof(buf), "%u/min   ", (unsigned)rate_lines_per_min);
        else            snprintf(buf, sizeof(buf), "idle     ");
        tft.print(buf);
        // Fixed 13-character field: the drop counter starts at x=196 and the
        // node count changes width as nodes come and go, so letting this one run
        // to its natural length leaves a stale glyph behind when it shrinks.
        tft.setCursor(116, WC_HERO_Y + WC_SUB_DY);
        snprintf(buf, sizeof(buf), "n %u/%u ch %u",
                 (unsigned)getActiveNodeCount(),
                 (unsigned)WARDRIVE_CORE_MAX_NODES,
                 (unsigned)WARDRIVE_CORE_CHANNEL);
        tft.printf("%-13s", buf);

        // Records that reached this radio and were thrown away anyway: the rx
        // queue overflowing, or a payload that would not parse. Blank while it
        // is zero, red the moment it is not.
        //
        // This was the one number that mattered and could not be seen. A node
        // deduplicates before it transmits, so a record dropped here is not
        // retried and not logged -- the drive simply comes back thinner, and
        // nothing on the console distinguishes that from a quiet street. It is
        // also the fastest way to tell a firmware fault from a real one: a rig
        // that is losing rows says so here. [warroom-rig]
        const uint32_t lost = total_rx_drops + total_rx_bad;
        tft.setTextColor(lost ? TFT_RED : WC_DIM, WC_PANEL);
        tft.setCursor(196, WC_HERO_Y + WC_SUB_DY);
        if (lost) {
            wcFmt(buf, sizeof(buf), lost);
            char lb[8];
            snprintf(lb, sizeof(lb), "!%s", buf);
            tft.printf("%-5s", lb);
        } else {
            tft.print("     ");
        }

        // ---- node rows ----
        // Compact the active slots so the table shows no gaps after a dropout.
        uint8_t slots[WARDRIVE_CORE_MAX_NODES];
        uint8_t n = 0;
        for (uint8_t i = 0; i < WARDRIVE_CORE_MAX_NODES; i++)
            if (node_table[i].flags & NODE_FLAG_ACTIVE) slots[n++] = i;

        const int pitch = RigTheme::COMPACT ? ((n <= 4) ? 14 : 12)
                                            : ((n <= 6) ? 24 : 15);
        const int rh    = pitch - (RigTheme::COMPACT ? 2 : 3);

        // How many rows the table area can actually hold. On the tall screen
        // this never bites (7 rows at the loose pitch, 12 at the tight one, for
        // a fleet capped at WARDRIVE_CORE_MAX_NODES); on the short one it does,
        // and a node that quietly is not drawn looks like a node that is not
        // there. Whatever does not fit is counted out loud below the last row.
        const uint8_t fits = (uint8_t)((WC_ROWS_END - WC_ROWS_Y) / pitch);
        // When some rows have to be dropped, give up one more so the "+N more"
        // line has a slot of its own instead of being written over the last row.
        const uint8_t vis  = (n > fits) ? (uint8_t)(fits ? fits - 1 : 0) : n;

        // Relayout only when the fleet size (and thus the density) changes.
        if (n != drawn_row_count || pitch != drawn_pitch) {
            tft.fillRect(0, WC_ROWS_Y, SCREEN_WIDTH, WC_ROWS_END - WC_ROWS_Y, TFT_BLACK);
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

        for (uint8_t k = 0; k < vis; k++) {
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
                if (rowbg != TFT_BLACK) tft.fillRoundRect(4, y, SCREEN_WIDTH - 8, rh, 3, rowbg);
                else                    tft.fillRect(4, y, SCREEN_WIDTH - 8, rh, TFT_BLACK);
                tft.fillRect(4, y, 3, rh, col);
                tft.setTextSize(1);
                tft.setTextColor(WC_INK, rowbg);
                tft.setCursor(WC_X_NODE, ty);
                tft.printf("%04X", nr.mac_suffix);
            }

            tft.setTextSize(1);

            // Assigned channel slice, in gold, plus a BLE marker on the node that
            // also runs the BLE scanner. The election itself is the CORE's and
            // travels in the admin tail, so the marker and the node now read the
            // same decision out of the same function instead of each evaluating
            // a predicate and hoping they were fed identical numbers.
            //
            // Two earlier versions of this got it wrong in ways the screen could
            // not show: first a live active-node count next to a frozen index,
            // then the frozen pair -- correct until the node holding the top
            // index went quiet, after which the marker pointed at a reserved
            // slot and no node ran BLE at all.
            const bool ble_host = slotIsBleHost(slots[k]);
            tft.setTextColor(WC_GOLD, rowbg);
            tft.setCursor(WC_X_SLICE, ty);
            // A leading marker means the slice shown is what we asked for, not
            // what we have seen being swept. It sits in front because the column
            // truncates at 10 and a trailing marker would be the first thing
            // lost. This is the state that used to be invisible: a node that
            // never heard its admin packet looked identical to one that had.
            //
            //   '?' the node can tell us and has not agreed yet
            //   '~' we cannot tell yet -- a stock node that sends no status
            //       echo, in the lobby, where it sends no records either
            //
            // The distinction matters because '?' used to be shown for both, so
            // every node in the lobby wore one, which is exactly where the
            // operator looks before pressing Start.
            const bool observable = (nr.flags & NODE_FLAG_REPORTS_STATUS) || collecting;
            const char* unconf = (nr.flags & NODE_FLAG_ADMIN_DIRTY)
                                     ? (observable ? "?" : "~") : "";
            if (!(nr.flags & NODE_FLAG_PARTITIONED))
                // Registered but outside the frozen partition -- a late joiner.
                // Its slice fields are allocation defaults, so printing them
                // would claim a range this node was never given.
                snprintf(buf, sizeof(buf), "wait");
            else if (nr.start_channel_idx < NUM_SCAN_CHANNELS &&
                nr.end_channel_idx   < NUM_SCAN_CHANNELS)
                snprintf(buf, sizeof(buf), "%s%u-%u%s", unconf,
                         (unsigned)scan_channels[nr.start_channel_idx],
                         (unsigned)scan_channels[nr.end_channel_idx],
                         ble_host ? " BLE" : "");
            else
                snprintf(buf, sizeof(buf), "%s%s", unconf, ble_host ? "BLE" : "-");
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

        // Nodes the table had no room for. They are still being logged -- this
        // says so, rather than letting the screen imply the fleet is smaller
        // than it is.
        if (n > vis) {
            tft.setTextSize(1);
            tft.setTextColor(WC_DIM2, TFT_BLACK);
            tft.setCursor(WC_X_NODE, WC_ROWS_Y + vis * pitch + 1);
            tft.printf("+%u more", (unsigned)(n - vis));
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
        display_obj.tft.fillRect(0, TC_BAR_Y - 2, SCREEN_WIDTH, TC_BAR_H + 6, TFT_BLACK);
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
