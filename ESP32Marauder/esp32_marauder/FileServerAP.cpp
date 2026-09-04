#include "FileServerAP.h"

#include "RigInput.h"
#include "RigTheme.h"       // screen shape; this view keeps the plain Marauder
                            // look but has to fit either panel
#ifdef MARAUDER_FILE_SERVER_AP

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>

#include "Display.h"
#include "SDInterface.h"
#include "WiFiScan.h"
extern WiFiScan wifi_scan_obj;

// Same pattern as WdgwarsUpload: ask RigInput for the button's current state.
// `Switches` keeps getButtonState() private and exposes only justPressed /
// justReleased, neither of which fits the long-hold-exit shape.

extern Display display_obj;
extern SDInterface sd_obj;

// =========================================================================
// Constants
// =========================================================================
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_GATEWAY(192, 168, 4, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);
static const uint16_t HTTP_PORT = 80;
static const uint8_t AP_CHANNEL = 6;
static const uint8_t AP_MAX_CONN = 4;
static const char* SETTINGS_PATH = "/fileserver.txt";
static const char* DEFAULT_HTTP_USER = "rig";

// There is deliberately no default PSK constant here. This repo is public, so
// a constant would ship the same PSK on every unit built from it, and the AP
// name announces what the AP is. The PSK and the HTTP password are generated
// once per device (see ensureSecrets()) and kept in NVS, so they survive a
// reboot but are not in the source tree.
static const char* PREFS_NAMESPACE = "wrfileap";
static const char* PREFS_KEY_PSK = "psk";
static const char* PREFS_KEY_HTTPPW = "httppw";
static const uint8_t GENERATED_PSK_LEN = 12;
static const uint8_t GENERATED_HTTPPW_LEN = 10;

// Exactly 32 symbols, so `& 31` picks one without modulo bias. 0/O and 1/I are
// left out on purpose: the operator reads these off the 1x TFT font and types
// them into a phone, and a misread character means a failed association with
// no diagnostic. 32 symbols = 5 bits per character.
static const char SECRET_ALPHABET[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

// Guards last_request_path, which the AsyncTCP task writes and the loop task
// reads. Kept file-static so the FreeRTOS types stay out of the header.
static portMUX_TYPE last_path_mux = portMUX_INITIALIZER_UNLOCKED;

// One authentication middleware for the whole server. Registered on the
// server (not per handler) so it also covers the catch-all 404 handler, and
// so no route can be added later that quietly bypasses it. Static because
// AsyncMiddlewareChain::addMiddleware(AsyncMiddleware*) does not take
// ownership — it only deletes middlewares it allocated itself.
static AsyncAuthenticationMiddleware http_auth;

// Display + exit-hold tuning. Reuse the WDGWARS constants if defined, else
// pick conservative local defaults.
#ifndef FILESERVER_DISPLAY_REFRESH_MS
    // 1000 ms — 500 ms was visibly flickery because the previous render
    // called clearScreen() each tick. The new render uses fillRect over the
    // dynamic region only, so 1 Hz is comfortable and silent.
    #define FILESERVER_DISPLAY_REFRESH_MS 1000
#endif
#ifndef FILESERVER_EXIT_HOLD_MS
    #define FILESERVER_EXIT_HOLD_MS 1200
#endif

// =========================================================================
// Global singleton
// =========================================================================
FileServerAP file_server_ap_obj;

// =========================================================================
// Path safety
// =========================================================================
String FileServerAP::sanitizePath(const String& raw) {
    if (raw.length() == 0) return "/";
    String p = raw;
    if (p.charAt(0) != '/') p = "/" + p;
    // Reject parent-traversal. The SD library is fairly forgiving but we
    // don't want to expose anything above SD-root either.
    if (p.indexOf("..") >= 0) return "";
    // Strip trailing slash unless it's the root.
    while (p.length() > 1 && p.endsWith("/")) p.remove(p.length() - 1);
    return p;
}

// =========================================================================
// Settings load
// =========================================================================
void FileServerAP::loadOptionalSettings() {
    // Default SSID from STA MAC.
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char ssid_buf[32];
    snprintf(ssid_buf, sizeof(ssid_buf), "warroom-rig-Files-%02X%02X", mac[4], mac[5]);
    ssid = String(ssid_buf);
    // Left empty on purpose — ensureSecrets() fills in whatever the operator
    // did not override, once the radio is up and esp_random() is a real TRNG.
    password = "";
    http_user = String(DEFAULT_HTTP_USER);
    http_password = "";
    http_auth_digest = true;

    if (!sd_obj.supported) return;  // SD missing — go with defaults.

    File f = SD.open(SETTINGS_PATH, FILE_READ);
    if (!f) return;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.charAt(0) == '#') continue;
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        String key = line.substring(0, eq);
        String val = line.substring(eq + 1);
        key.trim();
        val.trim();
        if (key == "ssid" && val.length() > 0) ssid = val;
        else if (key == "pass" && val.length() >= 8) password = val;
        else if (key == "user" && val.length() > 0) http_user = val;
        else if (key == "httppass" && val.length() > 0) http_password = val;
        else if (key == "auth") {
            val.toLowerCase();
            http_auth_digest = (val != "basic");
        }
    }
    f.close();
    // SSID only. The passwords go to the TFT, which needs someone standing at
    // the rig; on a screenless build drawStaticFrame() falls back to serial,
    // and that is the only path that ever prints them.
    Serial.printf("FILES: loaded settings ssid=%s\n", ssid.c_str());
}

