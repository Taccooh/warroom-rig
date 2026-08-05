#include "WdgwarsUpload.h"

#ifdef MARAUDER_WDGWARS_UPLOAD

#include <Arduino.h>
#include <SD.h>
#include <FS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "Display.h"
#include "SDInterface.h"
#include "WiFiScan.h"
extern WiFiScan wifi_scan_obj;   // to reset currentScanMode on self-exit (see deinit)

#ifdef HAS_BUTTONS
    #include "Switches.h"
    extern Switches c_btn;
#endif

extern Display display_obj;
extern SDInterface sd_obj;

// Global instance is defined in esp32_marauder.ino (consistent with the
// WardriveCore module pattern). The header re-declares it `extern`.

// =========================================================================
// Constants
// =========================================================================
static const char* WDG_HOST = "wdgwars.pl";
static const uint16_t WDG_PORT = 443;
// v2 is the generation the platform's own reference firmware talks to: upstream
// ESP32Marauder has POSTed here since it gained wdgwars support (v1.12.3), and
// still does in v1.14.1. The legacy /api/upload-csv route is what Bruce-era
// integrations used; both still authenticate, but only v2 is actively shipped
// against, so that is the one to be on.
static const char* WDG_PATH = "/api/v2/upload-csv";

// Multipart boundary — fixed, no need for randomness because it cannot
// collide with WigleWifi-1.6 CSV bytes (the format never emits "--marauder...").
static const char* WDG_BOUNDARY = "marauderboundary7f3a9c2e4b1d8f5a";

// Per-call upload window (timeouts in ms) and display/UI cadences come from
// configs.h #defines (WDGWARS_AP_TIMEOUT_MS, WDGWARS_TLS_TIMEOUT_MS,
// WDGWARS_HTTP_TIMEOUT_MS, WDGWARS_DISPLAY_REFRESH_MS, WDGWARS_EXIT_HOLD_MS)
// so a user can re-tune without touching this file.

// Upload-stream chunk size: 1 KB read from SD, written to TLS-socket.
// Larger = fewer TLS records but more transient RAM. 1 KB is the sweet
// spot for ESP32 mbedtls (default record buffer ~4 KB).
static const size_t UPLOAD_CHUNK_BYTES = 1024;

// =========================================================================
// Embedded root CAs — both issuer families Cloudflare fronts this host with
// =========================================================================
// wdgwars.pl sits behind Cloudflare, whose Universal SSL hands out edge certs
// from more than one CA and re-picks on renewal. That is precisely how this
// broke before: the pinned Let's Encrypt anchors stopped matching when the host
// came up on Google Trust Services (2026-05-31), and pinning only GTS in
// response just re-arms the same trap pointing the other way.
//
// So trust both families rather than whichever one happens to be live:
//   * GTS Root R1 (RSA) + R4 (ECDSA) — https://pki.goog/repo/
//   * ISRG Root X1 (RSA) + X2 (ECDSA) — https://letsencrypt.org/certificates/
// All four are self-signed roots valid into the 2035-2040 range, so a renewal
// or an intermediate re-anchoring inside either family stays covered.
//
// Current chain (2026-08-04): wdgwars.pl -> GTS "WE1" -> GTS Root R4.
// If uploads ever fail with "TLS connect failed" again, check what is actually
// being served — `openssl s_client -connect wdgwars.pl:443 -servername
// wdgwars.pl` — before touching anything else; a third CA family would need its
// root added here. Verify any root you add by SHA-256 fingerprint, not by URL.
static const char* WDG_CA_BUNDLE PROGMEM = R"PEM(
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo
27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w
Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw
TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl
qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH
szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8
Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk
MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92
wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p
aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN
VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb
C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe
QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy
h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4
7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J
ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef
MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/
Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT
6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ
0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm
2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb
bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw
CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg
R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00
MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT
ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw
EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW
+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9
ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T
AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI
zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW
tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1
/q4AaOeMSQ+2b1tbFfLn
-----END CERTIFICATE-----
)PEM";

// =========================================================================
// Lifecycle
// =========================================================================
void WdgwarsUpload::init() {
    Serial.println("WDG: init");
    active = true;
    state = State::INIT;
    pending_count = 0;
    current_idx = 0;
    ok_count = 0;
    err_count = 0;
    total_bytes_uploaded = 0;
    last_http_code = 0;
    last_error_msg = "";
    waiting_for_confirm = false;
    prompt_confirmed = false;
    center_press_start_ms = 0;
    center_was_pressed = false;
    last_display_refresh_ms = 0;

    #ifdef HAS_SCREEN
        display_obj.clearScreen();
        display_obj.tft.setTextSize(1);
        display_obj.tft.setTextColor(TFT_ORANGE);
        display_obj.tft.setCursor(4, 4);
        display_obj.tft.print("WDGWars Upload");
        display_obj.tft.setTextColor(TFT_WHITE);
        display_obj.tft.setCursor(4, 18);
        display_obj.tft.print("Loading settings...");
    #endif
}

void WdgwarsUpload::deinit() {
    Serial.println("WDG: deinit");
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect(true);
    }
    WiFi.mode(WIFI_OFF);

    // Wipe sensitive in-RAM material.
    apikey = "";
    pass = "";

    active = false;
    state = State::IDLE;

    // Self-complete the exit: leave the scan mode so the framework returns to the
    // menu. The idle/DONE states get away without this because MenuFunctions'
    // stop-scan handler catches CENTER between ticks — but the selection modal
    // blocks that handler, so its CENTER-hold-to-exit relied on this and froze.
    wifi_scan_obj.currentScanMode = WIFI_SCAN_OFF;

    #ifdef HAS_SCREEN
        display_obj.clearScreen();
    #endif
}

