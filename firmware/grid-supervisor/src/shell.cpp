#include "shell.h"

#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <time.h>
#include <atomic>
#include <cstring>

namespace shell {

static Preferences g_prefs;
static WebServer g_http(80);
static SemaphoreHandle_t g_mtx;
static Snapshot g_snap;
static gs::Config g_cfg;
static bool g_cfg_dirty = false;
static std::atomic<uint32_t> g_ext_bits{0};        // float как биты — без мьютекса
static std::atomic<int64_t> g_ext_at_ms{-1};
static std::atomic<bool> g_ota_active{false};
static std::atomic<uint64_t> g_hb_ms{0};
static std::atomic<uint32_t> g_publishes{0};
static String g_xtok;                 // токен только для /api/ext (отдельная роль для Pi)
static String g_setup_token;
static uint32_t g_boot_id = 0;
static String g_http_pass;            // пусто = пароль не задан (запись запрещена, пока не задан)
static bool g_ap_mode = false;
static uint32_t g_wifi_try_ms = 0;
static bool g_ever_connected = false;
static bool g_ota_validated = false;

static uint64_t now_ms() { return (uint64_t)(esp_timer_get_time() / 1000); }

// ---- NVS --------------------------------------------------------------------------------------
gs::Config load_config() {
    gs::Config c;
    Preferences p; p.begin("gs", true);
    const char* keys[32]; String vals[32]; const char* vp[32]; size_t n = 0;
    for (size_t i = 0; i < gs::kCfgFieldCount && n < 32; ++i) {
        char k[8]; snprintf(k, sizeof k, "c%02u", (unsigned)i);
        if (!p.isKey(k)) continue;
        vals[n] = p.getString(k, ""); keys[n] = gs::kCfgFields[i].key; vp[n] = vals[n].c_str(); ++n;
    }
    p.end();
    gs::Config cand;
    if (n && gs::config_apply_pairs(cand, keys, vp, n)) c = cand;   // весь набор или значения по умолчанию
    else if (n) Serial.println("NVS config invalid -> defaults");
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
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(20))) {
        if (g_cfg_dirty) { out = g_cfg; g_cfg_dirty = false; d = true; }
        xSemaphoreGive(g_mtx);
    }
    return d;
}

bool ext_charge(float& a, uint64_t& age_ms) {
    int64_t at = g_ext_at_ms.load();
    if (at < 0) return false;
    uint32_t bits = g_ext_bits.load(); memcpy(&a, &bits, 4);
    age_ms = now_ms() - (uint64_t)at;
    return true;
}

bool ota_active() { return g_ota_active.load(); }
uint64_t heartbeat_age_ms() { uint64_t h = g_hb_ms.load(); return h ? now_ms() - h : 0; }
void mark_healthy(uint32_t n) { g_publishes.store(n); }

// ---- HTTP -------------------------------------------------------------------------------------
// Чтение требует пароль, если он задан. Запись — всегда пароль + заголовок X-GS (защита от CSRF:
// кросс-сайтовая форма не может выставить произвольный заголовок).
String b64(const String& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String out; size_t n = in.length();
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint8_t)in[i] << 16 | (i + 1 < n ? (uint8_t)in[i + 1] << 8 : 0) | (i + 2 < n ? (uint8_t)in[i + 2] : 0);
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
        out += i + 1 < n ? T[(v >> 6) & 63] : '='; out += i + 2 < n ? T[v & 63] : '=';
    }
    return out;
}