// =========================================================================
// Per-device secrets
// =========================================================================
// A constant PSK in a public repo is the same PSK on every unit, and deriving
// one from the MAC is no better: the SoftAP puts its BSSID in every beacon, so
// a passive listener can recompute anything MAC-derived without ever
// associating. The only shape that actually holds is a random secret the
// device keeps to itself, which is why these live in NVS and are shown on the
// TFT rather than written anywhere a client could read them.
static String makeRandomSecret(uint8_t len) {
    String out;
    out.reserve(len);
    for (uint8_t i = 0; i < len; i++) {
        out += SECRET_ALPHABET[esp_random() & 31];
    }
    return out;
}

void FileServerAP::ensureSecrets() {
    bool need_psk = (password.length() < 8);
    bool need_httppw = (http_password.length() == 0);
    if (!need_psk && !need_httppw) return;  // both came from /fileserver.txt

    Preferences prefs;
    // Read-write: the first run has to store what it generates. If NVS is
    // unavailable we still come up secured, the secrets just change every
    // session and the phone has to forget the network each time.
    bool have_nvs = prefs.begin(PREFS_NAMESPACE, false);
    if (!have_nvs) Serial.println("FILES: NVS unavailable, secrets are session-only");

    if (need_psk) {
        String v = have_nvs ? prefs.getString(PREFS_KEY_PSK, "") : String("");
        if (v.length() < 8) {
            v = makeRandomSecret(GENERATED_PSK_LEN);
            if (have_nvs) prefs.putString(PREFS_KEY_PSK, v);
        }
        password = v;
    }
    if (need_httppw) {
        String v = have_nvs ? prefs.getString(PREFS_KEY_HTTPPW, "") : String("");
        if (v.length() == 0) {
            v = makeRandomSecret(GENERATED_HTTPPW_LEN);
            if (have_nvs) prefs.putString(PREFS_KEY_HTTPPW, v);
        }
        http_password = v;
    }
    if (have_nvs) prefs.end();
}

// =========================================================================
// AP bring-up
// =========================================================================
bool FileServerAP::startSoftAP() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    delay(50);
    // Only now — WiFi.mode() has started the radio, and esp_random() is only
    // specified to be a hardware RNG while RF is running. Generating the
    // secrets before this point would seed them from the bootup PRNG.
    ensureSecrets();
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    bool ok = WiFi.softAP(ssid.c_str(), password.c_str(), AP_CHANNEL, /*hidden*/ 0, AP_MAX_CONN);
    if (!ok) {
        last_error_msg = "softAP() returned false";
        return false;
    }
    Serial.printf("FILES: SoftAP \"%s\" @ %s\n", ssid.c_str(), WiFi.softAPIP().toString().c_str());
    return true;
}

// =========================================================================
// HTML rendering helpers
// =========================================================================
static String htmlEscape(const String& s) {
    String out;
    out.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        switch (c) {
            case '<': out += "&lt;";  break;
            case '>': out += "&gt;";  break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            // Attributes in this page are single-quoted (value='...'), so the
            // apostrophe is as much of an attribute terminator as the double
            // quote is. A filename is attacker-placeable — anything that lands
            // on the card ends up in this HTML.
            case '\'': out += "&#39;"; break;
            default:  out += c;
        }
    }
    return out;
}

// Escape for a single-quoted JavaScript string literal that lives inside an
// HTML attribute (the delete button's onsubmit="return confirm('...')").
// htmlEscape() on its own does not close that hole: the HTML parser turns
// &#39; back into a bare apostrophe *before* the JS parser ever sees the
// attribute, so the string literal still ends early. Backslash escapes survive
// the entity decode, so js-escape first and let htmlEscape() run over the
// result.
static String jsStringEscape(const String& s) {
    String out;
    out.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        if (c == '\\' || c == '\'' || c == '"') {
            out += '\\';
            out += c;
        } else if (c == '\r' || c == '\n') {
            out += ' ';  // a literal newline would also terminate the literal
        } else {
            out += c;
        }
    }
    return out;
}