// =========================================================================
// State machine driver
// =========================================================================
void WdgwarsUpload::runTick() {
    if (!active) return;
    uint32_t now = millis();

    // Draw once per state change — never on a timer. Killing the periodic full
    // redraw stops the 500 ms flash, and rendering on the transition means
    // CONNECTING_AP/UPLOADING appear the instant the user hits GO (no more
    // "stuck on the file list, nothing happening"). SELECTING owns its own
    // (modal) drawing; the transient INIT/LOAD/SCAN states draw nothing.
    if (state != displayed_state) {
        displayed_state = state;
        switch (state) {
            case State::SETTINGS_ERROR:
            case State::NO_FILES:
            case State::CONNECTING_AP:
            case State::AP_FAILED:
            case State::UPLOADING:
            case State::DONE:
                renderDisplay();
                break;
            default:
                break;
        }
    }

    switch (state) {
        case State::INIT: {
            state = State::LOAD_SETTINGS;
            break;
        }

        case State::LOAD_SETTINGS: {
            if (loadSettings()) {
                state = State::SCAN_FILES;
            } else {
                state = State::SETTINGS_ERROR;
            }
            break;
        }

        case State::SCAN_FILES: {
            if (scanForUnsyncedFiles() && pending_count > 0) {
                sel_cursor = 0;
                sel_top = 0;
                for (uint8_t i = 0; i < pending_count; i++) file_selected[i] = false;
                // Blocking pick UI: it owns the buttons for its whole duration, so
                // the menu's stop-scan handler cannot fire mid-selection. The modal
                // sets the next state itself (CONNECTING_AP) or exits on cancel.
                runSelectionModal();
            } else {
                state = State::NO_FILES;
            }
            break;
        }

        case State::SELECTING: {
            // Normally unreached — the modal sets a concrete next state. Defensive.
            runSelectionModal();
            break;
        }

        case State::SETTINGS_ERROR:
        case State::NO_FILES:
        case State::AP_FAILED: {
            // Idle states — drawn once on entry; just wait for the exit long-press.
            handleCenterLongPressForExit();
            break;
        }

        case State::CONNECTING_AP: {
            if (connectToAP()) {
                state = State::UPLOADING;
                current_idx = 0;
            } else {
                state = State::AP_FAILED;
            }
            break;
        }

        case State::UPLOADING: {
            // Skip files the user did not select.
            while (current_idx < pending_count && !file_selected[current_idx]) current_idx++;
            if (current_idx >= pending_count) {
                if (WiFi.status() == WL_CONNECTED) {
                    WiFi.disconnect(true);
                }
                state = State::DONE;
                break;
            }
            const String& path = pending_files[current_idx];
            if (uploadOneFile(path)) {
                renameToUploaded(path);
                ok_count++;
            } else {
                err_count++;
            }
            current_idx++;
            renderDisplay();
            break;
        }

        case State::DONE: {
            // Static summary — drawn once on entry; just wait for the exit long-press.
            handleCenterLongPressForExit();
            break;
        }

        case State::IDLE: {
            // Should not reach during active session. No-op.
            break;
        }
    }

    // Confirm-prompt path: short-press CENTER = confirm, long-press = cancel.
    if (waiting_for_confirm) {
        #if defined(HAS_BUTTONS) && (C_BTN >= 0) && \
            !defined(MARAUDER_CARDPUTER) && !defined(MARAUDER_CARDPUTER_ADV)
            bool pressed_now = (digitalRead(C_BTN) == LOW);
            if (pressed_now) {
                if (!center_was_pressed) {
                    center_was_pressed = true;
                    center_press_start_ms = now;
                } else if ((now - center_press_start_ms) >= WDGWARS_EXIT_HOLD_MS) {
                    // Long-press during confirm = cancel & exit.
                    Serial.println("WDG: confirm-cancel via long-press");
                    deinit();
                    return;
                }
            } else if (center_was_pressed) {
                uint32_t held = now - center_press_start_ms;
                center_was_pressed = false;
                center_press_start_ms = 0;
                if (held < WDGWARS_EXIT_HOLD_MS) {
                    // Short tap = confirm.
                    waiting_for_confirm = false;
                    prompt_confirmed = true;
                    state = State::CONNECTING_AP;
                }
            }
        #endif
    }
}

// =========================================================================
// Settings loader
// =========================================================================
bool WdgwarsUpload::loadSettings() {
    File f = SD.open("/wdgwars.txt", FILE_READ);
    if (!f) {
        last_error_msg = "/wdgwars.txt not found on SD";
        Serial.println("WDG: " + last_error_msg);
        return false;
    }

    ssid = ""; pass = ""; apikey = "";
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;

        int eq = line.indexOf('=');
        if (eq <= 0) continue;
        String key = line.substring(0, eq);
        String val = line.substring(eq + 1);
        key.trim(); val.trim();

        if (key == "ssid")        ssid   = val;
        else if (key == "pass")   pass   = val;
        else if (key == "apikey") apikey = val;
    }
    f.close();

    if (ssid.length() == 0 || apikey.length() == 0) {
        last_error_msg = "wdgwars.txt missing ssid or apikey";
        Serial.println("WDG: " + last_error_msg);
        return false;
    }
    // pass empty is allowed (open AP).
    Serial.printf("WDG: settings ok (ssid=%s, apikey-len=%u)\n",
                  ssid.c_str(), apikey.length());
    return true;
}

// =========================================================================
// File-iter scan
// =========================================================================
// Trailing numeric sequence in "wardrive[_core]_<N>.log" — a newest-first
// tiebreaker for when the SD clock was never set and timestamps are identical.
// Higher N = created later (the log counter only ever increments). 0 if none.
static uint32_t fileSeq(const String& path) {
    int end = path.lastIndexOf(".log");
    if (end < 0) end = path.length();
    int start = end;
    while (start > 0) {
        char c = path.charAt(start - 1);
        if (c < '0' || c > '9') break;
        start--;
    }
    if (start >= end) return 0;
    return (uint32_t)path.substring(start, end).toInt();
}

