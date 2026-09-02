// WardriveCore.h
//
// Marauder v7 Core Mode — ESP-NOW-Wardrive-Aggregator.
// Empfaengt Wardrive-Records (WiFi+BLE) von ESP32DualBandWardriver-Nodes,
// reichert mit GPS an und schreibt Wigle-CSV-Lines auf die SD.
//
// Adapted from JCMK ESP32DualBandWardriver, MIT License,
// Copyright (c) 2025 Just Call Me Koko.
// Marauder integration: Phase 3, 2026-05-06.
//
// Komplett unter `#ifdef MARAUDER_CORE_MODE` Guard. Default-Build ohne Toggle
// erzeugt keine Object-Code-Bytes aus diesem Header.

#pragma once

#ifndef WardriveCore_h
#define WardriveCore_h

#include "configs.h"

#ifdef MARAUDER_CORE_MODE

#include <Arduino.h>
#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "WardriveCoreProtocol.h"

// Defaults, falls in configs.h nicht ueberschrieben.
#ifndef WARDRIVE_CORE_CHANNEL
  #define WARDRIVE_CORE_CHANNEL 6
#endif
#ifndef WARDRIVE_CORE_MAX_NODES
  // Defensiver Default nur falls configs.h ihn NICHT setzt. configs.h definiert
  // unter MARAUDER_CORE_MODE bereits 12 (das gewinnt).
  //
  // ACHTUNG — 12 gilt fuer PLAINTEXT. Mit Verschluesselung bleibt die reale
  // Grenze bei 6: sendAdminToNodeSlot() stellt pro registriertem Node einen
  // DAUERHAFTEN encrypted Peer wieder her, und ESP-NOW erlaubt nur 6 davon
  // gleichzeitig (ESP_NOW_MAX_ENCRYPT_PEER_NUM). Node 7+ registriert sich zwar,
  // sein encrypted esp_now_add_peer schlaegt aber fehl. Stack-Limit, nicht unseres.
  #define WARDRIVE_CORE_MAX_NODES 12
#endif
#ifndef WARDRIVE_CORE_QUEUE_LEN
  #define WARDRIVE_CORE_QUEUE_LEN 12
#endif
#ifndef WARDRIVE_CORE_RATE_WINDOW_MS
  #define WARDRIVE_CORE_RATE_WINDOW_MS 15000
#endif
#ifndef WARDRIVE_CORE_DISPLAY_REFRESH_MS
  #define WARDRIVE_CORE_DISPLAY_REFRESH_MS 500
#endif
#ifndef WARDRIVE_CORE_NODE_TIMEOUT_MS
  #define WARDRIVE_CORE_NODE_TIMEOUT_MS 60000
#endif
#ifndef WARDRIVE_CORE_HEAP_MIN_INIT
  #define WARDRIVE_CORE_HEAP_MIN_INIT 30000
#endif
#ifndef WARDRIVE_CORE_HEAP_MIN_RUNTIME
  #define WARDRIVE_CORE_HEAP_MIN_RUNTIME 15000
#endif

// Center-Long-Press Threshold zum Beenden des Modes.
#define WARDRIVE_CORE_EXIT_HOLD_MS 2000

// Pro-Tick-Drain-Limit damit Display nicht blockiert.
#define WARDRIVE_CORE_MAX_DRAIN_PER_TICK 4

// Periodische Aufgaben.
#define WARDRIVE_CORE_STALE_CHECK_MS 1000
#define WARDRIVE_CORE_HEAP_CHECK_MS  5000
#define WARDRIVE_CORE_SD_CHECK_MS    30000

// Assignment delivery. A node heartbeats once per completed sweep, so between
// ~400 ms (small slice) and ~2.5 s (unassigned, all 40 channels) apart. The
// floor keeps a fast-sweeping node from pulling one 10-byte admin packet per
// heartbeat while it is still proving the previous one.
#define WARDRIVE_CORE_ADMIN_RESEND_MS 750
// In-slice records needed before an assignment counts as adopted. A wrong node
// contradicts itself within one sweep, so this only has to outlast the records
// still in flight from before the packet landed. Only used for nodes that do
// not send the status echo; one that does confirms in a single heartbeat.
#define WARDRIVE_CORE_ADMIN_CONFIRM_HITS 6
// Cadence of the admin packet sent to a node that is already confirmed. It
// costs 14 bytes per node per interval and buys two things nothing else does:
// the node's session state is repaired continuously instead of depending on it
// catching one broadcast, and the node gets a regular sign of life from the
// core, which is what lets it notice the core is gone and go back to the lobby
// instead of scanning into dead air forever.
#define WARDRIVE_CORE_ADMIN_KEEPALIVE_MS 15000
// Cadence of the fleet-wide session broadcast. One packet for everybody; it
// repairs lobby nodes (which park on channel 6) cheaply and is best-effort for
// collecting ones, which are covered by the per-node keepalive above.
#define WARDRIVE_CORE_SESSION_BEACON_MS 10000

