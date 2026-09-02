#pragma once
#ifndef WiFiOps_h
#define WiFiOps_h

#include "configs.h"
#include "utils.h"
#include "settings.h"
#include "GpsInterface.h"
#include "Buffer.h"
#include "display.h"
#include "SDInterface.h"

#include <esp_now.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "mbedtls/sha256.h"

#include <NimBLEDevice.h> // 2.3.0

extern GpsInterface gps;
extern SDInterface sd_obj;
extern Buffer buffer;
extern Utils utils;
extern Settings settings;
extern Display display;

extern WebServer server;

// Upload types
#define WIGLE_UPLOAD 0
#define WDG_UPLOAD   1
#define BOTH_UPLOAD  2

#define WIFI_STANDBY    0
#define WIFI_WARDRIVING 1
#define WIFI_UPDATE     2

#define MAX_NODES 24
#define NODE_TIMEOUT_MS 60000
#define ADMIN_WAIT_MS 300
#define DEBUG_OUTPUT_DELAY 30000
#define LOBBY_HB_INTERVAL_MS 1500   // node lobby rendezvous heartbeat cadence

// Longest the node may go without checking in. The regular heartbeat rides the
// end of a completed scan cycle, so anything that stops cycles from completing
// also stops the node from checking in — and after NODE_TIMEOUT_MS the CORE
// drops it. In plain mode, which is the fleet default, there is no CORE_REQUEST
// loop to get back in, so that means gone until power cycle. This is the floor
// under that: the node checks in on a timer as well and can never talk itself
// out of the fleet.
#define NODE_HB_WATCHDOG_MS 5000

// How long a collecting node keeps going without a single packet from the CORE
// before it drops back to the lobby. Only armed once the node has seen the CORE
// check in on it repeatedly (see g_core_beacons in WiFiOps.cpp), so an older
// CORE that never does cannot trigger it. Without this, a CORE that goes away
// without broadcasting a STOP — Rig Mode exited, battery pulled, out of range —
// leaves the fleet scanning into dead air, filling the dedup ring with APs that
// are then missing from the start of the next session.
#define NODE_CORE_LOSS_MS 90000

#define NODE_FLAG_ACTIVE       0x01
#define NODE_FLAG_ENCRYPTED    0x02
#define NODE_FLAG_ADMIN_DIRTY  0x04

// Session control (payload `command` in enow_session_msg_t / MSG_SESSION).
#define SESSION_CMD_STOP   0   // Node -> Lobby/idle: NICHT scannen/senden
#define SESSION_CMD_START  1   // Node -> Collecting: zugewiesenen Slice scannen+senden

typedef struct __attribute__((packed)) {
  char     magic[4];               // "ENOW"
  uint8_t  type;                   // MSG_TEXT
  uint32_t counter;                // heartbeat counter (valid for MSG_HEARTBEAT)
  uint16_t len;                    // number of bytes in text (not including NUL)
  char     text[ENOW_TEXT_MAX + 1];  // +1 for NUL terminator
} enow_text_msg_t;

typedef struct __attribute__((packed)) {
  char    magic[4];
  uint8_t type;               // MSG_ADMIN
  uint8_t assignment_version;
  uint8_t node_index;
  uint8_t node_count;
  uint8_t start_channel_idx;
  uint8_t end_channel_idx;
} enow_admin_msg_t;

// Core->Node session control. The node stays in the Lobby (registered +
// assigned, but idle) until it receives SESSION_CMD_START — barrier start.
typedef struct __attribute__((packed)) {
  char    magic[4];
  uint8_t type;               // MSG_SESSION
  uint8_t command;            // SESSION_CMD_STOP / SESSION_CMD_START
} enow_session_msg_t;

// ---------------------------------------------------------------------------
// warroom-rig extensions. Mirror of WardriveCoreProtocol.h on the CORE side —
// keep the two in step.
//
// Both ride inside packets the stock wardriver already accepts: the status echo
// goes in the unused `text` payload of a heartbeat, the admin extension after
// the 10 stock admin bytes (the MSG_ADMIN handler bounds itself with
// `len < sizeof(enow_admin_msg_t)` and ignores anything past that, so a stock
// node reads a 14-byte admin as the 10 bytes it knows).
// ---------------------------------------------------------------------------

#define ENOW_EXT_TAG0 'W'
#define ENOW_EXT_TAG1 'R'

#define ENOW_NODE_STATUS_VER 1

#define NODE_STATUS_FLAG_COLLECTING 0x01   // node believes a session is running
#define NODE_STATUS_FLAG_ASSIGNED   0x02   // node has adopted an admin packet
#define NODE_STATUS_FLAG_2G4_ONLY   0x04   // radio cannot tune 5 GHz at all

