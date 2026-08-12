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
// Everything behind this AP is sensitive: the card carries the complete
// wardrive movement history and /wdgwars.txt with the upload credentials in
// cleartext. The rig also runs unattended in a vehicle. So the AP is not the
// only gate — every HTTP route sits behind HTTP authentication as well, and
// neither the PSK nor the HTTP password is a compile-time constant.
//
// Defaults:
//   SSID:     warroom-rig-Files-XXXX  (XXXX = last 2 bytes of STA MAC, hex)
//   Password: 12 random characters, generated on first use and kept in NVS
//   Login:    user "rig" + 10 random characters, same storage
//   IP:       192.168.4.1
// Both generated secrets are printed on the TFT for the whole session — that
// is where the operator reads them. They are deliberately NOT derived from
// the MAC: the AP broadcasts its BSSID in every beacon, so anything derived
// from the MAC is computable by a passive listener and would be no secret at
// all.
//
// HTTP routes (all of them require authentication):
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
//     pass=My-Custom-Pw    (>= 8 chars, else the generated PSK is used)
//     user=my-login        (HTTP user, default "rig")
//     httppass=my-secret   (HTTP password, else the generated one is used)
//     auth=digest|basic    (default digest; basic only exists as an escape
//                           hatch for a browser that cannot do digest, and
//                           leaks the password to anyone else on the AP)
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

    // Longest "Last: ..." path kept for the display, terminator included.
    static const size_t LAST_PATH_MAX = 40;

    void init();
    void runTick();
    void deinit();
    bool isActive() const { return active; }

private:
    bool active = false;
    State state = State::IDLE;

    String ssid;
    String password;
    String http_user;
    String http_password;
    bool http_auth_digest = true;
    String last_error_msg;

    AsyncWebServer* server = nullptr;

    uint32_t last_display_refresh_ms = 0;
    uint32_t center_press_start_ms = 0;
    bool center_was_pressed = false;
    bool back_was_pressed = false;   // edge state for the BACK-key exit

    uint32_t total_get_count = 0;
    uint32_t total_dl_count = 0;
    uint32_t total_rm_count = 0;

    // Written by the route handlers (AsyncTCP task), read by renderDisplay()
    // (loop task). This used to be an Arduino String, which is a
    // use-after-free across those two tasks: String::operator= frees the old
    // buffer before the new pointer is published, so the 1 Hz display refresh
    // could dereference heap the HTTP task had just returned. Fixed storage
    // never moves, and the accessors below serialise the two sides.
    char last_request_path[LAST_PATH_MAX] = {0};

    // Lifecycle helpers.
    void loadOptionalSettings();
    void ensureSecrets();        // fill in whatever /fileserver.txt did not
    bool startSoftAP();
    void registerRoutes();
    void configureAuth();
    void drawStaticFrame();      // one-shot — title + constant labels
    void renderDisplay();        // periodic — dynamic values only
    void handleCenterLongPressForExit();

    void setLastRequestPath(const char* p);
    void copyLastRequestPath(char* dst, size_t n) const;

    // Path safety: any incoming `path` query arg must start with `/` and
    // must not contain `..` segments. Returns "" on rejection.
    static String sanitizePath(const String& raw);
};

extern FileServerAP file_server_ap_obj;

#endif  // MARAUDER_FILE_SERVER_AP
#endif  // FileServerAP_h
