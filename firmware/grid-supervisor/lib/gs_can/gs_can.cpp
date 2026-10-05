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
BatteryBrief::BatteryBrief() : v(std::numeric_limits<float>::quiet_NaN()), i(v), soc(v), charge_en(-1), discharge_en(-1), alarm(-1), age_s(-1) {}

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
    if (any) r.age_s = (int32_t)(best / 1000);
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
    char v[16], i[16], s[16];
    numstr(v, sizeof v, b.v, "%.2f"); numstr(i, sizeof i, b.i, "%.1f"); numstr(s, sizeof s, b.soc, "%.0f");
    char age[16]; if (b.age_s >= 0) snprintf(age, sizeof age, "%ld", (long)b.age_s); else snprintf(age, sizeof age, "null");
    W w(out, cap);
    w.put("{\"v\":%s,\"i\":%s,\"soc\":%s,\"charge_en\":%s,\"discharge_en\":%s,\"alarm\":%s,\"age_s\":%s}",
          v, i, s, tfn(b.charge_en), tfn(b.discharge_en), tfn(b.alarm), age);
    if (!w.ok) { if (cap) out[0] = 0; return 0; }
    return w.n;
}

size_t can_to_json(const CanDriverInfo& d, const CanStats& st, const PylonBattery& b, uint32_t now, char* out, size_t cap) {
    W w(out, cap);
    w.put("{\"enabled\":%s,\"speed_kbps\":%u,\"mode\":\"listen_only\",\"driver\":{\"state\":\"%s\",\"rx_msgs\":%lu,\"rx_missed\":%lu,"
          "\"bus_errors\":%lu,\"bus_off_count\":%lu,\"rx_err_counter\":%lu},\"frames_total\":%lu,\"ids_overflow\":%lu,\"ids\":[",
          tf(d.enabled), (unsigned)d.speed_kbps, d.state, (unsigned long)d.rx_msgs, (unsigned long)d.rx_missed,
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
