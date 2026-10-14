#include "gs_notify.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <strings.h>

namespace gs {

const char* notify_sev_name(uint8_t sev) { return sev >= kSevAlert ? "alert" : sev == kSevWarn ? "warn" : "info"; }

void notify_copy(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

// ---- шаблоны ----------------------------------------------------------------------------------------------
bool urlencode(const char* in, char* out, size_t cap) {
    static const char* H = "0123456789ABCDEF";
    size_t n = 0; bool ok = true;
    if (!cap) return false;
    for (const unsigned char* p = (const unsigned char*)in; *p; ++p) {
        const bool plain = isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        const size_t need = plain ? 1 : 3;
        if (n + need + 1 > cap) { ok = false; break; }
        if (plain) out[n++] = (char)*p;
        else { out[n++] = '%'; out[n++] = H[*p >> 4]; out[n++] = H[*p & 15]; }
    }
    out[n] = 0;
    return ok;
}

namespace {

bool is_name_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

bool known_placeholder(const char* name, size_t len) {
    for (size_t i = 0; i < kPlaceholderCount; ++i)
        if (strlen(kPlaceholders[i]) == len && !strncmp(kPlaceholders[i], name, len)) return true;
    return false;
}

// Ищет следующий "${имя}" начиная с p; возвращает указатель на '$' или nullptr. name/len — имя, end — за '}'.
const char* next_placeholder(const char* p, const char*& name, size_t& len, const char*& end) {
    for (; *p; ++p) {
        if (p[0] != '$' || p[1] != '{') continue;
        const char* q = p + 2;
        while (is_name_char(*q)) ++q;
        if (*q != '}') continue;           // "${a-b" и т.п. — обычный текст
        name = p + 2; len = (size_t)(q - name); end = q + 1;
        return p;
    }
    return nullptr;
}

char g_fw_version[32] = "";

void placeholder_value(const char* name, size_t len, const NotifyEvent& ev, const char* host, char* out, size_t cap) {
    out[0] = 0;
    auto is = [&](const char* n) { return strlen(n) == len && !strncmp(n, name, len); };
    if (is("host_name")) notify_copy(out, cap, host);
    else if (is("event_message")) notify_copy(out, cap, ev.message);
    else if (is("event_value")) format_event_value(ev.value, out, cap);
    else if (is("event_tag")) notify_copy(out, cap, ev.tag);
    else if (is("event_flags")) notify_copy(out, cap, ev.flags);
    else if (is("event_time")) format_event_time(ev, out, cap);
    else if (is("fw_version")) notify_copy(out, cap, g_fw_version);
}

}  // namespace

void notify_set_fw_version(const char* v) { notify_copy(g_fw_version, sizeof g_fw_version, v ? v : ""); }
const char* notify_fw_version() { return g_fw_version; }

bool validate_template(const char* tpl, char* bad, size_t bad_cap) {
    if (bad && bad_cap) bad[0] = 0;
    const char* name; size_t len; const char* end;
    for (const char* p = tpl; (p = next_placeholder(p, name, len, end)) != nullptr; p = end) {
        if (known_placeholder(name, len)) continue;
        if (bad && bad_cap) { size_t k = len < bad_cap - 1 ? len : bad_cap - 1; memcpy(bad, name, k); bad[k] = 0; }
        return false;
    }
    return true;
}

bool validate_method(const char* m) { return m && (!strcasecmp(m, "GET") || !strcasecmp(m, "POST")); }

bool validate_url(const char* url, char* err, size_t err_cap) {
    auto fail = [&](const char* msg, const char* arg = nullptr) {
        if (err && err_cap) { if (arg) snprintf(err, err_cap, msg, arg); else notify_copy(err, err_cap, msg); }
        return false;
    };
    if (!url) return fail("url is null");
    const size_t n = strlen(url);
    if (n > kUrlMax) return fail("url too long (max 255)");
    const char* rest;
    if (!strncmp(url, "http://", 7)) rest = url + 7;
    else if (!strncmp(url, "https://", 8)) rest = url + 8;
    else return fail("url must start with http:// or https://");
    if (!*rest || *rest == '/' || *rest == '?' || *rest == '#') return fail("url has no host");
    for (const unsigned char* p = (const unsigned char*)url; *p; ++p)
        if (*p <= 0x20 || *p == 0x7f) return fail("url must not contain spaces or control characters (use %20)");
    char bad[40];
    if (!validate_template(url, bad, sizeof bad)) return fail("unknown placeholder ${%s} (allowed: host_name, event_message, event_value, event_tag, event_flags, event_time, fw_version)", bad);
    return true;
}

void format_event_time(const NotifyEvent& ev, char* out, size_t cap) {
    if (ev.epoch_s >= 0) {
        time_t t = (time_t)ev.epoch_s; struct tm tmv; gmtime_r(&t, &tmv);
        snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    } else {
        snprintf(out, cap, "uptime+%lus", (unsigned long)(ev.uptime_ms / 1000));
    }
}

void format_event_value(float v, char* out, size_t cap) {
    if (!cap) return;
    if (!std::isfinite(v)) { out[0] = 0; return; }
    snprintf(out, cap, "%.2f", (double)v);
}

bool expand_template(const char* tpl, const NotifyEvent& ev, const char* host, char* out, size_t cap, bool url_encode) {
    if (!cap) return false;
    size_t n = 0; bool ok = true;
    auto put = [&](const char* s, size_t len) {
        if (!ok) return;
        if (n + len + 1 > cap) { ok = false; len = cap - 1 - n; }
        memcpy(out + n, s, len); n += len;
    };
    const char* p = tpl;
    const char* name; size_t len; const char* end;
    for (;;) {
        const char* ph = next_placeholder(p, name, len, end);
        if (!ph) { put(p, strlen(p)); break; }
        put(p, (size_t)(ph - p));
        if (!known_placeholder(name, len)) { ok = false; break; }
        char val[kExpandedMax / 2], enc[kExpandedMax];
        placeholder_value(name, len, ev, host ? host : "", val, sizeof val);
        if (url_encode) {
            if (!urlencode(val, enc, sizeof enc)) { ok = false; break; }
            put(enc, strlen(enc));
        } else {
            put(val, strlen(val));
        }
        if (!ok) break;
        p = end;
    }
    out[n] = 0;
    return ok;
}

void url_origin(const char* url, char* out, size_t cap, bool with_scheme) {
    if (!cap) return;
    out[0] = 0;
    const char* rest; const char* scheme;
    if (!strncmp(url, "http://", 7)) { rest = url + 7; scheme = "http://"; }
    else if (!strncmp(url, "https://", 8)) { rest = url + 8; scheme = "https://"; }
    else return;
    const char* e = rest;
    while (*e && *e != '/' && *e != '?' && *e != '#') ++e;      // конец authority
    const char* at = nullptr;
    for (const char* q = rest; q < e; ++q) if (*q == '@') at = q;
    if (at) rest = at + 1;                                       // user:pass@ — секрет, не показываем
    snprintf(out, cap, "%s%.*s", with_scheme ? scheme : "", (int)(e - rest), rest);
}

// ---- настройки --------------------------------------------------------------------------------------------
const char* const kWebhookKeys[] = {"url", "method", "body", "ctype", "err", "tmo", "min", "insecure", "en"};

namespace {

bool parse_bool(const char* v, bool& b) {
    if (!strcasecmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "true") || !strcasecmp(v, "yes")) { b = true; return true; }
    if (!strcasecmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "false") || !strcasecmp(v, "no")) { b = false; return true; }
    return false;
}

bool has_ctrl(const char* s) {
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) if (*p < 0x20 || *p == 0x7f) return true;
    return false;
}

}  // namespace

