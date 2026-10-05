#include "gs_can.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>

namespace gs {

uint32_t can_age_ms(uint32_t t_ms, uint32_t now_ms) {
    int32_t d = (int32_t)(now_ms - t_ms);
    return d < 0 ? 0 : (uint32_t)d;
}

// ---- статистика по ID ------------------------------------------------------------------------
static uint64_t key_of(uint32_t id, bool ext) { return ((uint64_t)(ext ? 1 : 0) << 32) | id; }

const CanIdStat* CanStats::find(uint32_t id, bool ext) const {
    for (size_t i = 0; i < n_; ++i) if (t_[i].id == id && t_[i].ext == ext) return &t_[i];
    return nullptr;
}

void CanStats::on_frame(const CanFrame& f) {
    ++total_;
    size_t pos = 0;
    while (pos < n_ && key_of(t_[pos].id, t_[pos].ext) < key_of(f.id, f.ext)) ++pos;
    if (pos < n_ && t_[pos].id == f.id && t_[pos].ext == f.ext) {
        CanIdStat& s = t_[pos];
        uint32_t dt = f.t_ms - s.last_ms;
        s.period_ms = s.period_ms ? (s.period_ms * 3 + dt + 2) / 4 : dt;
        ++s.count; s.last_ms = f.t_ms;
        s.dlc = f.dlc; memcpy(s.data, f.data, 8);
        return;
    }
    if (n_ >= kCanMaxIds) { ++overflow_; return; }
    for (size_t i = n_; i > pos; --i) t_[i] = t_[i - 1];
    CanIdStat& s = t_[pos];
    s = CanIdStat();
    s.id = f.id; s.ext = f.ext; s.count = 1; s.last_ms = f.t_ms;
    s.dlc = f.dlc; memcpy(s.data, f.data, 8);
    ++n_;
}

// ---- декодер ---------------------------------------------------------------------------------
static int16_t i16(const uint8_t* p) { return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint16_t u16(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

bool PylonBattery::feed(const CanFrame& f) {
    if (f.ext) return false;
    const uint8_t* d = f.data;
    switch (f.id) {
    case 0x351:                                 // пределы: Uзаряда, Iзаряда, Iразряда [0.1], Uразряда мин [0.1]
        if (f.dlc < 6) return false;
        limits.charge_v = i16(d) * 0.1f; limits.charge_a = i16(d + 2) * 0.1f; limits.discharge_a = i16(d + 4) * 0.1f;
        limits.discharge_v_min = f.dlc >= 8 ? i16(d + 6) * 0.1f : std::numeric_limits<float>::quiet_NaN();
        limits.have = true; limits.t_ms = f.t_ms; return true;
    case 0x355:                                 // SOC %, SOH %
        if (f.dlc < 4) return false;
        soc.soc = u16(d); soc.soh = u16(d + 2); soc.have = true; soc.t_ms = f.t_ms; return true;
    case 0x356:                                 // U 0.01 В, I 0.1 А (со знаком), T 0.1 °C
        if (f.dlc < 6) return false;
        pack.v = i16(d) * 0.01f; pack.i = i16(d + 2) * 0.1f; pack.temp_c = i16(d + 4) * 0.1f;
        pack.have = true; pack.t_ms = f.t_ms; return true;
    case 0x359:                                 // protection 1/2, alarm 1/2, число модулей, 'P' 'N'
        if (f.dlc < 5) return false;
        prot.prot1 = d[0]; prot.prot2 = d[1]; prot.alarm1 = d[2]; prot.alarm2 = d[3]; prot.modules = d[4];
        prot.pn = f.dlc >= 7 && d[5] == 'P' && d[6] == 'N';
        prot.have = true; prot.t_ms = f.t_ms; return true;
    case 0x35C:                                 // флаги запросов: бит7 заряд разрешён, бит6 разряд разрешён, бит5/4 принудительный заряд, бит3 полный заряд
        if (f.dlc < 1) return false;
        req.raw = d[0];
        req.charge_en = d[0] & 0x80; req.discharge_en = d[0] & 0x40;
        req.force1 = d[0] & 0x20; req.force2 = d[0] & 0x10; req.full_req = d[0] & 0x08;
        req.have = true; req.t_ms = f.t_ms; return true;
    case 0x35E: {                               // имя производителя, ASCII
        if (f.dlc < 1) return false;
        size_t n = f.dlc < 8 ? f.dlc : 8, k = 0;
        for (size_t i = 0; i < n; ++i) mfr.name[k++] = (d[i] >= 0x20 && d[i] < 0x7f && d[i] != '"' && d[i] != '\\') ? (char)d[i] : (d[i] == 0 ? ' ' : '?');   // безопасно для JSON
        while (k && mfr.name[k - 1] == ' ') --k;
        mfr.name[k] = 0; mfr.have = true; mfr.t_ms = f.t_ms; return true; }
    default: return false;
    }
}

// Seplos 100k: расширенный кадр 0x000300NN. Раскладки — гипотезы (см. gs_can.h).
bool SeplosBattery::feed(const CanFrame& f) {
    if (!f.ext || (f.id & 0xFFFF00FFu) != kSeplosIdBase) return false;
    const uint8_t* d = f.data;
    switch ((f.id >> 8) & 0xFF) {
    case 0x01:                                  // пределы: Uзаряда, Iзаряда, Uразряда мин, Iразряда (u16, 0,1)
        if (f.dlc < 8) return false;
        limits.charge_v = u16(d) * 0.1f; limits.charge_a = u16(d + 2) * 0.1f;
        limits.discharge_v_min = u16(d + 4) * 0.1f; limits.discharge_a = u16(d + 6) * 0.1f;
        limits.have = true; limits.t_ms = f.t_ms; return true;
    case 0x02: {                                // аварии/защиты (биты неизвестны): сырые байты
        if (f.dlc < 1) return false;
        bool nz = false;
        for (int i = 0; i < 8; ++i) { prot.raw[i] = i < f.dlc ? d[i] : 0; if (prot.raw[i]) nz = true; }
        prot.alarm = nz; prot.have = true; prot.t_ms = f.t_ms; return true; }
    case 0x04:                                  // U 0,1 В; |I| 0,1 А (знак неизвестен); температура °C (предположительно)
        if (f.dlc < 6) return false;
        pack.v = u16(d) * 0.1f; pack.i_abs = u16(d + 2) * 0.1f; pack.temp_c = (float)u16(d + 4);
        pack.have = true; pack.t_ms = f.t_ms; return true;
    case 0x05:                                  // SOC %, SOH % (вероятно), SOC 0,1 % (вероятно)
        if (f.dlc < 6) return false;
        soc.soc = u16(d); soc.soh = u16(d + 2); soc.soc10 = u16(d + 4);
        soc.have = true; soc.t_ms = f.t_ms; return true;
    default: return false;                      // остальные ID идут только в таблицу CanStats
    }
}

uint32_t SeplosBattery::last_ms() const {
    const struct { bool have; uint32_t t; } g[] = {{limits.have, limits.t_ms}, {prot.have, prot.t_ms}, {pack.have, pack.t_ms}, {soc.have, soc.t_ms}};
    bool got = false; uint32_t best = 0;
    for (auto& x : g) if (x.have && (!got || (int32_t)(x.t - best) > 0)) { best = x.t; got = true; }
    return best;
}

// ---- Seplos 500k: стандартные ID 0x311…0x322, u16 BIG-endian (уровни уверенности — в gs_can.h) ----
static uint16_t be16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static int16_t sbe16(const uint8_t* p) { return (int16_t)be16(p); }

bool Seplos500Battery::feed(const CanFrame& f) {
    if (f.ext || f.id < kSeplos5IdFirst || f.id > kSeplos5IdLast || f.dlc < 8) return false;
    const uint8_t* d = f.data;
    switch (f.id) {
    case 0x311:
        limits.charge_v_limit = be16(d) * 0.1f; limits.charge_a = be16(d + 2) * 0.1f; limits.discharge_a = be16(d + 4) * 0.1f; limits.unk = be16(d + 6);
        limits.have = true; limits.t_ms = f.t_ms; return true;
    case 0x312:
        memcpy(flags.raw, d, 8); flags.have = true; flags.t_ms = f.t_ms; return true;
    case 0x313:
        pack.v = be16(d) * 0.01f; pack.i_raw = sbe16(d + 2) * 0.1f; pack.temp_c = sbe16(d + 4) * 0.1f; pack.soc = d[6]; pack.soh = d[7];
        pack.have = true; pack.t_ms = f.t_ms; return true;
    case 0x314:
        cap.remaining_ah = be16(d) * 0.01f; cap.full_ah = be16(d + 2) * 0.01f; cap.counter = be16(d + 4); cap.cycles = be16(d + 6);
        cap.have = true; cap.t_ms = f.t_ms; return true;
    case 0x315: case 0x316: case 0x317: case 0x318: {
        const size_t g = f.id - 0x315;
        for (size_t k = 0; k < 4; ++k) cells.mv[g * 4 + k] = be16(d + k * 2);
        cells.have[g] = true; cells.t_ms[g] = f.t_ms; return true; }
    case 0x319:
        mm.b0 = d[0]; mm.max_mv = be16(d + 1); mm.min_mv = be16(d + 3); mm.idx[0] = d[5]; mm.idx[1] = d[6];
        mm.have = true; mm.t_ms = f.t_ms; return true;
    case 0x320:
        memcpy(unk320.raw, d, 8); unk320.have = true; unk320.t_ms = f.t_ms; return true;
    case 0x322:
        temps.max_c = sbe16(d) * 0.1f; temps.min_c = sbe16(d + 2) * 0.1f; memcpy(temps.unk, d + 4, 4);
        temps.have = true; temps.t_ms = f.t_ms; return true;
    default: return false;                      // 0x321, 0x323… (в диапазоне, но не известны) — только в таблицу CanStats
    }
}

bool Seplos500Battery::any() const {
    return pack.have || mm.have || limits.have || cap.have || temps.have || flags.have || unk320.have ||
           cells.have[0] || cells.have[1] || cells.have[2] || cells.have[3];
}

uint32_t Seplos500Battery::last_ms() const {
    const struct { bool have; uint32_t t; } g[] = {{pack.have, pack.t_ms}, {mm.have, mm.t_ms}, {limits.have, limits.t_ms}, {cap.have, cap.t_ms},
        {temps.have, temps.t_ms}, {flags.have, flags.t_ms}, {unk320.have, unk320.t_ms},
        {cells.have[0], cells.t_ms[0]}, {cells.have[1], cells.t_ms[1]}, {cells.have[2], cells.t_ms[2]}, {cells.have[3], cells.t_ms[3]}};
    bool got = false; uint32_t best = 0;
    for (auto& x : g) if (x.have && (!got || (int32_t)(x.t - best) > 0)) { best = x.t; got = true; }
    return best;
}

bool Seplos500Battery::cell_ok(size_t i, uint32_t now) const {
    return i < 16 && can_fresh(cells.have[i / 4], cells.t_ms[i / 4], now);
}

bool Seplos500Battery::cell_range(uint32_t now, int* mn, int* mx) const {
    if (can_fresh(mm.have, mm.t_ms, now)) { *mn = mm.min_mv; *mx = mm.max_mv; return true; }    // при расхождении с массивом верим 0x319
    for (size_t i = 0; i < 16; ++i) if (!cell_ok(i, now)) return false;
    *mn = *mx = cells.mv[0];
    for (size_t i = 1; i < 16; ++i) { if (cells.mv[i] < *mn) *mn = cells.mv[i]; if (cells.mv[i] > *mx) *mx = cells.mv[i]; }
    return true;
}

// Раскладка Pylontech low-voltage CAN, кадр 0x359 (из памяти, в PDF Seplos её нет — проверить на железе).
static const char* const kProt1[8] = {nullptr, "Cell or module over-voltage", "Cell or module under-voltage", "Cell or module over-temperature",
                                      "Cell or module under-temperature", nullptr, nullptr, "Discharge over-current"};
static const char* const kProt2[8] = {"Charge over-current", nullptr, nullptr, "System error", nullptr, nullptr, nullptr, "System error"};
static const char* const kAlm1[8]  = {nullptr, "Cell or module high voltage", "Cell or module low voltage", "Cell or module high temperature",
                                      "Cell or module low temperature", nullptr, nullptr, "Discharge high current"};
static const char* const kAlm2[8]  = {"Charge high current", nullptr, nullptr, "Internal communication fail", nullptr, nullptr, nullptr, nullptr};

const char* pylon_prot_name(int byte, int bit) {
    if (bit < 0 || bit > 7) return nullptr;
    return byte == 0 ? kProt1[bit] : byte == 1 ? kProt2[bit] : nullptr;
}
const char* pylon_alarm_name(int byte, int bit) {
    if (bit < 0 || bit > 7) return nullptr;
    return byte == 0 ? kAlm1[bit] : byte == 1 ? kAlm2[bit] : nullptr;
}

// ---- сводка ----------------------------------------------------------------------------------
BatteryBrief::BatteryBrief() : v(std::numeric_limits<float>::quiet_NaN()), i(v), soc(v), i_abs(v), cell_min_mv(-1), cell_max_mv(-1), cell_delta_mv(-1), protocol(nullptr), charge_en(-1), discharge_en(-1), alarm(-1), age_s(-1) {}

BatteryBrief battery_brief(const PylonBattery& b, uint32_t now) {
    BatteryBrief r;
    if (can_fresh(b.pack.have, b.pack.t_ms, now)) { r.v = b.pack.v; r.i = b.pack.i; }
    if (can_fresh(b.soc.have, b.soc.t_ms, now)) r.soc = (float)b.soc.soc;
    if (can_fresh(b.req.have, b.req.t_ms, now)) { r.charge_en = b.req.charge_en; r.discharge_en = b.req.discharge_en; }
    if (can_fresh(b.prot.have, b.prot.t_ms, now)) r.alarm = (b.prot.prot1 | b.prot.prot2 | b.prot.alarm1 | b.prot.alarm2) != 0;
    bool any = false; uint32_t best = 0;
    const struct { bool have; uint32_t t; } g[] = {{b.limits.have, b.limits.t_ms}, {b.soc.have, b.soc.t_ms}, {b.pack.have, b.pack.t_ms},
                                                   {b.prot.have, b.prot.t_ms}, {b.req.have, b.req.t_ms}, {b.mfr.have, b.mfr.t_ms}};
    for (auto& x : g) if (x.have) { uint32_t a = can_age_ms(x.t, now); if (!any || a < best) best = a; any = true; }
    if (any) { r.age_s = (int32_t)(best / 1000); r.protocol = "pylon"; }
    return r;
}

// Какой декодер получил кадр позже: 1 — Seplos 100k, 0 — Pylon, -1 — кадров не было.
static int pylon_last_ms(const PylonBattery& b, uint32_t* t) {
    const struct { bool have; uint32_t t; } g[] = {{b.limits.have, b.limits.t_ms}, {b.soc.have, b.soc.t_ms}, {b.pack.have, b.pack.t_ms},
                                                   {b.prot.have, b.prot.t_ms}, {b.req.have, b.req.t_ms}, {b.mfr.have, b.mfr.t_ms}};
    bool got = false;
    for (auto& x : g) if (x.have && (!got || (int32_t)(x.t - *t) > 0)) { *t = x.t; got = true; }
    return got ? 0 : -1;
}
// Активный протокол: 0 — Pylon, 1 — seplos_100k, 2 — seplos_500k, -1 — кадров не было; побеждает кадр свежее (при равенстве Pylon, затем 100k).
static int active_protocol(const PylonBattery& b, const SeplosBattery& s, const Seplos500Battery& s5) {
    int best = -1; uint32_t bt = 0, t = 0;
    if (pylon_last_ms(b, &t) == 0) { best = 0; bt = t; }
    if (s.any()) { t = s.last_ms(); if (best < 0 || (int32_t)(t - bt) > 0) { best = 1; bt = t; } }
    if (s5.any()) { t = s5.last_ms(); if (best < 0 || (int32_t)(t - bt) > 0) { best = 2; bt = t; } }
    return best;
}

BatteryBrief battery_brief(const PylonBattery& b, const SeplosBattery& s, uint32_t now) {
    return battery_brief(b, s, Seplos500Battery(), now);
}

BatteryBrief battery_brief(const PylonBattery& b, const SeplosBattery& s, const Seplos500Battery& s5, uint32_t now) {
    const int ap = active_protocol(b, s, s5);
    if (ap == 2) {
        BatteryBrief r;
        if (can_fresh(s5.pack.have, s5.pack.t_ms, now)) { r.v = s5.pack.v; r.i_abs = std::fabs(s5.pack.i_raw); r.soc = (float)s5.pack.soc; }    // i = NaN: знак тока не подтверждён
        int mn, mx;
        if (s5.cell_range(now, &mn, &mx)) { r.cell_min_mv = mn; r.cell_max_mv = mx; r.cell_delta_mv = mx - mn; }
        r.age_s = (int32_t)(can_age_ms(s5.last_ms(), now) / 1000);
        r.protocol = "seplos_500k";                // charge_en / discharge_en / alarm: -1 (раскладка флагов 0x312 неизвестна)
        return r;
    }
    if (ap != 1) return battery_brief(b, now);
    BatteryBrief r;
    if (can_fresh(s.pack.have, s.pack.t_ms, now)) { r.v = s.pack.v; r.i_abs = s.pack.i_abs; }    // i остаётся NaN: знак неизвестен
    if (can_fresh(s.soc.have, s.soc.t_ms, now)) r.soc = (float)s.soc.soc;
    if (can_fresh(s.prot.have, s.prot.t_ms, now)) r.alarm = s.prot.alarm;                          // charge_en / discharge_en: -1 (кадров нет)
    r.age_s = (int32_t)(can_age_ms(s.last_ms(), now) / 1000);
    r.protocol = "seplos_100k";
    return r;
}

// ---- JSON ------------------------------------------------------------------------------------
namespace {

// Накопитель в буфер: после первого переполнения ok = false.
struct W {
    char* o; size_t cap; size_t n = 0; bool ok = true;
    W(char* out, size_t c) : o(out), cap(c) {}
    __attribute__((format(printf, 2, 3))) void put(const char* fmt, ...) {
        if (!ok) return;
        if (n >= cap) { ok = false; return; }
        va_list ap; va_start(ap, fmt);
        int w = vsnprintf(o + n, cap - n, fmt, ap);
        va_end(ap);
        if (w < 0 || (size_t)w >= cap - n) { ok = false; return; }
        n += (size_t)w;
    }
};

void numstr(char* s, size_t cap, float v, const char* fmt) {
    if (std::isfinite(v)) snprintf(s, cap, fmt, (double)v); else snprintf(s, cap, "null");
}
const char* tf(bool v) { return v ? "true" : "false"; }
const char* tfn(int8_t v) { return v < 0 ? "null" : v ? "true" : "false"; }

// Список имён установленных битов двух байтов: ["...","..."]; неизвестный бит — "P1.b5" / "A2.b1".
void put_bits(W& w, const uint8_t* bytes, bool alarm) {
    w.put("[");
    bool first = true;
    for (int by = 0; by < 2; ++by)
        for (int bit = 7; bit >= 0; --bit) {
            if (!(bytes[by] >> bit & 1)) continue;
            const char* nm = alarm ? pylon_alarm_name(by, bit) : pylon_prot_name(by, bit);
            if (nm) w.put("%s\"%s\"", first ? "" : ",", nm);
            else w.put("%s\"%s%d.b%d\"", first ? "" : ",", alarm ? "A" : "P", by + 1, bit);
            first = false;
        }
    w.put("]");
}

void put_age(W& w, bool have, uint32_t t, uint32_t now) {
    if (have) w.put("%lu", (unsigned long)can_age_ms(t, now)); else w.put("null");
}
void put_stale(W& w, bool have, uint32_t t, uint32_t now) { w.put("%s", have ? tf(!can_fresh(have, t, now)) : "null"); }

}  // namespace

size_t battery_brief_json(const BatteryBrief& b, char* out, size_t cap) {
    char v[16], i[16], s[16], ia[16];
    numstr(v, sizeof v, b.v, "%.2f"); numstr(i, sizeof i, b.i, "%.1f"); numstr(s, sizeof s, b.soc, "%.0f"); numstr(ia, sizeof ia, b.i_abs, "%.1f");
    char age[16]; if (b.age_s >= 0) snprintf(age, sizeof age, "%ld", (long)b.age_s); else snprintf(age, sizeof age, "null");
    char cm[3][16];
    const int32_t cv[3] = {b.cell_min_mv, b.cell_max_mv, b.cell_delta_mv};
    for (int k = 0; k < 3; ++k) { if (cv[k] >= 0) snprintf(cm[k], 16, "%ld", (long)cv[k]); else snprintf(cm[k], 16, "null"); }
    W w(out, cap);
    w.put("{\"v\":%s,\"i\":%s,\"soc\":%s,\"charge_en\":%s,\"discharge_en\":%s,\"alarm\":%s,\"age_s\":%s,\"i_abs\":%s,\"protocol\":%s%s%s,\"cell_min_mv\":%s,\"cell_max_mv\":%s,\"cell_delta_mv\":%s}",
          v, i, s, tfn(b.charge_en), tfn(b.discharge_en), tfn(b.alarm), age, ia,
          b.protocol ? "\"" : "", b.protocol ? b.protocol : "null", b.protocol ? "\"" : "", cm[0], cm[1], cm[2]);
    if (!w.ok) { if (cap) out[0] = 0; return 0; }
    return w.n;
}

size_t can_to_json(const CanDriverInfo& d, const CanStats& st, const PylonBattery& b, uint32_t now, char* out, size_t cap) {
    return can_to_json(d, st, b, SeplosBattery(), now, out, cap);
}

size_t can_to_json(const CanDriverInfo& d, const CanStats& st, const PylonBattery& b, const SeplosBattery& sp, uint32_t now, char* out, size_t cap) {
    return can_to_json(d, st, b, sp, Seplos500Battery(), now, out, cap);
}

namespace {
// Блок battery для seplos_500k (после "stale_after_ms"). Уверенность: УВЕРЕННО — cells_mv, cell_min/max_mv, v, soc, soh;
// СРЕДНЕ — temp_c, i_raw (знак не подтверждён), charge_v_max; guess — остальное (см. README). Поля, которых нет в кадрах, — null.
void put_seplos500(W& w, const Seplos500Battery& q, uint32_t now) {
    const bool fl = can_fresh(q.limits.have, q.limits.t_ms, now), fp = can_fresh(q.pack.have, q.pack.t_ms, now),
               fm = can_fresh(q.mm.have, q.mm.t_ms, now), fc = can_fresh(q.cap.have, q.cap.t_ms, now),
               ft = can_fresh(q.temps.have, q.temps.t_ms, now), ff = can_fresh(q.flags.have, q.flags.t_ms, now),
               fu = can_fresh(q.unk320.have, q.unk320.t_ms, now);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    char c[14][16];
    numstr(c[0], 16, fl ? q.limits.charge_v_limit : nan, "%.1f");
    numstr(c[1], 16, fl ? q.limits.charge_a : nan, "%.1f");
    numstr(c[2], 16, fl ? q.limits.discharge_a : nan, "%.1f");
    numstr(c[3], 16, fp ? (float)q.pack.soc : nan, "%.0f");
    numstr(c[4], 16, fp ? (float)q.pack.soh : nan, "%.0f");
    numstr(c[5], 16, fp ? q.pack.v : nan, "%.2f");
    numstr(c[6], 16, fp ? q.pack.i_raw : nan, "%.1f");
    numstr(c[7], 16, fp ? std::fabs(q.pack.i_raw) : nan, "%.1f");
    numstr(c[8], 16, fp ? q.pack.temp_c : nan, "%.1f");
    numstr(c[9], 16, ft ? q.temps.max_c : nan, "%.1f");
    numstr(c[10], 16, ft ? q.temps.min_c : nan, "%.1f");
    numstr(c[11], 16, fc ? q.cap.remaining_ah : nan, "%.2f");
    numstr(c[12], 16, fc ? q.cap.full_ah : nan, "%.2f");
    if (fc) snprintf(c[13], 16, "%u", (unsigned)q.cap.cycles); else snprintf(c[13], 16, "null");
    w.put(",\"protocol\":\"seplos_500k\",\"confidence\":\"partial\",\"charge_v_max\":%s,\"charge_a_max\":%s,\"discharge_a_max\":%s,\"discharge_v_min\":null,"
          "\"soc\":%s,\"soh\":%s,\"soc10\":null,\"v\":%s,\"i\":null,\"i_raw\":%s,\"i_abs\":%s,\"temp_c\":%s,\"temp_max_c\":%s,\"temp_min_c\":%s,"
          "\"cap_remaining_ah\":%s,\"cap_full_ah\":%s,\"cycles\":%s,",
          c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13]);
    // 16 ячеек: null вместо числа для ячеек без свежего кадра; весь массив null, если нет ни одной
    bool anyc = false;
    for (size_t i = 0; i < 16; ++i) if (q.cell_ok(i, now)) anyc = true;
    w.put("\"cells_mv\":");
    if (anyc) {
        w.put("[");
        for (size_t i = 0; i < 16; ++i) { if (i) w.put(","); if (q.cell_ok(i, now)) w.put("%u", (unsigned)q.cells.mv[i]); else w.put("null"); }
        w.put("]");
    } else w.put("null");
    int mn = 0, mx = 0;
    if (q.cell_range(now, &mn, &mx)) w.put(",\"cell_min_mv\":%d,\"cell_max_mv\":%d,\"cell_delta_mv\":%d", mn, mx, mx - mn);
    else w.put(",\"cell_min_mv\":null,\"cell_max_mv\":null,\"cell_delta_mv\":null");
    w.put(",\"charge_en\":null,\"discharge_en\":null,\"force_charge\":null,\"force_charge2\":null,\"full_charge_req\":null,\"req_raw\":null,"
          "\"alarm\":null,\"protect\":null,\"alarms\":null,\"prot_raw\":null,\"modules\":null,\"mfr\":null,\"flags_raw\":");
    if (ff) { w.put("\""); for (int k = 0; k < 8; ++k) w.put("%s%02X", k ? " " : "", (unsigned)q.flags.raw[k]); w.put("\""); } else w.put("null");
    // сырьё неизвестных полей (только свежее)
    w.put(",\"unk\":{\"f311\":");
    if (fl) w.put("\"%02X %02X\"", (unsigned)(q.limits.unk >> 8), (unsigned)(q.limits.unk & 0xFF)); else w.put("null");
    w.put(",\"f314_cnt\":"); if (fc) w.put("%u", (unsigned)q.cap.counter); else w.put("null");
    w.put(",\"f319\":"); if (fm) w.put("\"%02X | %02X %02X\"", (unsigned)q.mm.b0, (unsigned)q.mm.idx[0], (unsigned)q.mm.idx[1]); else w.put("null");
    w.put(",\"f320\":");
    if (fu) { w.put("\""); for (int k = 0; k < 8; ++k) w.put("%s%02X", k ? " " : "", (unsigned)q.unk320.raw[k]); w.put("\""); } else w.put("null");
    w.put(",\"f322\":");
    if (ft) w.put("\"%02X %02X %02X %02X\"", (unsigned)q.temps.unk[0], (unsigned)q.temps.unk[1], (unsigned)q.temps.unk[2], (unsigned)q.temps.unk[3]); else w.put("null");
    // возрасты/устаревание по группам кадров; ячейки: по самому старому из полученных кадров 0x315..0x318
    bool anyh = false; uint32_t oldest = 0;
    for (size_t g = 0; g < 4; ++g) if (q.cells.have[g]) { uint32_t a = can_age_ms(q.cells.t_ms[g], now); if (!anyh || a > oldest) oldest = a; anyh = true; }
    w.put("},\"age_ms\":{\"cells\":"); if (anyh) w.put("%lu", (unsigned long)oldest); else w.put("null");
    w.put(",\"pack\":"); put_age(w, q.pack.have, q.pack.t_ms, now);
    w.put(",\"minmax\":"); put_age(w, q.mm.have, q.mm.t_ms, now);
    w.put(",\"limits\":"); put_age(w, q.limits.have, q.limits.t_ms, now);
    w.put(",\"cap\":"); put_age(w, q.cap.have, q.cap.t_ms, now);
    w.put(",\"temps\":"); put_age(w, q.temps.have, q.temps.t_ms, now);
    w.put(",\"flags\":"); put_age(w, q.flags.have, q.flags.t_ms, now);
    w.put("},\"stale\":{\"cells\":"); if (anyh) w.put("%s", tf(oldest > kCanStaleMs)); else w.put("null");
    w.put(",\"pack\":"); put_stale(w, q.pack.have, q.pack.t_ms, now);
    w.put(",\"minmax\":"); put_stale(w, q.mm.have, q.mm.t_ms, now);
    w.put(",\"limits\":"); put_stale(w, q.limits.have, q.limits.t_ms, now);
    w.put(",\"cap\":"); put_stale(w, q.cap.have, q.cap.t_ms, now);
    w.put(",\"temps\":"); put_stale(w, q.temps.have, q.temps.t_ms, now);
    w.put(",\"flags\":"); put_stale(w, q.flags.have, q.flags.t_ms, now);
    w.put("},\"pn_tag\":null}}");
}
}  // namespace

size_t can_to_json(const CanDriverInfo& d, const CanStats& st, const PylonBattery& b, const SeplosBattery& sp, const Seplos500Battery& s5, uint32_t now, char* out, size_t cap) {
    W w(out, cap);
    w.put("{\"enabled\":%s,\"speed_kbps\":%u,\"mode\":\"%s\",\"driver\":{\"state\":\"%s\",\"rx_msgs\":%lu,\"rx_missed\":%lu,"
          "\"bus_errors\":%lu,\"bus_off_count\":%lu,\"rx_err_counter\":%lu},\"frames_total\":%lu,\"ids_overflow\":%lu,\"ids\":[",
          tf(d.enabled), (unsigned)d.speed_kbps, d.hb ? "normal_ack_heartbeat" : d.ack ? "normal_ack" : "listen_only", d.state, (unsigned long)d.rx_msgs, (unsigned long)d.rx_missed,
          (unsigned long)d.bus_errors, (unsigned long)d.bus_off_count, (unsigned long)d.rx_err_counter,
          (unsigned long)st.total(), (unsigned long)st.overflow());
    for (size_t k = 0; k < st.size(); ++k) {
        const CanIdStat& s = st.at(k);
        char idh[16], data[17] = "";
        if (s.ext) snprintf(idh, sizeof idh, "0x%08lX", (unsigned long)s.id); else snprintf(idh, sizeof idh, "0x%03lX", (unsigned long)s.id);
        for (unsigned j = 0; j < s.dlc && j < 8; ++j) snprintf(data + j * 2, 3, "%02X", s.data[j]);
        w.put("%s{\"id\":\"%s\",\"ext\":%s,\"count\":%lu,\"period_ms\":", k ? "," : "", idh, tf(s.ext), (unsigned long)s.count);
        if (s.period_ms) w.put("%lu", (unsigned long)s.period_ms); else w.put("null");
        w.put(",\"age_ms\":%lu,\"dlc\":%u,\"data\":\"%s\"}", (unsigned long)can_age_ms(s.last_ms, now), (unsigned)s.dlc, data);
    }
    w.put("],\"battery\":{\"stale_after_ms\":%lu", (unsigned long)kCanStaleMs);
    const int ap = active_protocol(b, sp, s5);
    if (ap == 2) {
        put_seplos500(w, s5, now);
        if (!w.ok) { if (cap) out[0] = 0; return 0; }
        return w.n;
    }
    if (ap == 1) {
        // Seplos 100k: все раскладки — гипотезы (confidence = "guess"); поля, которых в этих кадрах нет, — null
        const bool fl = can_fresh(sp.limits.have, sp.limits.t_ms, now), fs = can_fresh(sp.soc.have, sp.soc.t_ms, now),
                   fp = can_fresh(sp.pack.have, sp.pack.t_ms, now), fx = can_fresh(sp.prot.have, sp.prot.t_ms, now);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        char c[10][16];
        numstr(c[0], 16, fl ? sp.limits.charge_v : nan, "%.1f");
        numstr(c[1], 16, fl ? sp.limits.charge_a : nan, "%.1f");
        numstr(c[2], 16, fl ? sp.limits.discharge_a : nan, "%.1f");
        numstr(c[3], 16, fl ? sp.limits.discharge_v_min : nan, "%.1f");
        numstr(c[4], 16, fs ? (float)sp.soc.soc : nan, "%.0f");
        numstr(c[5], 16, fs ? (float)sp.soc.soh : nan, "%.0f");
        numstr(c[6], 16, fp ? sp.pack.v : nan, "%.2f");
        numstr(c[7], 16, fp ? sp.pack.i_abs : nan, "%.1f");
        numstr(c[8], 16, fp ? sp.pack.temp_c : nan, "%.0f");
        if (fs) snprintf(c[9], 16, "%d", sp.soc.soc10); else snprintf(c[9], 16, "null");
        w.put(",\"protocol\":\"seplos_100k\",\"confidence\":\"guess\",\"charge_v_max\":%s,\"charge_a_max\":%s,\"discharge_a_max\":%s,\"discharge_v_min\":%s,"
              "\"soc\":%s,\"soh\":%s,\"soc10\":%s,\"v\":%s,\"i\":null,\"i_abs\":%s,\"temp_c\":%s,"
              "\"charge_en\":null,\"discharge_en\":null,\"force_charge\":null,\"force_charge2\":null,\"full_charge_req\":null,\"req_raw\":null",
              c[0], c[1], c[2], c[3], c[4], c[5], c[9], c[6], c[7], c[8]);
        w.put(",\"alarm\":%s,\"protect\":null,\"alarms\":null,\"prot_raw\":", fx ? tf(sp.prot.alarm) : "null");
        if (fx) { w.put("\""); for (int k = 0; k < 8; ++k) w.put("%s%02X", k ? " " : "", (unsigned)sp.prot.raw[k]); w.put("\""); } else w.put("null");
        w.put(",\"modules\":null,\"mfr\":null,\"age_ms\":{\"limits\":"); put_age(w, sp.limits.have, sp.limits.t_ms, now);
        w.put(",\"soc\":"); put_age(w, sp.soc.have, sp.soc.t_ms, now);
        w.put(",\"pack\":"); put_age(w, sp.pack.have, sp.pack.t_ms, now);
        w.put(",\"req\":null,\"prot\":"); put_age(w, sp.prot.have, sp.prot.t_ms, now);
        w.put(",\"mfr\":null},\"stale\":{\"limits\":"); put_stale(w, sp.limits.have, sp.limits.t_ms, now);
        w.put(",\"soc\":"); put_stale(w, sp.soc.have, sp.soc.t_ms, now);
        w.put(",\"pack\":"); put_stale(w, sp.pack.have, sp.pack.t_ms, now);
        w.put(",\"req\":null,\"prot\":"); put_stale(w, sp.prot.have, sp.prot.t_ms, now);
        w.put(",\"mfr\":null},\"pn_tag\":null}}");
        if (!w.ok) { if (cap) out[0] = 0; return 0; }
        return w.n;
    }
    w.put(",\"protocol\":%s,\"confidence\":null,\"soc10\":null,\"i_abs\":null", ap == 0 ? "\"pylon\"" : "null");

    const bool fl = can_fresh(b.limits.have, b.limits.t_ms, now), fs = can_fresh(b.soc.have, b.soc.t_ms, now),
               fp = can_fresh(b.pack.have, b.pack.t_ms, now), fr = can_fresh(b.req.have, b.req.t_ms, now),
               fx = can_fresh(b.prot.have, b.prot.t_ms, now), fm = can_fresh(b.mfr.have, b.mfr.t_ms, now);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    char c1[16], c2[16], c3[16], c4[16], c5[16], c6[16], c7[16], c8[16], c9[16];
    numstr(c1, sizeof c1, fl ? b.limits.charge_v : nan, "%.1f");
    numstr(c2, sizeof c2, fl ? b.limits.charge_a : nan, "%.1f");
    numstr(c3, sizeof c3, fl ? b.limits.discharge_a : nan, "%.1f");
    numstr(c4, sizeof c4, fl ? b.limits.discharge_v_min : nan, "%.1f");
    numstr(c5, sizeof c5, fs ? (float)b.soc.soc : nan, "%.0f");
    numstr(c6, sizeof c6, fs ? (float)b.soc.soh : nan, "%.0f");
    numstr(c7, sizeof c7, fp ? b.pack.v : nan, "%.2f");
    numstr(c8, sizeof c8, fp ? b.pack.i : nan, "%.1f");
    numstr(c9, sizeof c9, fp ? b.pack.temp_c : nan, "%.1f");
    w.put(",\"charge_v_max\":%s,\"charge_a_max\":%s,\"discharge_a_max\":%s,\"discharge_v_min\":%s,\"soc\":%s,\"soh\":%s,"
          "\"v\":%s,\"i\":%s,\"temp_c\":%s", c1, c2, c3, c4, c5, c6, c7, c8, c9);
    // МОСФЕТЫ BMS: разрешения заряда/разряда из 0x35C
    w.put(",\"charge_en\":%s,\"discharge_en\":%s,\"force_charge\":%s,\"force_charge2\":%s,\"full_charge_req\":%s,\"req_raw\":",
          fr ? tf(b.req.charge_en) : "null", fr ? tf(b.req.discharge_en) : "null", fr ? tf(b.req.force1) : "null",
          fr ? tf(b.req.force2) : "null", fr ? tf(b.req.full_req) : "null");
    if (fr) w.put("\"0x%02X\"", (unsigned)b.req.raw); else w.put("null");
    // аварии: protect — отключения BMS (байты 0-1 кадра 0x359), alarms — предупреждения (байты 2-3)
    w.put(",\"alarm\":%s,\"protect\":", fx ? tf((b.prot.prot1 | b.prot.prot2 | b.prot.alarm1 | b.prot.alarm2) != 0) : "null");
    if (fx) { const uint8_t p[2] = {b.prot.prot1, b.prot.prot2}; put_bits(w, p, false); } else w.put("null");
    w.put(",\"alarms\":");
    if (fx) { const uint8_t a[2] = {b.prot.alarm1, b.prot.alarm2}; put_bits(w, a, true); } else w.put("null");
    w.put(",\"prot_raw\":");
    if (fx) w.put("\"%02X %02X %02X %02X\",\"modules\":%u", (unsigned)b.prot.prot1, (unsigned)b.prot.prot2, (unsigned)b.prot.alarm1, (unsigned)b.prot.alarm2, (unsigned)b.prot.modules);
    else w.put("null,\"modules\":null");
    w.put(",\"mfr\":");
    if (fm) w.put("\"%s\"", b.mfr.name); else w.put("null");
    w.put(",\"age_ms\":{\"limits\":"); put_age(w, b.limits.have, b.limits.t_ms, now);
    w.put(",\"soc\":"); put_age(w, b.soc.have, b.soc.t_ms, now);
    w.put(",\"pack\":"); put_age(w, b.pack.have, b.pack.t_ms, now);
    w.put(",\"req\":"); put_age(w, b.req.have, b.req.t_ms, now);
    w.put(",\"prot\":"); put_age(w, b.prot.have, b.prot.t_ms, now);
    w.put(",\"mfr\":"); put_age(w, b.mfr.have, b.mfr.t_ms, now);
    w.put("},\"stale\":{\"limits\":"); put_stale(w, b.limits.have, b.limits.t_ms, now);
    w.put(",\"soc\":"); put_stale(w, b.soc.have, b.soc.t_ms, now);
    w.put(",\"pack\":"); put_stale(w, b.pack.have, b.pack.t_ms, now);
    w.put(",\"req\":"); put_stale(w, b.req.have, b.req.t_ms, now);
    w.put(",\"prot\":"); put_stale(w, b.prot.have, b.prot.t_ms, now);
    w.put(",\"mfr\":"); put_stale(w, b.mfr.have, b.mfr.t_ms, now);
    w.put("},\"pn_tag\":%s}}", b.prot.have ? tf(b.prot.pn) : "null");
    if (!w.ok) { if (cap) out[0] = 0; return 0; }
    return w.n;
}

}  // namespace gs
