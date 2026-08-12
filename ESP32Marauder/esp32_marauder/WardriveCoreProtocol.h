// WardriveCoreProtocol.h
//
// Wire-Protocol fuer Marauder v7 Core Mode <-> ESP32DualBandWardriver Nodes.
// Header-only. Byte-exakt kompatibel zu ESP32DualBandWardriver v2.2.0.
//
// Adapted from JCMK ESP32DualBandWardriver (src/WiFiOps.h:53-69, src/WiFiOps.cpp:4,44-74),
// MIT License, Copyright (c) 2025 Just Call Me Koko.
//
// Marauder Integration: 2026-05-06, Phase 3.
// Guarded by MARAUDER_CORE_MODE — kein Code wenn Toggle aus.

#pragma once

#ifndef WardriveCoreProtocol_h
#define WardriveCoreProtocol_h

#include "configs.h"

#ifdef MARAUDER_CORE_MODE

#include <stdint.h>

// "ENOW" Magic, exakt 4 Bytes, kein NUL.
// Wardriver-Quelle: WiFiOps.cpp:4
static const char ENOW_MAGIC[4] = { 'E', 'N', 'O', 'W' };

// Maximale TEXT-Payload-Laenge (ohne NUL). Wardrive-Lines werden hier gecappt.
// Wardriver-Quelle: configs.h:53
#ifndef ENOW_TEXT_MAX
  #define ENOW_TEXT_MAX 200
#endif

// MsgType Enum. Werte byte-exakt aus Wardriver `WiFiOps.cpp:68-74`.
enum WardriveCoreMsgType : uint8_t {
    MSG_CORE_REQUEST = 1,
    MSG_CORE_REPLY   = 2,
    MSG_HEARTBEAT    = 3,
    MSG_TEXT         = 4,
    MSG_ADMIN        = 5,
    MSG_SESSION      = 6   // Core->Node session control (Lobby <-> Collecting)
};

// Session-Commands (payload `command` in enow_session_msg_t).
#define SESSION_CMD_STOP   0   // Node -> Lobby/idle: NICHT scannen/senden
#define SESSION_CMD_START  1   // Node -> Collecting: zugewiesenen Slice scannen+senden

// 212-Byte Struct fuer Types 1, 2, 3, 4. Wardriver sendet immer die volle
// Struct-Groesse (auch fuer CORE_REQUEST/REPLY/HEARTBEAT, wo `text` und `len`
// ungenutzt sind). RX prueft strikt sizeof.
//
// Wardriver-Quelle: WiFiOps.h:53-59
typedef struct __attribute__((packed)) {
    char     magic[4];                     // "ENOW", kein NUL
    uint8_t  type;                         // siehe WardriveCoreMsgType
    uint32_t counter;                      // little-endian, nur fuer MSG_HEARTBEAT relevant
    uint16_t len;                          // little-endian, nur fuer MSG_TEXT
    char     text[ENOW_TEXT_MAX + 1];      // +1 fuer NUL-Termination beim Sender
} enow_text_msg_t;

// 10-Byte Struct fuer Type 5 (MSG_ADMIN). Core->Node Channel-Assignment.
// `start_channel_idx`/`end_channel_idx` sind INDIZES in scan_channels[],
// keine Channel-Nummern.
//
// Wardriver-Quelle: WiFiOps.h:61-69
typedef struct __attribute__((packed)) {
    char    magic[4];
    uint8_t type;                          // MSG_ADMIN = 5
    uint8_t assignment_version;
    uint8_t node_index;
    uint8_t node_count;
    uint8_t start_channel_idx;
    uint8_t end_channel_idx;
} enow_admin_msg_t;

// 6-Byte Struct fuer Type 6 (MSG_SESSION). Core->Node Session-Steuerung:
// die Node sammelt erst NACH einem SESSION_CMD_START, davor bleibt sie in der
// Lobby (registriert + zugewiesen, aber idle). Broadcast an alle Nodes.
typedef struct __attribute__((packed)) {
    char    magic[4];
    uint8_t type;                          // MSG_SESSION = 6
    uint8_t command;                       // SESSION_CMD_STOP / SESSION_CMD_START
} enow_session_msg_t;

// ---------------------------------------------------------------------------
// warroom-rig extensions
//
// Both of the following ride inside packets the stock wardriver already
// accepts, so a fleet of unmodified nodes keeps working byte for byte:
//
//  * The node status echo lives in the `text` payload of a MSG_HEARTBEAT, which
//    the stock protocol sends full-size and leaves zeroed. Nothing reads it
//    today -- upstream logs only the counter -- so filling it changes no
//    packet size and no existing behaviour.
//  * The admin extension is appended AFTER the 10 stock bytes. The node's
//    MSG_ADMIN handler bounds itself with `len < sizeof(enow_admin_msg_t)` and
//    then casts, i.e. it accepts a longer frame and ignores the tail
//    (verified in ESP32DualBandWardriver/src/WiFiOps.cpp, MSG_ADMIN branch).
//
// Both carry a two-byte tag, because "all zeroes" is exactly what a stock node
// puts in the heartbeat payload and we must never read that as a status report.
// ---------------------------------------------------------------------------

#define ENOW_EXT_TAG0 'W'
#define ENOW_EXT_TAG1 'R'

// Node -> Core, carried in enow_text_msg_t.text of a MSG_HEARTBEAT, with
// `len` set to sizeof(enow_node_status_t).
//
// This is what makes an assignment verifiable. Until now the core could only
// infer adoption from the wardrive records a node sends, which means it can
// learn nothing at all in the lobby -- where the operator is looking at the
// fleet and deciding whether to start. The heartbeat is the one moment a node
// is provably on channel 6 and talking, so it is where it should say what it
// currently believes it was told.
#define ENOW_NODE_STATUS_VER 1

