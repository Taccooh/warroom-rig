#pragma once

#ifndef WdgwarsUpload_h
#define WdgwarsUpload_h

#include "configs.h"

#ifdef MARAUDER_WDGWARS_UPLOAD

#include <Arduino.h>
#include <FS.h>
#include <WiFiClientSecure.h>
// Note: in ESP32 Arduino-Core 3.x, WiFiClientSecure is a typedef of
// NetworkClientSecure, so a `class WiFiClientSecure;` forward-decl does
// not work. We include the full header instead.

// =========================================================================
// WdgwarsUpload
// =========================================================================
// Phase-5 module: bulk-uploads finished Wigle-CSV wardrive logs from the SD
// card to https://wdgwars.pl/api/upload-csv via X-API-Key auth, then renames
// each successfully uploaded file to "<name>.uploaded" so subsequent runs
// skip it.
//
// Mode-exclusive: requires Wardrive Core (or any other WiFi mode) to be
// terminated first. The class itself does not check for ESP-NOW state; the
// dispatcher in WiFiScan::main() guarantees clean entry.
//
// Settings live in /wdgwars.txt on SD, plain-text key=value pairs:
//     ssid=<HomeAP_SSID>
//     pass=<HomeAP_password>
//     apikey=<64-char hex from wdgwars profile>
// File is loaded once at init(); init() bails out cleanly if any field is
// missing, displaying an error.
//
// TLS: uses WiFiClientSecure with two embedded Let's Encrypt root CAs
// (ISRG Root X1 + X2). No setInsecure() — strict cert validation.
// =========================================================================

class WdgwarsUpload {
public:
    enum class State : uint8_t {
        IDLE,
        INIT,
        LOAD_SETTINGS,
        SETTINGS_ERROR,
        SCAN_FILES,
        NO_FILES,
        SELECTING,        // on-device pick list (newest first) before uploading
        CONNECTING_AP,
        AP_FAILED,
        UPLOADING,
        DONE,
    };

    void init();
    void runTick();
    void deinit();
    bool isActive() const { return active; }

private:
    bool active = false;
    State state = State::IDLE;
    State displayed_state = State::IDLE;   // last state we drew, for render-on-change

    // Settings (loaded from /wdgwars.txt).
    String ssid;
    String pass;
    String apikey;

    // Pending upload queue. Fixed cap to keep memory bounded;
    // WDGWARS_MAX_FILES_PER_RUN limits how many files we upload in one session.
    // Show only the newest MAX_FILES logs (kept by highest log number); older
    // ones are not worth listing. The scan reads the whole directory but retains
    // just these newest entries, so the list is always current, not first-found.
    static const uint8_t MAX_FILES = 32;
    String pending_files[MAX_FILES];
    uint32_t file_seq[MAX_FILES] = {0};    // trailing log number, for newest-first sort
    uint32_t file_size[MAX_FILES] = {0};   // byte size, shown in the pick list
    bool file_selected[MAX_FILES] = {false};
    uint8_t pending_count = 0;
    uint8_t current_idx = 0;

    // On-device selection UI state.
    uint8_t sel_cursor = 0;   // highlighted row
    uint8_t sel_top = 0;      // first visible row (scroll window)
    uint8_t upload_total = 0; // number of selected files, set when upload starts

    // Per-session counters (display).
    uint16_t ok_count = 0;
    uint16_t err_count = 0;
    uint64_t total_bytes_uploaded = 0;

    // HTTP-response sticky-fields for the last attempt (for display on error).
    int last_http_code = 0;
    String last_error_msg;

    // Display + button-tracking.
    uint32_t last_display_refresh_ms = 0;
    uint32_t center_press_start_ms = 0;
    bool center_was_pressed = false;
    bool prompt_confirmed = false;     // user pressed Center to confirm upload
    bool waiting_for_confirm = false;  // prompt is up

    // Phase steps.
    bool loadSettings();
    bool scanForUnsyncedFiles();
    bool connectToAP();
    bool uploadOneFile(const String& path);
    void renameToUploaded(const String& path);

    // Multipart helpers.
    bool sendMultipartPOST(WiFiClientSecure& client,
                           const String& path,
                           File& file,
                           size_t file_size);
    int  parseHttpStatus(WiFiClientSecure& client);

    // UI.
    void renderDisplay();
    void renderConfirmPrompt();
    void renderSelectList();
    void runSelectionModal();   // blocking pick UI (owns the buttons like Core Mode)
    void handleCenterLongPressForExit();
};

extern WdgwarsUpload wdgwars_upload_obj;

#endif  // MARAUDER_WDGWARS_UPLOAD
#endif  // WdgwarsUpload_h
