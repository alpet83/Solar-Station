#include "notify.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_timer.h>
#include <time.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#ifndef GS_REV
#define GS_REV "dev"
#endif

namespace notify {

static SemaphoreHandle_t g_mtx = nullptr;
static gs::NotifyQueue g_q;
static gs::WebhookCfg g_cfg;
static char g_host[40] = "grid-supervisor";
static std::atomic<uint32_t> g_lost{0};            // событий потеряно из-за занятого mutex (в stats.dropped добавляется при выдаче)

static uint32_t now32() { return (uint32_t)(esp_timer_get_time() / 1000); }

static gs::NotifyPolicy policy_for(const gs::WebhookCfg& c) {
    gs::NotifyPolicy p;
    p.min_sev = c.min_sev;
    return p;
}

// ---- NVS ------------------------------------------------------------------------------------------------
static void load_host() {
    Preferences p; p.begin("gs", true);
    String h = p.getString("host", "grid-supervisor");
    p.end();
    char tmp[40]; gs::notify_copy(tmp, sizeof tmp, h.c_str());
    if (g_mtx && xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100))) { memcpy(g_host, tmp, sizeof g_host); xSemaphoreGive(g_mtx); }
    else if (!g_mtx) memcpy(g_host, tmp, sizeof g_host);
}

static void load_cfg() {
    gs::WebhookCfg c; char err[64];
    Preferences p; p.begin("gs", true);
    for (size_t i = 0; i < gs::kWebhookKeyCount; ++i) {          // "en" последним: он проверяет, что url задан
        char nk[20]; snprintf(nk, sizeof nk, "wh_%s", gs::kWebhookKeys[i]);
        if (!p.isKey(nk)) continue;
        String v = p.getString(nk, "");
        if (!gs::webhook_set(c, gs::kWebhookKeys[i], v.c_str(), err, sizeof err)) Serial.printf("NVS %s invalid (%s) -> ignored\n", nk, err);
    }
    p.end();
    g_cfg = c;
    g_q.set_policy(policy_for(c));
}

static void save_keys(const gs::WebhookCfg& c, const char* const* keys, size_t n) {
    Preferences p; p.begin("gs", false);
    for (size_t i = 0; i < n; ++i) {
        char nk[20]; snprintf(nk, sizeof nk, "wh_%s", keys[i]);
        static char v[gs::kBodyMax + 8];
        gs::webhook_get(c, keys[i], v, sizeof v);
        p.putString(nk, v);
    }
    p.end();
}

// ---- публичный API ------------------------------------------------------------------------------------------
void push(const gs::NotifyEvent& ev) {
    if (!g_mtx) return;
    if (!xSemaphoreTake(g_mtx, 0)) { g_lost.fetch_add(1); return; }
    if (g_cfg.enabled) g_q.push(ev, now32());
    xSemaphoreGive(g_mtx);
}

bool test(char* err, size_t err_cap) {
    if (!g_mtx) { gs::notify_copy(err, err_cap, "not started"); return false; }
    if (!xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100))) { gs::notify_copy(err, err_cap, "busy, retry"); return false; }
    bool ok = g_cfg.enabled && g_cfg.url[0];
    if (ok) {
        gs::NotifyEvent ev; ev.sev = gs::kSevInfo; ev.uptime_ms = now32();
        gs::notify_copy(ev.tag, sizeof ev.tag, "TEST");
        snprintf(ev.message, sizeof ev.message, "webhook test, rev %s", GS_REV);
        time_t t = time(nullptr); ev.epoch_s = t > 1700000000 ? (int64_t)t : -1;
        g_q.push(ev, now32(), true);
    } else gs::notify_copy(err, err_cap, g_cfg.url[0] ? "webhook is off (webhook on)" : "url is not set (webhook url <url>)");
    xSemaphoreGive(g_mtx);
    return ok;
}

gs::WebhookCfg config() {
    gs::WebhookCfg c;
    if (g_mtx && xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100))) { c = g_cfg; xSemaphoreGive(g_mtx); }
    return c;
}

