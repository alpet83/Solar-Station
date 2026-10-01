#include "shell.h"

#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <time.h>

namespace shell {

static Preferences g_prefs;
static WebServer g_http(80);
static SemaphoreHandle_t g_mtx;
static Snapshot g_snap;
static gs::Config g_cfg;
static bool g_cfg_dirty = false;
static float g_ext_a = 0; static int64_t g_ext_at_ms = -1;
static uint32_t g_boot_id = 0;
static String g_http_pass;            // пусто = пароль не задан (запись запрещена, пока не задан)
static bool g_ap_mode = false;
static uint32_t g_wifi_try_ms = 0;
static bool g_ota_validated = false;

static uint64_t now_ms() { return (uint64_t)(esp_timer_get_time() / 1000); }

// ---- NVS --------------------------------------------------------------------------------------
gs::Config load_config() {
    gs::Config c;
    Preferences p; p.begin("gs", true);
    for (size_t i = 0; i < gs::kCfgFieldCount; ++i) {
        char k[8]; snprintf(k, sizeof k, "c%02u", (unsigned)i);
        if (!p.isKey(k)) continue;
        String v = p.getString(k, "");
        gs::config_set(c, gs::kCfgFields[i].key, v.c_str());      // невалидное значение просто не применится
    }
    p.end();
    return c;
}

static void save_config(const gs::Config& c) {
    Preferences p; p.begin("gs", false);
    for (size_t i = 0; i < gs::kCfgFieldCount; ++i) {
        char k[8], v[32]; snprintf(k, sizeof k, "c%02u", (unsigned)i);
        gs::config_get(c, gs::kCfgFields[i].key, v, sizeof v);
        p.putString(k, v);
    }
    p.end();
}

// ---- публикация/получение --------------------------------------------------------------------
void publish(const Snapshot& s) {
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(50))) { g_snap = s; xSemaphoreGive(g_mtx); }
}

bool take_config(gs::Config& out) {
    bool d = false;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(50))) {
        if (g_cfg_dirty) { out = g_cfg; g_cfg_dirty = false; d = true; }
        xSemaphoreGive(g_mtx);
    }
    return d;
}

bool ext_charge(float& a, uint64_t& age_ms) {
    bool ok = false;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(50))) {
        if (g_ext_at_ms >= 0) { a = g_ext_a; age_ms = now_ms() - (uint64_t)g_ext_at_ms; ok = true; }
        xSemaphoreGive(g_mtx);
    }
    return ok;
}

// ---- HTTP -------------------------------------------------------------------------------------
// Чтение требует пароль, если он задан. Запись — всегда пароль + заголовок X-GS (защита от CSRF:
// кросс-сайтовая форма не может выставить произвольный заголовок).
static bool authed(bool write) {
    if (g_http_pass.length() == 0) {
        if (write) { g_http.send(403, "text/plain", "set password first (/setup)"); return false; }
        return true;
    }
    if (!g_http.authenticate("admin", g_http_pass.c_str())) { g_http.requestAuthentication(); return false; }
    if (write && !g_http.hasHeader("X-GS")) { g_http.send(403, "text/plain", "missing X-GS header"); return false; }
    return true;
}

static int64_t epoch_or_neg() { time_t t = time(nullptr); return t > 1700000000 ? (int64_t)t : -1; }

static void h_state() {
    if (!authed(false)) return;
    static char buf[1400];
    Snapshot s;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100))) { s = g_snap; xSemaphoreGive(g_mtx); }
    gs::StateView v{&s.tel, &s.dec, &s.in, s.uptime_ms, s.seq, g_boot_id, s.safe_mode, epoch_or_neg(),
                    s.paused, s.pause_left_ms, s.rx_ok, s.rx_err, s.rx_foreign, s.rx_busy, s.rx_exc};
    size_t n = gs::state_to_json(v, buf, sizeof buf);
    g_http.sendHeader("Cache-Control", "no-store");
    g_http.send(n ? 200 : 500, "application/json", n ? buf : "{}");
}