bool WdgwarsUpload::scanForUnsyncedFiles() {
    pending_count = 0;
    File root = SD.open("/");
    if (!root || !root.isDirectory()) {
        last_error_msg = "SD root not readable";
        return false;
    }

    // Scan the WHOLE directory (no early cap) and keep the newest MAX_FILES by
    // log number — otherwise the first files in directory order win and the
    // newest logs (highest number) get dropped, which is exactly backwards.
    File entry = root.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            // SD-API may return "/wardrive_0.log" or "wardrive_0.log" depending
            // on driver — strip a leading slash so name starts with "wardrive".
            int slash = name.lastIndexOf('/');
            String base = (slash >= 0) ? name.substring(slash + 1) : name;

            bool match = (base.startsWith("wardrive_") || base.startsWith("wardrive_core_")) &&
                         base.endsWith(".log") &&
                         !base.endsWith(".uploaded");
            if (match) {
                String full = (name.startsWith("/")) ? name : ("/" + name);
                uint32_t seq = fileSeq(full);
                uint32_t sz = (uint32_t)entry.size();
                if (pending_count < MAX_FILES) {
                    file_seq[pending_count]  = seq;
                    file_size[pending_count] = sz;
                    pending_files[pending_count] = full;
                    pending_count++;
                } else {
                    // At the cap: replace the oldest kept slot (lowest number),
                    // but only if this file is newer than it.
                    uint8_t minj = 0;
                    for (uint8_t k = 1; k < MAX_FILES; k++)
                        if (file_seq[k] < file_seq[minj]) minj = k;
                    if (seq > file_seq[minj]) {
                        file_seq[minj]  = seq;
                        file_size[minj] = sz;
                        pending_files[minj] = full;
                    }
                }
            }
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();

    // Newest first: sort by the trailing log number descending. The log counter
    // only ever increments, so higher number = created later — a reliable,
    // name-based chronological order that does not depend on the SD clock (the
    // logs carry no meaningful timestamp when the RTC was never set).
    for (uint8_t i = 1; i < pending_count; i++) {
        String p = pending_files[i];
        uint32_t seq = file_seq[i];
        uint32_t sz  = file_size[i];
        int8_t j = i - 1;
        while (j >= 0 && file_seq[j] < seq) {
            pending_files[j + 1] = pending_files[j];
            file_seq[j + 1]      = file_seq[j];
            file_size[j + 1]     = file_size[j];
            j--;
        }
        pending_files[j + 1] = p;
        file_seq[j + 1]      = seq;
        file_size[j + 1]     = sz;
    }

    Serial.printf("WDG: scan kept %u newest of the wardrive logs found\n", pending_count);
    return true;
}

// =========================================================================
// AP connect
// =========================================================================
bool WdgwarsUpload::connectToAP() {
    Serial.printf("WDG: connecting to AP \"%s\"\n", ssid.c_str());
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_MODE_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WDGWARS_AP_TIMEOUT_MS) {
            last_error_msg = "AP timeout (" + ssid + ")";
            Serial.println("WDG: " + last_error_msg);
            return false;
        }
        delay(250);
    }
    Serial.printf("WDG: AP ok, IP=%s\n", WiFi.localIP().toString().c_str());
    return true;
}

// =========================================================================
// Upload one file
// =========================================================================
bool WdgwarsUpload::uploadOneFile(const String& path) {
    File f = SD.open(path, FILE_READ);
    if (!f) {
        last_error_msg = "open failed: " + path;
        return false;
    }
    size_t fsize = f.size();
    if (fsize == 0) {
        f.close();
        last_error_msg = "empty file: " + path;
        return false;
    }

    WiFiClientSecure client;
    client.setCACert(WDG_CA_BUNDLE);
    client.setTimeout(WDGWARS_TLS_TIMEOUT_MS / 1000);

    Serial.printf("WDG: connect %s:%u\n", WDG_HOST, WDG_PORT);
    if (!client.connect(WDG_HOST, WDG_PORT)) {
        last_error_msg = "TLS connect failed";
        f.close();
        return false;
    }

    bool ok = sendMultipartPOST(client, path, f, fsize);
    f.close();

    if (!ok) {
        client.stop();
        return false;
    }

    String server_msg;
    int code = parseHttpStatus(client, server_msg);
    last_http_code = code;
    last_server_msg = server_msg;
    client.stop();

    if (code >= 200 && code < 300) {
        total_bytes_uploaded += fsize;
        // The success body carries the import summary (imported / captured /
        // duplicates), which is worth having in the serial log.
        Serial.printf("WDG: upload OK code=%d size=%u %s\n",
                      code, fsize, server_msg.c_str());
        return true;
    }

    // Negative codes are our own local failures, not the server's verdict.
    if (code == -1)      last_error_msg = "no reply (timeout)";
    else if (code == -2) last_error_msg = "bad reply from server";
    else                 last_error_msg = "HTTP " + String(code) +
                                          (server_msg.length() ? ": " + server_msg : "");
    Serial.printf("WDG: upload FAIL code=%d msg=%s\n", code, server_msg.c_str());
    return false;
}

// Multipart streaming: build header + opening + body + closing.
bool WdgwarsUpload::sendMultipartPOST(WiFiClientSecure& client,
                                      const String& path,
                                      File& file,
                                      size_t file_size) {
    // Filename for Content-Disposition (basename only).
    int slash = path.lastIndexOf('/');
    String fname = (slash >= 0) ? path.substring(slash + 1) : path;

    String opening =
        "--" + String(WDG_BOUNDARY) + "\r\n" +
        "Content-Disposition: form-data; name=\"file\"; filename=\"" + fname + "\"\r\n" +
        "Content-Type: text/csv\r\n\r\n";
    String closing = "\r\n--" + String(WDG_BOUNDARY) + "--\r\n";

    size_t total_len = opening.length() + file_size + closing.length();

    // Headers.
    client.print("POST ");
    client.print(WDG_PATH);
    client.print(" HTTP/1.1\r\n");
    client.print("Host: ");
    client.print(WDG_HOST);
    client.print("\r\n");
    client.print("User-Agent: warroom-rig/WDGWarsUpload\r\n");
    client.print("X-API-Key: ");
    client.print(apikey);
    client.print("\r\n");
    client.print("Content-Type: multipart/form-data; boundary=");
    client.print(WDG_BOUNDARY);
    client.print("\r\n");
    client.print("Content-Length: ");
    client.print(total_len);
    client.print("\r\n");
    client.print("Connection: close\r\n\r\n");

    // Opening boundary.
    if (client.print(opening) != (int)opening.length()) {
        last_error_msg = "write opening fail";
        return false;
    }

    // File body, chunked.
    uint8_t buf[UPLOAD_CHUNK_BYTES];
    size_t remaining = file_size;
    while (remaining > 0) {
        size_t want = (remaining < UPLOAD_CHUNK_BYTES) ? remaining : UPLOAD_CHUNK_BYTES;
        size_t got = file.read(buf, want);
        if (got == 0) {
            last_error_msg = "SD read EOF early";
            return false;
        }
        size_t written = 0;
        while (written < got) {
            int n = client.write(buf + written, got - written);
            if (n <= 0) {
                last_error_msg = "TLS write fail";
                return false;
            }
            written += n;
        }
        remaining -= got;

        // Yield to RTOS so the WiFi+TLS stacks can drain.
        yield();
    }

    // Closing boundary.
    if (client.print(closing) != (int)closing.length()) {
        last_error_msg = "write closing fail";
        return false;
    }
    return true;
}

