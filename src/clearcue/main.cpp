// Printer status/action button: product firmware (one binary, configured at runtime)
// ===============================================================================
// Hardware: ESP32-C3 SuperMini, tri-LED (RGB) illuminated NO momentary.
//   GPIO3 = GREEN, GPIO4 = RED, GPIO5 = BLUE (common anode, GPIO sinks: active-low),
//   GPIO10 = switch to GND (internal pull-up).
//
// Everything the old sketches had as compile-time constants (Wi-Fi, server, key,
// printer, hostname) now lives in NVS and is set by one of:
//   1. Captive portal: unconfigured (or Wi-Fi lost >5 min) -> open AP "<PRODUCT>-XXXX".
//      The phone gets a Wi-Fi-only step: the button joins the network without rebooting,
//      the phone shows http://<host>.local + the IP, and the rest is done from a PC.
//      (Full settings are still one tap away on the phone.)
//   2. Improv Serial: the ESP Web Tools flasher page pushes Wi-Fi creds after flashing
//      and links to the settings page.
//   3. Settings page on the LAN: http://<hostname>.local/ (optional admin password).
//
// Backends (picked at runtime):
//   bambuddy:   GET  {base}/printers/{id}/status          X-API-Key
//               POST {base}/printers/{id}/clear-plate
//               red = awaiting_plate_clear || state in {RUNNING,PREPARE,PAUSE}; armed = awaiting
//   watchtower: GET  {base}/printers/{name}               Authorization: Bearer
//               POST {base}/printers/{name}/commands {"command":"set_ready"}
//               red = !(ready && online); armed = online && !busy && !ready
//
// HTTPS to the printer server uses wolfSSL (tls_client.*), which handles Watchtower's
// name-constrained local CA. Optional CA pin + host/IP check; "Trust this server" on the
// settings page pins the server's CA by fingerprint. OTA stays on mbedTLS.
//
// LED patterns:
//   solid red/green          normal status
//   solid blue               setup network (AP) is up: first setup, or Wi-Fi lost >5 min
//   slow blue blink          on Wi-Fi, but printer-server settings not filled in yet
//   fast red/green alternate joining Wi-Fi
//   slow red blink           3+ failed checks in a row (server unreachable / auth / etc.)
//   fast white flicker       button held >3 s: release to cancel, keep holding to 10 s to factory reset
//
// Button: short press (<1.5 s) = action. Hold 10 s = factory reset.
// ===============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Update.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <memory>

#include "version.h"
#include "web_ui.h"
#include "ota_roots.h"
#include "improv.h"
#include "tls_client.h"

// wolfSSL handshakes (RSA-4096 CA verify + ECDHE in software) run on the loop task.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ---------------- Build-time product settings ----------------
#ifndef PRODUCT_NAME
#define PRODUCT_NAME "ClearCue"      // AP SSID prefix, default hostname prefix, page title
#endif
#ifndef DEFAULT_OTA_URL
#define DEFAULT_OTA_URL ""           // e.g. "https://updates.example.com/clearcue/ota.json"
#endif
#ifndef SOURCE_URL
#define SOURCE_URL ""
#endif
#ifndef OTA_ALLOW_INSECURE
#define OTA_ALLOW_INSECURE 0         // 1 = skip cert check on OTA downloads (not for shipping)
#endif
#ifndef DEBUG
#define DEBUG 1                      // 1 = debug logging compiled in (off at runtime until
#endif                               //     enabled on the settings page); 0 = compiled out
// Runtime switch for serial debug logging, stored in NVS ("debug"), set from the
// settings page. Off by default and after a factory reset.
volatile bool dbgEnabled = false;