static bool ct_equal(const String& a, const String& b) {
    uint8_t d = a.length() != b.length();
    size_t n = min(a.length(), b.length());
    for (size_t i = 0; i < n; ++i) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static bool basic_ok() {
    if (!g_http_pass.length() || !g_http.hasHeader("Authorization")) return false;
    String h = g_http.header("Authorization");
    if (!h.startsWith("Basic ")) return false;
    // g_http.authenticate сравнивает не за постоянное время, поэтому сравниваем сами по base64
    String user = "admin:" + g_http_pass;
    return ct_equal(h.substring(6), b64(user));
}

static bool authed(bool write) {
    if (g_http_pass.length() == 0) {
        if (write) { g_http.send(403, "text/plain", "set password first (/setup)"); return false; }
        return true;
    }
    if (!basic_ok()) { g_http.requestAuthentication(); return false; }
    if (write && !g_http.hasHeader("X-GS")) { g_http.send(403, "text/plain", "missing X-GS header"); return false; }
    return true;
}

static int64_t epoch_or_neg() { time_t t = time(nullptr); return t > 1700000000 ? (int64_t)t : -1; }

static void h_state() {
    if (!authed(false)) return;
    static char buf[1400];
    Snapshot s;
    if (!xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100))) { g_http.send(503, "text/plain", "busy"); return; }
    s = g_snap; xSemaphoreGive(g_mtx);
    gs::StateView v{&s.tel, &s.dec, &s.in, s.uptime_ms, s.seq, g_boot_id, s.safe_mode, epoch_or_neg(),
                    s.paused, s.pause_left_ms, s.rx_ok, s.rx_err, s.rx_foreign, s.rx_busy, s.rx_exc};
    size_t n = gs::state_to_json(v, buf, sizeof buf);
    g_http.sendHeader("Cache-Control", "no-store");
    g_http.send(n ? 200 : 500, "application/json", n ? buf : "{}");
}

static void h_config_get() {
    if (!authed(false)) return;
    static char buf[1400];
    gs::Config c;
    if (!xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100))) { g_http.send(503, "text/plain", "busy"); return; }
    c = g_cfg; xSemaphoreGive(g_mtx);
    size_t n = gs::config_to_json(c, buf, sizeof buf);
    g_http.send(n ? 200 : 500, "application/json", n ? buf : "{}");
}

// POST /api/config  <ключ>=<значение>[&<ключ>=<значение>...]  (или key=..&value=..).
// Набор применяется АТОМАРНО: либо вся конфигурация валидна, либо ничего не меняется.
static void h_config_set() {
    if (!authed(true)) return;
    const char* keys[32]; String vals[32]; const char* vp[32]; size_t n = 0;
    for (int i = 0; i < g_http.args() && n < 32; ++i) {
        String name = g_http.argName(i);
        if (name == "key" || name == "value" || name == "plain") continue;
        keys[n] = nullptr;
        for (size_t j = 0; j < gs::kCfgFieldCount; ++j) if (name == gs::kCfgFields[j].key) { keys[n] = gs::kCfgFields[j].key; break; }
        if (!keys[n]) { g_http.send(400, "text/plain", "unknown key"); return; }
        vals[n] = g_http.arg(i); vp[n] = vals[n].c_str(); ++n;
    }
    if (g_http.hasArg("key") && n < 32) {
        for (size_t j = 0; j < gs::kCfgFieldCount; ++j) if (g_http.arg("key") == gs::kCfgFields[j].key) { keys[n] = gs::kCfgFields[j].key; break; }
        if (keys[n]) { vals[n] = g_http.arg("value"); vp[n] = vals[n].c_str(); ++n; }
    }
    if (!n) { g_http.send(400, "text/plain", "nothing to set"); return; }
    gs::Config c;
    if (!xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200))) { g_http.send(503, "text/plain", "busy"); return; }
    c = g_cfg; xSemaphoreGive(g_mtx);
    if (!gs::config_apply_pairs(c, keys, vp, n)) { g_http.send(400, "text/plain", "rejected"); return; }
    save_config(c);                                   // медленная запись NVS — вне мьютекса
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200))) { g_cfg = c; g_cfg_dirty = true; xSemaphoreGive(g_mtx); g_http.send(200, "text/plain", "ok"); }
    else g_http.send(503, "text/plain", "busy");
}