// Pull the human-readable reason out of a wdgwars response body.
//
// The server answers JSON — {"ok":false,"error":"Missing or invalid API key"} on
// failure, and an import summary on success. Cloudflare may deliver it chunked,
// so rather than parse framing we lift the quoted value of the first "error" /
// "message" / "detail" key we find. If none is present (an HTML error page from
// the edge, say), fall back to the raw text with markup and whitespace runs
// collapsed, which still beats showing nothing.
static String extractServerMessage(const String& raw) {
    const char* keys[] = { "\"error\"", "\"message\"", "\"detail\"" };
    for (uint8_t k = 0; k < 3; k++) {
        int at = raw.indexOf(keys[k]);
        if (at < 0) continue;
        int colon = raw.indexOf(':', at + strlen(keys[k]) - 1);
        if (colon < 0) continue;
        int q1 = raw.indexOf('"', colon + 1);
        if (q1 < 0) continue;
        int q2 = raw.indexOf('"', q1 + 1);
        if (q2 < 0) continue;
        String msg = raw.substring(q1 + 1, q2);
        msg.trim();
        if (msg.length() > 0) return msg;
    }

    // No JSON message — flatten whatever came back.
    String flat;
    flat.reserve(raw.length());
    bool in_tag = false, last_space = false;
    for (size_t i = 0; i < raw.length(); i++) {
        char c = raw.charAt(i);
        if (c == '<') { in_tag = true;  continue; }
        if (c == '>') { in_tag = false; continue; }
        if (in_tag) continue;
        bool is_space = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (is_space) {
            if (!last_space && flat.length() > 0) flat += ' ';
            last_space = true;
        } else {
            flat += c;
            last_space = false;
        }
    }
    flat.trim();
    return flat;
}

// Parse the HTTP status code, and capture the start of the body so the caller
// can report *why* the server said no. Knowing "HTTP 400" alone is what made an
// ordinary rejection look like an unreachable API; the body carries the reason.
int WdgwarsUpload::parseHttpStatus(WiFiClientSecure& client, String& msg_out) {
    msg_out = "";
    uint32_t start = millis();
    while (client.connected() && !client.available()) {
        if (millis() - start > WDGWARS_HTTP_TIMEOUT_MS) return -1;
        delay(10);
    }
    String status = client.readStringUntil('\n');
    // Expected format: "HTTP/1.1 200 OK\r"
    int sp1 = status.indexOf(' ');
    int sp2 = status.indexOf(' ', sp1 + 1);
    if (sp1 < 0 || sp2 < 0) return -2;
    int code = status.substring(sp1 + 1, sp2).toInt();

    // Skip response headers — everything up to the blank separator line.
    while (millis() - start <= WDGWARS_HTTP_TIMEOUT_MS) {
        if (client.available()) {
            String h = client.readStringUntil('\n');
            h.trim();
            if (h.length() == 0) break;      // end of headers
        } else if (!client.connected()) {
            break;
        } else {
            delay(5);
        }
    }

    // Body prefix — bounded, we only want the reason string.
    String raw;
    raw.reserve(RESP_SNIPPET_BYTES);
    while (raw.length() < RESP_SNIPPET_BYTES &&
           (millis() - start) <= WDGWARS_HTTP_TIMEOUT_MS) {
        if (client.available()) {
            raw += (char)client.read();
        } else if (!client.connected()) {
            break;
        } else {
            delay(5);
        }
    }
    msg_out = extractServerMessage(raw);

    // Drain remainder so the connection closes cleanly.
    while (client.connected() && client.available()) {
        client.read();
    }
    return code;
}

// =========================================================================
// File rename helper
// =========================================================================
void WdgwarsUpload::renameToUploaded(const String& path) {
    String new_path = path + ".uploaded";
    if (SD.rename(path, new_path)) {
        Serial.printf("WDG: rename %s -> %s\n", path.c_str(), new_path.c_str());
    } else {
        Serial.printf("WDG: rename FAILED %s (file stays for retry)\n", path.c_str());
    }
}

// =========================================================================
// UI
// =========================================================================
#ifdef HAS_SCREEN
// Word-wrap a message across the narrow TFT. Server reasons ("Missing or
// invalid API key", "Invalid timestamps in file") do not fit one 240 px line,
// and a truncated reason is close to useless. Advances y past what it drew.
static void wdgPrintWrapped(int16_t x, int16_t& y, int16_t lh,
                            const String& s, uint8_t max_lines) {
    const uint8_t cpl = display_obj.tft.width() / 6;   // ~6 px per char at size 1
    if (cpl == 0 || s.length() == 0) return;
    uint16_t pos = 0;
    for (uint8_t line = 0; line < max_lines && pos < s.length(); line++) {
        uint16_t take = ((s.length() - pos) < cpl) ? (s.length() - pos) : cpl;
        // On the last line we are allowed to spend, keep the tail visible by
        // marking the cut rather than silently dropping the rest.
        bool truncating = (line + 1 == max_lines) && (pos + take < s.length());
        if (!truncating && take == cpl) {
            // Break on the last space in the window so words stay intact.
            int brk = -1;
            for (uint16_t i = take; i > 0; i--) {
                if (s.charAt(pos + i - 1) == ' ') { brk = i - 1; break; }
            }
            if (brk > 0) take = brk;
        }
        String chunk = s.substring(pos, pos + take);
        if (truncating && chunk.length() > 3) {
            chunk = chunk.substring(0, chunk.length() - 3) + "...";
        }
        display_obj.tft.setCursor(x, y); y += lh;
        display_obj.tft.print(chunk);
        pos += take;
        while (pos < s.length() && s.charAt(pos) == ' ') pos++;   // eat the break
    }
}
#endif  // HAS_SCREEN