// Node -> Core, carried in enow_text_msg_t.text of a MSG_HEARTBEAT with `len`
// set to sizeof(enow_node_status_t). This is what lets the CORE verify an
// assignment landed instead of assuming a queued packet was heard, and it works
// in the lobby, where the node sends no wardrive records to be judged by.
typedef struct __attribute__((packed)) {
  char    tag[2];                  // ENOW_EXT_TAG0/1
  uint8_t struct_version;          // ENOW_NODE_STATUS_VER
  uint8_t assignment_version;      // version the node holds; 0 = none
  uint8_t node_index;
  uint8_t node_count;
  uint8_t start_channel_idx;
  uint8_t end_channel_idx;
  uint8_t flags;                   // NODE_STATUS_FLAG_*
} enow_node_status_t;

#define ENOW_ADMIN_EXT_VER 1

// Core -> Node, MSG_ADMIN with a tail carrying the session command. A node that
// rebooted, or that was out of range when a START/STOP was broadcast, is put
// right by the next admin packet rather than having to wait for a Re-Sync.
typedef struct __attribute__((packed)) {
  enow_admin_msg_t base;           // the 10 stock bytes, unchanged
  char    tag[2];                  // ENOW_EXT_TAG0/1
  uint8_t struct_version;          // ENOW_ADMIN_EXT_VER
  uint8_t session;                 // SESSION_CMD_STOP / SESSION_CMD_START
} enow_admin_ext_msg_t;

// Second admin tail, appended after the first. It has its own length gate on
// both sides rather than a bumped struct_version, so the two halves of a mixed
// fleet still exchange everything they already understood: an older node reads
// the 14 bytes it knows and ignores these two, and a node built with this header
// against an older CORE finds the frame too short and falls back to the rules
// below. Neither case is worse off than before this existed.
#define ADMIN_EXT2_FLAG_BLE_HOST 0x01   // this node, and only this node, scans BLE

typedef struct __attribute__((packed)) {
  enow_admin_ext_msg_t ext1;       // the 14 bytes above, unchanged
  uint8_t flags;                   // ADMIN_EXT2_FLAG_*
  uint8_t session_epoch;           // bumped by the CORE per session; 0 = none
} enow_admin_ext2_msg_t;

// Wire compatibility is byte-for-byte or it is nothing — the CORE has the same
// assertions against the same numbers.
static_assert(sizeof(enow_admin_msg_t)      == 10, "enow_admin_msg_t must stay 10 bytes");
static_assert(sizeof(enow_session_msg_t)    == 6,  "enow_session_msg_t must stay 6 bytes");
static_assert(sizeof(enow_admin_ext_msg_t)  == 14, "enow_admin_ext_msg_t must be 10 stock bytes + 4");
static_assert(sizeof(enow_admin_ext2_msg_t) == 16, "enow_admin_ext2_msg_t must be the ext1 14 + 2");
static_assert(sizeof(enow_node_status_t)   <= ENOW_TEXT_MAX,
              "enow_node_status_t must fit in the heartbeat text payload");

struct WardriveRecord {
  String bssid;
  String essid;
  String security;
  int    channel;
  int    rssi;
  String type;
};

struct NodeRecord {
  uint16_t mac_suffix;
  uint32_t last_seen_ms;
  uint8_t assigned_index;
  uint8_t start_channel_idx;
  uint8_t end_channel_idx;
  uint8_t last_admin_version_sent;
  uint8_t flags;
};

class WiFiOps
{
  private:
    NimBLEScan* pBLEScan;

    wifi_country_t country = {
      .cc = "PH",
      .schan = 1,
      .nchan = 13,
      .policy = WIFI_COUNTRY_POLICY_AUTO,
    };

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    const char* apSSID = "c5wardriver";
    const char* apPassword = "c5wardriver";

    WiFiClientSecure *client = new WiFiClientSecure();

    String user_ap_ssid = "";
    String user_ap_password = "";
    String wigle_user = "";
    String wigle_token = "";
    String wdg_token = "";

    bool connected_as_client = false;

    uint8_t current_scan_mode;
    uint32_t init_time;
    struct mac_addr mac_history[mac_history_len];

    uint32_t current_net_count = 0;
    uint32_t current_2g4_count = 0;
    uint32_t current_5g_count = 0;
    uint32_t current_ble_count = 0;
    uint32_t total_net_count = 0;
    uint32_t total_ble_count = 0;

    // Pending BLE sightings, filled by the NimBLE discovery callback and emptied
    // by the loop task. See queueBleObservation() for why the callback is not
    // allowed to do the work itself.
    //
    // Single producer (the NimBLE host task), single consumer (the loop task),
    // so head and tail each have exactly one writer and the ring needs no lock.
    // One slot is always left empty to keep full and empty distinguishable.
    static const uint8_t ble_pending_len = 32;
    struct BlePending {
      uint8_t mac[6];
      int8_t  rssi;
    };
    BlePending ble_pending[ble_pending_len];
    volatile uint8_t ble_pending_head = 0;   // written by the consumer
    volatile uint8_t ble_pending_tail = 0;   // written by the producer
    uint32_t ble_pending_overflow = 0;