static String urlEncode(const String& s) {
    String out;
    out.reserve(s.length() + 8);
    const char* hex = "0123456789ABCDEF";
    for (size_t i = 0; i < s.length(); i++) {
        unsigned char c = (unsigned char)s.charAt(i);
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

static String formatSize(uint32_t bytes) {
    char b[24];
    if (bytes < 1024) snprintf(b, sizeof(b), "%u B", (unsigned)bytes);
    else if (bytes < 1024UL * 1024UL) snprintf(b, sizeof(b), "%.1f KB", bytes / 1024.0);
    else snprintf(b, sizeof(b), "%.1f MB", bytes / (1024.0 * 1024.0));
    return String(b);
}

static String guessMime(const String& path) {
    String lower = path;
    lower.toLowerCase();
    if (lower.endsWith(".csv") || lower.endsWith(".log") || lower.endsWith(".txt")) return "text/csv";
    if (lower.endsWith(".json")) return "application/json";
    if (lower.endsWith(".pcap") || lower.endsWith(".cap")) return "application/vnd.tcpdump.pcap";
    if (lower.endsWith(".html") || lower.endsWith(".htm")) return "text/html";
    return "application/octet-stream";
}

// Chunked-response state for /ls and /. ESPAsyncWebServer's callback variant
// asks the producer for chunks of up to `maxLen` bytes at a time; this means
// we never need to hold the entire HTML in RAM. The state machine keeps a
// `pending` String of bytes already generated but not yet handed to the
// network task, refills it as needed, and returns slices of it.
//
// Previous attempts (single big buffer at 1460 / 8K / 32K) either truncated
// silently because resizeAdd() failed under heap pressure (140-entry SD =
// ~21 KB HTML), or crashed the Marauder outright at 32K. Chunked sidesteps
// the entire allocation problem.
struct ListingChunkState {
    String dir_path;
    File dir;
    int phase = 0;       // 0=header, 1=entries, 2=footer, 3=done
    uint32_t count = 0;
    String pending;      // bytes produced but not yet handed to the network
};

static void appendEntryRow(String& out, const String& dir_path, File& entry) {
    String n = entry.name();
    int slash = n.lastIndexOf('/');
    String base = (slash >= 0) ? n.substring(slash + 1) : n;

    String full = dir_path;
    if (!full.endsWith("/")) full += "/";
    full += base;

    File handle = SD.open(full);
    bool is_dir = handle ? handle.isDirectory() : entry.isDirectory();
    uint32_t sz = is_dir ? 0 : (handle ? handle.size() : entry.size());
    if (handle) handle.close();

    out += "<tr><td>";
    if (is_dir) {
        out += "<a href='/ls?path=" + urlEncode(full) + "'>" + htmlEscape(base) + "/</a>";
    } else {
        out += "<a href='/dl?path=" + urlEncode(full) + "'>" + htmlEscape(base) + "</a>";
    }
    out += "</td><td class=n>";
    if (!is_dir) out += formatSize(sz);
    else out += "&mdash;";
    out += "</td><td>";
    if (!is_dir) {
        out += "<form method=post action='/rm' style='display:inline' onsubmit=\"return confirm('Delete ";
        out += htmlEscape(jsStringEscape(base));  // JS string inside an HTML attribute — both layers
        out += "?')\"><input type=hidden name=path value='";
        out += htmlEscape(full);
        out += "'><button class=rm type=submit>delete</button></form>";
    }
    out += "</td></tr>";
}

static void produceMoreBytes(ListingChunkState* s, size_t want) {
    while (s->pending.length() < want && s->phase != 3) {
        switch (s->phase) {
            case 0: {
                s->pending += F("<!doctype html><html><head><meta charset=utf-8>"
                                "<meta name=viewport content='width=device-width,initial-scale=1'>"
                                "<title>warroom-rig Files</title>"
                                "<style>"
                                "body{font:14px/1.4 -apple-system,Segoe UI,sans-serif;margin:1em;color:#222}"
                                "h1{font-size:1.1em;margin:0 0 .5em}"
                                "table{border-collapse:collapse;width:100%}"
                                "td,th{padding:.4em .6em;border-bottom:1px solid #eee;text-align:left}"
                                "th{background:#fafafa;font-weight:600}"
                                "tr:hover td{background:#f6f9ff}"
                                "a{color:#0055cc;text-decoration:none}a:hover{text-decoration:underline}"
                                ".n{text-align:right;color:#666;font-variant-numeric:tabular-nums}"
                                ".bc{color:#888;margin-bottom:.5em}"
                                "button.rm{background:none;border:1px solid #d33;color:#d33;cursor:pointer;border-radius:3px;padding:.1em .5em;font-size:.85em}"
                                "button.rm:hover{background:#fee}"
                                "</style></head><body><h1>SD Files</h1>");
                #ifdef MARAUDER_WDGWARS_UPLOAD
                s->pending += F("<p><a href='/wdgcfg'>&#9881; WDGWars Upload Config</a></p>");
                #endif

                // Breadcrumb.
                s->pending += "<div class=bc>";
                if (s->dir_path == "/") {
                    s->pending += "<a href='/'>/</a>";
                } else {
                    s->pending += "<a href='/'>/</a>";
                    String acc;
                    int from = 1;
                    while (from < (int)s->dir_path.length()) {
                        int next = s->dir_path.indexOf('/', from);
                        if (next < 0) next = s->dir_path.length();
                        String seg = s->dir_path.substring(from, next);
                        acc += "/" + seg;
                        s->pending += " / <a href='/ls?path=" + urlEncode(acc) + "'>" + htmlEscape(seg) + "</a>";
                        from = next + 1;
                    }
                }
                s->pending += "</div>";

                if (!s->dir || !s->dir.isDirectory()) {
                    s->pending += "<p style='color:#d33'>Cannot open directory.</p></body></html>";
                    s->phase = 3;
                    break;
                }
                s->pending += "<table><thead><tr><th>Name</th><th class=n>Size</th><th></th></tr></thead><tbody>";
                s->phase = 1;
                break;
            }
            case 1: {
                File entry = s->dir.openNextFile();
                if (!entry) {
                    s->phase = 2;
                    break;
                }
                appendEntryRow(s->pending, s->dir_path, entry);
                entry.close();
                s->count++;
                if (s->count > 500) { s->phase = 2; }
                break;
            }
            case 2: {
                s->pending += "</tbody></table>";
                if (s->count == 0) s->pending += "<p style='color:#888'>empty</p>";
                s->pending += "</body></html>";
                s->phase = 3;
                break;
            }
        }
    }
}

static void sendChunkedListing(AsyncWebServerRequest* request, const String& dir_path) {
    auto state = std::make_shared<ListingChunkState>();
    state->dir_path = dir_path;
    state->dir = SD.open(dir_path);

    AsyncWebServerResponse* response = request->beginChunkedResponse(
        "text/html; charset=utf-8",
        [state](uint8_t* buffer, size_t maxLen, size_t /*index*/) -> size_t {
            // Refill pending up to (and slightly beyond) maxLen, then hand
            // out up to maxLen bytes. Return 0 only when the state machine
            // is done AND the pending tail is fully drained — that signals
            // end-of-body to the framework.
            produceMoreBytes(state.get(), maxLen);
            size_t avail = state->pending.length();
            if (avail == 0) return 0;
            size_t take = avail < maxLen ? avail : maxLen;
            memcpy(buffer, state->pending.c_str(), take);
            state->pending.remove(0, take);
            return take;
        });
    request->send(response);
}

#ifdef MARAUDER_WDGWARS_UPLOAD
// Read the current wdgwars upload credentials from /wdgwars.txt — the exact file
// WdgwarsUpload::loadSettings() reads. Missing file or keys yield empty strings.
static void readWdgwarsCfg(String& ssid, String& pass, String& apikey) {
    ssid = ""; pass = ""; apikey = "";
    File f = SD.open("/wdgwars.txt", FILE_READ);
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;
        int eq = line.indexOf('=');
        if (eq <= 0) continue;
        String key = line.substring(0, eq); key.trim();
        String val = line.substring(eq + 1); val.trim();
        if (key == "ssid")        ssid   = val;
        else if (key == "pass")   pass   = val;
        else if (key == "apikey") apikey = val;
    }
    f.close();
}

// What the config form is allowed to say about a stored value: that it exists.
// Not its content, not its length.
static const __FlashStringHelper* wdgcfgStatus(const String& v) {
    return v.length() ? F("(configured)") : F("(not set)");
}
#endif  // MARAUDER_WDGWARS_UPLOAD

// =========================================================================
// Authentication
// =========================================================================
// Being associated to the AP is not authorisation. The card holds the full
// movement history and /wdgwars.txt, /rm deletes without a server-side
// confirmation, and the rig runs unattended — so the AP password is the outer
// gate and this is the inner one. The middleware is attached to the server
// rather than to each handler so that the catch-all 404 is covered too and a
// route added later cannot end up unprotected by omission.
void FileServerAP::configureAuth() {
    http_auth.setRealm("warroom-rig");
    http_auth.setAuthFailureMessage("Authentication required.");
    http_auth.setUsername(http_user.c_str());
    http_auth.setPassword(http_password.c_str());
    // Digest by default: the link is plain HTTP, and WPA2-PSK does not hide
    // one station's traffic from another station that knows the PSK, so basic
    // auth would hand the password to anyone else already on the AP.
    // AUTH_DENIED is the fail-closed branch — the middleware waves a request
    // through when it has no credentials, so a missing password must turn into
    // "nobody gets in", never "everybody gets in".
    if (http_auth.hasCredentials()) {
        http_auth.setAuthType(http_auth_digest ? AsyncAuthType::AUTH_DIGEST
                                               : AsyncAuthType::AUTH_BASIC);
    } else {
        http_auth.setAuthType(AsyncAuthType::AUTH_DENIED);
        Serial.println("FILES: no HTTP credentials, refusing every request");
    }
}

// =========================================================================
// Route handlers
// =========================================================================
void FileServerAP::registerRoutes() {
    if (!server) return;

    server->on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
        total_get_count++;
        setLastRequestPath("/");
        sendChunkedListing(request, "/");
    });

    server->on("/ls", HTTP_GET, [this](AsyncWebServerRequest* request) {
        total_get_count++;
        String raw = request->hasParam("path") ? request->getParam("path")->value() : String("/");
        String safe = sanitizePath(raw);
        setLastRequestPath(safe.c_str());
        if (safe.length() == 0) { request->send(400, "text/plain", "bad path"); return; }
        sendChunkedListing(request, safe);
    });

    server->on("/dl", HTTP_GET, [this](AsyncWebServerRequest* request) {
        total_get_count++;
        String raw = request->hasParam("path") ? request->getParam("path")->value() : String("");
        String safe = sanitizePath(raw);
        setLastRequestPath(safe.c_str());
        if (safe.length() == 0) { request->send(400, "text/plain", "bad path"); return; }
        if (!SD.exists(safe)) { request->send(404, "text/plain", "not found"); return; }
        File f = SD.open(safe, FILE_READ);
        if (!f) { request->send(500, "text/plain", "cannot open"); return; }
        if (f.isDirectory()) { f.close(); request->send(400, "text/plain", "is a directory"); return; }
        f.close();

        String base = safe;
        int s = safe.lastIndexOf('/');
        if (s >= 0) base = safe.substring(s + 1);

        // AsyncWebServer's send(FS&, ...) takes ownership of the file handle
        // and streams it asynchronously. download=true sets a Content-
        // Disposition: attachment header so browsers prompt for save.
        AsyncWebServerResponse* resp = request->beginResponse(SD, safe, guessMime(safe), true);
        resp->addHeader("Content-Disposition", "attachment; filename=\"" + base + "\"");
        request->send(resp);
        total_dl_count++;
    });

    server->on("/rm", HTTP_POST, [this](AsyncWebServerRequest* request) {
        String raw;
        if (request->hasParam("path", true)) raw = request->getParam("path", true)->value();
        else if (request->hasParam("path"))  raw = request->getParam("path")->value();
        String safe = sanitizePath(raw);
        setLastRequestPath(safe.c_str());
        if (safe.length() == 0) { request->send(400, "text/plain", "bad path"); return; }
        if (!SD.exists(safe)) { request->send(404, "text/plain", "not found"); return; }
        // Refuse to rm a directory — keep blast radius small.
        File probe = SD.open(safe);
        bool is_dir = probe && probe.isDirectory();
        if (probe) probe.close();
        if (is_dir) { request->send(400, "text/plain", "directory delete not supported"); return; }

        bool ok = SD.remove(safe);
        if (!ok) { request->send(500, "text/plain", "delete failed"); return; }
        total_rm_count++;
        Serial.printf("FILES: deleted %s\n", safe.c_str());
        // Redirect back to the parent directory after delete.
        String parent = safe;
        int p = parent.lastIndexOf('/');
        parent = (p > 0) ? parent.substring(0, p) : String("/");
        AsyncWebServerResponse* r = request->beginResponse(303, "text/plain", "deleted");
        r->addHeader("Location", "/ls?path=" + urlEncode(parent));
        request->send(r);
    });