static void h_config_get() {
    if (!authed(false)) return;
    static char buf[1400];
    gs::Config c; if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100))) { c = g_cfg; xSemaphoreGive(g_mtx); }
    size_t n = gs::config_to_json(c, buf, sizeof buf);
    g_http.send(n ? 200 : 500, "application/json", n ? buf : "{}");
}

// POST /api/config  key=...&value=...  (по одному полю; изменение применяется, если вся конфигурация валидна)
static void h_config_set() {
    if (!authed(true)) return;
    String k = g_http.arg("key"), v = g_http.arg("value");
    bool ok = false;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200))) {
        gs::Config c = g_cfg;
        ok = gs::config_set(c, k.c_str(), v.c_str());
        if (ok) { g_cfg = c; g_cfg_dirty = true; save_config(c); }
        xSemaphoreGive(g_mtx);
    }
    g_http.send(ok ? 200 : 400, "text/plain", ok ? "ok" : "rejected");
}

// POST /api/ext  a=<ток, А>  — ток Epever от Pi; значение живёт ext_ttl_ms, потом игнорируется.
static void h_ext() {
    if (!authed(true)) return;
    float a;
    if (!gs::parse_ext_current(g_http.arg("a").c_str(), a)) { g_http.send(400, "text/plain", "bad value"); return; }
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200))) { g_ext_a = a; g_ext_at_ms = (int64_t)now_ms(); xSemaphoreGive(g_mtx); }
    g_http.send(200, "text/plain", "ok");
}

static const char kIndex[] PROGMEM =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
    "<title>grid-supervisor</title><style>body{font:14px monospace;margin:12px}pre{white-space:pre-wrap}</style>"
    "<h3>grid-supervisor</h3><pre id=s>loading…</pre>"
    "<script>async function t(){try{const r=await fetch('/api/state');const j=await r.json();"
    "document.getElementById('s').textContent=JSON.stringify(j,null,1)}catch(e){}}t();setInterval(t,5000)</script>"
    "<p><a href=/api/config>config</a> · <a href=/update>OTA</a></p>";

static void h_index() { if (authed(false)) g_http.send_P(200, "text/html", kIndex); }

static const char kUpdForm[] PROGMEM =
    "<!doctype html><meta charset=utf-8><title>OTA</title><h3>OTA</h3>"
    "<input type=file id=f accept=.bin><button onclick=\"u()\">Upload</button><pre id=o></pre>"
    "<script>async function u(){const f=document.getElementById('f').files[0];if(!f)return;"
    "const d=new FormData();d.append('fw',f);const r=await fetch('/update',{method:'POST',headers:{'X-GS':'1'},body:d});"
    "document.getElementById('o').textContent=await r.text()}</script>";

static void h_update_form() { if (authed(false)) g_http.send_P(200, "text/html", kUpdForm); }

static bool g_upd_auth = false;
static void h_update_done() {
    if (!g_upd_auth) return;
    bool ok = !Update.hasError();
    g_http.send(ok ? 200 : 500, "text/plain", ok ? "OK, rebooting" : String("FAILED: ") + Update.errorString());
    if (ok) { delay(500); ESP.restart(); }
}
static void h_update_upload() {
    HTTPUpload& up = g_http.upload();
    if (up.status == UPLOAD_FILE_START) {
        g_upd_auth = g_http_pass.length() && g_http.authenticate("admin", g_http_pass.c_str()) && g_http.hasHeader("X-GS");
        if (!g_upd_auth) return;
        Update.begin(UPDATE_SIZE_UNKNOWN);
    } else if (!g_upd_auth) {
        return;
    } else if (up.status == UPLOAD_FILE_WRITE) {
        Update.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
        Update.end(true);
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
    }
}