void WdgwarsUpload::renderDisplay() {
    #ifdef HAS_SCREEN
        // Clear ONLY below the framework status bar + "WDGWars Upload" banner that
        // RunWdgwarsUpload draws at the top. A full clearScreen wiped the status
        // bar and then fought its redraw every tick -> flashing + overlap with the
        // GPS/sat line. Keeping our content in the area below fixes both.
        const int16_t top = 34;
        const int16_t LH = 14;
        display_obj.tft.fillRect(0, top, display_obj.tft.width(),
                                 display_obj.tft.height() - top, TFT_BLACK);
        display_obj.tft.setTextSize(1);
        display_obj.tft.setTextColor(TFT_WHITE);
        int16_t y = top + 2;

        switch (state) {
            case State::SETTINGS_ERROR:
                display_obj.tft.setTextColor(TFT_RED);
                display_obj.tft.setCursor(4, y); y += LH;
                display_obj.tft.print("Settings error:");
                display_obj.tft.setTextColor(TFT_WHITE);
                display_obj.tft.setCursor(4, y); y += LH;
                display_obj.tft.print(last_error_msg);
                display_obj.tft.setCursor(4, y + LH);
                display_obj.tft.print("Hold CENTER 2s to exit.");
                break;

            case State::NO_FILES:
                display_obj.tft.setCursor(4, y); y += LH;
                display_obj.tft.print("No unsynced wardrive_*");
                display_obj.tft.setCursor(4, y); y += LH;
                display_obj.tft.print("logs found.");
                display_obj.tft.setCursor(4, y + LH);
                display_obj.tft.print("Hold CENTER 2s to exit.");
                break;

            case State::CONNECTING_AP:
                display_obj.tft.setCursor(4, y); y += LH;
                display_obj.tft.print("Connecting AP:");
                display_obj.tft.setCursor(4, y);
                display_obj.tft.print(ssid);
                break;

            case State::AP_FAILED:
                display_obj.tft.setTextColor(TFT_RED);
                display_obj.tft.setCursor(4, y); y += LH;
                display_obj.tft.print("AP connect failed");
                display_obj.tft.setTextColor(TFT_WHITE);
                display_obj.tft.setCursor(4, y); y += LH;
                display_obj.tft.print(last_error_msg);
                display_obj.tft.setCursor(4, y + LH);
                display_obj.tft.print("Hold CENTER 2s to exit.");
                break;

            case State::UPLOADING: {
                display_obj.tft.setCursor(4, y); y += LH;
                display_obj.tft.printf("[%u/%u]", (unsigned)(ok_count + err_count + 1),
                                       (unsigned)upload_total);

                if (current_idx < pending_count) {
                    String name = pending_files[current_idx];
                    int s = name.lastIndexOf('/');
                    if (s >= 0) name = name.substring(s + 1);
                    if (name.length() > 30) name = name.substring(0, 30);
                    display_obj.tft.setCursor(4, y); y += LH;
                    display_obj.tft.print(name);
                }
                display_obj.tft.setCursor(4, y); y += LH;
                display_obj.tft.printf("OK:%u  ERR:%u", ok_count, err_count);

                display_obj.tft.setCursor(4, y); y += LH;
                display_obj.tft.printf("Sent: %llu KB",
                                       (unsigned long long)(total_bytes_uploaded / 1024));
                if (last_http_code != 0) {
                    display_obj.tft.setCursor(4, y); y += LH;
                    display_obj.tft.printf("Last HTTP: %d", last_http_code);
                }
                // Why the last one failed, in the server's own words.
                if (err_count > 0 && last_error_msg.length() > 0) {
                    display_obj.tft.setTextColor(TFT_RED);
                    wdgPrintWrapped(4, y, LH, last_error_msg, 3);
                    display_obj.tft.setTextColor(TFT_WHITE);
                }
                break;
            }

            case State::DONE: {
                display_obj.tft.setTextColor(TFT_GREEN);
                display_obj.tft.setCursor(4, y); y += LH;
                display_obj.tft.print("Done.");
                display_obj.tft.setTextColor(TFT_WHITE);
                display_obj.tft.setCursor(4, y); y += LH;
                display_obj.tft.printf("OK:  %u", ok_count);
                display_obj.tft.setCursor(4, y); y += LH;
                display_obj.tft.printf("ERR: %u", err_count);
                display_obj.tft.setCursor(4, y); y += LH;
                display_obj.tft.printf("Total: %llu KB",
                                       (unsigned long long)(total_bytes_uploaded / 1024));
                // A run that ended with failures must say why, not just count
                // them — otherwise every cause looks like "upload is broken".
                bool showed_err = (err_count > 0 && last_error_msg.length() > 0);
                if (showed_err) {
                    y += 4;
                    display_obj.tft.setTextColor(TFT_RED);
                    wdgPrintWrapped(4, y, LH, last_error_msg, 4);
                    display_obj.tft.setTextColor(TFT_WHITE);
                }
                // Keep the original blank-line gap before the hint; after a
                // wrapped error block the text already provides the separation.
                display_obj.tft.setCursor(4, y + (showed_err ? 4 : LH));
                display_obj.tft.print("Hold CENTER 2s to exit.");
                break;
            }

            default:
                display_obj.tft.setCursor(4, y);
                display_obj.tft.print("...");
                break;
        }
    #endif
}

