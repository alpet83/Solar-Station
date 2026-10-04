#include "shell.h"
#include "gs_cli.h"

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
static std::atomic<int> g_manual{-1};
static std::atomic<bool> g_sniff_only{false};
static std::atomic<int64_t> g_api_at_ms{-1};
static std::atomic<bool> g_mqtt_bad{false};
static std::atomic<bool> g_serial_log{true};
static std::atomic<uint32_t> g_lg_period_s{90};
static std::atomic<bool> g_lg_dirty{false};
static shell::SniffProvider g_sniff_prov = nullptr;
static std::atomic<uint64_t> g_hb_ms{0};
static std::atomic<uint32_t> g_publishes{0};
static String g_xtok;                 // токен только для /api/ext (отдельная роль для Pi)
static String g_setup_token;
static uint32_t g_boot_id = 0;
static String g_http_pass;            // пусто = пароль не задан (запись запрещена, пока не задан)
static bool g_ap_mode = false;
static uint32_t g_wifi_try_ms = 0;
static bool g_ever_connected = false;
static const int kNets = 4;                 // основная + резервные сети, порядок = приоритет
static String g_ssid[kNets], g_wpass[kNets];
static int g_net_cur = -1;                   // индекс сети, к которой идёт/шла попытка
static bool g_was_conn = false, g_scan_pending = false;
static uint32_t g_scan_ms = 0, g_boot_ms = 0;
#ifndef GS_REV
#define GS_REV "dev"
#endif
static const char* kRev = GS_REV;
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

static void touch_api() { g_api_at_ms.store((int64_t)(esp_timer_get_time() / 1000)); }
Link link_state() { return WiFi.status() == WL_CONNECTED ? Link::Connected : (g_ap_mode ? Link::Ap : Link::Offline); }
uint64_t api_age_ms() { int64_t a = g_api_at_ms.load(); int64_t n = (int64_t)(esp_timer_get_time() / 1000); return a < 0 ? UINT64_MAX : (uint64_t)(n - a); }
void set_mqtt_ok(bool ok) { g_mqtt_bad.store(!ok); }
bool mqtt_bad() { return g_mqtt_bad.load(); }
bool serial_log() { return g_serial_log.load(); }
uint32_t logger_period_s() { return g_lg_period_s.load(); }
bool take_logger_period(uint32_t& v) { if (!g_lg_dirty.exchange(false)) return false; v = g_lg_period_s.load(); return true; }
bool sniff_only() { return g_sniff_only.load(); }
void set_sniff_provider(SniffProvider p) { g_sniff_prov = p; }
int take_manual() { return g_manual.exchange(-1); }
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
    touch_api();
    static char buf[2800];
    static Snapshot s;
    if (!xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100))) { g_http.send(503, "text/plain", "busy"); return; }
    s = g_snap; xSemaphoreGive(g_mtx);
    gs::StateView v{&s.tel, &s.dec, &s.in, s.uptime_ms, s.seq, g_boot_id, s.safe_mode, epoch_or_neg(),
                    s.paused, s.pause_left_ms, s.rx_ok, s.rx_err, s.rx_foreign, s.rx_busy, s.rx_exc, s.events, s.n_events, s.sniff_only};
    size_t n = gs::state_to_json(v, buf, sizeof buf);
    g_http.sendHeader("Cache-Control", "no-store");
    g_http.send(n ? 200 : 500, "application/json", n ? buf : "{}");
}

static void h_config_get() {
    if (!authed(false)) return;
    touch_api();
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
    touch_api();
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
    touch_api();
    float a;
    if (!gs::parse_ext_current(g_http.arg("a").c_str(), a)) { g_http.send(400, "text/plain", "bad value"); return; }
    uint32_t bits; memcpy(&bits, &a, 4);
    g_ext_bits.store(bits); g_ext_at_ms.store((int64_t)now_ms());
    g_http.send(200, "text/plain", "ok");
}