// Первичная настройка: только если пароль ещё не задан или мы в режиме AP.
static const char kSetup[] PROGMEM =
    "<!doctype html><meta charset=utf-8><title>setup</title><h3>setup</h3>"
    "<form method=post action=/setup>SSID <input name=ssid><br>Wi-Fi password <input name=pass type=password><br>"
    "Admin password (web/OTA) <input name=hpass type=password><br><input type=submit value=Save></form>";

static void h_setup() {
    if (!(g_ap_mode || g_http_pass.length() == 0)) { g_http.send(403, "text/plain", "already configured"); return; }
    if (g_http.method() == HTTP_GET) { g_http.send_P(200, "text/html", kSetup); return; }
    Preferences p; p.begin("gs", false);
    if (g_http.arg("ssid").length()) { p.putString("ssid", g_http.arg("ssid")); p.putString("pass", g_http.arg("pass")); }
    if (g_http.arg("hpass").length() >= 8) p.putString("hpass", g_http.arg("hpass"));
    p.end();
    g_http.send(200, "text/plain", "saved, rebooting (admin password must be >= 8 chars to be stored)");
    delay(500); ESP.restart();
}

// ---- Wi-Fi ------------------------------------------------------------------------------------
static void start_ap() {
    String name = "gs-setup-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
    WiFi.mode(WIFI_AP);
    Preferences p; p.begin("gs", false);
    String pw = p.getString("appass", "");
    if (pw.length() < 8) { char b[16]; snprintf(b, sizeof b, "%08lx", (unsigned long)esp_random()); pw = String("gs") + b; p.putString("appass", pw); }
    p.end();
    WiFi.softAP(name.c_str(), pw.c_str());      // пароль AP случайный, хранится в NVS и печатается только в Serial
    g_ap_mode = true;
    Serial.printf("AP mode: ssid=%s pass=%s ip=%s\n", name.c_str(), pw.c_str(), WiFi.softAPIP().toString().c_str());
}

static void connect_sta() {
    Preferences p; p.begin("gs", true);
    String ssid = p.getString("ssid", ""), pass = p.getString("pass", "");
    String host = p.getString("host", "grid-supervisor");
    g_http_pass = p.getString("hpass", "");
    p.end();
    if (!ssid.length()) { start_ap(); return; }
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(host.c_str());
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    g_wifi_try_ms = millis();
}

void begin(uint32_t boot_id) {
    g_boot_id = boot_id;
    g_mtx = xSemaphoreCreateMutex();
    g_cfg = load_config();
    connect_sta();
    configTime(0, 0, "pool.ntp.org", "time.google.com");     // всё время — UTC
    const char* hdr[] = {"X-GS"};
    g_http.collectHeaders(hdr, 1);
    g_http.on("/", HTTP_GET, h_index);
    g_http.on("/api/state", HTTP_GET, h_state);
    g_http.on("/api/config", HTTP_GET, h_config_get);
    g_http.on("/api/config", HTTP_POST, h_config_set);
    g_http.on("/api/ext", HTTP_POST, h_ext);
    g_http.on("/setup", HTTP_ANY, h_setup);
    g_http.on("/update", HTTP_GET, h_update_form);
    g_http.on("/update", HTTP_POST, h_update_done, h_update_upload);
    g_http.begin();
}

void loop() {
    g_http.handleClient();
    // Если подключиться к сети не удалось за 2 минуты и AP ещё не поднят — поднимаем AP, чтобы платой можно было заняться.
    if (!g_ap_mode && WiFi.status() != WL_CONNECTED && millis() - g_wifi_try_ms > 120000) start_ap();
    // OTA-образ считается рабочим после 2 минут стабильной работы (если загрузчик поддерживает rollback).
    if (!g_ota_validated && now_ms() > 120000) { esp_ota_mark_app_valid_cancel_rollback(); g_ota_validated = true; }
}

}  // namespace shell
