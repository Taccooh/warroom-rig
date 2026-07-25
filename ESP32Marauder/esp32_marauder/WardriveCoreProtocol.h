// WardriveCoreProtocol.h
//
// Wire-Protocol fuer Marauder v7 Core Mode <-> ESP32DualBandWardriver Nodes.
// Header-only. Byte-exakt kompatibel zu Wardriver `b674bd8`.
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

// Compile-time Sanity-Checks: Struct-Groessen muessen stimmen, sonst ist die
// Wire-Kompatibilitaet zur Node gebrochen.
static_assert(sizeof(enow_text_msg_t)    == 212, "enow_text_msg_t must be 212 bytes (Wardriver-compatible)");
static_assert(sizeof(enow_admin_msg_t)   == 10,  "enow_admin_msg_t must be 10 bytes (Wardriver-compatible)");
static_assert(sizeof(enow_session_msg_t) == 6,   "enow_session_msg_t must be 6 bytes (Node-compatible)");

// NodeRecord-Flags. Aus Wardriver `WiFiOps.h:49-51`.
#define NODE_FLAG_ACTIVE       0x01
#define NODE_FLAG_ENCRYPTED    0x02
#define NODE_FLAG_ADMIN_DIRTY  0x04

#endif // MARAUDER_CORE_MODE
#endif // WardriveCoreProtocol_h
