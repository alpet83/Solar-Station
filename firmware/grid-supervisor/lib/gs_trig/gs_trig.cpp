#include "gs_trig.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace gs {

const char* trig_tag(TrigKind k) {
    switch (k) {
        case TrigKind::CellLowEnter: return "CELL_LOW";
        case TrigKind::CellLowExit: return "CELL_LOW_OK";
        case TrigKind::CellHardEnter: return "CELL_HARD";
        case TrigKind::CellHardExit: return "CELL_HARD_OK";
        case TrigKind::CellDeltaHigh: return "CELL_DELTA";
        case TrigKind::CellDeltaOk: return "CELL_DELTA_OK";
        case TrigKind::BmsFlagsChanged: return "BMS_FLAGS";
        case TrigKind::BmsStale: return "BMS_STALE";
        case TrigKind::BmsBack: return "BMS_BACK";
        default: return "UNKNOWN";
    }
}

TrigSeverity trig_severity(TrigKind k) {
    switch (k) {
        case TrigKind::CellHardEnter: return TrigSeverity::Alert;
        case TrigKind::CellLowEnter: case TrigKind::CellDeltaHigh: case TrigKind::BmsFlagsChanged: case TrigKind::BmsStale:
            return TrigSeverity::Warn;
        default: return TrigSeverity::Info;
    }
}

const char* trig_severity_name(TrigSeverity s) {
    return s == TrigSeverity::Alert ? "alert" : s == TrigSeverity::Warn ? "warn" : "info";
}

// ---- очередь ------------------------------------------------------------------------------------
uint32_t TriggerQueue::push(Trigger t) {
    t.id = next_id_++;
    if (n_ == kTrigQueueCap) { head_ = (head_ + 1) % kTrigQueueCap; --n_; ++dropped_; }   // вытесняем самый старый
    q_[(head_ + n_) % kTrigQueueCap] = t;
    ++n_;
    return t.id;
}

bool TriggerQueue::pop(Trigger& out) {
    if (!n_) return false;
    out = q_[head_];
    head_ = (head_ + 1) % kTrigQueueCap;
    --n_;
    return true;
}

// ---- журнал ---------------------------------------------------------------------------------------
void BmsEventLog::add(const Trigger& t, int64_t epoch_s) {
    BmsEvent& e = e_[head_];
    e.id = t.id; e.kind = t.kind; e.t_ms = t.t_ms; e.epoch_s = epoch_s; e.tag = trig_tag(t.kind); e.value = t.value;
    memcpy(e.text, t.text, sizeof e.text); e.text[sizeof e.text - 1] = 0;
    head_ = (head_ + 1) % kBmsLogCap;
    if (n_ < kBmsLogCap) ++n_;
    ++total_;
}

const BmsEvent& BmsEventLog::at(size_t i) const {
    if (i >= n_) i = n_ ? n_ - 1 : 0;
    return e_[(head_ + kBmsLogCap - 1 - i) % kBmsLogCap];
}