// Every line is prefixed with seconds since boot ("[   12.345] [button] down") and ends
// in CR LF, so PuTTY and other plain serial terminals don't stair-step the output.
// Stack buffers only: Wi-Fi event callbacks log from another task.
static void logWrite(const char* fmt, va_list ap) {
  char msg[512];
  int n = vsnprintf(msg, sizeof(msg), fmt, ap);
  char out[680];
  uint32_t ms = millis();
  size_t o = snprintf(out, sizeof(out), "[%5lu.%03lu] ", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
  for (const char* p = msg; *p && o < sizeof(out) - 6; p++) {
    if (*p == '\n') out[o++] = '\r';
    out[o++] = *p;
  }
  if (n >= (int)sizeof(msg)) { memcpy(out + o, "...\r\n", 5); o += 5; }
  Serial.write((const uint8_t*)out, o);
}
// Always printed (boot banner, debug on/off): no server data in these.
static void logAlways(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void logAlways(const char* fmt, ...) { va_list ap; va_start(ap, fmt); logWrite(fmt, ap); va_end(ap); }
#if DEBUG
static void dbgLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void dbgLog(const char* fmt, ...) {
  if (!dbgEnabled) return;
  va_list ap; va_start(ap, fmt); logWrite(fmt, ap); va_end(ap);
}
#define DBG(...) dbgLog(__VA_ARGS__)
#else
#define DBG(...) do {} while (0)
#endif

// ---------------- Hardware ----------------
const int  LED_RED_PIN    = 4;
const int  LED_GREEN_PIN  = 3;
const int  LED_BLUE_PIN   = 5;   // setup / AP mode. Not a strapping pin on the C3.
const int  BUTTON_PIN     = 10;
const bool LED_ACTIVE_LOW = true;

// ---------------- Timing ----------------
const uint32_t HTTP_TIMEOUT_MS     = 8000;
const uint32_t DEBOUNCE_MS         = 40;
const uint32_t SHORT_PRESS_MAX_MS  = 1500;
const uint32_t RESET_WARN_MS       = 3000;
const uint32_t RESET_HOLD_MS       = 10000;
const uint32_t AP_FALLBACK_MS      = 300000;  // STA down 5 min -> open setup AP alongside
                                               // (rides out router reboots without going blue)
const uint32_t AP_IDLE_CLOSE_MS    = 120000;  // setup AP closes once STA is up and no phone has
                                               // been connected to it for this long
const uint32_t WIFI_TRY_MAX_MS     = 25000;   // phone Wi-Fi step: give up joining after this
const uint32_t WIFI_TRY_FAST_FAIL_MS = 8000;  // ...or after this, once the router has said why
const uint32_t STA_RETRY_MS        = 30000;
const uint8_t  FAIL_STREAK_ERROR   = 3;

// ---------------- Config (NVS) ----------------
struct Config {
  String ssid, pass, backend, apiBase, apiKey, printer, hostname, caCert, adminPass, otaUrl;
  uint32_t pollSec = 10;
  bool     debug = false;          // serial debug logging
  bool hasWifi() const { return ssid.length() > 0; }
  bool configured() const {
    return hasWifi() && apiBase.length() && apiKey.length() && printer.length();
  }
  bool isWatchtower() const { return backend == "watchtower"; }
};

Config      cfg;
Preferences prefs;
static const char* NVS_NS = "btncfg";

// ---------------- Runtime state ----------------
struct PollResult {
  bool     ok = false;
  bool     red = true;
  bool     armed = false;
  int      http = 0;
  String   url, transport, tls, body, summary, hint;
  uint32_t at = 0;
};

WebServer        server(80);
DNSServer        dns;
TlsClient        apiTls;          // HTTPS to the printer server (wolfSSL)
WiFiClient       plainClient;
improv::Serial_  improvSerial;

String   macSuffix;            // "3f2a"
bool     apActive = false;
uint32_t apStartedAt = 0;
uint32_t apLastClientAt = 0;   // last time a phone was on the setup AP (or setup progressed)

// Phone setup, step 1: join Wi-Fi from the captive portal without rebooting, so the
// phone can show the address to finish setup at from a PC.
enum WifiTry : uint8_t { WT_IDLE, WT_CONNECTING, WT_OK, WT_FAIL };
WifiTry  wtState = WT_IDLE;
String   wtSsid, wtPass, wtError;
uint32_t wtStart = 0;
volatile uint8_t staDiscReason = 0;   // from ARDUINO_EVENT_WIFI_STA_DISCONNECTED
bool     apClosePending = false;
uint32_t apCloseAt = 0;
bool     staWasUp = false;
uint32_t staDownSince = 0;
uint32_t lastStaBegin = 0;
bool     mdnsUp = false;

PollResult last;
bool       haveStatus = false;
bool       showRed = true;
bool       armed = false;
uint8_t    failStreak = 0;
uint32_t   lastPoll = 0;
uint32_t   pollDelay = 10000;
bool       pollNowFlag = true;

String   pendingOtaUrl;         // set by /api/ota install; executed from loop()
bool     rebootPending = false;
uint32_t rebootAt = 0;

// Button
int      btnLastReading = HIGH, btnStable = HIGH;
uint32_t btnChangedAt = 0, btnDownAt = 0;
bool     btnHeld = false;

// ===================================================================
// Helpers
// ===================================================================
void setLed(int pin, bool on) { digitalWrite(pin, LED_ACTIVE_LOW ? !on : on); }
void leds(bool red, bool green, bool blue = false) {
  setLed(LED_RED_PIN, red); setLed(LED_GREEN_PIN, green); setLed(LED_BLUE_PIN, blue);
}

void blink(int pin, int times, int ms) {
  leds(false, false, false);
  for (int i = 0; i < times; i++) { setLed(pin, true); delay(ms); setLed(pin, false); delay(ms); }
}

// Why the chip last restarted. A button press showing up here as BROWNOUT or POWERON
// means the press itself is resetting the board (wiring), not a firmware path.
const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON (power applied / power dipped)";
    case ESP_RST_EXT:       return "EXT (reset pin)";
    case ESP_RST_SW:        return "SW (firmware restart)";
    case ESP_RST_PANIC:     return "PANIC (crash)";
    case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (task watchdog)";
    case ESP_RST_WDT:       return "WDT (other watchdog)";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (supply voltage dropped)";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB (reset over USB, e.g. flashing)";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "UNKNOWN";
  }
}

String sanitizeHostname(String h) {
  h.toLowerCase();
  String out;
  for (char c : h) if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') out += c;
  while (out.startsWith("-")) out.remove(0, 1);
  while (out.endsWith("-")) out.remove(out.length() - 1);
  if (out.length() > 32) out = out.substring(0, 32);
  return out;
}

String defaultHostname() {
  String p = PRODUCT_NAME;
  return sanitizeHostname(p + "-" + macSuffix);
}
String apSsid() { String s = macSuffix; s.toUpperCase(); return String(PRODUCT_NAME) + "-" + s; }

String macString() {
  uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_STA);
  char b[18]; snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return b;
}

String urlEncodeSegment(const String& s) {
  String o; char hex[4];
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { snprintf(hex, sizeof(hex), "%%%02X", c); o += hex; }
  }
  return o;
}

// Compare dotted versions numerically: returns >0 if a > b.
int versionCompare(const String& a, const String& b) {
  int ia = 0, ib = 0;
  for (int part = 0; part < 4; part++) {
    long va = 0, vb = 0;
    while (ia < (int)a.length() && isdigit(a[ia])) va = va * 10 + (a[ia++] - '0');
    while (ib < (int)b.length() && isdigit(b[ib])) vb = vb * 10 + (b[ib++] - '0');
    if (va != vb) return va > vb ? 1 : -1;
    if (ia < (int)a.length()) ia++;
    if (ib < (int)b.length()) ib++;
  }
  return 0;
}

// ===================================================================
// Config persistence
// ===================================================================
void loadConfig() {
  prefs.begin(NVS_NS, false);
  cfg.ssid      = prefs.getString("ssid", "");
  cfg.pass      = prefs.getString("pass", "");
  cfg.backend   = prefs.getString("backend", "watchtower");   // default server type
  cfg.apiBase   = prefs.getString("apiBase", "");
  cfg.apiKey    = prefs.getString("apiKey", "");
  cfg.printer   = prefs.getString("printer", "");
  cfg.hostname  = prefs.getString("hostname", "");
  cfg.caCert    = prefs.getString("caCert", "");
  cfg.adminPass = prefs.getString("adminPass", "");
  cfg.otaUrl    = prefs.getString("otaUrl", "");
  cfg.pollSec   = prefs.getUInt("pollSec", 10);
  cfg.debug     = prefs.getBool("debug", false);
  prefs.end();
  if (cfg.backend != "bambuddy") cfg.backend = "watchtower";
  if (!cfg.hostname.length()) cfg.hostname = defaultHostname();
  if (!cfg.otaUrl.length()) cfg.otaUrl = DEFAULT_OTA_URL;
  cfg.pollSec = constrain(cfg.pollSec, 5u, 300u);
}