#ifdef MARAUDER_WDGWARS_UPLOAD
    // Browser form to enter/edit the wdgwars upload credentials without pulling
    // the SD card. Writes /wdgwars.txt, which WdgwarsUpload reads on the next run.
    server->on("/wdgcfg", HTTP_GET, [this](AsyncWebServerRequest* request) {
        total_get_count++;
        setLastRequestPath("/wdgcfg");
        String ssid, pass, apikey;
        readWdgwarsCfg(ssid, pass, apikey);
        String h;
        h.reserve(1600);
        h += F("<!doctype html><html><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>WDGWars Config</title><style>"
               "body{font:14px/1.5 -apple-system,Segoe UI,sans-serif;margin:1em;color:#222;max-width:32em}"
               "h1{font-size:1.1em}label{display:block;margin:.8em 0 .2em;font-weight:600}"
               "input{width:100%;padding:.5em;border:1px solid #ccc;border-radius:4px;box-sizing:border-box}"
               "button{margin-top:1em;padding:.6em 1.2em;background:#0055cc;color:#fff;border:0;border-radius:4px;cursor:pointer}"
               "a{color:#0055cc}.ok{background:#e6ffed;border:1px solid #34c759;padding:.5em;border-radius:4px}"
               ".hint{color:#888;font-size:.9em}code{background:#f2f2f2;padding:0 .3em;border-radius:3px}"
               ".st{font-weight:400;color:#888;font-size:.9em}"
               "</style></head><body><h1>WDGWars Upload Config</h1>"
               "<p><a href='/'>&larr; SD Files</a></p>");
        if (request->hasParam("saved")) h += F("<p class=ok>Saved to /wdgwars.txt.</p>");
        // The stored values are never rendered back. This form used to
        // pre-fill them as plain input values, which put the home WiFi
        // password and the API key on screen in cleartext for anyone standing
        // near the phone or looking at a page left open. Showing only whether
        // a value is set keeps the form usable without doing that.
        h += F("<form method=post action='/wdgcfg' autocomplete=off>"
               "<label>WiFi SSID <span class=st>");
        h += wdgcfgStatus(ssid);
        h += F("</span></label>"
               "<input name=ssid type=text autocomplete=off autocapitalize=off autocorrect=off "
               "placeholder='leave empty to keep current'>"
               "<label>WiFi Password <span class=st>");
        h += wdgcfgStatus(pass);
        h += F("</span></label>"
               "<input name=pass type=text autocomplete=off autocapitalize=off autocorrect=off "
               "placeholder='leave empty to keep current'>"
               "<label>wdgwars API Key <span class=st>");
        h += wdgcfgStatus(apikey);
        h += F("</span></label>"
               "<input name=apikey type=text autocomplete=off autocapitalize=off autocorrect=off "
               "placeholder='leave empty to keep current'>"
               "<button type=submit>Save</button>"
               "<p class=hint>Stored as <code>/wdgwars.txt</code> on the SD card and used by "
               "WDGWars Upload mode to connect and authenticate. A field left empty keeps the "
               "value that is already stored; to wipe them, delete <code>/wdgwars.txt</code> "
               "from the file listing.</p>"
               "</form></body></html>");
        request->send(200, "text/html", h);
    });

    server->on("/wdgcfg", HTTP_POST, [this](AsyncWebServerRequest* request) {
        setLastRequestPath("/wdgcfg");
        auto getP = [request](const char* n) -> String {
            if (request->hasParam(n, true)) return request->getParam(n, true)->value();
            return String();
        };
        String ssid = getP("ssid"); ssid.trim();
        String pass = getP("pass");                 // not trimmed: keep the password verbatim
        String apikey = getP("apikey"); apikey.trim();

        // The form no longer pre-fills the stored values, so an empty field
        // means "I did not touch this one", not "clear it". Merge against what
        // is on the card, otherwise saving a new API key would silently wipe
        // the WiFi credentials.
        String cur_ssid, cur_pass, cur_apikey;
        readWdgwarsCfg(cur_ssid, cur_pass, cur_apikey);
        if (ssid.length() == 0)   ssid   = cur_ssid;
        if (pass.length() == 0)   pass   = cur_pass;
        if (apikey.length() == 0) apikey = cur_apikey;

        String body;
        body.reserve(256);
        body += F("# wdgwars upload credentials (written by File Server config UI)\n");
        body += "ssid=";   body += ssid;   body += "\n";
        body += "pass=";   body += pass;   body += "\n";
        body += "apikey="; body += apikey; body += "\n";

        // Write to a scratch file and swap it in. The old shape opened
        // /wdgwars.txt with FILE_WRITE, which is "w" and truncates on open, so
        // a full card or a yanked SD destroyed the only copy of the home WiFi
        // password and the API key and left a 500 as the whole story. Here the
        // live file is only touched once the replacement is complete on the
        // card; if the rename is what fails, the data is still in the scratch
        // file for the operator to recover.
        static const char* TMP_PATH = "/wdgwars.tmp";
        static const char* CFG_PATH = "/wdgwars.txt";
        SD.remove(TMP_PATH);
        File f = SD.open(TMP_PATH, FILE_WRITE);
        if (!f) { request->send(500, "text/plain", "cannot write /wdgwars.tmp"); return; }
        size_t written = f.print(body);
        f.close();

        bool ok = (written == body.length());
        if (ok) {
            // Re-open and compare sizes: a short write on a full card can
            // still report the bytes as accepted until they are flushed.
            File v = SD.open(TMP_PATH, FILE_READ);
            ok = v && (v.size() == body.length());
            if (v) v.close();
        }
        if (!ok) {
            SD.remove(TMP_PATH);
            request->send(500, "text/plain", "write failed, /wdgwars.txt unchanged");
            return;
        }

        // FAT rename refuses an existing destination, so the old file has to
        // go first. The replacement is already complete on the card at this
        // point, so the worst case is a /wdgwars.tmp left behind.
        SD.remove(CFG_PATH);
        if (!SD.rename(TMP_PATH, CFG_PATH)) {
            request->send(500, "text/plain", "rename failed, new values are in /wdgwars.tmp");
            return;
        }
        Serial.printf("FILES: wrote %s (ssid=%s, apikey-len=%u)\n",
                      CFG_PATH, ssid.c_str(), apikey.length());
        AsyncWebServerResponse* r = request->beginResponse(303, "text/plain", "saved");
        r->addHeader("Location", "/wdgcfg?saved=1");
        request->send(r);
    });