// Pro-Node State-Container. Adapted from Wardriver `WiFiOps.h:80-88`,
// um `mac[6]` erweitert (Marauder nutzt volle MAC fuer Peer-Mgmt) und um
// pro-Node-Counter fuer Stats. ~36 Byte mit Padding.
struct NodeRecord {
    uint8_t  mac[6];                    // 6
    uint16_t mac_suffix;                // 2 — analog Wardriver, fuer Logging/Lookup-Fast-Path
    uint8_t  flags;                     // 1 — NODE_FLAG_ACTIVE | _ENCRYPTED | _ADMIN_DIRTY
    uint8_t  assigned_index;            // 1 — Position 0..N-1 im Cluster
    uint32_t last_seen_ms;              // 4 — letzter Heartbeat ODER TEXT
    uint32_t hb_counter;                // 4 — letzter empfangener Heartbeat-Counter (Logging)
    uint8_t  start_channel_idx;         // 1
    uint8_t  end_channel_idx;           // 1
    uint8_t  last_admin_version_sent;   // 1
    uint16_t rx_text_count;             // 2 — pro-Node-Stats (== akzeptierte Wigle-Lines)
    uint16_t rx_bad_count;              // 2
    int8_t   last_rssi;                 // 1 — RSSI des letzten Pakets dieser Node (Display)
    uint16_t rate_prev_lines;           // 2 — rx_text_count beim letzten Raten-Fenster
    uint8_t  rate_per_min;              // 1 — Lines/min, aus dem Fenster hochgerechnet

    // --- Assignment delivery ----------------------------------------------
    // An admin packet used to count as delivered because esp_now_send() returned
    // OK. That return means "ESP-NOW queued it" and nothing more -- no send
    // callback is registered anywhere in this protocol, so there is no delivery
    // signal on the wire at all. A node that misses the packet keeps its previous
    // (index, count) pair, or its boot default of "I am alone", which makes it a
    // BLE host that sweeps all 40 channels. Two nodes in that state both collect
    // BLE and the slices they believe they own stop tiling the pool. Closing that
    // is what this state is for: the flag now clears on observed adoption, not on
    // transmission.
    uint8_t  admin_confirmed_version;   // 1 — version we have evidence for; 0 = none
    uint32_t admin_last_send_ms;        // 4 — resend / keepalive rate limit
    uint8_t  admin_resend_count;        // 1 — sends since the last confirmation
    uint8_t  admin_ok_streak;           // 1 — in-slice records since send/contradiction

    // What the last admin packet to this node actually contained. The streak
    // above has to survive a retransmit: a resend fires on roughly every
    // heartbeat, and the node deduplicates per BSSID, so after the first sweep
    // of a slice there is almost nothing new left to prove itself with. Zeroing
    // the streak on every send made the target recede faster than the node
    // could reach it. It is reset when the CONTENT changes -- which is the only
    // time old evidence actually stops meaning anything.
    uint8_t  admin_sent_index;          // 1
    uint8_t  admin_sent_count;          // 1
    uint8_t  admin_sent_start;          // 1
    uint8_t  admin_sent_end;            // 1
    // Padding auf naechste 4-byte-Grenze.
};

// Queue-Slot fuer RX-Callback -> Worker. Wir kopieren Source-MAC und RSSI
// dazu, weil das info->src_addr/rssi nach Callback-Return ungueltig sein kann.
struct WardriveCoreQueueMsg {
    uint8_t          src_mac[6];
    int8_t           rssi;
    uint8_t          msg_type;          // gespiegelt aus payload[4] zur schnelleren Dispatch
    enow_text_msg_t  payload;           // 212 bytes
};