void saveConfig() {
  prefs.begin(NVS_NS, false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("backend", cfg.backend);
  prefs.putString("apiBase", cfg.apiBase);
  prefs.putString("apiKey", cfg.apiKey);
  prefs.putString("printer", cfg.printer);
  prefs.putString("hostname", cfg.hostname);
  prefs.putString("caCert", cfg.caCert);
  prefs.putString("adminPass", cfg.adminPass);
  prefs.putString("otaUrl", cfg.otaUrl == DEFAULT_OTA_URL ? "" : cfg.otaUrl);
  prefs.putUInt("pollSec", cfg.pollSec);
  prefs.putBool("debug", cfg.debug);
  prefs.end();
}

[[noreturn]] void factoryReset() {
  DBG("[reset] erasing settings\n");
  prefs.begin(NVS_NS, false); prefs.clear(); prefs.end();
  for (int i = 0; i < 6; i++) { leds(true, true, true); delay(100); leds(false, false, false); delay(100); }
  ESP.restart();
  while (true) {}
}

void configureTls() {
  // TlsClient keeps a pointer to the PEM, so cfg.caCert must outlive it;
  // call this again whenever cfg.caCert is reassigned.
  apiTls.setCACert(cfg.caCert.length() ? cfg.caCert.c_str() : nullptr);
}

// ===================================================================
// Backend: poll + action
// ===================================================================
String apiBase() {
  String b = cfg.apiBase; b.trim();
  while (b.endsWith("/")) b.remove(b.length() - 1);
  return b;
}
bool   useTls()     { return apiBase().startsWith("https://"); }
String printerSeg() { return urlEncodeSegment(cfg.printer); }
String statusUrl()  { return apiBase() + "/printers/" + printerSeg() + (cfg.isWatchtower() ? "" : "/status"); }
String actionUrl()  { return apiBase() + "/printers/" + printerSeg() + (cfg.isWatchtower() ? "/commands" : "/clear-plate"); }

bool beginRequest(HTTPClient& http, const String& url) {
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  bool ok = useTls() ? http.begin(apiTls, url) : http.begin(plainClient, url);
  if (!ok) return false;
  if (cfg.isWatchtower()) http.addHeader("Authorization", "Bearer " + cfg.apiKey);
  else                    http.addHeader("X-API-Key", cfg.apiKey);
  return true;
}

String tlsError() {
  if (!useTls()) return "";
  return apiTls.lastError() ? apiTls.lastErrorString() : "";
}

// Turn a failed result into one plain-English sentence for the settings page.
String hintFor(const PollResult& r) {
  bool wt = cfg.isWatchtower();
  if (r.http <= 0) {
    String t = r.tls + " " + r.transport;
    int tlsCode = r.tls.toInt();                      // "-188 ..." -> -188
    switch (tlsCode) {
      case -188: case -155:                           // ASN_NO_SIGNER_E, ASN_SIG_CONFIRM_E
        return "The server's certificate isn't signed by the trusted certificate saved on this button. "
               "If the server's certificate was regenerated, use Trust this server again.";
      case -322:                                      // DOMAIN_NAME_MISMATCH
        return "The server's certificate isn't issued for this host name. Use a name listed in the certificate.";
      case -325:                                      // IPADDR_MISMATCH
        return "The server's certificate doesn't list this IP address. Use an address listed in the certificate.";
      case -198:                                      // ASN_NAME_INVALID_E
        return "The server's certificate is outside what its own CA is allowed to issue (name constraints). "
               "That's a server-side certificate problem.";
      case -150: case -151:                           // ASN_BEFORE_DATE_E / ASN_AFTER_DATE_E
        return "The server's certificate is expired or not yet valid.";
      default: break;
    }
    if (r.tls.length() || t.indexOf("SSL") >= 0 || t.indexOf("TLS") >= 0)
      return "HTTPS handshake failed. Check http:// vs https:// and the port.";
    if (t.indexOf("refused") >= 0)
      return "The server refused the connection. Check the address, port, and http:// vs https://.";
    if (t.indexOf("not connected") >= 0 || t.indexOf("Wi-Fi") >= 0)
      return "The button isn't on Wi-Fi.";
    return "Can't reach the server. Check the address and that this Wi-Fi network can reach it.";
  }
  switch (r.http) {
    case 200: return "Got a reply, but not the expected data. Check that the base URL ends in " +
                     String(wt ? "/api/public/v1" : "/api/v1") + ".";
    case 401: return "The API key was rejected. Re-copy it from the server.";
    case 403:
      if (r.body.indexOf("FEATURE_NOT_LICENSED") >= 0) return "Your Watchtower plan doesn't include API access.";
      return wt ? "The API key is missing the control scope or set_ready permission."
                : "The API key is missing printer read or clear-plate permission.";
    case 404: return wt ? "Printer not found. The name must match Watchtower exactly (case-sensitive)."
                        : "Printer not found. Check the printer ID number, and that the base URL ends in /api/v1.";
    case 429: return "The server is rate-limiting this key. The button will slow down automatically.";
    default:
      if (r.http >= 500) return "The server returned an error (" + String(r.http) + "). Check that it's running.";
      return "Unexpected response (HTTP " + String(r.http) + ").";
  }
}

// Log the raw status response (first 400 chars) whenever it changes, so serial shows
// exactly what the server said without repeating it every poll.
uint32_t pollBodyHash = 0;          // reset when logging is switched on, to log the next body
void logPollBodyIfChanged(const String& body) {
#if DEBUG
  if (!dbgEnabled) return;
  uint32_t& lastHash = pollBodyHash;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < body.length(); i++) { h ^= (uint8_t)body[i]; h *= 16777619u; }
  if (h == lastHash) return;
  lastHash = h;
  String b = body.length() > 400 ? body.substring(0, 400) + "..." : body;
  DBG("[poll] response changed: %s\n", b.c_str());
#else
  (void)body;
#endif
}

bool isBusyState(String v) {
  v.toUpperCase();
  return v == "RUNNING" || v == "PREPARE" || v == "PAUSE";
}