    void drainBlePending();

    // Drop the record of the last setFixedChannel(), so the next one runs its
    // full sequence again. See the comment there: what it does is a workaround,
    // not just a channel write, and it is skipped only for a repeat of a fix
    // nothing has disturbed since. Static because setFixedChannel() is.
    static void invalidateChannelFix();

    bool startNextNodeAssignedScan();
    void runAdminWindowAfterScanCycle();
    void debugPrintNodeTable();
    void handleNodeTopologyChange();
    void markAllActiveNodesAdminDirty();
    int findNodeByMacSuffix(uint16_t suffix);
    int findNodeByMac(const uint8_t* mac);
    int allocateNodeSlot(const uint8_t* mac);
    bool removeStaleNodes();
    void recalculateChannelAssignments();
    int touchNode(const uint8_t* mac, bool& isNewNode);
    uint8_t getNodeStartChannel(uint8_t slot);
    uint8_t getNodeEndChannel(uint8_t slot);
    void showCountdown();
    int runWardrive(uint32_t currentTime);
    void scanBLE();
    bool mac_cmp(struct mac_addr addr1, struct mac_addr addr2);
    void clearMacHistory();
    String security_int_to_string(int security_type);
    void processWardrive(uint16_t networks);
    void shutdownAccessPoint(bool ap_active = true);

  public:
    #ifdef CORE
      int run_mode = CORE_MODE;
    #elif defined(NODE)
      int run_mode = NODE_MODE;
    #else
      int run_mode = SOLO_MODE;
    #endif

    uint mac_history_cursor = 0;
    bool clientConnected = false;
    bool serving = false;
    uint32_t last_web_client_activity;
    uint32_t last_timer;
    bool use_encryption = false;

    uint8_t current_assignment_version = 1;
    uint8_t current_assigned_scan_idx = 0;

    // Barrier session state. Default false = Lobby: the node registers with the
    // CORE and receives its channel assignment but does NOT scan/send wardrive
    // data until the CORE issues SESSION_CMD_START. Set/cleared in OnDataRecv.
    volatile bool session_active = false;
    uint32_t last_lobby_hb_ms = 0;   // paces the lobby rendezvous heartbeat

    String esp_now_key = "";

    bool begin(bool skip_admin = false);
    void main(uint32_t currentTime);
    void startLog(String file_name);
    void initBLE();
    void initWiFi(bool set_country = false);
    void deinitBLE();
    void deinitWiFi();
    uint8_t getActiveNodeCount();
    bool tryConnectToWiFi(unsigned long timeoutMs = STATION_CONNECT_TIMEOUT);
    bool uploadToWDG(String filePath, File fileToUpload);
    bool uploadToWigle(String filePath, File fileToUpload);
    bool backendUpload(String filePath, uint8_t upload_type = WIGLE_UPLOAD);
    void setCurrentScanMode(uint8_t scan_mode);
    uint8_t getCurrentScanMode();
    void setTotalNetCount(uint32_t count);
    void setTotalBLECount(uint32_t count);
    void setCurrentNetCount(uint32_t count);
    void setCurrent2g4Count(uint32_t count);
    void setCurrent5gCount(uint32_t count);
    void setCurrentBLECount(uint32_t count);
    uint32_t getTotalNetCount();
    uint32_t getTotalBLECount();
    uint32_t getCurrentNetCount();
    uint32_t getCurrent2g4Count();
    uint32_t getCurrent5gCount();
    uint32_t getCurrentBLECount();
    bool seen_mac(unsigned char* mac);
    void save_mac(unsigned char* mac);
    // Hand a BLE sighting to the loop task. Called from the NimBLE discovery
    // callback, and deliberately the only thing that callback does.
    void queueBleObservation(const uint8_t* mac, int8_t rssi);
    void startESPNow();
    bool getHasCore();
    bool getSecureReady();
    bool getNodeReady();
    bool sendEncryptedStringToCore(const String& s);
    bool sendBroadcastStringPlain(const String& s);
    bool parseWardriveLine(const enow_text_msg_t& msg, WardriveRecord& out);
    int getAuthType(const wifi_promiscuous_pkt_t *ppkt);

    void startAccessPoint();
    void serveConfigPage();
    bool monitorAP(unsigned long timeoutMs = WEB_PAGE_TIMEOUT);

    static void setFixedChannel(uint8_t ch);
    static bool addPeerWithMode(const uint8_t* mac, bool encrypt, const uint8_t lmk16[16]);
    static void sendCoreRequest();
    static void sendCoreReply(const uint8_t* destMac);
    static bool sendAdminToNodeSlot(uint8_t slot, const uint8_t* dest_mac);
    static void sendHeartbeat();
    static void OnDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len);
    static void derive_key_16(const String& s, uint8_t out16[16]);
    static void computeKeysFromEnowKey();
    static uint16_t macToSuffix(const uint8_t* mac);
    static void macSuffixToStr(uint16_t suffix, char* out6);

};

#endif