class WardriveCore {
public:
    WardriveCore();

    // Lifecycle. Nur aus dem Marauder-Loop-Kontext aufrufen, nicht aus ISR/Callback.
    void init();
    void runTick(uint32_t currentTime);
    void deinit();

    bool isRunning() const { return is_running; }

    // Repaint the whole Core Mode screen after something else has drawn over it
    // -- the session modal covers it and has to hand it back intact.
    //
    // Clearing the screen and waiting for the next refresh does not do that:
    // refreshCoreDisplay() only rewrites the values, because the frame around
    // them (bronze bar, hero tile, column headers, key hint) is painted once on
    // entry and then left alone. Clearing without repainting the frame leaves
    // numbers floating on a black screen with half the console missing, and no
    // periodic tick ever brings it back -- only leaving and re-entering
    // Core Mode does.
    void redrawScreen();

    // ---- Barrier-Session-Steuerung (aus dem Marauder-UI aufgerufen) ----
    // Der CORE startet in der LOBBY: er registriert Nodes + weist Kanaele zu,
    // aber die Nodes bleiben idle bis startSession() SESSION_CMD_START
    // broadcastet. Waehrend COLLECTING ist die Partition EINGEFROREN (neue/tote
    // Nodes reshuffeln die Flotte NICHT) — resyncSession() re-partitioniert auf
    // Befehl und zieht Spaet-Joiner rein.
    void    startSession();
    void    stopSession();
    void    resyncSession();
    bool    isCollecting() const { return collecting; }
    uint8_t getNodeCount();      // aktive registrierte Nodes (fuer Lobby-Display)

    #ifdef HAS_TOUCH
    // Touch boards (Marauder V8): the R-button session menu doesn't exist, so the
    // Start/Stop/Re-Sync actions live as on-screen buttons drawn by
    // drawTouchControls(). handleTouch() maps a tap to an action and returns true
    // ONLY if the Exit button was hit (the caller then stops the scan). Called
    // from MenuFunctions before the generic tap-to-exit so buttons aren't swallowed.
    bool    handleTouch(uint16_t x, uint16_t y);
    #endif

    // Static ESP-NOW-Recv-Callback. Schreibt RX in die Queue. Laeuft im
    // WiFi-Task-Context, daher minimal: Magic-Check, Type-Check, enqueue.
    static void onDataRecv_static(const esp_now_recv_info_t* info,
                                  const uint8_t* data,
                                  int len);

private:
    // ---- Node-Tabelle / Topologie ----
    NodeRecord node_table[WARDRIVE_CORE_MAX_NODES];

    // touchNode: findet/legt Slot fuer MAC an. Returns slot index, oder -1
    // bei Hard-Reject (alle Slots belegt). isNewNode wird gesetzt wenn
    // ein neuer Slot allokiert wurde.
    int touchNode(const uint8_t* mac, bool& isNewNode);
    int findNodeByMacSuffix(uint16_t suffix);
    int findNodeByMac(const uint8_t* mac);
    int allocateNodeSlot(const uint8_t* mac);

    bool removeStaleNodes();             // returns true bei Aenderung
    void recalculateChannelAssignments();
    void markAllActiveNodesAdminDirty();
    void handleNodeTopologyChange();
    uint8_t getActiveNodeCount();

    // ---- Assignment delivery ----
    // Everything a node needs when it checks in: read back what it says it is
    // running, re-arm it if that is not what we handed out, and correct its
    // session state. Called from the heartbeat and core-request paths, which
    // are the two moments a node is provably sitting on channel 6.
    void serviceNodeCheckin(uint8_t slot, const uint8_t* src_mac,
                            const enow_text_msg_t& hb);

    // The node status echo out of a heartbeat payload, or nullptr when the
    // sender is a stock node that does not send one.
    static const enow_node_status_t* nodeStatusFromHeartbeat(const enow_text_msg_t& hb);

    // Send the admin packet to a node that still owes us evidence, or that is
    // due its keepalive, rate limited.
    void maybeResendAdmin(uint8_t slot, const uint8_t* dest_mac);

    // Unicast session command to one node. Used for a node that is registered
    // but outside the frozen partition: it must be told to stay idle, and it
    // must NOT be sent an admin packet, because its slice fields are still the
    // allocation defaults.
    bool sendSessionToNode(uint8_t slot, const uint8_t* dest_mac, uint8_t command);