#define NODE_STATUS_FLAG_COLLECTING 0x01   // node believes a session is running
#define NODE_STATUS_FLAG_ASSIGNED   0x02   // node has adopted an admin packet
#define NODE_STATUS_FLAG_2G4_ONLY   0x04   // radio cannot tune 5 GHz at all

typedef struct __attribute__((packed)) {
    char    tag[2];                        // ENOW_EXT_TAG0/1
    uint8_t struct_version;                // ENOW_NODE_STATUS_VER
    uint8_t assignment_version;            // version the node holds; 0 = none
    uint8_t node_index;
    uint8_t node_count;
    uint8_t start_channel_idx;
    uint8_t end_channel_idx;
    uint8_t flags;                         // NODE_STATUS_FLAG_*
} enow_node_status_t;

// Core -> Node, MSG_ADMIN with a tail. The session command travels with the
// assignment because those are the two things a node that just rebooted, or
// that missed a broadcast, needs before it is useful again -- and the admin
// packet is already sent unicast at a moment the node is listening.
#define ENOW_ADMIN_EXT_VER 1

typedef struct __attribute__((packed)) {
    enow_admin_msg_t base;                 // the 10 stock bytes, unchanged
    char    tag[2];                        // ENOW_EXT_TAG0/1
    uint8_t struct_version;                // ENOW_ADMIN_EXT_VER
    uint8_t session;                       // SESSION_CMD_STOP / SESSION_CMD_START
} enow_admin_ext_msg_t;

// Channel-Tabelle. Byte-exakt aus Wardriver `WiFiOps.cpp:44-53` portiert.
// 14 x 2.4 GHz + 26 x 5 GHz = 40 Eintraege.
// Auffaellige Luecke 5GHz: 104+108 fehlen zwischen 100 und 112. Reihe endet bei 177.
//
// Wardriver-Quelle: WiFiOps.cpp:44-53
static const uint8_t scan_channels[40] = {
    // 2.4 GHz (14 Eintraege)
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    // 5 GHz (26 Eintraege)
    36, 40, 44, 48,
    52, 56, 60, 64,
    100, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165, 169, 173, 177
};

// NUM_SCAN_CHANNELS = sizeof(scan_channels) — als Macro fuer compile-time.
#define NUM_SCAN_CHANNELS ((uint8_t)(sizeof(scan_channels) / sizeof(scan_channels[0])))

// Last 2.4-GHz entry in scan_channels[] (channel 14); everything above is
// 5 GHz. Needed whenever a slice has to stay inside a band, not just under the
// WARDRIVE_2_4_ONLY build.
#define WARDRIVE_CORE_2G4_END_IDX 13

// Compile-time Sanity-Checks: Struct-Groessen muessen stimmen, sonst ist die
// Wire-Kompatibilitaet zur Node gebrochen.
static_assert(sizeof(enow_text_msg_t)    == 212, "enow_text_msg_t must be 212 bytes (Wardriver-compatible)");
static_assert(sizeof(enow_admin_msg_t)   == 10,  "enow_admin_msg_t must be 10 bytes (Wardriver-compatible)");
static_assert(sizeof(enow_session_msg_t) == 6,   "enow_session_msg_t must be 6 bytes (Node-compatible)");
// The extensions must not disturb the stock layout: the admin tail sits behind
// the 10 bytes a stock node reads, and the status echo has to fit in the text
// payload it is carried in.
static_assert(sizeof(enow_admin_ext_msg_t) == 14, "enow_admin_ext_msg_t must be 10 stock bytes + 4");
static_assert(sizeof(enow_node_status_t)   <= ENOW_TEXT_MAX,
              "enow_node_status_t must fit in the heartbeat text payload");

// NodeRecord-Flags. Aus Wardriver `WiFiOps.h:49-51`.
#define NODE_FLAG_ACTIVE       0x01
#define NODE_FLAG_ENCRYPTED    0x02
#define NODE_FLAG_ADMIN_DIRTY  0x04
// Set by recalculateChannelAssignments() on every slot it hands a slice to, and
// cleared when a slot is allocated. A node that registers mid-session is not in
// the frozen partition -- the fleet is not re-partitioned while collecting, by
// design -- so its slice fields are still the allocation defaults of index 0 and
// the full 0..39 range. Sending those out tells a late joiner it owns every
// channel, on top of the nodes that actually do. It stays unassigned until the
// next resyncSession() brings it into a partition. [warroom-rig]
#define NODE_FLAG_PARTITIONED  0x08
// Slot whose node went quiet DURING a session. The partition is frozen while
// collecting, so wiping the slot would cost that node its place in it: when the
// node comes back -- a reboot, a minute behind a hill -- it would register as a
// late joiner with no slice and stay dark for the rest of the drive. Reserved
// slots keep the MAC and the slice and are reclaimed by that same MAC, so a
// dropout costs only the time it was away. They are cleared whenever the
// partition is recomputed. [warroom-rig]
#define NODE_FLAG_RESERVED     0x10
// Node has told us its radio cannot tune 5 GHz (status echo). Slices for these
// nodes must stay inside the 2.4-GHz part of scan_channels[]. [warroom-rig]
#define NODE_FLAG_2G4_ONLY     0x20
// Node sends the status echo in its heartbeats, i.e. it can confirm or deny an
// assignment on its own. Without this the only evidence is wardrive records,
// which never arrive in the lobby -- so it also decides whether "unconfirmed"
// means "the node disagrees" or "we cannot see yet". [warroom-rig]
#define NODE_FLAG_REPORTS_STATUS 0x40

#endif // MARAUDER_CORE_MODE
#endif // WardriveCoreProtocol_h