bool webhook_set(WebhookCfg& cfg, const char* key, const char* val, char* err, size_t err_cap) {
    auto fail = [&](const char* msg, const char* arg = nullptr) {
        if (err && err_cap) { if (arg) snprintf(err, err_cap, msg, arg); else notify_copy(err, err_cap, msg); }
        return false;
    };
    if (err && err_cap) err[0] = 0;
    if (!key || !val) return fail("bad argument");
    if (!strcmp(key, "url")) {
        if (!*val) { cfg.url[0] = 0; cfg.enabled = false; return true; }     // без URL слать нечего
        if (!validate_url(val, err, err_cap)) return false;
        notify_copy(cfg.url, sizeof cfg.url, val);
        return true;
    }
    if (!strcmp(key, "method")) {
        if (!validate_method(val)) return fail("method must be GET or POST");
        notify_copy(cfg.method, sizeof cfg.method, !strcasecmp(val, "POST") ? "POST" : "GET");
        return true;
    }
    if (!strcmp(key, "body")) {
        if (strlen(val) > kBodyMax) return fail("body too long (max 400)");
        char bad[40];
        if (!validate_template(val, bad, sizeof bad)) return fail("unknown placeholder ${%s} (allowed: host_name, event_message, event_value, event_tag, event_flags, event_time, fw_version)", bad);
        notify_copy(cfg.body, sizeof cfg.body, val);
        return true;
    }
    if (!strcmp(key, "ctype")) {
        if (strlen(val) > kCtypeMax) return fail("content type too long (max 64)");
        if (has_ctrl(val)) return fail("content type must not contain control characters");
        notify_copy(cfg.ctype, sizeof cfg.ctype, *val ? val : kDefaultCtype);
        return true;
    }
    if (!strcmp(key, "err")) {
        if (strlen(val) > kErrTextMax) return fail("error text too long (max 48)");
        if (has_ctrl(val)) return fail("error text must not contain control characters");
        notify_copy(cfg.err, sizeof cfg.err, val);
        return true;
    }
    if (!strcmp(key, "tmo")) {
        char* e = nullptr; long v = strtol(val, &e, 10);
        if (!*val || !e || *e || v < 1 || v > 60) return fail("timeout must be 1..60 seconds");
        cfg.timeout_s = (uint8_t)v;
        return true;
    }
    if (!strcmp(key, "min")) {
        if (!strcasecmp(val, "info") || !strcmp(val, "0")) cfg.min_sev = kSevInfo;
        else if (!strcasecmp(val, "warn") || !strcmp(val, "1")) cfg.min_sev = kSevWarn;
        else if (!strcasecmp(val, "alert") || !strcmp(val, "2")) cfg.min_sev = kSevAlert;
        else return fail("min must be info, warn or alert (0..2)");
        return true;
    }
    if (!strcmp(key, "insecure")) {
        bool b; if (!parse_bool(val, b)) return fail("insecure must be 0/1 or on/off");
        cfg.insecure = b;
        return true;
    }
    if (!strcmp(key, "en")) {
        bool b; if (!parse_bool(val, b)) return fail("en must be 0/1 or on/off");
        if (b && !cfg.url[0]) return fail("set url first");
        cfg.enabled = b;
        return true;
    }
    return fail("unknown key '%s'", key);
}