#endif  // MARAUDER_WDGWARS_UPLOAD

    server->onNotFound([](AsyncWebServerRequest* request) {
        request->send(404, "text/plain", "not found");
    });
}

// =========================================================================
// Lifecycle
// =========================================================================
void FileServerAP::init() {
    active = true;
    state = State::INIT;
    total_get_count = total_dl_count = total_rm_count = 0;
    setLastRequestPath("");
    last_error_msg = "";
    center_press_start_ms = 0;
    center_was_pressed = false;
    // Whatever BACK is doing right now counts as already seen -- the key that
    // opened this view may still be down. See handleCenterLongPressForExit().
    #if defined(RIG_HAS_NAV) && defined(RIG_HAS_BACK)
      back_was_pressed = RigInput::down(RigInput::BACK);
    #else
      back_was_pressed = false;
    #endif
    last_display_refresh_ms = 0;

    loadOptionalSettings();

    if (!startSoftAP()) {
        state = State::AP_FAILED;
        drawStaticFrame();
        return;
    }

    // The server, its handlers and the auth middleware are allocated on the
    // first entry into this mode and then kept for the rest of the process.
    // The old shape did `new AsyncWebServer` per session and never deleted it,
    // so every entry leaked a server plus seven handlers holding std::function
    // closures — a few KB each time out of ~150-200 KB free. Deleting on exit
    // is what the empirical teardown order in deinit() warns about (AsyncTCP
    // still fires cleanup events into the server after the AP goes down), so
    // the fix is to stop allocating instead of to start deleting: one
    // allocation total, no per-session growth, and nothing to free late.
    // Routes capture `this`, which is a global, so they stay valid across
    // sessions; only the credentials change, and those live in the middleware.
    if (!server) {
        server = new AsyncWebServer(HTTP_PORT);
        server->addMiddleware(&http_auth);
        registerRoutes();
    }
    configureAuth();
    server->begin();

    state = State::AP_UP;
    drawStaticFrame();
    renderDisplay();  // also paint the dynamic values once so the first
                     // second of operation doesn't show stale zeroes.
    Serial.println("FILES: server up");
}

