#include "gs_bus.h"

#include <cmath>
#include <cstring>

#include "gs_modbus.h"

namespace gs {

static bool crc_good(const uint8_t* b, size_t n) {
    return n >= 4 && crc16(b, n - 2) == (uint16_t)(b[n - 2] | (b[n - 1] << 8));
}

static bool fill_request(const uint8_t* b, FrameRec& r) {
    if (!crc_good(b, 8)) return false;
    if (b[1] == 0x03 || b[1] == 0x04) r.kind = FrameKind::Request;
    else if (b[1] == 0x06) r.kind = FrameKind::WriteEcho;
    else return false;
    r.id = b[0]; r.fn = b[1];
    r.addr = (uint16_t)((b[2] << 8) | b[3]);
    r.count = (uint16_t)((b[4] << 8) | b[5]);
    r.crc_ok = true;
    return true;
}

FrameRec classify_frame(const uint8_t* b, size_t len) {
    FrameRec r;
    r.len = (uint16_t)len;
    if (len >= 5 && crc_good(b, len)) {
        r.crc_ok = true; r.id = b[0]; r.fn = b[1];
        if (len == 8 && fill_request(b, r)) return r;
        if (b[1] & 0x80) { r.kind = FrameKind::Exception; return r; }
        if ((b[1] == 0x03 || b[1] == 0x04) && len == 5u + b[2] && (b[2] % 2) == 0) {
            r.kind = FrameKind::Response; r.count = b[2] / 2; return r;
        }
        r.crc_ok = false;
    }
    // Склейка/мусор впереди: ищем валидный 8-байтный запрос внутри буфера.
    FrameRec q;
    for (size_t s = 0; s + 8 <= len; ++s) {
        if (fill_request(b + s, q) && q.kind == FrameKind::Request) { q.len = (uint16_t)len; return q; }
    }
    return r;   // Junk
}

bool BusSniffer::close(uint64_t now_ms) {
    (void)now_ms;
    if (!len_) return false;
    FrameRec r = classify_frame(buf_, len_);
    r.t_ms = (uint32_t)first_ms_;
    uint64_t d = last_byte_ms_ - first_ms_;
    r.dur_ms = d > 65535 ? 65535 : (uint16_t)d;
    if (r.kind == FrameKind::Junk) ++junk_;
    ring_[n_ % kRing] = r; ++n_;
    last_ = r;
    memcpy(last_buf_, buf_, len_);
    len_ = 0;
    return true;
}

bool BusSniffer::poll(uint64_t now_ms) {
    if (len_ && now_ms >= last_byte_ms_ && now_ms - last_byte_ms_ >= gap_ms_) return close(now_ms);
    return false;
}

bool BusSniffer::feed(const uint8_t* p, size_t n, uint64_t now_ms) {
    bool closed = false;
    if (n == 0) return false;
    if (len_ && now_ms >= last_byte_ms_ && now_ms - last_byte_ms_ >= gap_ms_) closed = close(now_ms);
    if (!len_) first_ms_ = now_ms;
    for (size_t i = 0; i < n; ++i) {
        if (len_ == kBuf) { closed |= close(now_ms); first_ms_ = now_ms; }   // переполнение: режем кадр
        buf_[len_++] = p[i];
    }
    last_byte_ms_ = now_ms;
    return closed;
}

bool BusSniffer::recent(size_t i, FrameRec& out) const {
    if (i >= count()) return false;
    out = ring_[(n_ - 1 - i) % kRing];
    return true;
}

// ---- PassiveTelemetry ------------------------------------------------------------------------

void PassiveTelemetry::on_frame(const FrameRec& f, const uint8_t* d, uint64_t now) {
    if (!f.crc_ok || f.id != id_) return;
    if (f.kind == FrameKind::Request && f.fn == 0x03) {
        have_req_ = true; req_addr_ = f.addr; req_cnt_ = f.count; req_ms_ = now;
        return;
    }
    if (f.kind != FrameKind::Response || f.fn != 0x03 || !have_req_) return;
    have_req_ = false;
    if (f.count != req_cnt_ || now < req_ms_ || now - req_ms_ > kPairMs || f.count > kMaxBlockRegs) return;
    uint16_t r[kMaxBlockRegs];
    for (uint16_t i = 0; i < f.count; ++i) r[i] = (uint16_t)((d[3 + 2 * i] << 8) | d[4 + 2 * i]);
    if (req_addr_ == kBlockRtAddr && f.count >= 38)       { rt_ = Telemetry();  decode_rt(r, rt_);   rt_ms_ = now; }
    else if (req_addr_ == kBlockChgAddr && f.count >= 12) { chg_ = Telemetry(); decode_chg(r, chg_); chg_ms_ = now; ++chg_n_; }
    else if (req_addr_ == kBlockCfgAddr && f.count >= 5)  { cfg_ = Telemetry(); decode_cfg(r, cfg_); cfg_ms_ = now; }
}

Telemetry PassiveTelemetry::get(uint64_t now) const {
    Telemetry t;
    if (rt_.rt_ok && now >= rt_ms_ && now - rt_ms_ <= kFreshMs) { t.pack_v = rt_.pack_v; t.grid_v = rt_.grid_v; t.load_w = rt_.load_w; t.relay_grid = rt_.relay_grid; t.rt_ok = true; }
    if (chg_.chg_ok && now >= chg_ms_ && now - chg_ms_ <= kFreshMs) { t.pack_v_alt = chg_.pack_v_alt; t.chg_curr_a = chg_.chg_curr_a; t.pv_v = chg_.pv_v; t.chg_w = chg_.chg_w; t.pv_relay = chg_.pv_relay; t.chg_state = chg_.chg_state; t.chg_ok = true; }
    if (cfg_.cfg_ok && now >= cfg_ms_ && now - cfg_ms_ <= kFreshMs) { t.ongrid_reg = cfg_.ongrid_reg; t.cfg_ok = true; }
    return t;
}

// ---- LoggerTracker ---------------------------------------------------------------------------

bool LoggerTracker::ignore(uint8_t fn, uint16_t addr, uint16_t count) {
    if (n_ign_ >= kIgnore) return false;
    ign_[n_ign_++] = {fn, addr, count};
    return true;
}

void LoggerTracker::observe(const FrameRec& f, uint64_t t) {
    if (!f.crc_ok || f.id != cfg_.target_id) return;
    if (f.kind != FrameKind::Request && f.kind != FrameKind::WriteEcho) return;
    for (size_t i = 0; i < n_ign_; ++i)
        if (ign_[i].fn == f.fn && ign_[i].addr == f.addr && ign_[i].count == f.count) { ++ignored_; return; }
    if (seen_any_ && t < last_req_) return;              // время назад — игнорируем
    if (open_ && t - cur_last_ > cfg_.burst_gap_ms) close_burst();
    if (!open_) { open_ = true; cur_start_ = t; cur_n_ = 0; cur_first_addr_ = f.addr; }
    cur_last_ = t; ++cur_n_;
    last_req_ = t; seen_any_ = true;
}

bool LoggerTracker::in_burst(uint64_t now) const {
    // Бесконечный чужой опрос (не логгер) не должен глушить нас навсегда: после burst_max_ms «пачка» не гейтит.
    if (open_ && cur_last_ >= cur_start_ && cur_last_ - cur_start_ > cfg_.burst_max_ms) return false;
    return seen_any_ && (now < last_req_ || now - last_req_ < cfg_.post_guard_ms + (open_ ? cfg_.burst_gap_ms : 0));
}

void LoggerTracker::close_burst() {
    if (!open_) return;
    open_ = false;
    const uint64_t dur = cur_last_ - cur_start_;
    BurstRec r; r.start_ms = (uint32_t)cur_start_; r.dur_ms = (uint32_t)dur; r.n_req = cur_n_; r.first_addr = cur_first_addr_;

    // Фаза: попадание в ритм относительно последней «опорной» пачки.
    bool in_phase = false;
    if (!have_ref_ || (cur_start_ > ref_ && cur_start_ - ref_ > 12 * (uint64_t)period_)) {
        have_ref_ = true; ref_ = cur_start_; inphase_ = 0; miss_ = 0; locked_ = false; len_est_ = (double)dur;
        last_checked_ = cur_start_;
    } else if (cur_start_ > ref_) {
        double d = (double)(cur_start_ - ref_);
        double k = std::floor(d / period_ + 0.5);
        if (k >= 1 && std::fabs(d - k * period_) <= cfg_.period_tol_ms) {
            in_phase = true;
            double np = 0.75 * period_ + 0.25 * (d / k);
            double lo = cfg_.period_ms * 0.8, hi = cfg_.period_ms * 1.2;
            period_ = np < lo ? lo : (np > hi ? hi : np);
            ref_ = cur_start_;
            if (inphase_ < 255) ++inphase_;
            miss_ = 0;
            len_est_ = len_est_ > 0 ? 0.75 * len_est_ + 0.25 * (double)dur : (double)dur;
            if (inphase_ >= 2) locked_ = true;
        } else {
            ++off_;
            if (!locked_) { ref_ = cur_start_; inphase_ = 0; miss_ = 0; last_checked_ = cur_start_; }   // чужая «опора» не должна блокировать захват
        }
    }
    r.in_phase = in_phase;
    ring_[total_ % kBursts] = r; ++total_;
}

uint64_t LoggerTracker::window_len() const {
    double l = len_est_ * 1.5;
    if (l < cfg_.min_burst_ms) l = cfg_.min_burst_ms;
    return (uint64_t)l + cfg_.post_guard_ms;
}

bool LoggerTracker::next_start(uint64_t now, uint64_t& t) const {
    if (!locked_ || !have_ref_ || now < ref_) return false;
    const uint64_t P = (uint64_t)period_, W = window_len();
    uint64_t k = now > ref_ + W ? (now - ref_ - W) / P + 1 : 0;
    t = ref_ + k * P;
    return true;
}

bool LoggerTracker::set_period(uint32_t ms) {
    if (ms < 30000 || ms > 900000) return false;
    cfg_.period_ms = ms; period_ = ms;
    cfg_.period_tol_ms = ms / 8 < 5000 ? 5000 : (ms / 8 > 20000 ? 20000 : ms / 8);
    have_ref_ = false; locked_ = false; inphase_ = 0; miss_ = 0;
    return true;
}

void LoggerTracker::tick(uint64_t now) {
    if (open_ && now >= cur_last_ && now - cur_last_ > cfg_.burst_gap_ms) close_burst();
    if (!locked_ || !have_ref_ || now <= ref_) return;
    // Учёт пропусков: ожидаемая пачка не пришла — после 3 подряд теряем захват.
    const uint64_t P = (uint64_t)period_, W = window_len();
    if (now <= ref_ + W) return;
    uint64_t k = (now - ref_ - W) / P;            // последняя предсказанная пачка, чьё окно уже закрыто
    if (k == 0) return;
    uint64_t exp = ref_ + k * P;
    if (exp > last_checked_ && exp + W < now) {
        last_checked_ = exp;
        if (miss_ < 255) ++miss_;
        if (miss_ >= 3) { locked_ = false; inphase_ = 0; }
    }
}

Gate LoggerTracker::may_transmit(uint64_t now, uint32_t need_ms) const {
    Gate g;
    if (in_burst(now)) {                         // идёт пачка или тишина сразу после неё
        uint64_t until = last_req_ + cfg_.post_guard_ms + (open_ ? cfg_.burst_gap_ms : 0);
        g.ok = false; g.why = Gate::InBurst; g.wait_ms = until > now ? (uint32_t)(until - now) : 0;
        return g;
    }
    uint64_t t;
    if (next_start(now, t)) {
        const uint64_t qs = t > cfg_.pre_quiet_ms ? t - cfg_.pre_quiet_ms : 0, qe = t + window_len();
        if (now + need_ms >= qs && now < qe) {
            g.ok = false; g.why = Gate::Predicted; g.wait_ms = (uint32_t)(qe - now);
        }
    }
    return g;
}

bool LoggerTracker::active_window(uint64_t now) const {
    uint64_t t;
    if (!locked_ || !next_start(now, t) || in_burst(now)) return false;
    const uint64_t qs = t > cfg_.pre_quiet_ms ? t - cfg_.pre_quiet_ms : 0;
    return now < qs && qs - now > cfg_.fast_gap_ms;
}

bool LoggerTracker::burst(size_t i, BurstRec& out) const {
    if (i >= burst_count()) return false;
    out = ring_[(total_ - 1 - i) % kBursts];
    return true;
}

}  // namespace gs