// POST /api/cmd  ongrid=0|1 — команда оператора (из HA через мост на Pi). Включает ручную паузу
// автоматики; защиты (hard-floor и др.) команду при необходимости перебивают.
static void h_cmd() {
    if (!authed(true)) return;
    touch_api();
    if (g_sniff_only.load()) { g_http.send(409, "text/plain", "sniff mode: control disabled"); return; }
    String v = g_http.arg("ongrid");
    if (v != "0" && v != "1") { g_http.send(400, "text/plain", "ongrid must be 0 or 1"); return; }
    g_manual.store(v.toInt());
    g_http.send(200, "text/plain", "queued");
}

// GET /api/sniff — ритм логгера и последние кадры шины (готовый JSON от задачи шины).
static void h_sniff() {
    if (!authed(false)) return;
    touch_api();
    if (!g_sniff_prov) { g_http.send(404, "text/plain", "no sniffer"); return; }
    const size_t cap = 9000;
    char* buf = (char*)malloc(cap);
    if (!buf) { g_http.send(503, "text/plain", "nomem"); return; }
    size_t n = g_sniff_prov(buf, cap);
    g_http.sendHeader("Cache-Control", "no-store");
    g_http.send(n ? 200 : 503, "application/json", n ? buf : "{}");
    free(buf);
}

// POST /api/logger  period_s=30..900 — ожидаемый период пачек даталоггера (сбрасывает захват ритма).
static void h_logger() {
    if (!authed(true)) return;
    long v = g_http.arg("period_s").toInt();
    if (v < 30 || v > 900) { g_http.send(400, "text/plain", "period_s must be 30..900"); return; }
    Preferences p; p.begin("gs", false); p.putUInt("lgp", (uint32_t)v); p.end();
    g_lg_period_s.store((uint32_t)v); g_lg_dirty.store(true);
    g_http.send(200, "text/plain", "ok");
}

// POST /api/mode  m=sniff|normal — режим только приёма (управление сетью выключено, на шину не
// передаём; Pi продолжает управлять сам). Сохраняется в NVS.
static void h_mode() {
    if (!authed(true)) return;
    String m = g_http.arg("m");
    if (m != "sniff" && m != "normal") { g_http.send(400, "text/plain", "m must be sniff or normal"); return; }
    if (m == "sniff") {            // в режиме приёма защит нет: не входим, пока сеть отключена
        static Snapshot s;
        if (!xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100))) { g_http.send(503, "text/plain", "busy"); return; }
        s = g_snap; xSemaphoreGive(g_mtx);
        if (!gs::sniff_entry_ok(s.tel.ongrid_reg)) { g_http.send(409, "text/plain", "grid is disconnected (20105=0): sniff mode has no protection"); return; }
    }
    Preferences p; p.begin("gs", false); p.putBool("sniff", m == "sniff"); p.end();
    g_sniff_only.store(m == "sniff");
    g_http.send(200, "text/plain", "ok");
}

// GET /api/regs?addr=25201&count=79 — сырые регистры из кэша (без обращения к шине). 404, если
// диапазон не опрашивается; age_ms в ответе — возраст блока, свежесть проверяет потребитель.
static void h_regs() {
    if (!authed(false)) return;
    touch_api();
    long addr = g_http.arg("addr").toInt(), cnt = g_http.arg("count").toInt();
    if (addr < 0 || addr > 65535 || cnt < 1 || cnt > gs::kMaxBlockRegs) { g_http.send(400, "text/plain", "bad range"); return; }
    static Snapshot s;
    if (!xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100))) { g_http.send(503, "text/plain", "busy"); return; }
    s = g_snap; xSemaphoreGive(g_mtx);
    static uint16_t regs[gs::kMaxBlockRegs]; uint64_t age = 0;
    if (!s.cache.lookup((uint16_t)addr, (uint16_t)cnt, now_ms(), regs, &age)) { g_http.send(404, "text/plain", "not cached"); return; }
    static char buf[700];
    size_t n = gs::regs_to_json((uint16_t)addr, (uint16_t)cnt, age, regs, buf, sizeof buf);
    g_http.sendHeader("Cache-Control", "no-store");
    g_http.send(n ? 200 : 500, "application/json", n ? buf : "{}");
}