bool set_many(const char* const* keys, const char* const* vals, size_t n, char* err, size_t err_cap) {
    if (!g_mtx) { gs::notify_copy(err, err_cap, "not started"); return false; }
    gs::WebhookCfg c = config();
    const gs::WebhookCfg old = c;
    for (int pass = 0; pass < 2; ++pass)                           // сначала всё, кроме "en", затем "en"
        for (size_t i = 0; i < n; ++i)
            if ((!strcmp(keys[i], "en")) == (pass == 1) && !gs::webhook_set(c, keys[i], vals[i], err, err_cap)) return false;
    // что изменилось — в NVS (вне mutex: запись во flash медленная)
    const char* ch[gs::kWebhookKeyCount]; size_t nc = 0;
    static char a[gs::kBodyMax + 8], b[gs::kBodyMax + 8];
    for (size_t i = 0; i < gs::kWebhookKeyCount; ++i) {
        gs::webhook_get(old, gs::kWebhookKeys[i], a, sizeof a); gs::webhook_get(c, gs::kWebhookKeys[i], b, sizeof b);
        if (strcmp(a, b)) ch[nc++] = gs::kWebhookKeys[i];
    }
    if (nc) save_keys(c, ch, nc);
    if (!xSemaphoreTake(g_mtx, pdMS_TO_TICKS(500))) { gs::notify_copy(err, err_cap, "busy, retry"); return false; }
    g_cfg = c;
    g_q.set_policy(policy_for(c));
    if (old.enabled && !c.enabled) g_q.clear();                    // выключили: накопленное не отправляем
    xSemaphoreGive(g_mtx);
    return true;
}

void reload_host() { load_host(); }

bool status(Status& out) {
    if (!g_mtx || !xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100))) return false;
    out.cfg = g_cfg; out.st = g_q.stats(); out.queue_len = g_q.size(); out.now_ms = now32();
    xSemaphoreGive(g_mtx);
    out.st.dropped += g_lost.load();
    return true;
}

static void json_esc(const char* in, char* out, size_t cap) {
    size_t k = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p && k + 7 < cap; ++p) {
        if (*p == '"' || *p == '\\') { out[k++] = '\\'; out[k++] = (char)*p; }
        else if (*p < 0x20 || *p >= 0x7f) out[k++] = ' ';
        else out[k++] = (char)*p;
    }
    out[k] = 0;
}

size_t status_json(char* out, size_t cap) {
    static Status s;
    if (!status(s)) return 0;
    char host[96], hesc[200], eesc[120];
    gs::url_origin(s.cfg.url, host, sizeof host, false);
    json_esc(host, hesc, sizeof hesc); json_esc(s.st.last_error, eesc, sizeof eesc);
    char last[24] = "null", age[16] = "null";
    if (s.st.have_last) { snprintf(last, sizeof last, "%d", s.st.last_http); snprintf(age, sizeof age, "%lu", (unsigned long)((uint32_t)(s.now_ms - s.st.last_ms) / 1000)); }
    int w = snprintf(out, cap,
        "{\"enabled\":%s,\"host\":\"%s\",\"method\":\"%s\",\"min_sev\":\"%s\",\"timeout_s\":%u,\"insecure\":%s,"
        "\"sent\":%lu,\"failed\":%lu,\"dropped\":%lu,\"suppressed\":%lu,\"filtered\":%lu,\"queue_len\":%u,"
        "\"last_http\":%s,\"last_ok\":%s,\"last_error\":\"%s\",\"last_age_s\":%s}",
        s.cfg.enabled ? "true" : "false", hesc, s.cfg.method, gs::notify_sev_name(s.cfg.min_sev), (unsigned)s.cfg.timeout_s, s.cfg.insecure ? "true" : "false",
        (unsigned long)s.st.sent, (unsigned long)s.st.failed, (unsigned long)s.st.dropped, (unsigned long)s.st.suppressed, (unsigned long)s.st.filtered,
        (unsigned)s.queue_len, last, s.st.have_last && s.st.last_ok ? "true" : "false", eesc, age);
    return (w > 0 && (size_t)w < cap) ? (size_t)w : 0;
}

// ---- отправка (только в задаче notify) ---------------------------------------------------------------------------
struct SendResult { bool ok = false; int http = 0; char err[48] = {0}; };

