#pragma once

#ifndef FileServerAP_h
#define FileServerAP_h

#include "configs.h"

#ifdef MARAUDER_FILE_SERVER_AP

#include <Arduino.h>

// Forward-decl: full ESPAsyncWebServer include is heavy; the impl includes
// it. Keep the header light to avoid pulling AsyncTCP into every TU.
class AsyncWebServer;

// =========================================================================
// FileServerAP
// =========================================================================
// Spins up a WPA2 SoftAP and a small HTTP server that lists / downloads /
// deletes any file on the SD card. Designed for ad-hoc field extraction of
// wardrive_core_*.log and any other artefacts (PCAPs, settings, etc.) the
// user has accumulated, without popping the microSD.
//
// Defaults:
//   SSID:     warroom-rig-Files-XXXX  (XXXX = last 2 bytes of STA MAC, hex)
//   Password: warroomrig              (changeable via /fileserver.txt on SD)
//   IP:       192.168.4.1
//
// HTTP routes:
//   GET  /              HTML directory listing of "/"
//   GET  /ls?path=/dir  HTML directory listing of an arbitrary dir
//   GET  /dl?path=/p    File download (Content-Disposition: attachment)
//   POST /rm?path=/p    Delete a file (no confirmation step, intentional —
//                       this is an on-prem field tool, not a public API)
//
// Mode-exclusive: started via the wifi-scan dispatcher. Pre-empts any
// currently-running WiFi mode (the dispatcher tears those down first).
// Holding Center for >= the exit-hold threshold exits the AP and returns
// to the menu.
//
// Optional override file /fileserver.txt on SD (plain key=value):
//     ssid=My-Custom-SSID
//     pass=My-Custom-Pw  (>= 8 chars, else default is used)
// File is read once at init(); missing values fall back to the defaults
// above.
// =========================================================================

class FileServerAP {
public:
    enum class State : uint8_t {
        IDLE,
        INIT,
        AP_FAILED,
        AP_UP,
        DONE,
    };

    void init();
    void runTick();
    void deinit();
    bool isActive() const { return active; }

private:
    bool active = false;
    State state = State::IDLE;

    String ssid;
    String password;
    String last_error_msg;

    AsyncWebServer* server = nullptr;

    uint32_t last_display_refresh_ms = 0;
    uint32_t center_press_start_ms = 0;
    bool center_was_pressed = false;

    uint32_t total_get_count = 0;
    uint32_t total_dl_count = 0;
    uint32_t total_rm_count = 0;
    String last_request_path;

    // Lifecycle helpers.
    void loadOptionalSettings();
    bool startSoftAP();
    void registerRoutes();
    void drawStaticFrame();      // one-shot — title + constant labels
    void renderDisplay();        // periodic — dynamic values only
    void handleCenterLongPressForExit();

    // Path safety: any incoming `path` query arg must start with `/` and
    // must not contain `..` segments. Returns "" on rejection.
    static String sanitizePath(const String& raw);
};

extern FileServerAP file_server_ap_obj;

#endif  // MARAUDER_FILE_SERVER_AP
#endif  // FileServerAP_h