static const char kIndex[] PROGMEM =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
    "<title>grid-supervisor</title><style>body{font:14px monospace;margin:12px}pre{white-space:pre-wrap}</style>"
    "<h3>grid-supervisor</h3><pre id=s>loading…</pre>"
    "<script>async function t(){try{const r=await fetch('/api/state');const j=await r.json();"
    "document.getElementById('s').textContent=JSON.stringify(j,null,1)}catch(e){}}t();setInterval(t,5000)</script>"
    "<p><a href=/api/config>config</a> · <a href=/api/sniff>sniff</a> · <a href=/update>OTA</a></p>";

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
static String esc(const String& in) {
    String o; o.reserve(in.length() + 8);
    for (size_t k = 0; k < in.length(); ++k) {
        char c = in[k];
        if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;"; else if (c == '\'') o += "&#39;"; else o += c;
    }
    return o;
}

static void h_setup() {
    bool open_setup = g_http_pass.length() == 0;
    if (!open_setup && !basic_ok()) { g_http.requestAuthentication(); return; }
    if (g_http.method() == HTTP_GET) {
        char b[24]; snprintf(b, sizeof b, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
        g_setup_token = b;
        Preferences p; p.begin("gs", true); String host = p.getString("host", "grid-supervisor"); p.end();
        String page = String("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
            "<title>setup</title><style>body{font:14px sans-serif;margin:12px}input{margin:2px 0}</style>"
            "<h3>grid-supervisor setup</h3><p>rev ") + kRev + " · ";
        if (WiFi.status() == WL_CONNECTED) page += "Wi-Fi: " + esc(WiFi.SSID()) + ", " + String(WiFi.RSSI()) + " dBm, " + WiFi.localIP().toString();
        else page += g_ap_mode ? "Wi-Fi not connected (AP mode)" : "Wi-Fi not connected";
        page += "</p><form method=post action=/setup><input type=hidden name=t value=" + g_setup_token + ">"
                "<p>Wi-Fi networks by priority (1 = main, others = backup; empty SSID = remove; empty password = keep)</p>";
        for (int k = 0; k < kNets; ++k)
            page += String(k + 1) + ". SSID <input name=s" + k + " value=\"" + esc(g_ssid[k]) + "\" maxlength=32> password <input name=w" + k +
                    " type=password placeholder=\"" + (g_ssid[k].length() ? "(unchanged)" : "") + "\" maxlength=63><br>";
        page += "<p>Hostname <input name=host value=\"" + esc(host) + "\" maxlength=32><br>"
                "Admin password (&gt;= 8" + String(open_setup ? ", required" : ", empty = keep") + ") <input name=hpass type=password><br>"
                "Setup AP password (&gt;= 8, empty = keep) <input name=appass type=password><br>"
                "Ext token for Pi (&gt;= 16, empty = keep) <input name=xtok type=password></p>"
                "<input type=submit value=\"Save and reboot\"></form>";
        g_http.send(200, "text/html", page);
        return;
    }
    if (!g_setup_token.length() || !ct_equal(g_http.arg("t"), g_setup_token)) { g_http.send(403, "text/plain", "bad token"); return; }
    g_setup_token = "";
    String hp = g_http.arg("hpass"), xt = g_http.arg("xtok"), ap = g_http.arg("appass"), host = g_http.arg("host");
    if (hp.length() && hp.length() < 8) { g_http.send(400, "text/plain", "admin password too short"); return; }
    if (ap.length() && ap.length() < 8) { g_http.send(400, "text/plain", "AP password too short"); return; }
    if (xt.length() && xt.length() < 16) { g_http.send(400, "text/plain", "ext token too short"); return; }
    if (open_setup && !hp.length()) { g_http.send(400, "text/plain", "admin password required"); return; }
    for (int k = 0; k < kNets; ++k) {
        if (g_http.arg("s" + String(k)).length() > 32 || g_http.arg("w" + String(k)).length() > 63) { g_http.send(400, "text/plain", "ssid/password too long"); return; }
    }
    if (host.length() > 32) { g_http.send(400, "text/plain", "hostname too long"); return; }
    for (size_t k = 0; k < host.length(); ++k) {
        char c = host[k];
        if (!(isalnum((unsigned char)c) || c == '-')) { g_http.send(400, "text/plain", "hostname: letters, digits, '-' only"); return; }
    }
    Preferences p; p.begin("gs", false);
    for (int k = 0; k < kNets; ++k) {
        String ks = "s" + String(k), kw = "w" + String(k);
        if (!g_http.hasArg(ks)) continue;
        String ns = g_http.arg(ks), nw = g_http.arg(kw);
        if (!ns.length()) { p.remove(ks.c_str()); p.remove(kw.c_str()); continue; }
        p.putString(ks.c_str(), ns);
        if (nw.length()) p.putString(kw.c_str(), nw);
        else if (ns != g_ssid[k]) p.putString(kw.c_str(), "");      // новая сеть без пароля = открытая
    }
    p.remove("ssid"); p.remove("pass");                              // прежний формат (одна сеть) вытеснен
    if (host.length()) p.putString("host", host);
    if (hp.length()) p.putString("hpass", hp);
    if (ap.length()) p.putString("appass", ap);
    if (xt.length()) p.putString("xtok", xt);
    p.end();
    g_http.send(200, "text/plain", "saved, rebooting");
    delay(500); ESP.restart();
}

// Удержание BOOT (GPIO0) ~3 с при включении стирает пароль администратора, Wi-Fi и токен.
static void factory_reset_if_requested() {
    pinMode(0, INPUT_PULLUP);
    if (digitalRead(0) != LOW) return;
    uint32_t t0 = millis();
    while (digitalRead(0) == LOW) { if (millis() - t0 > 3000) {
        Preferences p; p.begin("gs", false); p.remove("hpass"); p.remove("ssid"); p.remove("pass"); p.remove("xtok"); p.remove("appass");
        for (int k = 0; k < kNets; ++k) { p.remove(("s" + String(k)).c_str()); p.remove(("w" + String(k)).c_str()); }
        p.end();
        Serial.println("factory reset of access settings done");
        return; } delay(10); }
}

// ---- Wi-Fi ------------------------------------------------------------------------------------
static void start_ap() {
    String name = "gs-setup-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
    Preferences p; p.begin("gs", false);
    String pw = p.getString("appass", "");
    if (pw.length() < 8) pw = String("gs-") + kRev;          // пока не задан свой: "gs-<ревизия сборки>"
    p.end();
    WiFi.mode(WIFI_AP_STA);       // STA продолжает пытаться подключиться
    WiFi.softAP(name.c_str(), pw.c_str());
    g_ap_mode = true;
    Serial.printf("AP mode: ssid=%s pass=%s ip=%s\n", name.c_str(), pw.c_str(), WiFi.softAPIP().toString().c_str());
}

static int n_nets() { int n = 0; for (int k = 0; k < kNets; ++k) if (g_ssid[k].length()) ++n; return n; }

static void begin_net(int k) {
    g_net_cur = k;
    g_wifi_try_ms = millis();
    WiFi.disconnect(false, false);
    WiFi.begin(g_ssid[k].c_str(), g_wpass[k].c_str());
    Serial.printf("wifi: try #%d '%s'\n", k + 1, g_ssid[k].c_str());
}

static void next_net() {            // следующая настроенная сеть по кругу
    for (int step = 1; step <= kNets; ++step) {
        int k = (g_net_cur + step + kNets) % kNets;
        if (g_ssid[k].length()) { begin_net(k); return; }
    }
}

static void connect_sta() {
    Preferences p; p.begin("gs", true);
    for (int k = 0; k < kNets; ++k) { g_ssid[k] = p.getString(("s" + String(k)).c_str(), ""); g_wpass[k] = p.getString(("w" + String(k)).c_str(), ""); }
    if (!g_ssid[0].length()) { g_ssid[0] = p.getString("ssid", ""); g_wpass[0] = p.getString("pass", ""); }   // прежний формат
    String host = p.getString("host", "grid-supervisor");
    g_http_pass = p.getString("hpass", "");
    g_xtok = p.getString("xtok", "");
    p.end();
    g_boot_ms = millis();
    WiFi.persistent(false);
    Serial.printf("gs rev %s\n", kRev);
    if (!n_nets()) { start_ap(); return; }
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(host.c_str());
    WiFi.setAutoReconnect(false);   // переподключением и сменой сети управляет wifi_tick()
    g_net_cur = -1; next_net();
}

// Управление Wi-Fi: основная сеть, при недоступности — резервные по кругу (15 с на попытку);
// на резервной сети раз в 5 мин ищем более приоритетную (асинхронный скан).
static void wifi_tick() {
    const bool conn = WiFi.status() == WL_CONNECTED;
    if (conn) {
        g_ever_connected = true; g_was_conn = true;
        if (g_ap_mode) { WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); g_ap_mode = false; }
        if (g_net_cur > 0) {
            if (g_scan_pending) {
                int n = WiFi.scanComplete();
                if (n >= 0) {
                    int best = -1;
                    for (int k = 0; k < g_net_cur && best < 0; ++k)
                        for (int i = 0; i < n; ++i) if (g_ssid[k].length() && WiFi.SSID(i) == g_ssid[k]) { best = k; break; }
                    WiFi.scanDelete(); g_scan_pending = false;
                    if (best >= 0) begin_net(best);
                } else if (n == WIFI_SCAN_FAILED) { g_scan_pending = false; }
            } else if (millis() - g_scan_ms > 300000) {
                g_scan_ms = millis(); WiFi.scanNetworks(true); g_scan_pending = true;
            }
        }
        return;
    }
    g_scan_pending = false;
    if (n_nets() && millis() - g_wifi_try_ms > 15000) {
        if (g_was_conn) { g_was_conn = false; begin_net(g_net_cur); }   // только что потеряли — сначала та же сеть
        else next_net();
    }
    // AP — только если Wi-Fi ни разу не подключался с загрузки: короткий обрыв в AP не уводит.
    if (!g_ever_connected && !g_ap_mode && millis() - g_boot_ms > 120000) start_ap();
}