// POST /api/ext  a=<ток, А>  — ток Epever от Pi. Принимает admin-пароль ИЛИ отдельный токен
// (заголовок X-GS-Token), чтобы компрометация Pi не давала OTA. Значение живёт ext_ttl_ms.
static void h_ext() {
    bool tok = g_xtok.length() >= 16 && g_http.hasHeader("X-GS-Token") && ct_equal(g_http.header("X-GS-Token"), g_xtok)
               && g_http.hasHeader("X-GS");
    if (!tok && !authed(true)) return;
    float a;
    if (!gs::parse_ext_current(g_http.arg("a").c_str(), a)) { g_http.send(400, "text/plain", "bad value"); return; }
    uint32_t bits; memcpy(&bits, &a, 4);
    g_ext_bits.store(bits); g_ext_at_ms.store((int64_t)now_ms());
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

static bool g_upd_auth = false, g_upd_ok = false;
static String g_upd_err;

static void h_update_done() {
    if (!g_upd_auth) { g_http.requestAuthentication(); return; }
    bool ok = g_upd_ok && !Update.hasError();
    g_http.send(ok ? 200 : 500, "text/plain", ok ? String("OK, rebooting") : String("FAILED: ") + (g_upd_err.length() ? g_upd_err : Update.errorString()));
    g_upd_auth = false; g_ota_active = false;
    if (ok) { delay(500); ESP.restart(); }
}

static void upd_fail(const char* why) { g_upd_err = why; g_upd_ok = false; Update.abort(); g_ota_active = false; }

static void h_update_upload() {
    HTTPUpload& up = g_http.upload();
    if (up.status == UPLOAD_FILE_START) {
        g_upd_auth = basic_ok() && g_http.hasHeader("X-GS");
        g_upd_ok = false; g_upd_err = "";
        if (!g_upd_auth) return;
        g_ota_active = true;
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) upd_fail("begin");
    } else if (!g_upd_auth || !g_ota_active) {
        return;
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (up.totalSize == up.currentSize) {            // первый чанк: проверка заголовка образа ESP32
            if (up.currentSize < 48 || up.buf[0] != 0xE9) { upd_fail("not an ESP32 image"); return; }
            uint32_t magic; memcpy(&magic, up.buf + 32, 4);   // esp_app_desc_t.magic_word
            if (magic != 0xABCD5432u) { upd_fail("no app descriptor"); return; }
        }
        if (Update.write(up.buf, up.currentSize) != up.currentSize) upd_fail("write");
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.isRunning()) g_upd_ok = Update.end(false);     // false: размер должен совпасть с записанным
        if (!g_upd_ok && !g_upd_err.length()) g_upd_err = Update.errorString();
        g_ota_active = false;
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        upd_fail("aborted");
    }
}

