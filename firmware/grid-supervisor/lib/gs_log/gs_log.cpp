#include "gs_log.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace gs {

// Строка в буфере: "<day>|<текст>\n"
void BatchLog::drop_first_line() {
    char* nl = (char*)memchr(buf_, '\n', len_);
    size_t n = nl ? (size_t)(nl - buf_) + 1 : len_;
    memmove(buf_, buf_ + n, len_ - n);
    len_ -= n;
    ++st_.dropped;
}

bool BatchLog::add(uint32_t day, const char* line, uint32_t now_ms) {
    if (!line || cap_ < 64) return false;
    char tag[16];
    int tl = snprintf(tag, sizeof tag, "%lu|", (unsigned long)day);
    size_t ll = strlen(line);
    while (ll && (line[ll - 1] == '\n' || line[ll - 1] == '\r')) --ll;
    const size_t need = (size_t)tl + ll + 1;
    if (need > cap_ / 2) { ++st_.dropped; return false; }
    while (len_ + need > cap_) drop_first_line();
    if (len_ == 0) first_ms_ = now_ms;
    memcpy(buf_ + len_, tag, (size_t)tl); len_ += (size_t)tl;
    memcpy(buf_ + len_, line, ll); len_ += ll;
    buf_[len_++] = '\n';
    ++st_.lines;
    return true;
}

static uint32_t parse_day(const char* p, const char* end, const char** after) {
    uint32_t d = 0;
    while (p < end && *p >= '0' && *p <= '9') d = d * 10 + (uint32_t)(*p++ - '0');
    *after = (p < end && *p == '|') ? p + 1 : p;
    return d;
}

size_t BatchLog::take(uint32_t now_ms, bool force, char* out, size_t out_cap, uint32_t& day_out) {
    if (len_ == 0 || !out || out_cap == 0) return 0;
    const char* after;
    const uint32_t d0 = parse_day(buf_, buf_ + len_, &after);
    // граница порции: первая строка другого дня (если есть), иначе весь буфер
    size_t pos = 0, blk_end = len_, body_total = 0; bool later_day = false;
    while (pos < len_) {
        const char* nl = (const char*)memchr(buf_ + pos, '\n', len_ - pos);
        size_t le = nl ? (size_t)(nl - buf_) + 1 : len_;
        const char* a; uint32_t d = parse_day(buf_ + pos, buf_ + le, &a);
        if (d != d0) { blk_end = pos; later_day = true; break; }
        body_total += le - pos - (size_t)(a - (buf_ + pos));      // считаем только данные, без служебных меток
        pos = le;
    }
    const bool due = force || later_day || body_total >= cfg_.flush_bytes || (uint32_t)(now_ms - first_ms_) >= cfg_.flush_ms;
    if (!due) return 0;
    // копируем строки порции без меток, не больше out_cap (целыми строками)
    size_t o = 0, p = 0;
    while (p < blk_end) {
        const char* nl = (const char*)memchr(buf_ + p, '\n', blk_end - p);
        size_t le = nl ? (size_t)(nl - buf_) + 1 : blk_end;
        const char* a; parse_day(buf_ + p, buf_ + le, &a);
        size_t tl = (size_t)(a - (buf_ + p));
        size_t body = le - p - tl;
        if (o + body > out_cap) break;
        memcpy(out + o, a, body); o += body;
        p = le;
    }
    if (o == 0) return 0;
    memmove(buf_, buf_ + p, len_ - p);
    len_ -= p;
    first_ms_ = now_ms;
    day_out = d0;
    return o;
}

void BatchLog::note_flush(bool ok, size_t bytes, size_t lines) {
    if (ok) { ++st_.flushes; st_.bytes += (uint32_t)bytes; }
    else { ++st_.errors; st_.dropped += (uint32_t)lines; }
}

bool RateLimit::allow(uint32_t now_ms) {
    if (!init_) { init_ = true; last_ms_ = now_ms; }
    const uint32_t dt = now_ms - last_ms_;
    last_ms_ = now_ms;
    // токены в тысячных: за dt мс набегает dt*rate/3600 (остаток переносится, чтобы частые вызовы не теряли дробную часть)
    const uint64_t acc = (uint64_t)dt * rate_ + carry_;
    tokens_x1000_ += acc / 3600; carry_ = acc % 3600;
    if (tokens_x1000_ > burst_ * 1000ull) tokens_x1000_ = burst_ * 1000ull;
    if (tokens_x1000_ < 1000) return false;
    tokens_x1000_ -= 1000;
    return true;
}