// ---- консоль по Serial ------------------------------------------------------------------------
// Физический доступ к USB = доверенный доступ, поэтому без пароля. Выполняется в задаче оболочки
// (как и HTTP-обработчики), гонок с массивами сетей нет.
static String net_status_line(bool with_ap_hint) {
    String s;
    if (WiFi.status() == WL_CONNECTED)
        s = "net: STA up ssid='" + WiFi.SSID() + "' ip=" + WiFi.localIP().toString() + " rssi=" + String(WiFi.RSSI()) +
            " dBm net#" + String(g_net_cur + 1) + "/" + String(n_nets());
    else if (!n_nets()) s = "net: no networks configured (use: net 1 <ssid> <pass>)";
    else s = "net: STA down, trying #" + String(g_net_cur + 1) + " '" + (g_net_cur >= 0 ? g_ssid[g_net_cur] : String("?")) + "'";
    if (g_ap_mode) s += " | AP on ssid=gs-setup-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX) + " ip=" + WiFi.softAPIP().toString();
    else if (with_ap_hint) s += " | AP off";
    return s;
}

// Строка статуса: сразу при смене состояния и раз в 30 с — чтобы по консоли всегда было видно, где плата.
static void net_status_tick() {
    static String last; static uint32_t at = 0;
    String key = WiFi.status() == WL_CONNECTED ? "up:" + WiFi.SSID() + WiFi.localIP().toString() : String("down:") + g_net_cur + (g_ap_mode ? "ap" : "");
    if (key != last || millis() - at >= 30000) { last = key; at = millis(); Serial.println(net_status_line(true)); }
}