    // What this slot should be doing right now. A node without a slice is told
    // to stay in the lobby even during a session -- it has nothing to sweep.
    uint8_t desiredSessionFor(uint8_t slot) const;

    // Make every node's next check-in deliver the new session state.
    void rearmAllNodesOnSessionChange();

    // Read a node's assignment back out of the data it sends. Every wardrive
    // record carries the channel it was seen on, so a node reporting outside its
    // slice -- or reporting BLE while not being the elected host -- is running an
    // assignment we did not give it. Fallback for a stock node, which sends no
    // status echo; it can only say anything while records are flowing, i.e.
    // never in the lobby.
    void observeAssignmentEvidence(uint8_t slot, const enow_text_msg_t& msg);

    // Index of a channel number in scan_channels[], or 0xFF if it is not one.
    static uint8_t scanIndexOfChannel(uint8_t channel);

    // Slot that should be collecting BLE: the highest assigned index among the
    // nodes that are actually here, not the highest index in the partition.
    // 0xFF when there is nobody to elect.
    uint8_t bleHostSlot() const;

    // True when this slot is that one. Also the predicate the node's records are
    // judged against in observeAssignmentEvidence().
    bool slotIsBleHost(uint8_t slot) const;

    // Re-run the election and, if it moved, make sure both the outgoing and the
    // incoming host hear about it at the next opportunity rather than at the
    // next keepalive.
    void refreshBleHostElection();

    // Advance session_epoch, skipping 0. Called where a drive begins.
    void bumpSessionEpoch();

    // ---- ESP-NOW-Send ----
    bool sendCoreReply(const uint8_t* destMac);
    bool sendAdminToNodeSlot(uint8_t slot, const uint8_t* dest_mac);
    bool addPeerWithMode(const uint8_t* mac, bool encrypt, const uint8_t lmk16[16]);
    // MSG_SESSION an alle Nodes (FF:FF:...). `repeats` trades airtime for the
    // chance of being heard: an operator-driven state change is worth five
    // tries, the periodic beacon is not.
    bool broadcastSession(uint8_t command, uint8_t repeats = 5);

    // Channel-6-fix Workaround: Promisc-on/set_channel/Promisc-off.
    // Adapted from Wardriver WiFiOps.cpp:573-593.
    void setFixedChannel(uint8_t ch);

    // ---- Crypto ----
    // SHA-256-derive aus String, erste 16 Bytes. Adapted from Wardriver
    // WiFiOps.cpp:1108-1118.
    static void derive_key_16(const String& s, uint8_t out16[16]);
    void computeKeysFromUserKey();

    // ---- Wigle-Line ----
    // Compose 11-Feld-Wigle-Line aus 6-Feld-Node-Text plus GPS.
    // Adapted from Wardriver WiFiOps.cpp:821-980 (parseWardriveLine + Compose).
    String composeWigleLineFromNodeText(const enow_text_msg_t& msg);

    // ---- Display ----
    void drawCoreModeFrame();            // Init-Once-Layout
    void refreshCoreDisplay();           // Periodic-Refresh
    void drawRigBar();                   // bronze header: wordmark + satellites + battery
    void updateRates(uint32_t now);      // roll the lines/min counters
    #ifdef HAS_TOUCH
    void drawTouchControls();            // on-screen Start/Stop/Re-Sync/Exit bar (V8)
    #endif

    // ---- Helpers ----
    static uint16_t macToSuffix(const uint8_t* mac);
    void updateLastRx(int slot, int8_t rssi);

    // ---- State ----
    QueueHandle_t rx_queue;
    bool          is_running;
    bool          use_encryption;

    // Barrier-Session: false = Lobby (registrieren+zuweisen, nicht loggen),
    // true = Collecting (Partition eingefroren, Nodes sammeln). bcast_peer_ready
    // = FF:FF:...-Peer fuer Session-Broadcasts einmalig angelegt.
    bool          collecting;
    bool          bcast_peer_ready;
    uint8_t       pmk[16];
    uint8_t       lmk[16];
    String        user_key;              // aus Settings, leer = no-encrypt
    uint8_t       assignment_version;

