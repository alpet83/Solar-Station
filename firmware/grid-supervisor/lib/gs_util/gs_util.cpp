#include "gs_util.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace gs {

#define F(name, type, k) {#name, CfgField::k, offsetof(Config, name)}
const CfgField kCfgFields[] = {
    F(hard_floor_v, float, F32), F(soft_on_v, float, F32), F(soft_off_v, float, F32), F(full_v, float, F32),
    F(hard_dwell_ms, x, U32), F(soft_on_dwell_ms, u, U32), F(soft_off_dwell_ms, u, U32),
    F(no_data_timeout_ms, x, U32), F(manual_pause_ms, u, U32), F(hold_after_connect_ms, u, U32),
    F(cmd_retry_ms, x, U32), F(pow_min_base_w, f, F32), F(load_factor, f, F32), F(ext_coef, f, F32),
    F(ext_ttl_ms, x, U32), F(ext_max_a, f, F32), F(charge_max_a, f, F32), F(step_gap_ms, u, U32),
    F(v_valid_min, x, F32), F(v_valid_max, f, F32), F(v_mismatch_max, f, F32), F(grid_present_v_min, f, F32),
};
#undef F
const size_t kCfgFieldCount = sizeof(kCfgFields) / sizeof(kCfgFields[0]);

static const CfgField* find(const char* key) {
    for (size_t i = 0; i < kCfgFieldCount; ++i)
        if (strcmp(kCfgFields[i].key, key) == 0) return &kCfgFields[i];
    return nullptr;
}

// Разбор значения и запись в cfg БЕЗ проверки всей конфигурации.
static bool set_raw(Config& cfg, const char* key, const char* value) {
    const CfgField* f = key && value ? find(key) : nullptr;
    if (!f || !*value) return false;
    char* end = nullptr;
    if (f->kind == CfgField::F32) {
        float v = strtof(value, &end);
        if (*end || !std::isfinite(v)) return false;
        memcpy(reinterpret_cast<char*>(&cfg) + f->off, &v, 4);
    } else {
        if (*value == '-') return false;
        unsigned long v = strtoul(value, &end, 10);
        if (*end || v > 0xFFFFFFFFul) return false;
        uint32_t u = (uint32_t)v;
        memcpy(reinterpret_cast<char*>(&cfg) + f->off, &u, 4);
    }
    return true;
}

bool config_set(Config& cfg, const char* key, const char* value) {
    return config_apply_pairs(cfg, &key, &value, 1);
}

bool config_apply_pairs(Config& cfg, const char* const* keys, const char* const* vals, size_t n) {
    Config cand = cfg;
    for (size_t i = 0; i < n; ++i)
        if (!set_raw(cand, keys[i], vals[i])) return false;
    if (!cand.validate()) return false;
    cfg = cand;
    return true;
}

bool config_get(const Config& cfg, const char* key, char* out, size_t cap) {
    const CfgField* f = find(key);
    if (!f) return false;
    const char* p = reinterpret_cast<const char*>(&cfg) + f->off;
    if (f->kind == CfgField::F32) { float v; memcpy(&v, p, 4); snprintf(out, cap, "%.9g", v); }
    else { uint32_t v; memcpy(&v, p, 4); snprintf(out, cap, "%lu", (unsigned long)v); }
    return true;
}

size_t config_to_json(const Config& cfg, char* out, size_t cap) {
    size_t n = 0;
    auto put = [&](const char* fmt, const char* a, const char* b) {
        if (n < cap) { int w = snprintf(out + n, cap - n, fmt, a, b); if (w > 0) n += (size_t)w; }
    };
    put("%s", "{", "");
    for (size_t i = 0; i < kCfgFieldCount; ++i) {
        char v[24]; config_get(cfg, kCfgFields[i].key, v, sizeof v);
        char item[64]; snprintf(item, sizeof item, "%s\"%s\":%s", i ? "," : "", kCfgFields[i].key, v);
        put("%s", item, "");
    }
    put("%s", "}", "");
    if (n >= cap) { if (cap) out[cap - 1] = 0; return 0; }
    return n;
}