size_t bms_events_to_json(const BmsEventLog& log, uint32_t now_ms, char* out, size_t cap) {
    size_t n = 0; bool ok = true;
    auto put = [&](const char* fmt, auto... a) {
        if (!ok) return;
        if (n >= cap) { ok = false; return; }
        int w = snprintf(out + n, cap - n, fmt, a...);
        if (w < 0 || (size_t)w >= cap - n) { ok = false; return; }
        n += (size_t)w;
    };
    put("{\"total\":%lu,\"now_ms\":%lu,\"events\":[", (unsigned long)log.total(), (unsigned long)now_ms);
    for (size_t i = 0; ok && i < log.size(); ++i) {
        const BmsEvent& e = log.at(i);
        char ts[28] = "null";
        if (e.epoch_s >= 0) {
            time_t t = (time_t)e.epoch_s; struct tm tmv; gmtime_r(&t, &tmv);
            snprintf(ts, sizeof ts, "\"%04d-%02d-%02d %02d:%02d:%02d\"", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        }
        char esc[100]; size_t k = 0;      // текст: экранируем кавычки/обратную косую, управляющие символы — пробелом
        for (const char* p = e.text; *p && k + 2 < sizeof esc; ++p) {
            if (*p == '"' || *p == '\\') esc[k++] = '\\';
            esc[k++] = (unsigned char)*p < 0x20 ? ' ' : *p;
        }
        esc[k] = 0;
        const uint32_t age = (int32_t)(now_ms - e.t_ms) >= 0 ? (uint32_t)(now_ms - e.t_ms) / 1000 : 0;
        char val[24];
        if (std::isfinite(e.value)) snprintf(val, sizeof val, "%.0f", (double)e.value); else snprintf(val, sizeof val, "null");
        put("%s{\"id\":%lu,\"t_ms\":%lu,\"age_s\":%lu,\"ts\":%s,\"tag\":\"%s\",\"sev\":\"%s\",\"value\":%s,\"text\":\"%s\"}",
            i ? "," : "", (unsigned long)e.id, (unsigned long)e.t_ms, (unsigned long)age, ts, e.tag, trig_severity_name(trig_severity(e.kind)), val, esc);
    }
    put("%s", "]}");
    if (!ok) { if (cap) out[0] = 0; return 0; }
    return n;
}

// ---- снимок из декодеров ----------------------------------------------------------------------------------
WatchCfg make_watch_cfg(const Config& cfg) {
    WatchCfg w;
    w.cell_warn_mv = (uint16_t)std::lround(cfg.cell_soft_on_v * 1000.0f);
    w.cell_hard_mv = (uint16_t)std::lround(cfg.cell_hard_floor_v * 1000.0f);
    return w;
}

namespace {
void set_field(BmsSnapshot& s, size_t slot, const char* name, const uint8_t* d, uint8_t len) {
    WatchField& f = s.watch[slot];
    strncpy(f.name, name, sizeof f.name - 1); f.name[sizeof f.name - 1] = 0;
    f.len = len; memcpy(f.data, d, len); f.valid = true; s.flags_valid = true;
}
}  // namespace

BmsSnapshot make_bms_snapshot(const PylonBattery& b, const SeplosBattery& s, const Seplos500Battery& s5, uint32_t now) {
    BmsSnapshot r;
    int mn, mx;
    if (s5.cell_range(now, &mn, &mx) && mn >= 0 && mx >= 0 && mn <= 0xFFFF && mx <= 0xFFFF) {
        r.cells_valid = true; r.cell_min_mv = (uint16_t)mn; r.cell_max_mv = (uint16_t)mx;
    }
    // 500 кбит/с
    if (can_fresh(s5.flags.have, s5.flags.t_ms, now)) set_field(r, 0, "f312", s5.flags.raw, 8);
    if (can_fresh(s5.unk320.have, s5.unk320.t_ms, now)) set_field(r, 1, "f320", s5.unk320.raw, 8);
    if (can_fresh(s5.mm.have, s5.mm.t_ms, now)) { const uint8_t d[3] = {s5.mm.b0, s5.mm.idx[0], s5.mm.idx[1]}; set_field(r, 2, "f319", d, 3); }
    if (can_fresh(s5.temps.have, s5.temps.t_ms, now)) set_field(r, 3, "f322", s5.temps.unk, 4);
    if (can_fresh(s5.limits.have, s5.limits.t_ms, now)) { const uint8_t d[2] = {(uint8_t)(s5.limits.unk >> 8), (uint8_t)s5.limits.unk}; set_field(r, 4, "f311", d, 2); }
    // 100 кбит/с
    if (can_fresh(s.prot.have, s.prot.t_ms, now)) set_field(r, 5, "a200", s.prot.raw, 8);
    // Pylon
    if (can_fresh(b.prot.have, b.prot.t_ms, now)) { const uint8_t d[4] = {b.prot.prot1, b.prot.prot2, b.prot.alarm1, b.prot.alarm2}; set_field(r, 6, "p359", d, 4); }
    if (can_fresh(b.req.have, b.req.t_ms, now)) set_field(r, 7, "p35c", &b.req.raw, 1);
    // кадры живы: свежий кадр у любого из декодеров
    const struct { bool have; uint32_t t; } g[] = {
        {b.limits.have, b.limits.t_ms}, {b.soc.have, b.soc.t_ms}, {b.pack.have, b.pack.t_ms}, {b.prot.have, b.prot.t_ms},
        {b.req.have, b.req.t_ms}, {b.mfr.have, b.mfr.t_ms}, {s.any(), s.any() ? s.last_ms() : 0}, {s5.any(), s5.any() ? s5.last_ms() : 0}};
    for (auto& x : g) if (can_fresh(x.have, x.t, now)) { r.frames_alive = true; break; }
    return r;
}

// ---- генератор ----------------------------------------------------------------------------------------
void BmsWatch::emit(TriggerQueue& q, BmsEventLog* log, int64_t epoch_s, Trigger t) {
    t.text[sizeof t.text - 1] = 0;
    t.id = q.push(t);
    if (log) log->add(t, epoch_s);
}

namespace {
// "f312 00 00 .. -> 00 00 .." — если не влезает в cap, то без пробелов между байтами; если и так не влезает — обрезка.
void fmt_change(char* out, size_t cap, const char* name, const uint8_t* o, const uint8_t* n, uint8_t len) {
    char a[32], b[32];
    auto hex = [&](char* dst, const uint8_t* d, bool sp) {
        size_t k = 0;
        for (uint8_t i = 0; i < len; ++i) k += (size_t)snprintf(dst + k, 4, "%s%02x", sp && i ? " " : "", d[i]);
    };
    const size_t full = strlen(name) + 1 + (len ? len * 3u - 1 : 0) * 2 + 4;
    const bool sp = full < cap;
    hex(a, o, sp); hex(b, n, sp);
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s %s%s%s", name, a, sp ? " -> " : "->", b);
    strncpy(out, tmp, cap - 1); out[cap - 1] = 0;   // обрезка по границе text[]
}
}  // namespace

void BmsWatch::update(const BmsSnapshot& s, uint32_t now, TriggerQueue& q, BmsEventLog* log, int64_t epoch_s) {
    // 1. жив ли поток кадров
    if (s.frames_alive) {
        if (stale_) {
            stale_ = false;
            for (auto& f : f_) f.base = false;                     // после паузы значения — заново baseline
            Trigger t; t.kind = TrigKind::BmsBack; t.t_ms = now; t.value = (float)((uint32_t)(now - stale_since_ms_) / 1000);
            snprintf(t.text, sizeof t.text, "BMS frames back after %lu s", (unsigned long)(t.value));
            emit(q, log, epoch_s, t);
        }
        ever_alive_ = true;
    } else {
        if (ever_alive_ && !stale_) {
            stale_ = true; stale_since_ms_ = now;
            for (auto& f : f_) f.base = false;
            Trigger t; t.kind = TrigKind::BmsStale; t.t_ms = now; t.value = 0;
            snprintf(t.text, sizeof t.text, "no BMS frames for >%lu s", (unsigned long)(kCanStaleMs / 1000));
            emit(q, log, epoch_s, t);
        }
        return;       // без кадров сравнивать нечего
    }

    // 2. ячейки: переходы с гистерезисом. Состояние переживает пропажу данных (не сбрасываем «в норму» молча).
    if (s.cells_valid) {
        const uint16_t mn = s.cell_min_mv, mx = s.cell_max_mv;
        const uint16_t delta = mx >= mn ? (uint16_t)(mx - mn) : 0;
        auto cell_trig = [&](TrigKind k, const char* what, uint16_t thr) {
            Trigger t; t.kind = k; t.t_ms = now; t.value = (float)mn; t.aux = mx;
            snprintf(t.text, sizeof t.text, "%s: min %u mV (thr %u), max %u", what, (unsigned)mn, (unsigned)thr, (unsigned)mx);
            emit(q, log, epoch_s, t);
        };
        if (!low_ && mn <= cfg_.cell_warn_mv) { low_ = true; cell_trig(TrigKind::CellLowEnter, "cell low", cfg_.cell_warn_mv); }
        else if (low_ && mn >= (uint32_t)cfg_.cell_warn_mv + cfg_.hyst_mv) { low_ = false; cell_trig(TrigKind::CellLowExit, "cell low ok", cfg_.cell_warn_mv); }
        if (!hard_ && mn <= cfg_.cell_hard_mv) { hard_ = true; cell_trig(TrigKind::CellHardEnter, "cell HARD", cfg_.cell_hard_mv); }
        else if (hard_ && mn >= (uint32_t)cfg_.cell_hard_mv + cfg_.hyst_mv) { hard_ = false; cell_trig(TrigKind::CellHardExit, "cell hard ok", cfg_.cell_hard_mv); }
        if (!delta_hi_ && delta > cfg_.delta_hi_mv) {
            delta_hi_ = true;
            Trigger t; t.kind = TrigKind::CellDeltaHigh; t.t_ms = now; t.value = (float)delta; t.aux = mx;
            snprintf(t.text, sizeof t.text, "cell delta %u mV (min %u, max %u)", (unsigned)delta, (unsigned)mn, (unsigned)mx);
            emit(q, log, epoch_s, t);
        } else if (delta_hi_ && delta < cfg_.delta_ok_mv) {
            delta_hi_ = false;
            Trigger t; t.kind = TrigKind::CellDeltaOk; t.t_ms = now; t.value = (float)delta; t.aux = mx;
            snprintf(t.text, sizeof t.text, "cell delta ok %u mV (min %u, max %u)", (unsigned)delta, (unsigned)mn, (unsigned)mx);
            emit(q, log, epoch_s, t);
        }
    }

    // 3. «знаковые» сырые поля: изменение значения; не чаще раза в debounce_ms на поле
    for (size_t i = 0; i < kWatchMax; ++i) {
        const WatchField& w = s.watch[i];
        Field& f = f_[i];
        if (!w.valid || w.len == 0 || w.len > 8) { f.base = false; continue; }     // данные пропали: при возвращении — baseline
        if (!f.base || f.len != w.len) {
            f.base = true; f.emitted = false; f.len = w.len;
            memcpy(f.sent, w.data, w.len); memcpy(f.cur, w.data, w.len);
            continue;
        }
        memcpy(f.cur, w.data, w.len);
        if (memcmp(f.cur, f.sent, f.len) == 0) continue;                        // нет отличий от последнего отправленного (в т.ч. «туда-обратно»)
        if (f.emitted && (uint32_t)(now - f.last_emit_ms) < cfg_.debounce_ms) continue;   // слияние: ждём конца окна, шлём актуальное
        Trigger t; t.kind = TrigKind::BmsFlagsChanged; t.t_ms = now;
        uint32_t mask = 0; unsigned cnt = 0;
        for (uint8_t k = 0; k < f.len; ++k) if (f.cur[k] != f.sent[k]) { mask |= 1u << k; ++cnt; }
        t.value = (float)cnt; t.aux = ((uint32_t)i << 8) | mask;
        fmt_change(t.text, sizeof t.text, w.name, f.sent, f.cur, f.len);
        memcpy(f.sent, f.cur, f.len); f.emitted = true; f.last_emit_ms = now;
        emit(q, log, epoch_s, t);
    }
}

}  // namespace gs