void FileServerAP::deinit() {
    if (!active) return;
    // Empirical teardown order — the previous reverse order (WiFi off
    // first, then server end) hung the Marauder reliably. Theory: killing
    // softAP RST-closes any open client TCP sockets, AsyncTCP fires per-
    // connection cleanup events into the still-registered server lambdas,
    // and one of those touches state the new ordering nukes first.
    //
    // New order: server end -> generous drain delay -> WiFi off. The server
    // object itself deliberately stays alive and keeps its routes; init()
    // reuses it, so nothing has to be freed here while AsyncTCP may still be
    // dispatching into it. AsyncServer::end() drops the listening pcb and
    // begin() recreates it, so the reuse is a supported cycle.
    Serial.println("FILES: deinit step 1: server->end()");
    if (server) server->end();
    Serial.println("FILES: deinit step 2: drain 250ms");
    delay(250);
    Serial.println("FILES: deinit step 3: softAPdisconnect");
    WiFi.softAPdisconnect(true);
    delay(50);
    Serial.println("FILES: deinit step 4: WiFi.mode(WIFI_OFF)");
    WiFi.mode(WIFI_OFF);
    active = false;
    state = State::DONE;
    // Hand control back to the Marauder menu — same trick WardriveCore uses
    // at its exit. Without this the WiFiScan dispatch loop keeps re-entering
    // our (now no-op) runTick() forever and the menu never repaints.
    #ifdef HAS_SCREEN
        display_obj.clearScreen();
    #endif
    wifi_scan_obj.currentScanMode = WIFI_SCAN_OFF;
    Serial.println("FILES: deinit done");
}

void FileServerAP::runTick() {
    if (!active) return;
    uint32_t now = millis();
    if (now - last_display_refresh_ms >= FILESERVER_DISPLAY_REFRESH_MS) {
        last_display_refresh_ms = now;
        renderDisplay();
    }
    handleCenterLongPressForExit();
}