size_t flags_to_str(uint32_t fl, char* out, size_t cap) {
    static const struct { uint32_t bit; const char* name; } names[] = {
        {F_HARD_LOW, "HARD_LOW"}, {F_SOFT_LOW, "SOFT_LOW"}, {F_NO_DATA, "NO_DATA"}, {F_STALE, "STALE"},
        {F_DATA_DOUBTFUL, "DOUBTFUL"}, {F_MANUAL_PAUSE, "MANUAL_PAUSE"}, {F_MANUAL_OVERRIDE, "MANUAL_OVERRIDE"},
        {F_GRID_LOST_LOW, "GRID_LOST_LOW"}, {F_SAFE_MODE, "SAFE_MODE"}, {F_CMD_PENDING, "CMD_PENDING"},
        {F_CMD_RETRY, "CMD_RETRY"}, {F_OFF_COND, "OFF_COND"}, {F_HOLD, "HOLD"}, {F_SINGLE_SRC, "SINGLE_SRC"},
        {F_TIME_JUMP, "TIME_JUMP"}, {F_LOAD_UNKNOWN, "LOAD_UNKNOWN"}, {F_RELAY_MISMATCH, "RELAY_MISMATCH"},
    };
    size_t n = 0;
    if (!cap) return 0;
    out[0] = 0;
    for (auto& e : names) {
        if (!(fl & e.bit)) continue;
        int w = snprintf(out + n, cap > n ? cap - n : 0, "%s%s", n ? "," : "", e.name);
        if (w < 0 || (size_t)w >= cap - n) { out[n] = 0; return n; }
        n += (size_t)w;
    }
    return n;
}

static void num(char* out, size_t cap, float v) {
    if (std::isfinite(v)) snprintf(out, cap, "%.2f", v); else snprintf(out, cap, "null");
}

const char* reason_name(Reason r) {
    switch (r) {
        case Reason::HardFloor: return "HARD_FLOOR"; case Reason::SoftLow: return "SOFT_LOW"; case Reason::NoData: return "NO_DATA";
        case Reason::Charged: return "CHARGED"; case Reason::SafeMode: return "SAFE_MODE"; case Reason::Manual: return "MANUAL";
        default: return "NONE";
    }
}