void WdgwarsUpload::renderConfirmPrompt() {
    #ifdef HAS_SCREEN
        display_obj.clearScreen();
        display_obj.tft.setTextSize(1);
        display_obj.tft.setTextColor(TFT_ORANGE);
        display_obj.tft.setCursor(4, 4);
        display_obj.tft.print("WDGWars Upload");

        display_obj.tft.setTextColor(TFT_WHITE);
        int16_t y = 22;
        const int16_t LH = 12;

        // Sum file sizes for the prompt.
        uint64_t total = 0;
        for (uint8_t i = 0; i < pending_count; ++i) {
            File q = SD.open(pending_files[i], FILE_READ);
            if (q) {
                total += q.size();
                q.close();
            }
        }

        display_obj.tft.setCursor(4, y); y += LH;
        display_obj.tft.printf("Found %u file(s)", pending_count);
        display_obj.tft.setCursor(4, y); y += LH + 4;
        display_obj.tft.printf("Total: %llu KB",
                               (unsigned long long)(total / 1024));

        display_obj.tft.setTextColor(TFT_CYAN);
        display_obj.tft.setCursor(4, y); y += LH;
        display_obj.tft.print("Tap CENTER to upload");
        display_obj.tft.setCursor(4, y);
        display_obj.tft.print("Hold 2s to cancel");
    #endif
}

// Rows that fit in the pick-list window (screen height minus header + footer).
static uint8_t selVisibleRows() {
    #ifdef HAS_SCREEN
        // Rows between the banner (top=34, content from ~36) and the footer (~24px).
        int rows = (display_obj.tft.height() - 34 - 26) / 12;
        if (rows < 1) rows = 1;
        if (rows > 32) rows = 32;
        return (uint8_t)rows;
    #else
        return 8;
    #endif
}

void WdgwarsUpload::renderSelectList() {
    #ifdef HAS_SCREEN
        // Clear only below the framework banner (keep the top status bar) so the
        // top stays consistent from selection through upload — same as renderDisplay.
        const int16_t top = 34;
        display_obj.tft.fillRect(0, top, display_obj.tft.width(),
                                 display_obj.tft.height() - top, TFT_BLACK);
        display_obj.tft.setTextSize(1);

        uint8_t rows = selVisibleRows();
        uint8_t sel_n = 0;
        for (uint8_t i = 0; i < pending_count; i++) if (file_selected[i]) sel_n++;

        int16_t y = top + 2;
        const int16_t LH = 12;
        for (uint8_t r = 0; r < rows; r++) {
            uint8_t i = sel_top + r;
            if (i >= pending_count) break;
            bool cursor = (i == sel_cursor);
            display_obj.tft.setTextColor(cursor ? TFT_CYAN : TFT_WHITE);
            display_obj.tft.setCursor(4, y); y += LH;
            String name = pending_files[i];
            int s = name.lastIndexOf('/');
            if (s >= 0) name = name.substring(s + 1);
            int dot = name.lastIndexOf(".log");
            if (dot > 0) name = name.substring(0, dot);   // drop ".log" to save room
            if (name.length() > 20) name = name.substring(0, 20);
            uint32_t sz = file_size[i];
            String szs = (sz >= 1024) ? (String((sz + 512) / 1024) + "K")
                                      : (String(sz) + "B");
            display_obj.tft.print(String(cursor ? ">" : " ") +
                                  (file_selected[i] ? "[x] " : "[ ] ") + name + "  " + szs);
        }

        // Footer: selected count + button legend.
        int16_t fy = display_obj.tft.height();
        display_obj.tft.setTextColor(TFT_GREEN);
        display_obj.tft.setCursor(4, fy - 22);
        display_obj.tft.printf("%u/%u selected", sel_n, pending_count);
        display_obj.tft.setTextColor(TFT_DARKGREY);
        display_obj.tft.setCursor(4, fy - 10);
        display_obj.tft.print("C pick  L all  R GO  holdC exit");
    #endif
}

#ifdef HAS_TOUCH
// ============================================================
// Touch file picker (Marauder V8)
// ============================================================
//
// Large tappable rows the user can actually hit with a finger, plus a scroll
// bar (pages of WT_ROWS) and an action bar. Geometry is shared between the
// renderer and the hit-test so the two never drift.
static const int WT_LIST_TOP    = 52;   // first row y
static const int WT_ROW_H       = 32;   // row pitch
static const int WT_ROW_INNER_H = 30;   // drawn row height
static const int WT_ROWS        = 5;    // visible rows per page
static const int WT_ROW_X       = 2;
static const int WT_ROW_W       = 236;
// Scroll bar.
static const int WT_SCR_Y = 216, WT_SCR_H = 44;
static const int WT_UP_X  = 4,   WT_UP_W  = 112;
static const int WT_DN_X  = 124, WT_DN_W  = 112;
// Action bar.
static const int WT_ACT_Y  = 266, WT_ACT_H = 46;
static const int WT_ALL_X  = 4,   WT_ALL_W  = 72;
static const int WT_GO_X   = 80,  WT_GO_W   = 80;
static const int WT_EXIT_X = 164, WT_EXIT_W = 72;

enum { WTS_NONE = -1, WTS_UP = -2, WTS_DOWN = -3, WTS_ALL = -4, WTS_GO = -5, WTS_EXIT = -6 };

// Return a visible-row index [0..visible), or one of the WTS_* action codes.
static int wdgTouchHitTest(uint16_t x, uint16_t y, uint8_t visible) {
    for (int r = 0; r < visible; r++) {
        int ry = WT_LIST_TOP + r * WT_ROW_H;
        if ((int)y >= ry && (int)y < ry + WT_ROW_INNER_H &&
            (int)x >= WT_ROW_X && (int)x <= WT_ROW_X + WT_ROW_W)
            return r;
    }
    if ((int)y >= WT_SCR_Y && (int)y <= WT_SCR_Y + WT_SCR_H) {
        if ((int)x >= WT_UP_X && (int)x <= WT_UP_X + WT_UP_W) return WTS_UP;
        if ((int)x >= WT_DN_X && (int)x <= WT_DN_X + WT_DN_W) return WTS_DOWN;
    }
    if ((int)y >= WT_ACT_Y && (int)y <= WT_ACT_Y + WT_ACT_H) {
        if ((int)x >= WT_ALL_X  && (int)x <= WT_ALL_X  + WT_ALL_W ) return WTS_ALL;
        if ((int)x >= WT_GO_X   && (int)x <= WT_GO_X   + WT_GO_W  ) return WTS_GO;
        if ((int)x >= WT_EXIT_X && (int)x <= WT_EXIT_X + WT_EXIT_W) return WTS_EXIT;
    }
    return WTS_NONE;
}