PollResult pollOnce() {
  PollResult r;
  r.at = millis();
  r.url = statusUrl();
  if (!cfg.configured()) { r.transport = "not configured"; r.hint = "Fill in the printer server settings."; return r; }
  if (WiFi.status() != WL_CONNECTED) { r.transport = "Wi-Fi not connected"; r.hint = hintFor(r); return r; }

  HTTPClient http;
  if (!beginRequest(http, r.url)) { r.transport = "invalid URL"; r.hint = "The API base URL isn't valid. It must start with http:// or https://."; return r; }
  const char* hdrs[] = {"Retry-After"};
  http.collectHeaders(hdrs, 1);

  r.http = http.GET();
  if (r.http <= 0) {
    r.transport = HTTPClient::errorToString(r.http);
    r.tls = tlsError();
    http.end();
    r.hint = hintFor(r);
    DBG("[poll] %s -> transport error %s %s\n", r.url.c_str(), r.transport.c_str(), r.tls.c_str());
    return r;
  }
  if (r.http == 429) {
    long ra = http.header("Retry-After").toInt();
    pollDelay = (ra > 0 ? (uint32_t)ra : 30) * 1000UL;
  }
  String body = http.getString();
  http.end();
  if (r.http != 200) {
    r.body = body.substring(0, 240);
    r.hint = hintFor(r);
    DBG("[poll] http=%d body=%s\n", r.http, r.body.c_str());
    return r;
  }
  pollDelay = cfg.pollSec * 1000UL;

  JsonDocument doc;
  if (deserializeJson(doc, body)) { r.body = body.substring(0, 240); r.transport = "bad JSON"; r.hint = hintFor(r); return r; }

  if (cfg.isWatchtower()) {
    JsonObject d = doc["data"];
    logPollBodyIfChanged(body);
    if (d.isNull() || d["ready"].isNull() || d["online"].isNull()) {
      // Missing fields used to read as false, which looks exactly like "offline". Say so instead.
      r.body = body.substring(0, 240);
      r.transport = "unexpected JSON";
      r.hint = "Got a reply from Watchtower, but it has no data.ready / data.online fields. "
               "Check the printer name and that the base URL ends in /api/public/v1.";
      DBG("[poll] response has no data.ready/data.online: %s\n", r.body.c_str());
      return r;
    }
    bool ready = d["ready"] | false, busy = d["busy"] | false, online = d["online"] | false;
    r.red   = !(ready && online);
    r.armed = online && !busy && !ready;
    r.summary = String("ready=") + ready + " busy=" + busy + " online=" + online;
    if (!online) r.summary += " (Watchtower reports the printer offline: button disabled)";
  } else {
    if (!doc["state"].is<const char*>() && doc["awaiting_plate_clear"].isNull()) {
      r.body = body.substring(0, 240); r.hint = hintFor(r); return r;
    }
    logPollBodyIfChanged(body);
    bool awaiting = doc["awaiting_plate_clear"] | false;
    const char* state = doc["state"] | "";
    r.red   = awaiting || isBusyState(state);
    r.armed = awaiting;
    r.summary = String("state=") + state + " awaiting_plate_clear=" + awaiting;
  }
  r.ok = true;
  r.hint = "Connected. The button is talking to the printer server.";
  DBG("[poll] ok %s -> red=%d armed=%d\n", r.summary.c_str(), r.red, r.armed);
  return r;
}

void applyPoll(const PollResult& r) {
  last = r;
  haveStatus = true;
  if (r.ok) { showRed = r.red; armed = r.armed; failStreak = 0; }
  else if (failStreak < 255) failStreak++;
}

int sendAction() {
  if (!cfg.configured() || WiFi.status() != WL_CONNECTED) return -1;
  HTTPClient http;
  if (!beginRequest(http, actionUrl())) return -1;
  http.addHeader("Content-Type", "application/json");
  const char* body = cfg.isWatchtower() ? "{\"command\":\"set_ready\"}" : "";
  DBG("[action] POST %s body=%s\n", actionUrl().c_str(), body);
  uint32_t t0 = millis();
  int code = http.POST(body);
  uint32_t dt = millis() - t0;
  if (code > 0) {
    String resp = http.getString();
    if (resp.length() > 300) resp = resp.substring(0, 300) + "...";
    DBG("[action] -> HTTP %d in %lu ms, response: %s\n", code, (unsigned long)dt, resp.c_str());
  } else {
    DBG("[action] -> transport error %d (%s) after %lu ms%s%s\n", code, HTTPClient::errorToString(code).c_str(),
        (unsigned long)dt, tlsError().length() ? ", TLS: " : "", tlsError().c_str());
  }
  http.end();
  return code;
}

void onShortPress() {
  bool up = WiFi.status() == WL_CONNECTED;
  DBG("[button] short press: configured=%d wifi=%d failStreak=%u armed=%d showRed=%d\n",
      cfg.configured(), up, failStreak, armed, showRed);
  if (haveStatus)
    DBG("[button]   last check %lu s ago: %s\n", (unsigned long)((millis() - last.at) / 1000),
        last.ok ? last.summary.c_str() : last.hint.c_str());
  if (!cfg.configured() || !up || failStreak >= FAIL_STREAK_ERROR) {
    DBG("[button] -> ignored: %s (3x red blink)\n",
        !cfg.configured() ? "server settings incomplete" : !up ? "not on Wi-Fi" : "server unreachable");
    blink(LED_RED_PIN, 3, 80);
    return;
  }
  if (!armed) {
    DBG("[button] -> nothing to do: %s (2x green blink)\n",
        cfg.isWatchtower() ? "Watchtower only accepts set_ready when online, not busy and not already ready"
                           : "Bambuddy reports no plate waiting to be cleared");
    blink(LED_GREEN_PIN, 2, 60);
    return;
  }
  DBG("[button] -> sending %s\n", cfg.isWatchtower() ? "set_ready" : "clear-plate");
  int code = sendAction();
  if (code >= 200 && code < 300) {
    DBG("[button] -> accepted (3x green blink)\n");
    showRed = false; armed = false;                         // optimistic; next poll confirms
    blink(LED_GREEN_PIN, 3, 80);
    lastPoll = millis();                                    // give the server a beat
  } else {
    DBG("[button] -> failed (3x red blink)\n");
    blink(LED_RED_PIN, 3, 80);
  }
}

// ===================================================================
// Wi-Fi
// ===================================================================
void startAP() {
  if (apActive) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid().c_str());
  delay(100);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());
  apActive = true;
  apStartedAt = millis();
  apLastClientAt = millis();
  DBG("[wifi] setup AP '%s' at %s\n", apSsid().c_str(), WiFi.softAPIP().toString().c_str());
}

void stopAP() {
  if (!apActive) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
  DBG("[wifi] setup AP closed\n");
}

void beginSta() {
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  lastStaBegin = millis();
}

bool ntpStarted = false;

