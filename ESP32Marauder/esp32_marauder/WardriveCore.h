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

    // ---- ESP-NOW-Send ----
    bool sendCoreReply(const uint8_t* destMac);
    bool sendAdminToNodeSlot(uint8_t slot, const uint8_t* dest_mac);
    bool addPeerWithMode(const uint8_t* mac, bool encrypt, const uint8_t lmk16[16]);
    bool broadcastSession(uint8_t command);   // MSG_SESSION an alle Nodes (FF:FF:...)

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

    // Center-long-press detection. Pin C_BTN ist auf V7 GPIO 34. Wir reusen
    // den existing `c_btn`-Switches-Wrapper aus dem .ino — daher kein
    // direktes GPIO-Polling.
    uint32_t center_press_start_ms;
    bool     center_was_pressed;

    // SD-Health.
    bool     sd_healthy;
};

// Globale Instanz wird in esp32_marauder.ino unter MARAUDER_CORE_MODE definiert.
extern WardriveCore wardrive_core_obj;

#endif // MARAUDER_CORE_MODE
#endif // WardriveCore_h