    // Fleet size the current partition was computed for. Frozen together with
    // the per-node assigned_index, and it has to be: a node derives its BLE
    // role from the pair as
    //     ble_host = (node_count <= 1) || (node_index == node_count - 1)
    // so the two values only mean anything together. Sending a live count next
    // to a frozen index lets the election move on its own -- one node drops,
    // every remaining node recomputes, and the BLE host silently becomes a
    // different node than the one the console is pointing at. Worse, since the
    // count used to be read per packet, two nodes of the *same* partition could
    // receive different counts and both conclude they were the host, or neither.
    // 0 means "no partition yet".
    //
    // The BLE election no longer rides on this pair alone: the CORE names the
    // host outright in the admin tail (ADMIN_EXT2_FLAG_BLE_HOST), because
    // "highest index in the partition" answers the wrong question the moment
    // that node goes quiet -- its slot is reserved, the count stays put, and the
    // index it names belongs to nobody. The pair is still sent, and still means
    // what it says, for nodes built before that tail existed.
    uint8_t       partition_node_count;

    // Slot currently elected to collect BLE, as last handed out. Kept so a
    // change of host can be pushed at once rather than waiting out the
    // keepalive -- until the new host is told, nobody in the fleet scans BLE.
    // 0xFF = nobody.
    uint8_t       ble_host_slot;

    // Bumped on every startSession()/resyncSession(), sent in the admin tail. A
    // node empties its dedup ring when this changes, which the session command
    // cannot express on its own: keepalives repeat START for the whole drive.
    // Never 0 once a session has started; 0 is the node's "never been told".
    uint8_t       session_epoch;

    // Counter fuer Display + Stats.
    uint32_t total_rx_lines;             // alle akzeptierten Wigle-Lines
    uint32_t total_rx_wifi;
    uint32_t total_rx_ble;
    uint32_t total_rx_bad;               // malformed packets (post-magic-check)
    uint32_t total_rx_drops;             // Queue-overflows
    uint32_t low_heap_events;
    uint32_t buffer_overruns;            // TODO: hooked from Buffer if it exposes overrun stats

    // Last-RX fuer "letzter Node"-Display-Feld.
    int      last_rx_node_idx;
    uint32_t last_rx_ms;
    int8_t   last_rx_rssi;

    // Rolling throughput. Every WARDRIVE_CORE_RATE_WINDOW_MS the deltas since the
    // last window are scaled to lines/min (global + per node) for the display.
    uint32_t rate_window_ms;
    uint32_t rate_prev_total;
    uint16_t rate_lines_per_min;

    // Node-table render cache. Row chrome (zebra panel, status stripe, node id)
    // is only repainted when a row's identity changes — redrawing it on every
    // 500 ms refresh would visibly flicker.
    uint8_t  drawn_row_count;
    uint8_t  drawn_pitch;
    uint16_t drawn_sig[WARDRIVE_CORE_MAX_NODES];

    // Periodic-Tick-Tracking.
    uint32_t session_start_ms;
    uint32_t last_display_refresh_ms;
    uint32_t last_stale_check_ms;
    uint32_t last_heap_check_ms;
    uint32_t last_sd_check_ms;
    uint32_t last_session_beacon_ms;

    // Center-long-press detection. Pin C_BTN ist auf V7 GPIO 34. Wir reusen
    // den existing `c_btn`-Switches-Wrapper aus dem .ino — daher kein
    // direktes GPIO-Polling.
    uint32_t center_press_start_ms;
    bool     center_was_pressed;
    // Edge latch for the BACK key, so a still-held ESC carried in from the menu
    // that opened this mode cannot immediately close it again.
    bool     back_was_pressed;

    // SD-Health. Re-armed on every Rig Mode entry and by the periodic check --
    // it used to be a one-way latch, so a single bad trip silenced logging for
    // the rest of the power cycle while the screen kept counting lines in. The
    // header bar now says so out loud.
    bool     sd_healthy;

    // Gesetzt, sobald das Session-Log wirklich angelegt wurde. init() legt
    // keine Datei mehr an, damit blosses Betreten von Rig Mode nichts auf der
    // Karte hinterlaesst; ensureLogOpen() holt das beim Session-Start nach.
    bool     log_open = false;
    void     ensureLogOpen();
};

// Globale Instanz wird in esp32_marauder.ino unter MARAUDER_CORE_MODE definiert.
extern WardriveCore wardrive_core_obj;

#endif // MARAUDER_CORE_MODE
#endif // WardriveCore_h