bool webhook_get(const WebhookCfg& c, const char* key, char* out, size_t cap) {
    if (!cap) return false;
    if (!strcmp(key, "url")) notify_copy(out, cap, c.url);
    else if (!strcmp(key, "method")) notify_copy(out, cap, c.method);
    else if (!strcmp(key, "body")) notify_copy(out, cap, c.body);
    else if (!strcmp(key, "ctype")) notify_copy(out, cap, c.ctype);
    else if (!strcmp(key, "err")) notify_copy(out, cap, c.err);
    else if (!strcmp(key, "tmo")) snprintf(out, cap, "%u", (unsigned)c.timeout_s);
    else if (!strcmp(key, "min")) notify_copy(out, cap, notify_sev_name(c.min_sev));
    else if (!strcmp(key, "insecure")) notify_copy(out, cap, c.insecure ? "1" : "0");
    else if (!strcmp(key, "en")) notify_copy(out, cap, c.enabled ? "1" : "0");
    else { out[0] = 0; return false; }
    return true;
}

// ---- очередь ----------------------------------------------------------------------------------------------
namespace {
uint32_t fnv1a(const char* tag, const char* msg) {
    uint32_t h = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)tag; *p; ++p) { h ^= *p; h *= 16777619u; }
    h ^= 0; h *= 16777619u;                       // разделитель tag/message
    for (const unsigned char* p = (const unsigned char*)msg; *p; ++p) { h ^= *p; h *= 16777619u; }
    return h;
}
}  // namespace