static SendResult send(const gs::WebhookCfg& cfg, const char* host, const gs::NotifyEvent& ev) {
    static char url[gs::kExpandedMax], body[gs::kExpandedMax];     // статические: стек задачи под TLS нужен целиком
    SendResult r;
    if (!gs::expand_template(cfg.url, ev, host, url, sizeof url, true)) { gs::notify_copy(r.err, sizeof r.err, "url template expands too long"); return r; }
    const bool post = !strcmp(cfg.method, "POST");
    if (post && !gs::expand_template(cfg.body, ev, host, body, sizeof body, true)) { gs::notify_copy(r.err, sizeof r.err, "body template expands too long"); return r; }
    const bool https = !strncmp(url, "https://", 8);
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure sec;
    // https: по умолчанию БЕЗ проверки сертификата (insecure=1) — компромисс для LAN-устройства без хранилища CA и точного времени.
    // insecure=0 — поведение библиотеки по умолчанию (проверка по встроенному набору CA, нужно синхронное время).
    if (https && cfg.insecure) sec.setInsecure();
    const bool begun = https ? http.begin(sec, url) : http.begin(plain, url);
    if (!begun) { gs::notify_copy(r.err, sizeof r.err, "bad url"); return r; }
    http.setConnectTimeout((int32_t)cfg.timeout_s * 1000);
    http.setTimeout((uint16_t)(cfg.timeout_s * 1000));
    http.setReuse(false);
    http.setUserAgent("grid-supervisor/" GS_REV);
    int code;
    if (post) { http.addHeader("Content-Type", cfg.ctype); code = http.POST((uint8_t*)body, strlen(body)); }
    else code = http.GET();
    r.http = code;
    if (code <= 0) { gs::notify_copy(r.err, sizeof r.err, HTTPClient::errorToString(code).c_str()); http.end(); return r; }
    r.ok = code >= 200 && code < 300;
    if (!r.ok) snprintf(r.err, sizeof r.err, "HTTP %d", code);
    else if (cfg.err[0]) {                                          // признак сбоя в теле ответа (как alert_webhook_error_text в прокси)
        const int len = http.getSize();
        if (len < 0 || len <= 16384) {
            String resp = http.getString();
            if (resp.indexOf(cfg.err) >= 0) { r.ok = false; gs::notify_copy(r.err, sizeof r.err, "response contains error text"); }
        }
    }
    http.end();
    return r;
}

static void notify_task(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (WiFi.status() != WL_CONNECTED) continue;               // без Wi-Fi очередь копится
        gs::NotifyEvent ev; gs::WebhookCfg cfg; char host[40]; bool got = false;
        if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(50))) {
            if (g_cfg.enabled) { got = g_q.take(now32(), ev); cfg = g_cfg; memcpy(host, g_host, sizeof host); }
            xSemaphoreGive(g_mtx);
        }
        if (!got) continue;
        if (ev.epoch_s < 0) {                                       // событие возникло до синхронизации времени: восстанавливаем по uptime
            time_t t = time(nullptr);
            if (t > 1700000000) ev.epoch_s = (int64_t)t - (int32_t)(now32() - ev.uptime_ms) / 1000;
        }
        const SendResult r = send(cfg, host, ev);                  // сеть — вне mutex
        Serial.printf("NOTIFY #%lu %s sev=%s -> %s http=%d %s\n", (unsigned long)ev.id, ev.tag, gs::notify_sev_name(ev.sev), r.ok ? "ok" : "FAIL", r.http, r.err);   // URL не печатаем (секреты)
        xSemaphoreTake(g_mtx, portMAX_DELAY);
        g_q.done(r.ok, now32(), r.http, r.err);
        xSemaphoreGive(g_mtx);
    }
}

void begin() {
    g_mtx = xSemaphoreCreateMutex();
    load_host();
    load_cfg();
    // 12 КБ: TLS-рукопожатие (mbedTLS) + HTTPClient. Приоритет 1 (ниже ядра/CAN), ядро 0 — вместе со стеком Wi-Fi, ядро управления на ядре 1.
    xTaskCreatePinnedToCore(notify_task, "gs_notify", 12288, nullptr, 1, nullptr, 0);
}

}  // namespace notify