// Первичная настройка. Доступна, пока пароль администратора не задан; после этого — только
// с паролем. Одноразовый токен формы защищает от кросс-сайтовой подмены (CSRF). Сброс
// забытого пароля — удержанием BOOT при включении (см. factory_reset_if_requested).
static void h_setup() {
    bool open_setup = g_http_pass.length() == 0;
    if (!open_setup && !basic_ok()) { g_http.requestAuthentication(); return; }
    if (g_http.method() == HTTP_GET) {
        char b[24]; snprintf(b, sizeof b, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
        g_setup_token = b;
        String page = String("<!doctype html><meta charset=utf-8><title>setup</title><h3>setup</h3>"
            "<form method=post action=/setup><input type=hidden name=t value=") + g_setup_token +
            ">SSID <input name=ssid><br>Wi-Fi password <input name=pass type=password><br>"
            "Admin password (&gt;= 8) <input name=hpass type=password><br>"
            "Ext token for Pi (&gt;= 16, optional) <input name=xtok type=password><br>"
            "<input type=submit value=Save></form>";
        g_http.send(200, "text/html", page);
        return;
    }
    if (!g_setup_token.length() || !ct_equal(g_http.arg("t"), g_setup_token)) { g_http.send(403, "text/plain", "bad token"); return; }
    g_setup_token = "";
    String hp = g_http.arg("hpass"), xt = g_http.arg("xtok");
    if (hp.length() && hp.length() < 8) { g_http.send(400, "text/plain", "admin password too short"); return; }
    if (xt.length() && xt.length() < 16) { g_http.send(400, "text/plain", "ext token too short"); return; }
    if (open_setup && !hp.length()) { g_http.send(400, "text/plain", "admin password required"); return; }
    bool changed = false;
    Preferences p; p.begin("gs", false);
    if (g_http.arg("ssid").length()) { p.putString("ssid", g_http.arg("ssid")); p.putString("pass", g_http.arg("pass")); changed = true; }
    if (hp.length()) { p.putString("hpass", hp); changed = true; }
    if (xt.length()) { p.putString("xtok", xt); changed = true; }
    p.end();
    g_http.send(changed ? 200 : 400, "text/plain", changed ? "saved, rebooting" : "nothing changed");
    if (changed) { delay(500); ESP.restart(); }
}

// Удержание BOOT (GPIO0) ~3 с при включении стирает пароль администратора, Wi-Fi и токен.
static void factory_reset_if_requested() {
    pinMode(0, INPUT_PULLUP);
    if (digitalRead(0) != LOW) return;
    uint32_t t0 = millis();
    while (digitalRead(0) == LOW) { if (millis() - t0 > 3000) {
        Preferences p; p.begin("gs", false); p.remove("hpass"); p.remove("ssid"); p.remove("pass"); p.remove("xtok"); p.remove("appass"); p.end();
        Serial.println("factory reset of access settings done");
        return; } delay(10); }
}

// ---- Wi-Fi ------------------------------------------------------------------------------------
static void start_ap() {
    String name = "gs-setup-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
    Preferences p; p.begin("gs", false);
    String pw = p.getString("appass", "");
    if (pw.length() < 24) {      // 24 hex = 96 бит
        char b[40]; snprintf(b, sizeof b, "%08lx%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
        pw = b; p.putString("appass", pw);
    }
    p.end();
    WiFi.mode(WIFI_AP_STA);       // STA продолжает пытаться подключиться
    WiFi.softAP(name.c_str(), pw.c_str());
    g_ap_mode = true;
    Serial.printf("AP mode: ssid=%s pass=%s ip=%s\n", name.c_str(), pw.c_str(), WiFi.softAPIP().toString().c_str());
}

static void connect_sta() {
    Preferences p; p.begin("gs", true);
    String ssid = p.getString("ssid", ""), pass = p.getString("pass", "");
    String host = p.getString("host", "grid-supervisor");
    g_http_pass = p.getString("hpass", "");
    g_xtok = p.getString("xtok", "");
    p.end();
    WiFi.persistent(false);
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
    factory_reset_if_requested();
    g_cfg = load_config();
    connect_sta();
    configTime(0, 0, "pool.ntp.org", "time.google.com");     // всё время — UTC
    const char* hdr[] = {"X-GS", "X-GS-Token", "Authorization"};
    g_http.collectHeaders(hdr, 3);
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
    g_hb_ms.store(now_ms());
    g_http.handleClient();
    // AP поднимаем только если с начала загрузки Wi-Fi ни разу не подключался (потом — никогда:
    // иначе короткий обрыв/деаутентификация загоняли бы устройство в AP).
    if (WiFi.status() == WL_CONNECTED) {
        if (!g_ever_connected) { g_ever_connected = true; }
        if (g_ap_mode) { WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); g_ap_mode = false; }
    } else if (!g_ever_connected && !g_ap_mode && millis() - g_wifi_try_ms > 120000) {
        start_ap();
    }
    // OTA-образ считается рабочим только при признаках здоровья: ядро сделало >= 10 циклов
    // (шина опрашивается) и прошло >= 2 минут. На платах без rollback в загрузчике вызов безвреден.
    if (!g_ota_validated && now_ms() > 120000 && g_publishes.load() >= 10) {
        esp_ota_mark_app_valid_cancel_rollback(); g_ota_validated = true;
    }
}

}  // namespace shell