static void wdgDrawButton(int x, int y, int w, int h, const char* label,
                          uint16_t fill, uint16_t border, bool enabled = true) {
    uint16_t f = enabled ? fill : TFT_BLACK;
    uint16_t b = enabled ? border : TFT_DARKGREY;
    display_obj.tft.fillRoundRect(x, y, w, h, 5, f);
    display_obj.tft.drawRoundRect(x, y, w, h, 5, b);
    display_obj.tft.setTextSize(2);
    display_obj.tft.setTextColor(b, f);
    int tw = (int)strlen(label) * 12;
    int tx = x + (w - tw) / 2; if (tx < x + 3) tx = x + 3;
    int ty = y + (h - 16) / 2;
    display_obj.tft.setCursor(tx, ty);
    display_obj.tft.print(label);
}

void WdgwarsUpload::renderSelectListTouch() {
    #ifdef HAS_SCREEN
        const int16_t top = 34;   // keep the framework status bar
        display_obj.tft.fillRect(0, top, display_obj.tft.width(),
                                 display_obj.tft.height() - top, TFT_BLACK);

        uint8_t sel_n = 0;
        for (uint8_t i = 0; i < pending_count; i++) if (file_selected[i]) sel_n++;

        display_obj.tft.setTextSize(1);
        display_obj.tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        display_obj.tft.setCursor(4, 40);
        display_obj.tft.printf("Tap logs   %u/%u selected", sel_n, pending_count);

        // File rows.
        for (int r = 0; r < WT_ROWS; r++) {
            uint8_t i = sel_top + r;
            if (i >= pending_count) break;
            int ry = WT_LIST_TOP + r * WT_ROW_H;
            bool on = file_selected[i];

            if (on) display_obj.tft.fillRoundRect(WT_ROW_X, ry, WT_ROW_W, WT_ROW_INNER_H, 4, TFT_DARKGREEN);
            display_obj.tft.drawRoundRect(WT_ROW_X, ry, WT_ROW_W, WT_ROW_INNER_H, 4,
                                          on ? TFT_GREEN : TFT_DARKGREY);
            // Checkbox.
            int cbx = WT_ROW_X + 8, cby = ry + (WT_ROW_INNER_H - 16) / 2;
            display_obj.tft.drawRect(cbx, cby, 16, 16, on ? TFT_GREEN : TFT_LIGHTGREY);
            if (on) display_obj.tft.fillRect(cbx + 3, cby + 3, 10, 10, TFT_GREEN);

            // Filename (strip path + ".log", trim to fit).
            String name = pending_files[i];
            int s = name.lastIndexOf('/');
            if (s >= 0) name = name.substring(s + 1);
            int dot = name.lastIndexOf(".log");
            if (dot > 0) name = name.substring(0, dot);
            if (name.length() > 12) name = name.substring(0, 12);
            display_obj.tft.setTextSize(2);
            display_obj.tft.setTextColor(on ? TFT_WHITE : TFT_LIGHTGREY,
                                         on ? TFT_DARKGREEN : TFT_BLACK);
            display_obj.tft.setCursor(cbx + 24, ry + 7);
            display_obj.tft.print(name);

            // Size, small, right-aligned-ish.
            uint32_t sz = file_size[i];
            String szs = (sz >= 1024) ? (String((sz + 512) / 1024) + "K")
                                      : (String(sz) + "B");
            display_obj.tft.setTextSize(1);
            display_obj.tft.setTextColor(on ? TFT_GREEN : TFT_DARKGREY,
                                         on ? TFT_DARKGREEN : TFT_BLACK);
            display_obj.tft.setCursor(WT_ROW_X + WT_ROW_W - 40, ry + 11);
            display_obj.tft.print(szs);
        }

        // Scroll bar (paged). Greyed out when there is nothing to scroll.
        bool can_up   = (sel_top > 0);
        bool can_down = (sel_top + WT_ROWS < pending_count);
        wdgDrawButton(WT_UP_X, WT_SCR_Y, WT_UP_W, WT_SCR_H, "UP",   TFT_NAVY, TFT_CYAN, can_up);
        wdgDrawButton(WT_DN_X, WT_SCR_Y, WT_DN_W, WT_SCR_H, "DOWN", TFT_NAVY, TFT_CYAN, can_down);

        // Action bar.
        wdgDrawButton(WT_ALL_X, WT_ACT_Y, WT_ALL_W, WT_ACT_H, "ALL", TFT_DARKGREY, TFT_WHITE);
        char golbl[16];
        snprintf(golbl, sizeof(golbl), "GO %u", (unsigned)sel_n);
        wdgDrawButton(WT_GO_X, WT_ACT_Y, WT_GO_W, WT_ACT_H, golbl,
                      sel_n ? TFT_DARKGREEN : TFT_BLACK, sel_n ? TFT_GREEN : TFT_DARKGREY, sel_n > 0);
        wdgDrawButton(WT_EXIT_X, WT_ACT_Y, WT_EXIT_W, WT_ACT_H, "EXIT", TFT_MAROON, TFT_RED);
    #endif
}
#endif // HAS_TOUCH