static void nets_reload_apply(int k, bool deleted) {     // сети изменились: применяем без перезагрузки
    if (!n_nets()) { WiFi.disconnect(false, false); return; }
    const bool conn = WiFi.status() == WL_CONNECTED;
    if (!conn) { g_was_conn = false; if (deleted && k == g_net_cur) next_net(); else begin_net(k); }
    else if (k == g_net_cur) { if (deleted) next_net(); else begin_net(k); }
}

static void cli_exec(const gs::CliCmd& c) {
    using gs::CliKind;
    switch (c.kind) {
    case CliKind::None: break;
    case CliKind::Error: Serial.printf("error: %s\n", c.err); break;
    case CliKind::Help: Serial.print(gs::cli_help_text()); break;
    case CliKind::Status: Serial.println(net_status_line(true)); Serial.printf("rev %s, mode %s, uptime %lu s\n", kRev, g_sniff_only.load() ? "sniff" : "normal", (unsigned long)(now_ms() / 1000)); break;
    case CliKind::Nets:
        for (int k = 0; k < kNets; ++k) {
            if (!g_ssid[k].length()) { Serial.printf("%d. (empty)\n", k + 1); continue; }
            Serial.printf("%d. '%s' %s%s\n", k + 1, g_ssid[k].c_str(), g_wpass[k].length() ? "pass=*** " : "(open) ",
                          (WiFi.status() == WL_CONNECTED && k == g_net_cur) ? "<- connected" : "");
        }
        break;
    case CliKind::NetSet: {
        const int k = c.net - 1;
        Preferences p; p.begin("gs", false);
        p.putString(("s" + String(k)).c_str(), c.a); p.putString(("w" + String(k)).c_str(), c.b);
        p.remove("ssid"); p.remove("pass"); p.end();
        g_ssid[k] = c.a; g_wpass[k] = c.b;
        Serial.printf("net %d saved: '%s' %s\n", c.net, c.a, c.b[0] ? "" : "(open)");
        nets_reload_apply(k, false);
        break; }
    case CliKind::NetDel: {
        const int k = c.net - 1;
        Preferences p; p.begin("gs", false);
        p.remove(("s" + String(k)).c_str()); p.remove(("w" + String(k)).c_str()); p.remove("ssid"); p.remove("pass"); p.end();
        g_ssid[k] = ""; g_wpass[k] = "";
        Serial.printf("net %d removed\n", c.net);
        nets_reload_apply(k, true);
        break; }
    case CliKind::Host: { Preferences p; p.begin("gs", false); p.putString("host", c.a); p.end(); Serial.println("hostname saved, applies after reboot"); break; }
    case CliKind::ApPass: { Preferences p; p.begin("gs", false); p.putString("appass", c.a); p.end(); Serial.println("AP password saved, applies at next AP start"); break; }
    case CliKind::AdminPass: { Preferences p; p.begin("gs", false); p.putString("hpass", c.a); p.end(); g_http_pass = c.a; Serial.println("admin password saved"); break; }
    case CliKind::Mode:
        if (c.flag < 0) { Serial.printf("mode: %s\n", g_sniff_only.load() ? "sniff" : "normal"); break; }
        if (c.flag == 1) {
            static Snapshot s;
            if (!xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100))) { Serial.println("error: busy, retry"); break; }
            s = g_snap; xSemaphoreGive(g_mtx);
            if (!gs::sniff_entry_ok(s.tel.ongrid_reg)) { Serial.println("error: grid is disconnected (20105=0): sniff mode has no protection"); break; }
        }
        { Preferences p; p.begin("gs", false); p.putBool("sniff", c.flag == 1); p.end(); }
        g_sniff_only.store(c.flag == 1);
        Serial.printf("mode: %s\n", c.flag == 1 ? "sniff (receive only, no bus writes)" : "normal (active control)");
        break;
    case CliKind::Logger: {
        Preferences p; p.begin("gs", false); p.putUInt("lgp", (uint32_t)c.num); p.end();
        g_lg_period_s.store((uint32_t)c.num); g_lg_dirty.store(true);
        Serial.printf("logger period: %ld s\n", c.num);
        break; }
    case CliKind::Log:
        if (c.flag >= 0) g_serial_log.store(c.flag == 1);
        Serial.printf("log: %s\n", g_serial_log.load() ? "on" : "off");
        break;
    case CliKind::Reboot: Serial.println("rebooting"); Serial.flush(); delay(200); ESP.restart(); break;
    }
}

