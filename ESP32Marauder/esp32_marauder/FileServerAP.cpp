#include "FileServerAP.h"

#include "RigInput.h"
#ifdef MARAUDER_FILE_SERVER_AP

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_wifi.h>
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
static const char* DEFAULT_PASSWORD = "warroomrig";
static const char* SETTINGS_PATH = "/fileserver.txt";

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
    password = String(DEFAULT_PASSWORD);

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
    }
    f.close();
    Serial.printf("FILES: loaded settings ssid=%s\n", ssid.c_str());
}

// =========================================================================
// AP bring-up
// =========================================================================
bool FileServerAP::startSoftAP() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    delay(50);
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
            default:  out += c;
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

// Stream the directory listing directly into a Print target (typically an
// AsyncResponseStream). The earlier `renderListingHtml() -> String` variant
// truncated to ~8 KB even when the SD held 140+ entries — observed on TJ's
// Armbian-overlay SD: serial showed all entries enumerated, browser only
// rendered the first two table rows. Streaming sidesteps the String-size
// limit entirely and keeps heap pressure low.
static void writeListingHtml(Print& out, const String& dir_path) {
    out.print(F("<!doctype html><html><head><meta charset=utf-8>"
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
              "</style></head><body>"));
    out.print(F("<h1>SD Files</h1>"));

    // Breadcrumb.
    out.print(F("<div class=bc>"));
    if (dir_path == "/") {
        out.print(F("<a href='/'>/</a>"));
    } else {
        out.print(F("<a href='/'>/</a>"));
        String acc;
        int from = 1;
        while (from < (int)dir_path.length()) {
            int next = dir_path.indexOf('/', from);
            if (next < 0) next = dir_path.length();
            String seg = dir_path.substring(from, next);
            acc += "/" + seg;
            out.print(F(" / <a href='/ls?path="));
            out.print(urlEncode(acc));
            out.print(F("'>"));
            out.print(htmlEscape(seg));
            out.print(F("</a>"));
            from = next + 1;
        }
    }
    out.print(F("</div>"));

    File dir = SD.open(dir_path);
    if (!dir) {
        out.print(F("<p style='color:#d33'>Cannot open directory.</p></body></html>"));
        return;
    }
    if (!dir.isDirectory()) {
        dir.close();
        out.print(F("<p style='color:#d33'>Not a directory.</p></body></html>"));
        return;
    }

    out.print(F("<table><thead><tr><th>Name</th><th class=n>Size</th><th></th></tr></thead><tbody>"));

    File entry = dir.openNextFile();
    uint32_t count = 0;
    while (entry) {
        String n = entry.name();
        int slash = n.lastIndexOf('/');
        String base = (slash >= 0) ? n.substring(slash + 1) : n;

        // Build absolute path of this entry.
        String full = dir_path;
        if (!full.endsWith("/")) full += "/";
        full += base;

        // Defensive: the openNextFile() iterator returns File handles whose
        // isDirectory() and size() metadata is unreliable on some ESP32 SD
        // library versions. Re-open by full path to get correct metadata.
        File handle = SD.open(full);
        bool is_dir = handle ? handle.isDirectory() : entry.isDirectory();
        uint32_t sz = is_dir ? 0 : (handle ? handle.size() : entry.size());
        if (handle) handle.close();

        out.print(F("<tr><td>"));
        if (is_dir) {
            out.print(F("<a href='/ls?path="));
            out.print(urlEncode(full));
            out.print(F("'>"));
            out.print(htmlEscape(base));
            out.print(F("/</a>"));
        } else {
            out.print(F("<a href='/dl?path="));
            out.print(urlEncode(full));
            out.print(F("'>"));
            out.print(htmlEscape(base));
            out.print(F("</a>"));
        }
        out.print(F("</td><td class=n>"));
        if (!is_dir) out.print(formatSize(sz));
        else out.print(F("&mdash;"));
        out.print(F("</td><td>"));
        if (!is_dir) {
            out.print(F("<form method=post action='/rm' style='display:inline' "
                        "onsubmit=\"return confirm('Delete "));
            out.print(htmlEscape(base));
            out.print(F("?')\"><input type=hidden name=path value='"));
            out.print(htmlEscape(full));
            out.print(F("'><button class=rm type=submit>delete</button></form>"));
        }
        out.print(F("</td></tr>"));

        entry.close();
        entry = dir.openNextFile();
        count++;
        if (count > 500) break;  // hard cap to keep iteration bounded
    }
    dir.close();

    out.print(F("</tbody></table>"));
    if (count == 0) out.print(F("<p style='color:#888'>empty</p>"));
    out.print(F("</body></html>"));
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
        out += htmlEscape(base);
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
#endif  // MARAUDER_WDGWARS_UPLOAD

// =========================================================================
// Route handlers
// =========================================================================
void FileServerAP::registerRoutes() {
    if (!server) return;

    server->on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
        total_get_count++;
        last_request_path = "/";
        sendChunkedListing(request, "/");
    });

    server->on("/ls", HTTP_GET, [this](AsyncWebServerRequest* request) {
        total_get_count++;
        String raw = request->hasParam("path") ? request->getParam("path")->value() : String("/");
        String safe = sanitizePath(raw);
        last_request_path = safe;
        if (safe.length() == 0) { request->send(400, "text/plain", "bad path"); return; }
        sendChunkedListing(request, safe);
    });

    server->on("/dl", HTTP_GET, [this](AsyncWebServerRequest* request) {
        total_get_count++;
        String raw = request->hasParam("path") ? request->getParam("path")->value() : String("");
        String safe = sanitizePath(raw);
        last_request_path = safe;
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
        last_request_path = safe;
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
        last_request_path = "/wdgcfg";
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
               "</style></head><body><h1>WDGWars Upload Config</h1>"
               "<p><a href='/'>&larr; SD Files</a></p>");
        if (request->hasParam("saved")) h += F("<p class=ok>Saved to /wdgwars.txt.</p>");
        h += F("<form method=post action='/wdgcfg'>"
               "<label>WiFi SSID</label><input name=ssid autocapitalize=off autocorrect=off value=\"");
        h += htmlEscape(ssid);
        h += F("\"><label>WiFi Password</label><input name=pass autocapitalize=off autocorrect=off value=\"");
        h += htmlEscape(pass);
        h += F("\"><label>wdgwars API Key</label><input name=apikey autocapitalize=off autocorrect=off value=\"");
        h += htmlEscape(apikey);
        h += F("\"><button type=submit>Save</button>"
               "<p class=hint>Stored as <code>/wdgwars.txt</code> on the SD card and used by "
               "WDGWars Upload mode to connect and authenticate. An empty password means an open AP.</p>"
               "</form></body></html>");
        request->send(200, "text/html", h);
    });

    server->on("/wdgcfg", HTTP_POST, [this](AsyncWebServerRequest* request) {
        auto getP = [request](const char* n) -> String {
            if (request->hasParam(n, true)) return request->getParam(n, true)->value();
            return String();
        };
        String ssid = getP("ssid"); ssid.trim();
        String pass = getP("pass");                 // not trimmed: keep the password verbatim
        String apikey = getP("apikey"); apikey.trim();
        // Rewrite from scratch so stale keys never linger.
        SD.remove("/wdgwars.txt");
        File f = SD.open("/wdgwars.txt", FILE_WRITE);
        if (!f) { request->send(500, "text/plain", "cannot write /wdgwars.txt"); return; }
        f.println("# wdgwars upload credentials (written by File Server config UI)");
        f.print("ssid=");   f.println(ssid);
        f.print("pass=");   f.println(pass);
        f.print("apikey="); f.println(apikey);
        f.close();
        Serial.printf("FILES: wrote /wdgwars.txt (ssid=%s, apikey-len=%u)\n",
                      ssid.c_str(), apikey.length());
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
    last_request_path = "";
    last_error_msg = "";
    center_press_start_ms = 0;
    center_was_pressed = false;
    last_display_refresh_ms = 0;

    loadOptionalSettings();

    if (!startSoftAP()) {
        state = State::AP_FAILED;
        drawStaticFrame();
        return;
    }

    server = new AsyncWebServer(HTTP_PORT);
    registerRoutes();
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
    // New order: server end -> generous drain delay -> WiFi off. The
    // server is never deleted (AsyncWebServer leak ~few KB per session).
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

static const int FS_X = 4;
static const int FS_LH = 16;
static const int FS_Y_TITLE = 28;
static const int FS_Y_SSID = 60;
static const int FS_Y_PASS = FS_Y_SSID + FS_LH;
static const int FS_Y_IP = FS_Y_PASS + FS_LH;
static const int FS_Y_CHAN = FS_Y_IP + FS_LH;
static const int FS_Y_CLIENTS = FS_Y_CHAN + FS_LH + 8;
static const int FS_Y_COUNTS = FS_Y_CLIENTS + FS_LH;
static const int FS_Y_LAST = FS_Y_COUNTS + FS_LH + 8;
static const int FS_Y_FOOTER = 300;

void FileServerAP::drawStaticFrame() {
    #ifdef HAS_SCREEN
        display_obj.clearScreen();
        display_obj.tft.setTextSize(2);
        display_obj.tft.setTextColor(TFT_CYAN, TFT_BLACK);
        display_obj.tft.setCursor(FS_X, FS_Y_TITLE);
        display_obj.tft.print("File Server");

        display_obj.tft.setTextSize(1);

        if (state == State::AP_FAILED) {
            display_obj.tft.setTextColor(TFT_RED, TFT_BLACK);
            display_obj.tft.setCursor(FS_X, FS_Y_SSID);
            display_obj.tft.print("AP start failed");
            display_obj.tft.setCursor(FS_X, FS_Y_PASS);
            display_obj.tft.print(last_error_msg);
        } else {
            // Constant-for-the-session values: print once here so
            // renderDisplay() never touches them.
            display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
            display_obj.tft.setCursor(FS_X, FS_Y_SSID);
            display_obj.tft.printf("SSID:  %s", ssid.c_str());
            display_obj.tft.setCursor(FS_X, FS_Y_PASS);
            display_obj.tft.printf("Pass:  %s", password.c_str());
            display_obj.tft.setCursor(FS_X, FS_Y_IP);
            display_obj.tft.printf("IP:    %s", WiFi.softAPIP().toString().c_str());
            display_obj.tft.setCursor(FS_X, FS_Y_CHAN);
            display_obj.tft.printf("Chan:  %u", (unsigned)AP_CHANNEL);
        }

        display_obj.tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        display_obj.tft.setCursor(FS_X, FS_Y_FOOTER);
        #ifdef HAS_TOUCH
            display_obj.tft.print("[tap screen to exit]");
        #else
            display_obj.tft.print("[CENTER hold to exit]");
        #endif
    #endif
}

void FileServerAP::renderDisplay() {
    #ifdef HAS_SCREEN
        if (state == State::AP_FAILED) return;  // nothing dynamic to refresh
        display_obj.tft.setTextSize(1);

        // Clients counter — written in green, padded so a drop from 2 -> 1
        // clean-overwrites the trailing digit slot.
        display_obj.tft.setTextColor(TFT_GREEN, TFT_BLACK);
        display_obj.tft.setCursor(FS_X, FS_Y_CLIENTS);
        display_obj.tft.printf("Clients: %u   ", (unsigned)WiFi.softAPgetStationNum());

        // Hit counters.
        display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
        display_obj.tft.setCursor(FS_X, FS_Y_COUNTS);
        display_obj.tft.printf("GET %lu  DL %lu  RM %lu          ",
                               (unsigned long)total_get_count,
                               (unsigned long)total_dl_count,
                               (unsigned long)total_rm_count);

        // Last request line — trim long paths from the left so the tail
        // (which is the most informative bit) stays visible. Pad to a fixed
        // width so a shorter follow-up path doesn't leave stale glyphs.
        display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        display_obj.tft.setCursor(FS_X, FS_Y_LAST);
        String shown = last_request_path;
        if (shown.length() > 30) shown = "..." + shown.substring(shown.length() - 27);
        // Pad to 36 chars so any previous value gets fully overwritten.
        while (shown.length() < 36) shown += ' ';
        display_obj.tft.printf("Last: %s", shown.c_str());
    #endif
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
            }
        } else {
            center_was_pressed = false;
            center_press_start_ms = 0;
        }
    #endif
}

#endif  // MARAUDER_FILE_SERVER_AP