void NotifyQueue::clear() {
    head_ = n_ = 0; retry_ = inflight_ = blocked_ = false; attempts_ = 0; att_n_ = att_head_ = 0;
    for (auto& s : seen_) s.used = false;
}

bool NotifyQueue::push(NotifyEvent ev, uint32_t now, bool force) {
    if (!force) {
        if (ev.sev < pol_.min_sev) { ++st_.filtered; return false; }
        if (pol_.dedup_ms) {
            const uint32_t key = fnv1a(ev.tag, ev.message);
            Seen* slot = nullptr; Seen* oldest = &seen_[0];
            for (auto& s : seen_) {
                if (s.used && (uint32_t)(now - s.at) >= pol_.dedup_ms) s.used = false;     // окно истекло
                if (s.used && s.key == key) { ++st_.suppressed; return false; }
                if (!s.used && !slot) slot = &s;
                if (s.used && (uint32_t)(now - s.at) > (uint32_t)(now - oldest->at)) oldest = &s;
            }
            if (!slot) slot = oldest;
            slot->used = true; slot->key = key; slot->at = now;
        }
    }
    ev.id = next_id_++;
    if (n_ == kNotifyQueueCap) { head_ = (head_ + 1) % kNotifyQueueCap; --n_; ++st_.dropped; }   // вытесняем самое старое
    q_[(head_ + n_) % kNotifyQueueCap] = ev;
    ++n_;
    return true;
}

bool NotifyQueue::rate_ok(uint32_t now) {
    const size_t cap = sizeof att_ / sizeof att_[0];
    const uint32_t max = pol_.rate_max < cap ? pol_.rate_max : (uint32_t)cap;
    if (!max) return false;
    uint32_t recent = 0;
    for (size_t i = 0; i < att_n_; ++i) if ((uint32_t)(now - att_[(att_head_ + cap - 1 - i) % cap]) < pol_.rate_window_ms) ++recent;
    return recent < max;
}

bool NotifyQueue::take(uint32_t now, NotifyEvent& out) {
    if (inflight_) return false;
    if (blocked_) { if ((int32_t)(now - blocked_until_) < 0) return false; blocked_ = false; }
    if (retry_) { if ((int32_t)(now - retry_at_) < 0) return false; }
    else if (!n_) return false;
    if (!rate_ok(now)) return false;
    if (retry_) retry_ = false;
    else { cur_ = q_[head_]; head_ = (head_ + 1) % kNotifyQueueCap; --n_; attempts_ = 0; }
    ++attempts_; inflight_ = true;
    const size_t cap = sizeof att_ / sizeof att_[0];
    att_[att_head_] = now; att_head_ = (att_head_ + 1) % cap; if (att_n_ < cap) ++att_n_;
    out = cur_;
    return true;
}

void NotifyQueue::done(bool ok, uint32_t now, int http, const char* err) {
    if (!inflight_) return;
    inflight_ = false;
    st_.have_last = true; st_.last_ok = ok; st_.last_http = http; st_.last_ms = now;
    if (ok) { st_.last_error[0] = 0; ++st_.sent; attempts_ = 0; return; }
    notify_copy(st_.last_error, sizeof st_.last_error, err && *err ? err : "send failed");
    if (attempts_ >= pol_.max_attempts) {          // сдаёмся: событие отбрасываем, endpoint не долбим
        ++st_.failed; attempts_ = 0;
        blocked_ = true; blocked_until_ = now + pol_.backoff_ms[2];
    } else {
        retry_ = true; retry_at_ = now + pol_.backoff_ms[attempts_ - 1 < 2 ? attempts_ - 1 : 2];
    }
}