static void cli_poll() {
    static gs::CliLine ln;
    static bool shown_prompt = false;
    int guard = 0;
    while (Serial.available() > 0 && guard++ < 256) {
        const char ch = (char)Serial.read();
        if (ch == '\r' || ch == '\n') { if (shown_prompt) { Serial.print("\r\n"); shown_prompt = false; } }
        else if ((unsigned char)ch >= 0x20 || ch == 0x08 || ch == 0x7f) { if (ch == 0x08 || ch == 0x7f) Serial.print("\b \b"); else Serial.write(ch); shown_prompt = true; }
        if (ln.feed(ch)) { cli_exec(gs::cli_parse(ln.line(), kNets)); shown_prompt = false; }
        else if (ln.overflowed()) Serial.println("error: line too long");
    }
}

void begin(uint32_t boot_id) {
    g_boot_id = boot_id;
    g_mtx = xSemaphoreCreateMutex();
    factory_reset_if_requested();
    g_cfg = load_config();
    { Preferences p; p.begin("gs", true); g_sniff_only.store(p.getBool("sniff", false)); { uint32_t v = p.getUInt("lgp", 90); g_lg_period_s.store(v >= 30 && v <= 900 ? v : 90); g_lg_dirty.store(true); } p.end(); }
    connect_sta();
    Serial.println("serial console: type 'help'");
    configTime(0, 0, "pool.ntp.org", "time.google.com");     // всё время — UTC
    const char* hdr[] = {"X-GS", "X-GS-Token", "Authorization"};
    g_http.collectHeaders(hdr, 3);
    g_http.on("/", HTTP_GET, h_index);
    g_http.on("/api/state", HTTP_GET, h_state);
    g_http.on("/api/regs", HTTP_GET, h_regs);
    g_http.on("/api/config", HTTP_GET, h_config_get);
    g_http.on("/api/config", HTTP_POST, h_config_set);
    g_http.on("/api/ext", HTTP_POST, h_ext);
    g_http.on("/api/cmd", HTTP_POST, h_cmd);
    g_http.on("/api/sniff", HTTP_GET, h_sniff);
    g_http.on("/api/mode", HTTP_POST, h_mode);
    g_http.on("/api/logger", HTTP_POST, h_logger);
    g_http.on("/setup", HTTP_ANY, h_setup);
    g_http.on("/update", HTTP_GET, h_update_form);
    g_http.on("/update", HTTP_POST, h_update_done, h_update_upload);
    g_http.begin();
}

void loop() {
    g_hb_ms.store(now_ms());
    g_http.handleClient();
    wifi_tick();
    cli_poll();
    net_status_tick();
    // OTA-образ считается рабочим только при признаках здоровья: ядро сделало >= 10 циклов
    // (шина опрашивается) и прошло >= 2 минут. На платах без rollback в загрузчике вызов безвреден.
    if (!g_ota_validated && now_ms() > 120000 && g_publishes.load() >= 10) {
        esp_ota_mark_app_valid_cancel_rollback(); g_ota_validated = true;
    }
}

}  // namespace shell