// ---- имена ----
size_t day_path(uint32_t day, char* out, size_t cap) {
    if (!out || cap < 24) return 0;
    if (day == 0) return (size_t)snprintf(out, cap, "/m/nosync.csv");
    // civil_from_days (Howard Hinnant)
    int64_t z = (int64_t)day + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    return (size_t)snprintf(out, cap, "/m/%04d%02u%02u.csv", (int)y, m, d);
}

static bool is_day_name(const char* n) {
    if (strlen(n) != 12) return false;
    for (int i = 0; i < 8; ++i) if (n[i] < '0' || n[i] > '9') return false;
    return strcmp(n + 8, ".csv") == 0;
}

bool valid_log_name(const char* name) {
    if (!name || !*name) return false;
    return is_day_name(name) || !strcmp(name, "nosync.csv") || !strcmp(name, "events.log") || !strcmp(name, "events.old");
}

size_t select_old(const LogFile* files, size_t n, uint32_t max_days, uint32_t free_pct, uint32_t min_free_pct, size_t* out_idx, size_t out_cap) {
    // суточные файлы, отсортированные по имени (старые → новые) — индексы
    size_t idx[512]; size_t m = 0;
    for (size_t i = 0; i < n && m < 512; ++i) if (is_day_name(files[i].name)) idx[m++] = i;
    for (size_t a = 1; a < m; ++a) { size_t v = idx[a]; size_t b = a; while (b > 0 && strcmp(files[idx[b - 1]].name, files[v].name) > 0) { idx[b] = idx[b - 1]; --b; } idx[b] = v; }
    size_t keep = m;
    if (max_days > 0 && keep > max_days) keep = max_days;                 // по возрасту
    if (free_pct < min_free_pct && keep > 2) --keep;                       // мало места: ещё один самый старый (вызывается периодически)
    size_t cnt = 0;
    for (size_t k = 0; k < m - keep && cnt < out_cap; ++k) out_idx[cnt++] = idx[k];
    return cnt;
}

// ---- метрики ----
const char kMetricsHeader[] = "epoch,pack_v,alt_v,grid_v,load_w,chg_a,chg_w,pv_v,pv_relay,relay_grid,ongrid_reg,bms_v,bms_soc,cell_min_mv\n";

MetricsRow::MetricsRow() : pack_v(NAN), alt_v(NAN), grid_v(NAN), load_w(NAN), chg_a(NAN), chg_w(NAN), pv_v(NAN),
    pv_relay(-1), relay_grid(-1), ongrid_reg(-1), bms_v(NAN), bms_soc(NAN), cell_min_mv(-1) {}

namespace {
struct W {
    char* b; size_t cap, n; bool ok;
    void sep() { if (n) put(","); }
    void put(const char* s) { size_t l = strlen(s); if (n + l >= cap) { ok = false; return; } memcpy(b + n, s, l); n += l; b[n] = 0; }
    void f(float v, const char* fmt) { sep(); if (std::isfinite(v)) { char t[24]; snprintf(t, sizeof t, fmt, (double)v); put(t); } }
    void i(int v) { sep(); if (v >= 0) { char t[16]; snprintf(t, sizeof t, "%d", v); put(t); } }
};
}  // namespace

size_t format_metrics(int64_t epoch_s, const MetricsRow& r, char* out, size_t cap) {
    if (!out || cap < 16) return 0;
    out[0] = 0;
    W w{out, cap, 0, true};
    { char t[24]; snprintf(t, sizeof t, "%lld", (long long)(epoch_s > 0 ? epoch_s : 0)); w.put(t); }
    w.f(r.pack_v, "%.2f"); w.f(r.alt_v, "%.2f"); w.f(r.grid_v, "%.1f"); w.f(r.load_w, "%.0f");
    w.f(r.chg_a, "%.1f"); w.f(r.chg_w, "%.0f"); w.f(r.pv_v, "%.1f");
    w.i(r.pv_relay); w.i(r.relay_grid); w.i(r.ongrid_reg);
    w.f(r.bms_v, "%.2f"); w.f(r.bms_soc, "%.1f"); w.i(r.cell_min_mv);
    return w.ok ? w.n : 0;
}

}  // namespace gs
