#include "gs_stats.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace gs {

static float fin(float v) { return std::isfinite(v) ? v : 0.0f; }

PvDay& PvStats::cur() { return days_[(n_days_ - 1) % kPvDays]; }

void PvStats::push_day(uint32_t day) {
    PvDay& d = days_[n_days_ % kPvDays];
    d = PvDay(); d.used = true; d.day = day;
    ++n_days_;
}

void PvStats::add_event(const PvEvent& e) { ev_[n_ev_ % kPvEvents] = e; ++n_ev_; }

bool PvStats::event(size_t i, PvEvent& out) const {
    if (i >= event_count()) return false;
    out = ev_[(n_ev_ - 1 - i) % kPvEvents];
    return true;
}

bool PvStats::day(size_t i, PvDay& out) const {
    if (i >= day_count()) return false;
    out = days_[(n_days_ - 1 - i) % kPvDays];
    return true;
}

uint32_t PvStats::run_s(uint32_t now_ms) const {
    return have_ ? (uint32_t)((uint32_t)(now_ms - since_ms_) / 1000u) : 0;
}

void PvStats::feed(const PvSample& s) {
    const float pw = fin(s.chg_w), pv = fin(s.pv_v), ca = fin(s.chg_a), bv = fin(s.pack_v);
    const bool on = (s.pv_relay >= 0) ? (s.pv_relay == 1) : (pw >= cfg_.min_w_on);
    const int64_t ep = s.epoch_s > 0 ? s.epoch_s : 0;
    const uint32_t day = ep > 0 ? (uint32_t)(ep / 86400) : 0;
    ++total_;

    if (n_days_ == 0 || cur().day != day) push_day(day);   // смена суток (или первое время после синхронизации)
    PvDay& d = cur();
    ++d.samples;

    // энергия заряда: прямоугольники по предыдущему интервалу, ограниченные max_dt_s
    if (have_ && s.t_ms > last_ms_) {
        uint32_t dt = (s.t_ms - last_ms_) / 1000u;
        if (dt > cfg_.max_dt_s) dt = cfg_.max_dt_s;
        d.chg_wh += pw * (float)dt / 3600.0f;
    }
    last_ms_ = s.t_ms;
    if (pv > d.max_pv_v) d.max_pv_v = pv;
    if (pw > d.max_chg_w) d.max_chg_w = pw;

    if (!have_) {                       // базовое состояние: событие не создаём
        have_ = true; on_ = on; stable_ = false; since_ms_ = s.t_ms; since_epoch_ = ep;
        run_pv_v_ = pv; run_chg_w_ = pw;
        return;
    }

    if (on && !on_) {                   // включилась
        on_ = true; stable_ = false; since_ms_ = s.t_ms; since_epoch_ = ep; run_pv_v_ = pv; run_chg_w_ = pw;
        ++d.on_edges;
        if (d.first_on == 0) d.first_on = ep;
        PvEvent e; e.epoch_s = ep; e.t_ms = s.t_ms; e.on = 1; e.pv_v = pv; e.chg_a = ca; e.chg_w = pw; e.pack_v = bv;
        add_event(e);
    } else if (!on && on_) {            // выключилась
        const uint32_t run = (uint32_t)(s.t_ms - since_ms_) / 1000u;
        if (!stable_ && run >= cfg_.stable_s) {          // длительность дотянула до порога между двумя выборками
            stable_ = true;
            if (d.stable_start == 0) { d.stable_start = since_epoch_; d.start_pv_v = run_pv_v_; d.start_chg_w = run_chg_w_; }
        }
        if (!stable_) ++d.short_runs; else d.stable_end = ep;
        d.last_off = ep;
        on_ = false; stable_ = false; since_ms_ = s.t_ms; since_epoch_ = ep;
        PvEvent e; e.epoch_s = ep; e.t_ms = s.t_ms; e.on = 0; e.run_s = run; e.pv_v = pv; e.chg_a = ca; e.chg_w = pw; e.pack_v = bv;
        add_event(e);
    }

    if (on_) {                          // включение стало «устойчивым»
        const uint32_t run = (uint32_t)(s.t_ms - since_ms_) / 1000u;
        if (!stable_ && run >= cfg_.stable_s) {
            stable_ = true;
            if (d.stable_start == 0) { d.stable_start = since_epoch_; d.start_pv_v = run_pv_v_; d.start_chg_w = run_chg_w_; }
        }
        if (stable_) d.stable_end = ep;
    }
}

namespace {
struct Out {
    char* b; size_t cap, n; bool ok;
    Out(char* buf, size_t c) : b(buf), cap(c), n(0), ok(true) {}
    void f(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (!ok) return;
        va_list ap; va_start(ap, fmt);
        int r = vsnprintf(b + n, cap - n, fmt, ap);
        va_end(ap);
        if (r < 0 || (size_t)r >= cap - n) ok = false; else n += (size_t)r;
    }
};
}  // namespace

size_t PvStats::to_json(uint32_t now_ms, char* buf, size_t cap) const {
    if (!buf || cap < 64) return 0;
    Out o(buf, cap);
    o.f("{\"cfg\":{\"stable_s\":%lu,\"min_w_on\":%.0f},\"now\":{\"known\":%s,\"pv_on\":%d,\"run_s\":%lu,\"samples\":%lu},\"days\":[",
        (unsigned long)cfg_.stable_s, (double)cfg_.min_w_on, have_ ? "true" : "false", pv_on() ? 1 : 0,
        (unsigned long)run_s(now_ms), (unsigned long)total_);
    for (size_t i = 0; i < day_count(); ++i) {
        PvDay d; day(i, d);
        o.f("%s{\"day\":%lu,\"on_edges\":%u,\"short_runs\":%u,\"first_on\":%lld,\"last_off\":%lld,\"stable_start\":%lld,\"stable_end\":%lld,"
            "\"start_pv_v\":%.1f,\"start_chg_w\":%.0f,\"max_pv_v\":%.1f,\"max_chg_w\":%.0f,\"chg_wh\":%.1f,\"samples\":%lu}",
            i ? "," : "", (unsigned long)d.day, (unsigned)d.on_edges, (unsigned)d.short_runs, (long long)d.first_on, (long long)d.last_off,
            (long long)d.stable_start, (long long)d.stable_end, (double)d.start_pv_v, (double)d.start_chg_w, (double)d.max_pv_v,
            (double)d.max_chg_w, (double)d.chg_wh, (unsigned long)d.samples);
    }
    o.f("],\"events\":[");
    for (size_t i = 0; i < event_count(); ++i) {
        PvEvent e; event(i, e);
        o.f("%s{\"t\":%lld,\"up_ms\":%lu,\"on\":%u,\"run_s\":%lu,\"pv_v\":%.1f,\"chg_a\":%.1f,\"chg_w\":%.0f,\"pack_v\":%.1f}",
            i ? "," : "", (long long)e.epoch_s, (unsigned long)e.t_ms, (unsigned)e.on, (unsigned long)e.run_s, (double)e.pv_v, (double)e.chg_a,
            (double)e.chg_w, (double)e.pack_v);
    }
    o.f("]}");
    return o.ok ? o.n : 0;
}

}  // namespace gs