void onStaConnected() {
  if (!ntpStarted) {              // certificate dates are only checked once this syncs
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    ntpStarted = true;
  }
  DBG("[wifi] connected ssid=%s host=%s ip=%s\n", WiFi.SSID().c_str(), WiFi.getHostname(),
      WiFi.localIP().toString().c_str());
  if (!mdnsUp && MDNS.begin(cfg.hostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
    mdnsUp = true;
  }
  pollNowFlag = true;
}

void wifiLoop() {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !staWasUp) onStaConnected();
  if (!up && staWasUp) staDownSince = millis();
  staWasUp = up;

  if (!up && cfg.hasWifi() && wtState != WT_CONNECTING) {
    if (!apActive && millis() - staDownSince > AP_FALLBACK_MS) startAP();
    bool apBusy = apActive && WiFi.softAPgetStationNum() > 0;   // don't channel-hop under a user
    if (!apBusy && millis() - lastStaBegin > STA_RETRY_MS) beginSta();
  }
  if (apActive && WiFi.softAPgetStationNum() > 0) apLastClientAt = millis();
  // Close the setup AP once we're on Wi-Fi and nobody has used it for a while. Timed from
  // the last client, not AP start, so a phone briefly dropped by the channel change that
  // joining Wi-Fi causes can come back and read the "finish on your PC" address.
  if (up && apActive && WiFi.softAPgetStationNum() == 0 &&
      millis() - apLastClientAt > AP_IDLE_CLOSE_MS) {
    stopAP();
  }
  if (apClosePending && (int32_t)(millis() - apCloseAt) >= 0) { apClosePending = false; if (up) stopAP(); }
  if (apActive) dns.processNextRequest();
}

String discReasonText(uint8_t r) {
  switch (r) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
      return "Couldn't find that network. Check the name, that it's a 2.4 GHz network, and that the button is in range.";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_MIC_FAILURE:
      return "The network rejected the password. Check it and try again.";
    case 0:
      return "Couldn't join the network (no response). Check the name and password, and that the button is in range.";
    default:
      return "Couldn't join the network (reason " + String(r) + ").";
  }
}

// Phone setup step 1: start joining without blocking the web server.
void startWifiTry(const String& ssid, const String& pass) {
  wtSsid = ssid; wtPass = pass; wtError = "";
  staDiscReason = 0;
  WiFi.disconnect(false);              // keep STA (and the AP) up
  WiFi.begin(ssid.c_str(), pass.c_str());
  wtStart = millis();
  wtState = WT_CONNECTING;
  apLastClientAt = millis();
  DBG("[setup] joining '%s'\n", ssid.c_str());
}

void wifiTryLoop() {
  if (wtState != WT_CONNECTING) return;
  uint32_t el = millis() - wtStart;
  if (WiFi.status() == WL_CONNECTED) {
    cfg.ssid = wtSsid; cfg.pass = wtPass;
    saveConfig();
    wtPass = "";
    wtState = WT_OK;
    apLastClientAt = millis();         // keep the AP around for the phone to read the result
    DBG("[setup] joined, ip=%s\n", WiFi.localIP().toString().c_str());
    return;
  }
  if (el > WIFI_TRY_MAX_MS || (staDiscReason && el > WIFI_TRY_FAST_FAIL_MS)) {
    wtError = discReasonText(staDiscReason);
    wtState = WT_FAIL;
    wtPass = "";
    DBG("[setup] join failed: %u\n", staDiscReason);
    WiFi.disconnect(false);
    if (cfg.hasWifi()) beginSta();     // fall back to the previously saved network, if any
  }
}

// Used by Improv: try new credentials, persist only if they work.
bool tryCredentials(const String& ssid, const String& pass) {
  DBG("[improv] trying '%s'\n", ssid.c_str());
  WiFi.disconnect(false);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    leds(true, false); delay(150); leds(false, true); delay(150);
  }
  if (WiFi.status() == WL_CONNECTED) {
    cfg.ssid = ssid; cfg.pass = pass;
    saveConfig();
    staWasUp = true;
    onStaConnected();
    return true;
  }
  if (cfg.hasWifi()) beginSta();
  return false;
}