size_t state_to_json(const StateView& s, char* out, size_t cap) {
    char vp[16], va[16], ch[16], ld[16], gv[16], vu[16], ef[16], pm[16], fl[200], ts[32] = "";
    num(vp, sizeof vp, s.tel->pack_v); num(va, sizeof va, s.tel->pack_v_alt);
    num(ch, sizeof ch, s.tel->chg_curr_a); num(ld, sizeof ld, s.tel->load_w); num(gv, sizeof gv, s.tel->grid_v);
    num(vu, sizeof vu, s.dec->v_used); num(ef, sizeof ef, s.dec->eff_charge_a); num(pm, sizeof pm, s.dec->pow_min_w);
    flags_to_str(s.dec->flags, fl, sizeof fl);
    if (s.epoch_s >= 0) {
        time_t t = (time_t)s.epoch_s; struct tm tmv;
        gmtime_r(&t, &tmv);
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    }
    int w = snprintf(out, cap,
        "{\"source\":\"supervisor\",\"seq\":%lu,\"boot_id\":%lu,\"uptime_ms\":%llu,\"ntp_synced\":%s,"
        "\"t\":%lld,\"ts\":%s%s%s,"
        "\"voltages\":{\"battery\":%s,\"alt\":%s,\"grid\":%s},\"charge_a\":%s,\"load_w\":%s,"
        "\"ongrid_reg\":%d,\"relay_grid\":%d,\"rt_ok\":%s,\"chg_ok\":%s,\"cfg_ok\":%s,"
        "\"decision\":{\"v_used\":%s,\"eff_charge_a\":%s,\"pow_min_w\":%s,\"flags\":\"%s\",\"flags_raw\":%lu},"
        "\"safe_mode\":%s,\"paused\":%s,\"pause_left_s\":%llu,"
        "\"bus\":{\"ok\":%lu,\"err\":%lu,\"foreign\":%lu,\"busy\":%lu,\"exc\":%lu},\"events\":[",
        (unsigned long)s.seq, (unsigned long)s.boot_id, (unsigned long long)s.uptime_ms,
        s.epoch_s >= 0 ? "true" : "false", (long long)s.epoch_s,
        s.epoch_s >= 0 ? "\"" : "", s.epoch_s >= 0 ? ts : "null", s.epoch_s >= 0 ? "\"" : "",
        vp, va, gv, ch, ld, (int)s.tel->ongrid_reg, (int)s.tel->relay_grid,
        s.tel->rt_ok ? "true" : "false", s.tel->chg_ok ? "true" : "false", s.tel->cfg_ok ? "true" : "false",
        vu, ef, pm, fl, (unsigned long)s.dec->flags,
        s.safe_mode ? "true" : "false", s.paused ? "true" : "false", (unsigned long long)(s.pause_left_ms / 1000),
        (unsigned long)s.rx_ok, (unsigned long)s.rx_err, (unsigned long)s.rx_foreign, (unsigned long)s.rx_busy,
        (unsigned long)s.rx_exc);
    if (w < 0 || (size_t)w >= cap) { if (cap) out[0] = 0; return 0; }
    size_t n = (size_t)w;
    for (size_t i = 0; i < s.n_events && s.events; ++i) {
        const Event& e = s.events[i];
        if (e.cmd == Cmd::None) continue;
        char ets[24] = "null";
        if (e.epoch_s >= 0) {
            time_t t = (time_t)e.epoch_s; struct tm tmv; gmtime_r(&t, &tmv);
            snprintf(ets, sizeof ets, "\"%04d-%02d-%02d %02d:%02d:%02d\"", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        }
        char vv[16]; num(vv, sizeof vv, e.v);
        int x = snprintf(out + n, cap - n, "%s{\"id\":%lu,\"cmd\":\"%s\",\"reason\":\"%s\",\"ts\":%s,\"uptime_ms\":%llu,\"v\":%s,\"ok\":%s}",
                         i ? "," : "", (unsigned long)e.id, e.cmd == Cmd::Connect ? "CONNECT" : "DISCONNECT", reason_name(e.reason),
                         ets, (unsigned long long)e.uptime_ms, vv, e.ok ? "true" : "false");
        if (x < 0 || (size_t)x >= cap - n) { if (cap) out[0] = 0; return 0; }
        n += (size_t)x;
    }
    if (n + 3 > cap) { if (cap) out[0] = 0; return 0; }
    out[n++] = ']'; out[n++] = '}'; out[n] = 0;
    return n;
}

size_t regs_to_json(uint16_t addr, uint16_t count, uint64_t age_ms, const uint16_t* regs, char* out, size_t cap) {
    int w = snprintf(out, cap, "{\"addr\":%u,\"count\":%u,\"age_ms\":%llu,\"regs\":[", (unsigned)addr, (unsigned)count, (unsigned long long)age_ms);
    if (w < 0 || (size_t)w >= cap) { if (cap) out[0] = 0; return 0; }
    size_t n = (size_t)w;
    for (uint16_t i = 0; i < count; ++i) {
        int x = snprintf(out + n, cap - n, "%s%u", i ? "," : "", (unsigned)regs[i]);
        if (x < 0 || (size_t)x >= cap - n) { out[0] = 0; return 0; }
        n += (size_t)x;
    }
    if (n + 3 > cap) { if (cap) out[0] = 0; return 0; }
    out[n++] = ']'; out[n++] = '}'; out[n] = 0;
    return n;
}

bool parse_ext_current(const char* s, float& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    float v = strtof(s, &end);
    if (*end || !std::isfinite(v) || v < 0.0f || v > 500.0f) return false;
    out = v;
    return true;
}

}  // namespace gs