// =========================================================================
// UI
// =========================================================================
// Layout strategy: a static frame (title, constant fields, footer) is
// painted exactly once in drawStaticFrame() at init(). The periodic
// renderDisplay() only refreshes the dynamic value lines, and does so by
// re-printing with setTextColor(fg, bg) so the new glyphs overwrite the
// old ones in place — no clear-then-redraw flicker.

// These were written for the 240x320 Marauder panel and nothing else. On the
// Cardputer ADV's 240x135 the credentials ran off the bottom and the footer sat
// at y=300 on a 135 px screen -- so the one line telling the operator how to
// leave was the one line they could not see. The tall numbers below are exactly
// what they always were; only the compact column is new.
//
// Gaps are 8 px on the tall panel and 4 on the short one: on 135 px a group
// separator that costs a whole extra line is a separator that pushes a line off
// the screen.
static const int FS_GAP       = RigTheme::COMPACT ? 4 : 8;
static const int FS_X         = 4;
static const int FS_LH        = RigTheme::COMPACT ? 10 : 16;
// The rows start under the rig case's bar and status line, which RigView
// paints for this view; the old "File Server" title is carried by the bar.
// On the short screen the channel row is dropped -- the status line shows
// the channel there -- so the block still clears the footer.
#include "RigView.h"
static const int FS_Y_SSID    = RigTheme::HEADER_H + 8;
static const int FS_Y_PASS    = FS_Y_SSID + FS_LH;
static const int FS_Y_IP      = FS_Y_PASS + FS_LH;
static const int FS_Y_CHAN    = FS_Y_IP + FS_LH;
static const int FS_Y_LOGIN   = RigTheme::COMPACT ? FS_Y_CHAN : (FS_Y_CHAN + FS_LH);
static const int FS_Y_CLIENTS = FS_Y_LOGIN + FS_LH + FS_GAP;
static const int FS_Y_COUNTS  = FS_Y_CLIENTS + FS_LH;
static const int FS_Y_LAST    = FS_Y_COUNTS + FS_LH + FS_GAP;
// The tall panel keeps its literal 300 rather than SCREEN_HEIGHT - FOOTER_H,
// which is 298. The two look identical and the derived form is tidier, but the
// V7 is the field device: this change set exists to fix the ADV, not to move
// anything on a screen that was already right.
static const int FS_Y_FOOTER  = RigTheme::COMPACT ? (SCREEN_HEIGHT - RigTheme::FOOTER_H)
                                                  : 300;

// How much of a request path fits on the "Last:" line. Font 1 is 6 px per
// character, and "Last: " eats six of them. The old code trimmed to 30 and
// padded to 36, i.e. 42 columns on a 40-column screen -- it wrapped onto the
// next line on *both* panels, which on the short one is the line above the
// footer. Derive it instead of guessing.
static const int FS_PATH_COLS = (SCREEN_WIDTH / 6) - 6;

void FileServerAP::drawStaticFrame() {
    #ifdef HAS_SCREEN
        display_obj.clearScreen();
        rig_view_obj.drawCaseChrome("FILE SRV");

        display_obj.tft.setTextSize(1);

        if (state == State::AP_FAILED) {
            display_obj.tft.setTextColor(RigTheme::RED, TFT_BLACK);
            display_obj.tft.setCursor(FS_X, FS_Y_SSID);
            display_obj.tft.print("AP start failed");
            display_obj.tft.setCursor(FS_X, FS_Y_PASS);
            display_obj.tft.print(last_error_msg);
        } else {
            // Constant-for-the-session values: print once here so
            // renderDisplay() never touches them.
            //
            // The PSK and the login are generated per device, so this screen
            // is the only place the operator can read them. That is the point:
            // whoever can see the display is standing at the rig. Nothing here
            // is derivable from what the AP puts on the air.
            display_obj.tft.setTextColor(RigTheme::INK, TFT_BLACK);
            display_obj.tft.setCursor(FS_X, FS_Y_SSID);
            display_obj.tft.printf("SSID:  %s", ssid.c_str());
            display_obj.tft.setCursor(FS_X, FS_Y_PASS);
            display_obj.tft.printf("Pass:  %s", password.c_str());
            display_obj.tft.setCursor(FS_X, FS_Y_IP);
            display_obj.tft.printf("IP:    %s", WiFi.softAPIP().toString().c_str());
            if (!RigTheme::COMPACT) {   // the short screen's status line carries the channel
                display_obj.tft.setCursor(FS_X, FS_Y_CHAN);
                display_obj.tft.printf("Chan:  %u", (unsigned)AP_CHANNEL);
            }
            display_obj.tft.setTextColor(RigTheme::GOLD, TFT_BLACK);
            display_obj.tft.setCursor(FS_X, FS_Y_LOGIN);
            display_obj.tft.printf("Login: %s / %s", http_user.c_str(), http_password.c_str());
        }

        // Name the keys this board actually has. "CENTER hold" is meaningless on
        // a keyboard, and it was the only instruction on screen.
        display_obj.tft.setTextColor(RigTheme::DIM2, TFT_BLACK);
        display_obj.tft.setCursor(FS_X, FS_Y_FOOTER);
        #ifdef HAS_TOUCH
            display_obj.tft.print("[tap screen to exit]");
        #else
            display_obj.tft.print(RIG_HINT_EXIT);
        #endif
    #else
        // Screenless target: the console is the only channel the operator has
        // for the generated credentials, so this is the one place they go to
        // serial. It is not free — whoever has the USB port has them — but a
        // build that cannot show its own PSK cannot be used at all, and USB
        // access already means standing at the rig.
        if (state == State::AP_FAILED) {
            Serial.printf("FILES: AP start failed: %s\n", last_error_msg.c_str());
        } else {
            Serial.printf("FILES: SSID=%s PSK=%s login=%s/%s\n",
                          ssid.c_str(), password.c_str(),
                          http_user.c_str(), http_password.c_str());
        }
    #endif
}