// ---- преобразование -----------------------------------------------------------------------------------------
NotifyEvent notify_from_trigger(const Trigger& t, uint32_t now_ms, int64_t epoch_now_s) {
    NotifyEvent ev;
    ev.sev = (uint8_t)trig_severity(t.kind);
    notify_copy(ev.tag, sizeof ev.tag, trig_tag(t.kind));
    notify_copy(ev.message, sizeof ev.message, t.text);
    ev.value = (t.kind == TrigKind::BmsFlagsChanged || t.kind == TrigKind::BmsStale) ? NAN : t.value;
    if (t.kind == TrigKind::BmsFlagsChanged) snprintf(ev.flags, sizeof ev.flags, "f%u mask=0x%02x", (unsigned)(t.aux >> 8), (unsigned)(t.aux & 0xFF));
    ev.uptime_ms = t.t_ms;
    if (epoch_now_s >= 0) {
        const int32_t age_s = (int32_t)(now_ms - t.t_ms) / 1000;
        ev.epoch_s = epoch_now_s - (age_s > 0 ? age_s : 0);
    }
    return ev;
}

const char* notify_reason_name(Reason r) {
    switch (r) {
        case Reason::HardFloor: return "HARD_FLOOR";
        case Reason::SoftLow: return "SOFT_LOW";
        case Reason::NoData: return "NO_DATA";
        case Reason::Charged: return "CHARGED";
        case Reason::SafeMode: return "SAFE_MODE";
        case Reason::Manual: return "MANUAL";
        default: return "NONE";
    }
}

namespace {
// Наблюдаемые флаги. Теги <= 15 символов (NotifyEvent::tag[16]); выход из состояния — info с тегом ok_tag.
struct FlagDef { uint32_t bit; const char* tag; const char* ok_tag; uint8_t sev; const char* msg; const char* ok_msg; };
const FlagDef kFlagDefs[] = {
    {F_HARD_LOW, "HARD_LOW", "HARD_LOW_OK", kSevAlert, "supervisor: pack voltage at/below hard floor", "supervisor: pack voltage above hard floor"},
    {F_CELL_HARD, "CELL_HARD", "CELL_HARD_OK", kSevAlert, "supervisor: weakest cell at/below cell hard floor", "supervisor: weakest cell above cell hard floor"},
    {F_NO_DATA, "NO_DATA", "NO_DATA_OK", kSevAlert, "supervisor: no valid inverter data", "supervisor: inverter data is back"},
    {F_GRID_LOST_LOW, "GRID_LOST_LOW", "GRID_LOST_OK", kSevAlert, "supervisor: grid lost while battery is low", "supervisor: grid-lost-while-low cleared"},
    {F_SAFE_MODE, "SAFE_MODE", "SAFE_MODE_OK", kSevAlert, "supervisor: SAFE MODE (reboot loop), grid forced on", "supervisor: left safe mode"},
    {F_CELL_BLOCK, "CELL_BLOCK", "CELL_BLOCK_OK", kSevWarn, "supervisor: grid disconnect blocked by cell guard", "supervisor: cell guard no longer blocks disconnect"},
    {F_RELAY_MISMATCH, "RELAY_MISMATCH", "RELAY_MISM_OK", kSevWarn, "supervisor: register 20105 and grid relay disagree", "supervisor: register 20105 and grid relay agree"},
    {F_CELL_LOW, "CELL_LOW", "CELL_LOW_OK", kSevWarn, "supervisor: weakest cell at/below cell soft threshold", "supervisor: weakest cell above cell soft threshold"},
    {F_SOFT_LOW, "SOFT_LOW", "SOFT_LOW_OK", kSevWarn, "supervisor: pack voltage at/below soft threshold", "supervisor: pack voltage above soft threshold"},
};
}  // namespace