// ===================================================================
// Web server
// ===================================================================
bool authorized() {
  if (!cfg.adminPass.length()) return true;
  if (server.authenticate("admin", cfg.adminPass.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, PRODUCT_NAME, "Settings password required");
  return false;
}

void sendJson(JsonDocument& d, int code = 200) {
  String out; serializeJson(d, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", out);
}

void sendOk() { JsonDocument d; d["ok"] = true; sendJson(d); }
void sendErr(const String& e) { JsonDocument d; d["ok"] = false; d["error"] = e; sendJson(d); }

void handleRoot() {
  if (!authorized()) return;
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", INDEX_HTML);
}

void fillPoll(JsonObject p, const PollResult& r) {
  p["ok"] = r.ok; p["red"] = r.red; p["http"] = r.http; p["url"] = r.url;
  p["transport"] = r.transport; p["tls"] = r.tls; p["body"] = r.body;
  p["summary"] = r.summary; p["hint"] = r.hint;
  p["age"] = haveStatus ? (int32_t)((millis() - r.at) / 1000) : -1;
}

void handleInfo() {
  if (!authorized()) return;
  JsonDocument d;
  d["product"] = PRODUCT_NAME; d["version"] = FW_VERSION; d["source"] = SOURCE_URL;
  d["host"] = cfg.hostname; d["mac"] = macString();
  d["ap"] = apActive; d["apSsid"] = apSsid();
  bool up = WiFi.status() == WL_CONNECTED;
  d["wifi"] = up; d["ssid"] = up ? WiFi.SSID() : "";
  d["rssi"] = up ? WiFi.RSSI() : 0;
  d["ip"] = up ? WiFi.localIP().toString() : "";
  d["configured"] = cfg.configured();
  d["uptime"] = millis() / 1000;
  if (haveStatus) fillPoll(d["poll"].to<JsonObject>(), last);
  sendJson(d);
}

void handleGetConfig() {
  if (!authorized()) return;
  JsonDocument d;
  d["ssid"] = cfg.ssid; d["hasPass"] = cfg.pass.length() > 0;
  d["backend"] = cfg.backend; d["apiBase"] = cfg.apiBase;
  d["hasKey"] = cfg.apiKey.length() > 0;                 // never echo secrets back
  d["printer"] = cfg.printer; d["caCert"] = cfg.caCert;
  d["hostname"] = cfg.hostname; d["pollSec"] = cfg.pollSec;
  d["hasAdmin"] = cfg.adminPass.length() > 0;
  d["debug"] = cfg.debug;
  d["debugAvailable"] = (bool)DEBUG;
  d["otaUrl"] = cfg.otaUrl;
  sendJson(d);
}

// Switch serial debug logging to cfg.debug. Turning it on prints a one-time snapshot
// (no secrets) so a fresh terminal session starts with context.
void applyDebug() {
#if DEBUG
  bool was = dbgEnabled;
  dbgEnabled = cfg.debug;
  if (was == dbgEnabled) return;
  logAlways("[debug] serial debug logging %s\n", dbgEnabled ? "ON" : "OFF");
  if (!dbgEnabled) return;
  pollBodyHash = 0;                         // log the next status response in full
  bool up = WiFi.status() == WL_CONNECTED;
  DBG("[debug] %s %s, up %lu s, last reset: %s\n", PRODUCT_NAME, FW_VERSION,
      (unsigned long)(millis() / 1000), resetReasonText());
  DBG("[debug] wifi=%s ssid=%s ip=%s rssi=%d ap=%s\n", up ? "up" : "down", up ? WiFi.SSID().c_str() : "-",
      up ? WiFi.localIP().toString().c_str() : "-", up ? WiFi.RSSI() : 0, apActive ? apSsid().c_str() : "off");
  DBG("[debug] backend=%s base=%s printer=%s key=%s ca=%s poll=%lus\n", cfg.backend.c_str(),
      cfg.apiBase.c_str(), cfg.printer.c_str(), cfg.apiKey.length() ? "set" : "missing",
      cfg.caCert.length() ? "pinned" : "none", (unsigned long)cfg.pollSec);
  if (haveStatus)
    DBG("[debug] last check %lu s ago: %s\n", (unsigned long)((millis() - last.at) / 1000),
        last.ok ? last.summary.c_str() : last.hint.c_str());
#endif
}

void handlePostConfig() {
  if (!authorized()) return;
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) { sendErr("Bad JSON"); return; }

  String oldSsid = cfg.ssid, oldPass = cfg.pass, oldHost = cfg.hostname;
  String oldBase = cfg.apiBase, oldBackend = cfg.backend;

  auto str = [&](const char* k) -> String { String v = in[k] | ""; v.trim(); return v; };
  if (in["ssid"].is<const char*>()) {
    String s = str("ssid");
    String p = in["pass"] | "";
    if (s != cfg.ssid) cfg.pass = p;                      // new network: take pass as given (may be open)
    else if (p.length()) cfg.pass = p;                    // same network: blank = keep
    cfg.ssid = s;
  }
  if (in["backend"].is<const char*>()) cfg.backend = str("backend") == "bambuddy" ? "bambuddy" : "watchtower";
  if (in["apiBase"].is<const char*>()) cfg.apiBase = str("apiBase");
  if (str("apiKey").length()) cfg.apiKey = str("apiKey");
  else if (cfg.apiBase != oldBase || cfg.backend != oldBackend) {
    // Never send the saved key to a server it wasn't entered for: anyone who can reach this
    // page (e.g. the open fallback AP during a Wi-Fi outage) could otherwise point the
    // button at their own host and harvest the key.
    cfg.apiKey = "";
  }
  if (in["printer"].is<const char*>()) cfg.printer = str("printer");
  if (in["caCert"].is<const char*>()) cfg.caCert = str("caCert");
  if (in["hostname"].is<const char*>()) {
    String h = sanitizeHostname(str("hostname"));
    cfg.hostname = h.length() ? h : defaultHostname();
  }
  if (!in["pollSec"].isNull()) cfg.pollSec = constrain(in["pollSec"].as<uint32_t>(), 5u, 300u);
  if (str("adminPass").length()) cfg.adminPass = str("adminPass");
  bool debugWas = cfg.debug;
  if (in["debug"].is<bool>()) cfg.debug = in["debug"].as<bool>();
  if (in["clearAdmin"] | false) cfg.adminPass = "";
  if (in["otaUrl"].is<const char*>()) cfg.otaUrl = str("otaUrl");

  if (cfg.apiBase.length() && !cfg.apiBase.startsWith("http://") && !cfg.apiBase.startsWith("https://")) {
    loadConfig(); configureTls();
    sendErr("API base URL must start with http:// or https://");
    return;
  }

  saveConfig();
  configureTls();
  if (cfg.debug != debugWas) applyDebug();
  pollDelay = cfg.pollSec * 1000UL;
  failStreak = 0;
  pollNowFlag = true;

  bool reboot = cfg.ssid != oldSsid || cfg.pass != oldPass || cfg.hostname != oldHost ||
                (apActive && WiFi.status() != WL_CONNECTED);
  JsonDocument d; d["ok"] = true; d["reboot"] = reboot;
  sendJson(d);
  if (reboot) { rebootPending = true; rebootAt = millis() + 1500; }
}

void handleTest() {
  if (!authorized()) return;
  PollResult r = pollOnce();
  applyPoll(r);
  JsonDocument d;
  fillPoll(d.to<JsonObject>(), r);
  sendJson(d);
}

void handleScan() {
  if (!authorized()) return;
  int n = WiFi.scanNetworks();
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (JsonObject o : a) if (s == (const char*)o["ssid"]) { dup = true; break; }
    if (dup) continue;
    JsonObject o = a.add<JsonObject>();
    o["ssid"] = s; o["rssi"] = WiFi.RSSI(i); o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  sendJson(d);
}

void prepareOtaClient(WiFiClientSecure& c) {
#if OTA_ALLOW_INSECURE
  c.setInsecure();
#else
  c.setCACert(OTA_ROOT_CAS);
#endif
}

// POST /api/ota {"install":false} -> check manifest; {"install":true} -> install if newer.
// Manifest: {"version":"2.0.1","url":"https://.../firmware.bin"}
void handleOta() {
  if (!authorized()) return;
  if (!cfg.otaUrl.length()) { sendErr("No update server is configured for this device."); return; }
  if (WiFi.status() != WL_CONNECTED) { sendErr("Not connected to Wi-Fi."); return; }
  JsonDocument in; deserializeJson(in, server.arg("plain"));
  bool install = in["install"] | false;

  WiFiClientSecure c; WiFiClient pc;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  bool tls = cfg.otaUrl.startsWith("https://");
  if (tls) prepareOtaClient(c);
  if (!(tls ? http.begin(c, cfg.otaUrl) : http.begin(pc, cfg.otaUrl))) { sendErr("Bad update URL"); return; }
  int code = http.GET();
  String body = code == 200 ? http.getString() : "";
  http.end();
  if (code != 200) { sendErr("Update server returned " + String(code) + " " + HTTPClient::errorToString(code)); return; }

  JsonDocument m;
  if (deserializeJson(m, body) || !m["version"].is<const char*>() || !m["url"].is<const char*>()) {
    sendErr("Update manifest is malformed"); return;
  }
  String latest = m["version"].as<String>();
  bool newer = versionCompare(latest, FW_VERSION) > 0;
  JsonDocument d;
  d["ok"] = true; d["current"] = FW_VERSION; d["latest"] = latest; d["newer"] = newer;
  if (install && newer) { pendingOtaUrl = m["url"].as<String>(); d["installing"] = true; }
  sendJson(d);
}

void runPendingOta() {
  String url = pendingOtaUrl; pendingOtaUrl = "";
  DBG("[ota] installing from %s\n", url.c_str());
  httpUpdate.setLedPin(LED_GREEN_PIN, LED_ACTIVE_LOW ? LOW : HIGH);
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  t_httpUpdate_return ret;
  if (url.startsWith("https://")) {
    WiFiClientSecure c; prepareOtaClient(c);
    ret = httpUpdate.update(c, url);
  } else {
    WiFiClient c;
    ret = httpUpdate.update(c, url);
  }
  if (ret == HTTP_UPDATE_FAILED) {
    DBG("[ota] failed: %s\n", httpUpdate.getLastErrorString().c_str());
    blink(LED_RED_PIN, 6, 120);
  }
}

// Browser upload of a .bin (app image, not the merged factory image).
void handleUploadDone() {
  if (!authorized()) return;
  bool ok = !Update.hasError();
  server.sendHeader("Connection", "close");
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : Update.errorString());
  if (ok) { rebootPending = true; rebootAt = millis() + 1000; }
}