void FileServerAP::renderDisplay() {
    #ifdef HAS_SCREEN
        if (state == State::AP_FAILED) return;  // nothing dynamic to refresh
        display_obj.tft.setTextSize(1);

        // Clients counter — written in green, padded so a drop from 2 -> 1
        // clean-overwrites the trailing digit slot.
        display_obj.tft.setTextColor(RigTheme::GREEN, TFT_BLACK);
        display_obj.tft.setCursor(FS_X, FS_Y_CLIENTS);
        display_obj.tft.printf("Clients: %u   ", (unsigned)WiFi.softAPgetStationNum());

        // Feed the case's traffic pulse: requests served since the last paint.
        {
            static uint32_t pulsed_hits = 0;
            uint32_t hits = (uint32_t)total_get_count + (uint32_t)total_dl_count + (uint32_t)total_rm_count;
            if (hits < pulsed_hits) pulsed_hits = 0;   // counters reset on a new session
            rig_view_obj.pulse((uint16_t)((hits - pulsed_hits) > 60 ? 60 : (hits - pulsed_hits)));
            pulsed_hits = hits;
        }

        // Hit counters.
        display_obj.tft.setTextColor(RigTheme::INK, TFT_BLACK);
        display_obj.tft.setCursor(FS_X, FS_Y_COUNTS);
        display_obj.tft.printf("GET %lu  DL %lu  RM %lu          ",
                               (unsigned long)total_get_count,
                               (unsigned long)total_dl_count,
                               (unsigned long)total_rm_count);

        // Last request line — trim long paths from the left so the tail
        // (which is the most informative bit) stays visible. Pad to a fixed
        // width so a shorter follow-up path doesn't leave stale glyphs.
        display_obj.tft.setTextColor(RigTheme::DIM, TFT_BLACK);
        display_obj.tft.setCursor(FS_X, FS_Y_LAST);
        // Take a private copy under the lock first — everything below runs on
        // the loop task and must not walk storage the HTTP task can rewrite.
        char path_copy[LAST_PATH_MAX];
        copyLastRequestPath(path_copy, sizeof(path_copy));
        String shown = path_copy;
        if ((int)shown.length() > FS_PATH_COLS)
            shown = "..." + shown.substring(shown.length() - (FS_PATH_COLS - 3));
        // Pad to the full width so any previous value gets fully overwritten.
        while ((int)shown.length() < FS_PATH_COLS) shown += ' ';
        display_obj.tft.printf("Last: %s", shown.c_str());
    #endif
}

// =========================================================================
// Cross-task display state
// =========================================================================
// The route handlers run on the AsyncTCP task, renderDisplay() on the loop
// task. The two used to share an Arduino String, and String assignment frees
// the old buffer before it publishes the new pointer — so a browse that
// happened to land between the display's read of the pointer and its read of
// the bytes handed the TFT freed heap: garbage glyphs at best, a
// LoadProhibited panic at worst. Normal use triggers it, no attacker needed.
//
// Fixed storage removes the free entirely, and the spinlock removes the torn
// read on top of it. Both sides are a bounded memcpy, so holding a critical
// section across them is cheap.
void FileServerAP::setLastRequestPath(const char* p) {
    if (!p) p = "";
    size_t n = strlen(p);
    if (n > LAST_PATH_MAX - 1) n = LAST_PATH_MAX - 1;
    portENTER_CRITICAL(&last_path_mux);
    memcpy(last_request_path, p, n);
    last_request_path[n] = '\0';
    portEXIT_CRITICAL(&last_path_mux);
}

void FileServerAP::copyLastRequestPath(char* dst, size_t n) const {
    if (!dst || n == 0) return;
    size_t i = 0;
    portENTER_CRITICAL(&last_path_mux);
    while (i < n - 1 && last_request_path[i] != '\0') {
        dst[i] = last_request_path[i];
        i++;
    }
    portEXIT_CRITICAL(&last_path_mux);
    dst[i] = '\0';
}

void FileServerAP::handleCenterLongPressForExit() {
    #ifdef RIG_HAS_NAV
        bool pressed_now = (RigInput::down(RigInput::SELECT));
        uint32_t now = millis();
        if (pressed_now) {
            if (!center_was_pressed) {
                center_was_pressed = true;
                center_press_start_ms = now;
            } else if ((now - center_press_start_ms) >= FILESERVER_EXIT_HOLD_MS) {
                Serial.println("FILES: long-press -> exit");
                deinit();
                return;
            }
        } else {
            center_was_pressed = false;
            center_press_start_ms = 0;
        }

        #ifdef RIG_HAS_BACK
          // A board with a dedicated back key should not have to learn a hold --
          // and on the ADV the hold was the *only* way out while the line saying
          // so was drawn below the bottom of the screen. Same shape as Rig Mode.
          //
          // Edge-triggered, and seeded from the live key state in init(): ESC may
          // still be down from whatever menu opened this view, and an unseeded
          // detector reads that as a fresh press and closes on the first frame.
          const bool back_now = RigInput::down(RigInput::BACK);
          if (back_now && !back_was_pressed) {
              back_was_pressed = true;
              Serial.println("FILES: BACK -> exit");
              deinit();
              return;
          }
          if (!back_now) back_was_pressed = false;
        #endif
    #endif
}

#endif  // MARAUDER_FILE_SERVER_AP