void flags_text(uint32_t flags, char* out, size_t cap) {
    static const struct { uint32_t bit; const char* name; } all[] = {
        {F_HARD_LOW, "HARD_LOW"}, {F_SOFT_LOW, "SOFT_LOW"}, {F_NO_DATA, "NO_DATA"}, {F_STALE, "STALE"}, {F_DATA_DOUBTFUL, "DATA_DOUBTFUL"},
        {F_MANUAL_PAUSE, "MANUAL_PAUSE"}, {F_MANUAL_OVERRIDE, "MANUAL_OVERRIDE"}, {F_GRID_LOST_LOW, "GRID_LOST_LOW"}, {F_SAFE_MODE, "SAFE_MODE"},
        {F_CMD_PENDING, "CMD_PENDING"}, {F_CMD_RETRY, "CMD_RETRY"}, {F_OFF_COND, "OFF_COND"}, {F_HOLD, "HOLD"}, {F_SINGLE_SRC, "SINGLE_SRC"},
        {F_TIME_JUMP, "TIME_JUMP"}, {F_LOAD_UNKNOWN, "LOAD_UNKNOWN"}, {F_RELAY_MISMATCH, "RELAY_MISMATCH"}, {F_CELL_HARD, "CELL_HARD"},
        {F_CELL_LOW, "CELL_LOW"}, {F_CELL_BLOCK, "CELL_BLOCK"}};
    if (!cap) return;
    size_t n = 0; out[0] = 0;
    for (auto& f : all) {
        if (!(flags & f.bit)) continue;
        const size_t l = strlen(f.name);
        if (n + l + (n ? 1 : 0) + 1 > cap) break;
        if (n) out[n++] = '|';
        memcpy(out + n, f.name, l); n += l; out[n] = 0;
    }
}

size_t SupervisorNotifier::on_flags(uint32_t flags, float v, uint32_t uptime_ms, int64_t epoch_s, NotifyEvent* out, size_t cap) {
    size_t n = 0;
    for (const FlagDef& d : kFlagDefs) {
        const bool now = (flags & d.bit) != 0, was = (prev_ & d.bit) != 0;
        if (now == was) continue;
        if (n >= cap) break;                          // не влезло: prev_ не трогаем, событие уйдёт со следующим вызовом
        NotifyEvent& ev = out[n++];
        ev = NotifyEvent();
        ev.sev = now ? d.sev : (uint8_t)kSevInfo;
        notify_copy(ev.tag, sizeof ev.tag, now ? d.tag : d.ok_tag);
        notify_copy(ev.message, sizeof ev.message, now ? d.msg : d.ok_msg);
        ev.value = v;
        flags_text(flags, ev.flags, sizeof ev.flags);
        ev.epoch_s = epoch_s; ev.uptime_ms = uptime_ms;
        prev_ = now ? (prev_ | d.bit) : (prev_ & ~d.bit);
    }
    return n;
}

bool SupervisorNotifier::on_command(Cmd cmd, Reason r, bool ok, float v, uint32_t uptime_ms, int64_t epoch_s, NotifyEvent& out) {
    if (cmd != Cmd::Connect && cmd != Cmd::Disconnect) return false;
    out = NotifyEvent();
    const char* name = cmd == Cmd::Connect ? "CONNECT" : "DISCONNECT";
    out.sev = (r == Reason::HardFloor || r == Reason::NoData || r == Reason::SafeMode) ? kSevAlert : kSevWarn;
    notify_copy(out.tag, sizeof out.tag, name);
    char vs[24] = "";
    if (std::isfinite(v)) snprintf(vs, sizeof vs, ", %.1f V", (double)v);
    snprintf(out.message, sizeof out.message, "grid %s: reason=%s%s%s", name, notify_reason_name(r), vs, ok ? "" : ", bus write FAILED");
    out.value = v;
    out.epoch_s = epoch_s; out.uptime_ms = uptime_ms;
    return true;
}

NotifyEvent SupervisorNotifier::boot(const char* ver, const char* reset_reason, uint32_t uptime_ms, int64_t epoch_s) {
    NotifyEvent ev;
    ev.sev = kSevInfo;
    notify_copy(ev.tag, sizeof ev.tag, "BOOT");
    snprintf(ev.message, sizeof ev.message, "boot ver=%s reset=%s", ver && *ver ? ver : "?", reset_reason && *reset_reason ? reset_reason : "?");
    ev.epoch_s = epoch_s; ev.uptime_ms = uptime_ms;
    return ev;
}

}  // namespace gs