void handleUploadChunk() {
  if (cfg.adminPass.length() && !server.authenticate("admin", cfg.adminPass.c_str())) return;
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    DBG("[upload] %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) Update.printError(Serial);
    else DBG("[upload] %u bytes ok\n", up.totalSize);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
  }
}

bool isOurHost() {
  String h = server.hostHeader();
  int colon = h.indexOf(':'); if (colon >= 0) h = h.substring(0, colon);
  return h == WiFi.softAPIP().toString() || h == WiFi.localIP().toString() ||
         h.equalsIgnoreCase(cfg.hostname + ".local") || h.equalsIgnoreCase(cfg.hostname);
}

void handleNotFound() {
  if (apActive && !isOurHost()) {                         // captive-portal probe -> setup page
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  server.send(404, "text/plain", "Not found");
}

// Split "https://host:port/path" (IPv6 as [addr]) into host + port.
bool parseHttpsUrl(const String& url, String& host, uint16_t& port) {
  if (!url.startsWith("https://")) return false;
  String rest = url.substring(8);
  int slash = rest.indexOf('/');
  String hp = slash >= 0 ? rest.substring(0, slash) : rest;
  port = 443;
  if (hp.startsWith("[")) {
    int end = hp.indexOf(']');
    if (end < 0) return false;
    host = hp.substring(1, end);
    if (hp.length() > (unsigned)end + 2 && hp[end + 1] == ':') port = hp.substring(end + 2).toInt();
  } else {
    int colon = hp.lastIndexOf(':');
    host = colon >= 0 ? hp.substring(0, colon) : hp;
    if (colon >= 0) port = hp.substring(colon + 1).toInt();
  }
  return host.length() > 0 && port > 0;
}

// POST /api/trust {"apiBase": "https://..."} -> connect without verification and
// return the top-most certificate the server presents (normally its CA) for the
// user to confirm by fingerprint. Saving it goes through /api/config as caCert.
void handleTrust() {
  if (!authorized()) return;
  JsonDocument in; deserializeJson(in, server.arg("plain"));
  String base = in["apiBase"] | cfg.apiBase;
  base.trim();
  String host; uint16_t port;
  if (!parseHttpsUrl(base, host, port)) { sendErr("The API base URL must start with https:// to trust a certificate."); return; }
  if (WiFi.status() != WL_CONNECTED) { sendErr("Not connected to Wi-Fi."); return; }

  TlsClient probe;                 // no CA: encrypt-only, just to see the chain
  if (!probe.connect(host.c_str(), port, HTTP_TIMEOUT_MS)) {
    sendErr("Couldn't connect to " + host + ":" + String(port) +
            (probe.lastError() ? " (" + probe.lastErrorString() + ")" : String("")));
    return;
  }
  std::unique_ptr<bb_tls_anchor> a(new bb_tls_anchor);
  String err;
  bool ok = probe.captureAnchor(a.get(), err);
  probe.stop();
  if (!ok) { sendErr(err); return; }
  if (!a->isCA) {
    sendErr("The server only sent its own certificate, not the CA that signed it. "
            "Paste the CA certificate into the box instead.");
    return;
  }
  JsonDocument d;
  d["ok"] = true;
  d["subject"] = a->subject; d["issuer"] = a->issuer; d["sha256"] = a->sha256;
  d["selfSigned"] = (bool)a->selfSigned; d["chainLen"] = a->chainLen;
  d["pem"] = a->pem;
  sendJson(d);
}

// POST /api/wifi {"ssid","pass"}: phone setup step 1 (no reboot).
void handleWifiPost() {
  if (!authorized()) return;
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) { sendErr("Bad JSON"); return; }
  String ssid = in["ssid"] | ""; String pass = in["pass"] | "";
  ssid.trim();
  if (!ssid.length() || ssid.length() > 32) { sendErr("Enter the network name."); return; }
  if (pass.length() && (pass.length() < 8 || pass.length() > 63)) {
    sendErr("Wi-Fi passwords are 8 to 63 characters. Leave it blank only for an open network."); return;
  }
  if (wtState == WT_CONNECTING) { sendErr("Already connecting. Wait a moment."); return; }
  startWifiTry(ssid, pass);
  JsonDocument d; d["ok"] = true; d["host"] = cfg.hostname; d["apSsid"] = apSsid();
  sendJson(d);
}

// GET /api/wifi: progress of the phone Wi-Fi step.
void handleWifiGet() {
  if (!authorized()) return;
  static const char* names[] = {"idle", "connecting", "ok", "fail"};
  JsonDocument d;
  d["state"] = names[wtState];
  d["ssid"] = wtState == WT_IDLE ? cfg.ssid : wtSsid;
  d["host"] = cfg.hostname;
  d["apSsid"] = apSsid();
  bool up = WiFi.status() == WL_CONNECTED;
  d["wifi"] = up;
  d["ip"] = up ? WiFi.localIP().toString() : "";
  d["configured"] = cfg.configured();
  if (wtState == WT_FAIL) d["error"] = wtError;
  sendJson(d);
}

// POST /api/ap/close: "Finish" on the phone. Only once Wi-Fi works, so the button
// can't be stranded with no way in.
void handleApClose() {
  if (!authorized()) return;
  if (WiFi.status() != WL_CONNECTED) { sendErr("The button isn't on your Wi-Fi yet."); return; }
  sendOk();
  apClosePending = true; apCloseAt = millis() + 1500;
}

void setupServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/info", HTTP_GET, handleInfo);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/test", HTTP_POST, handleTest);
  server.on("/api/trust", HTTP_POST, handleTrust);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/wifi", HTTP_POST, handleWifiPost);
  server.on("/api/wifi", HTTP_GET, handleWifiGet);
  server.on("/api/ap/close", HTTP_POST, handleApClose);
  server.on("/api/ota", HTTP_POST, handleOta);
  server.on("/api/reboot", HTTP_POST, [] {
    if (!authorized()) return;
    sendOk(); rebootPending = true; rebootAt = millis() + 500;
  });
  server.on("/api/reset", HTTP_POST, [] {
    if (!authorized()) return;
    sendOk(); delay(300); factoryReset();
  });
  server.on("/update", HTTP_POST, handleUploadDone, handleUploadChunk);
  server.onNotFound(handleNotFound);
  server.begin();
}

// ===================================================================
// LEDs + button
// ===================================================================
enum LedMode : uint8_t { LM_NONE, LM_RESET_WARN, LM_SETUP_AP, LM_JOINING, LM_NEEDS_SETTINGS,
                         LM_ERROR, LM_RED, LM_GREEN };
void logLedMode(LedMode m) {
  static LedMode prev = LM_NONE;
  if (m == prev) return;
  prev = m;
  static const char* names[] = {"", "white flicker (reset warning)", "solid blue (setup network)",
      "red/green alternating (joining Wi-Fi)", "slow blue blink (needs server settings)",
      "slow red blink (server unreachable)", "solid red", "solid green"};
  DBG("[led] %s\n", names[m]);
}

void updateLeds() {
  uint32_t t = millis();
  if (btnHeld && t - btnDownAt > RESET_WARN_MS) { logLedMode(LM_RESET_WARN); bool on = (t / 80) % 2; leds(on, on, on); return; }
  bool up = WiFi.status() == WL_CONNECTED;
  // AP mode is blue, unless Wi-Fi is up and fully configured (the AP is then about to close).
  if (apActive && !(up && cfg.configured())) { logLedMode(LM_SETUP_AP); leds(false, false, true); return; }
  if (!up)                          { logLedMode(LM_JOINING); bool p = (t / 150) % 2; leds(p, !p); return; }
  if (!cfg.configured())            { logLedMode(LM_NEEDS_SETTINGS); leds(false, false, (t / 600) % 2); return; }
  if (failStreak >= FAIL_STREAK_ERROR) { logLedMode(LM_ERROR); leds((t / 500) % 2, false); return; }
  logLedMode(showRed ? LM_RED : LM_GREEN);
  leds(showRed, !showRed);
}

void buttonLoop() {
  int reading = digitalRead(BUTTON_PIN);
  uint32_t now = millis();
  if (reading != btnLastReading) { btnChangedAt = now; btnLastReading = reading; }
  if (now - btnChangedAt > DEBOUNCE_MS && reading != btnStable) {
    btnStable = reading;
    if (btnStable == LOW) { btnHeld = true; btnDownAt = now; DBG("[button] down (GPIO%d low)\n", BUTTON_PIN); }
    else if (btnHeld) {
      btnHeld = false;
      uint32_t held = now - btnDownAt;
      DBG("[button] up after %lu ms\n", (unsigned long)held);
      if (held < SHORT_PRESS_MAX_MS) onShortPress();
      else DBG("[button] long press (>= %lu ms) ignored; hold %lu s for factory reset\n",
               (unsigned long)SHORT_PRESS_MAX_MS, (unsigned long)(RESET_HOLD_MS / 1000));
    }
  }
  static bool warned = false;
  if (btnHeld && !warned && now - btnDownAt >= RESET_WARN_MS) {
    warned = true;
    DBG("[button] held %lu s: release to cancel, keep holding to %lu s to factory reset\n",
        (unsigned long)(RESET_WARN_MS / 1000), (unsigned long)(RESET_HOLD_MS / 1000));
  }
  if (!btnHeld) warned = false;
  if (btnHeld && now - btnDownAt >= RESET_HOLD_MS) factoryReset();
}

// ===================================================================
// setup / loop
// ===================================================================
void setup() {
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(LED_BLUE_PIN, OUTPUT);
  leds(false, false, false);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Serial.setTxBufferSize(4096);      // log bursts (TLS errors, bodies) without dropping lines
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);          // don't stall when no USB host is attached
  delay(200);

  uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_STA);
  char sfx[5]; snprintf(sfx, sizeof(sfx), "%02x%02x", m[4], m[5]);
  macSuffix = sfx;

  loadConfig();
  configureTls();
  pollDelay = cfg.pollSec * 1000UL;
  dbgEnabled = DEBUG && cfg.debug;
  Serial.println();
  logAlways("[boot] %s %s, reset reason: %s\n", PRODUCT_NAME, FW_VERSION, resetReasonText());
  logAlways("[boot] serial debug logging %s\n", !DEBUG ? "not built in" :
            dbgEnabled ? "ON" : "off (turn on under Device on the settings page)");
  DBG("[boot] host=%s backend=%s configured=%d\n",
      cfg.hostname.c_str(), cfg.backend.c_str(), cfg.configured());

  // Hostname must be set before the transition INTO STA, or DHCP sends esp32c3-XXXX.
  WiFi.persistent(false);
  WiFi.mode(WIFI_MODE_NULL);
  WiFi.setHostname(cfg.hostname.c_str());
  if (cfg.hasWifi()) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    beginSta();
    staDownSince = millis();
  } else {
    startAP();                       // AP_STA: STA idle, available for scans + Improv
  }

  setupServer();

  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    staDiscReason = info.wifi_sta_disconnected.reason;
    DBG("[wifi] disconnected, reason %u\n", info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  improvSerial.begin(Serial, {
    tryCredentials,
    [] { return WiFi.status() == WL_CONNECTED; },
    [] { return String("http://") + WiFi.localIP().toString() + "/"; },
    [] { return std::vector<String>{PRODUCT_NAME, FW_VERSION, "ESP32-C3", cfg.hostname}; },
  });
}

void loop() {
  improvSerial.loop();
  wifiLoop();
  wifiTryLoop();
  server.handleClient();
  buttonLoop();

  if (WiFi.status() == WL_CONNECTED && cfg.configured() && !btnHeld &&
      (pollNowFlag || millis() - lastPoll >= pollDelay)) {
    pollNowFlag = false;
    lastPoll = millis();
    applyPoll(pollOnce());
  }

  if (pendingOtaUrl.length()) runPendingOta();
  if (rebootPending && (int32_t)(millis() - rebootAt) >= 0) ESP.restart();

  updateLeds();
  delay(2);
}