// Blocking pick UI. Like the Core Mode session menu, it owns the buttons for its
// whole duration via a delay()-yielding loop — so MenuFunctions' stop-scan handler
// cannot fire mid-selection (which was making CENTER exit instead of toggling).
// Returns having set state = CONNECTING_AP (user pressed GO) or after deinit().
void WdgwarsUpload::runSelectionModal() {
    #if defined(HAS_BUTTONS) && (C_BTN >= 0) && (U_BTN >= 0) && (D_BTN >= 0) && \
        !defined(MARAUDER_CARDPUTER) && !defined(MARAUDER_CARDPUTER_ADV)
        // Wait for the button that opened this mode to be released, else it lands
        // as the first action inside the loop.
        while (digitalRead(C_BTN) == LOW) delay(10);
        delay(60);
        renderSelectList();

        bool nav_u = false, nav_d = false, nav_l = false, nav_r = false;
        bool c_held = false;
        uint32_t c_start = 0;

        for (;;) {
            uint32_t now = millis();
            bool changed = false;

            // CENTER: short tap = toggle current file; long-press = cancel & exit.
            bool c_now = (digitalRead(C_BTN) == LOW);
            if (c_now) {
                if (!c_held) { c_held = true; c_start = now; }
                else if ((now - c_start) >= WDGWARS_EXIT_HOLD_MS) {
                    Serial.println("WDG: selection cancelled (hold CENTER)");
                    deinit();
                    return;
                }
            } else if (c_held) {
                uint32_t held = now - c_start;
                c_held = false;
                if (held < WDGWARS_EXIT_HOLD_MS && sel_cursor < pending_count) {
                    file_selected[sel_cursor] = !file_selected[sel_cursor];
                    changed = true;
                }
            }

            // UP / DOWN: move the cursor.
            bool u = (digitalRead(U_BTN) == LOW);
            if (u && !nav_u && sel_cursor > 0) { sel_cursor--; changed = true; }
            nav_u = u;
            bool d = (digitalRead(D_BTN) == LOW);
            if (d && !nav_d && sel_cursor + 1 < pending_count) { sel_cursor++; changed = true; }
            nav_d = d;

            // LEFT: select all / none.
            #if (L_BTN >= 0)
            bool l = (digitalRead(L_BTN) == LOW);
            if (l && !nav_l) {
                bool any_off = false;
                for (uint8_t i = 0; i < pending_count; i++)
                    if (!file_selected[i]) { any_off = true; break; }
                for (uint8_t i = 0; i < pending_count; i++) file_selected[i] = any_off;
                changed = true;
            }
            nav_l = l;
            #endif

            // RIGHT: start uploading the selected files.
            #if (R_BTN >= 0)
            bool r = (digitalRead(R_BTN) == LOW);
            if (r && !nav_r) {
                uint8_t n = 0;
                for (uint8_t i = 0; i < pending_count; i++) if (file_selected[i]) n++;
                if (n > 0) {
                    upload_total = n;
                    current_idx = 0;
                    state = State::CONNECTING_AP;
                    Serial.printf("WDG: uploading %u selected file(s)\n", n);
                    return;
                }
                changed = true;  // nothing selected yet — footer nudges the user
            }
            nav_r = r;
            #endif

            // Keep the cursor within the visible scroll window.
            uint8_t rows = selVisibleRows();
            if (sel_cursor < sel_top) sel_top = sel_cursor;
            else if (sel_cursor >= sel_top + rows) sel_top = sel_cursor - rows + 1;

            if (changed) renderSelectList();
            delay(15);
        }
    #elif defined(HAS_TOUCH)
        // Touch pick UI (Marauder V8). Owns the screen for its whole duration,
        // exactly like the button loop above, so the menu's stop-scan handler
        // cannot fire mid-selection. Sets CONNECTING_AP on GO, or deinit()s on EXIT.
        { uint16_t rx, ry; while (display_obj.updateTouch(&rx, &ry)) delay(5); }  // release opener
        sel_top = 0;
        renderSelectListTouch();
        for (;;) {
            uint16_t tx, ty;
            if (display_obj.updateTouch(&tx, &ty)) {
                // Wait for release, keeping the last (finger-up) coordinates.
                uint16_t lx = tx, ly = ty;
                while (display_obj.updateTouch(&tx, &ty)) { lx = tx; ly = ty; delay(5); }

                uint8_t visible = pending_count - sel_top;
                if (visible > WT_ROWS) visible = WT_ROWS;
                int act = wdgTouchHitTest(lx, ly, visible);

                if (act >= 0) {                                   // row -> toggle
                    uint8_t i = sel_top + (uint8_t)act;
                    if (i < pending_count) {
                        file_selected[i] = !file_selected[i];
                        renderSelectListTouch();
                    }
                } else if (act == WTS_UP) {
                    if (sel_top >= WT_ROWS)   { sel_top -= WT_ROWS; renderSelectListTouch(); }
                    else if (sel_top > 0)     { sel_top = 0;        renderSelectListTouch(); }
                } else if (act == WTS_DOWN) {
                    if (sel_top + WT_ROWS < pending_count) { sel_top += WT_ROWS; renderSelectListTouch(); }
                } else if (act == WTS_ALL) {
                    bool any_off = false;
                    for (uint8_t i = 0; i < pending_count; i++)
                        if (!file_selected[i]) { any_off = true; break; }
                    for (uint8_t i = 0; i < pending_count; i++) file_selected[i] = any_off;
                    renderSelectListTouch();
                } else if (act == WTS_GO) {
                    uint8_t n = 0;
                    for (uint8_t i = 0; i < pending_count; i++) if (file_selected[i]) n++;
                    if (n > 0) {
                        upload_total = n;
                        current_idx = 0;
                        state = State::CONNECTING_AP;
                        Serial.printf("WDG: uploading %u selected file(s)\n", n);
                        return;
                    }
                    // nothing selected — GO is a no-op (button is greyed anyway)
                } else if (act == WTS_EXIT) {
                    Serial.println("WDG: selection cancelled (EXIT tap)");
                    deinit();
                    return;
                }
            }
            delay(15);
        }
    #else
        // No navigation buttons on this board: fall back to uploading everything.
        for (uint8_t i = 0; i < pending_count; i++) file_selected[i] = true;
        upload_total = pending_count;
        current_idx = 0;
        state = State::CONNECTING_AP;
    #endif
}

void WdgwarsUpload::handleCenterLongPressForExit() {
    #if defined(HAS_BUTTONS) && (C_BTN >= 0) && \
        !defined(MARAUDER_CARDPUTER) && !defined(MARAUDER_CARDPUTER_ADV)
        bool pressed_now = (digitalRead(C_BTN) == LOW);
        uint32_t now = millis();
        if (pressed_now) {
            if (!center_was_pressed) {
                center_was_pressed = true;
                center_press_start_ms = now;
            } else if ((now - center_press_start_ms) >= WDGWARS_EXIT_HOLD_MS) {
                Serial.println("WDG: long-press -> exit");
                deinit();
            }
        } else {
            center_was_pressed = false;
            center_press_start_ms = 0;
        }
    #endif
}

#endif  // MARAUDER_WDGWARS_UPLOAD
