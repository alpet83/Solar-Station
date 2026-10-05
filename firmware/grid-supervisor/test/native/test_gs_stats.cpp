#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "gs_stats.h"
#include "gs_modbus.h"

using namespace gs;
static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool json_ok(const std::string& s) {
    int depth = 0; bool str = false; char prev = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (str) { if (c == '\\') { ++i; continue; } if (c == '"') str = false; prev = c; continue; }
        if (c == '"') str = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') { if (--depth < 0 || prev == ',') return false; }
        prev = c;
    }
    return depth == 0 && !str;
}

static const int64_t kDay0 = 1790000000LL - (1790000000LL % 86400);   // начало суток UTC

struct Rig {
    PvStats st; uint32_t t = 1000; int64_t ep = kDay0 + 6 * 3600;
    void step(uint32_t dt_s, int rel, float w = 0, float pv = 0, float a = 0) {
        t += dt_s * 1000u; ep += dt_s;
        PvSample s; s.t_ms = t; s.epoch_s = ep; s.pv_relay = (int8_t)rel; s.chg_w = w; s.pv_v = pv; s.chg_a = a; s.pack_v = 53.0f;
        st.feed(s);
    }
};

static void test_baseline_no_event() {
    Rig r; r.step(10, 0); r.step(10, 0);
    CHECK(r.st.event_count() == 0 && !r.st.pv_on());
    PvDay d; CHECK(r.st.day(0, d) && d.on_edges == 0 && d.samples == 2);
}

static void test_flap_and_stable() {
    Rig r; r.step(5, 0);
    // два коротких включения (по 20 с), затем устойчивое (15 мин), затем выключение
    r.step(5, 1, 30, 120, 0.5f); r.step(15, 1, 30, 120); r.step(5, 0);
    r.step(30, 0);
    r.step(5, 1, 40, 125, 0.7f); r.step(15, 1, 40, 125); r.step(5, 0);
    r.step(60, 0);
    r.step(5, 1, 100, 140, 2.0f);
    for (int i = 0; i < 90; ++i) r.step(10, 1, 300, 150, 5.0f);          // 900 с
    PvDay d; CHECK(r.st.day(0, d));
    CHECK(d.on_edges == 3 && d.short_runs == 2);
    CHECK(d.first_on == kDay0 + 6 * 3600 + 10);
    CHECK(d.stable_start != 0 && d.stable_start > d.first_on);
    CHECK(fabsf(d.start_pv_v - 140.0f) < 0.01f && fabsf(d.start_chg_w - 100.0f) < 0.01f);   // значения на момент начала включения
    CHECK(r.st.pv_on() && r.st.run_s(r.t) >= 890);
    r.step(10, 0);
    CHECK(r.st.day(0, d) && d.last_off == r.ep && d.short_runs == 2 && d.stable_end == r.ep);
    PvEvent e; CHECK(r.st.event(0, e) && e.on == 0 && e.run_s >= 900);     // самое новое — выключение
    CHECK(r.st.event_count() == 6);
    CHECK(d.max_chg_w == 300.0f && d.max_pv_v == 150.0f);
}

static void test_short_run_boundary() {
    Rig r; r.step(5, 0);
    PvStatsCfg c; c.stable_s = 60; r.st.set_config(c);
    r.step(5, 1, 10, 100);
    r.step(59, 1, 10, 100); r.step(1, 0);       // ровно 60 с (включено на 5 с раньше) -> устойчиво
    PvDay d; r.st.day(0, d);
    CHECK(d.short_runs == 0 && d.stable_start != 0);
}

static void test_relay_unknown_uses_power() {
    Rig r; r.step(5, -1, 0); r.step(5, -1, 50, 100); CHECK(r.st.pv_on());
    r.step(5, -1, 3, 100); CHECK(!r.st.pv_on());
}

static void test_energy_and_gap_cap() {
    Rig r; r.step(1, 1, 360);           // база
    r.step(10, 1, 360);                 // 360 Вт * 10 с = 1 Вт*ч
    PvDay d; r.st.day(0, d); CHECK(fabsf(d.chg_wh - 1.0f) < 0.01f);
    r.step(3600, 1, 360);               // пропуск данных: шаг режется до max_dt_s = 300 с -> +30 Вт*ч
    r.st.day(0, d); CHECK(fabsf(d.chg_wh - 31.0f) < 0.05f);
}

static void test_day_rollover() {
    Rig r; r.ep = kDay0 + 86400 - 20; r.step(5, 1, 100, 120);
    r.step(5, 1, 100, 120);             // всё ещё в тех же сутках
    r.step(20, 1, 100, 120);            // следующие сутки
    CHECK(r.st.day_count() == 2);
    PvDay d0, d1; r.st.day(0, d0); r.st.day(1, d1);
    CHECK(d0.day == d1.day + 1);
    CHECK(d1.on_edges == 0);            // начало в базе (событие не создавалось)
    for (int k = 0; k < 20; ++k) { r.step(86400 / 2, 0); r.step(86400 / 2, 0); }
    CHECK(r.st.day_count() == kPvDays);
}

static void test_unsynced_time() {
    PvStats st; PvSample s; s.t_ms = 1000; s.epoch_s = -1; s.pv_relay = 0; st.feed(s);
    s.t_ms = 2000; s.pv_relay = 1; s.chg_w = 20; s.pv_v = 100; st.feed(s);
    PvDay d; CHECK(st.day(0, d) && d.day == 0 && d.on_edges == 1 && d.first_on == 0);
    PvEvent e; CHECK(st.event(0, e) && e.epoch_s == 0 && e.on == 1);
    s.t_ms = 3000; s.epoch_s = kDay0 + 100; st.feed(s);      // время синхронизировалось -> новые сутки, без падений
    CHECK(st.day_count() == 2);
}

static void test_nan_inputs() {
    PvStats st; PvSample s; s.t_ms = 1000; s.epoch_s = kDay0; s.pv_relay = 1; s.chg_w = NAN; s.pv_v = INFINITY; s.chg_a = NAN; s.pack_v = NAN; st.feed(s);
    s.t_ms = 2000; s.pv_relay = 0; st.feed(s); s.t_ms = 3000; s.pv_relay = 1; st.feed(s);
    char js[6000]; size_t n = st.to_json(3000, js, sizeof js);
    CHECK(n > 0 && json_ok(js) && !strstr(js, "nan") && !strstr(js, "inf"));
}

static void test_json_size() {
    Rig r; r.step(5, 0);
    for (int i = 0; i < 200; ++i) { r.step(3, i & 1, 12345.0f, 999.9f, 123.4f); }
    for (int d = 0; d < 20; ++d) { r.step(86400, 1, 5000.0f); }
    char js[6000]; size_t n = r.st.to_json(r.t, js, sizeof js);
    CHECK(n > 0 && json_ok(js));
    printf("pvstats worst case: %zu bytes\n", n);
    char small[100]; CHECK(r.st.to_json(r.t, small, sizeof small) == 0);   // не влезло -> 0, без переполнения
}

static void test_decode_chg_fields() {
    uint16_t r[22] = {0}; r[2] = 3; r[4] = 1234; r[5] = 536; r[6] = 87; r[7] = 465; r[11] = 1;
    Telemetry t; decode_chg(r, t);
    CHECK(fabsf(t.pv_v - 123.4f) < 0.01f && fabsf(t.pack_v_alt - 53.6f) < 0.01f && fabsf(t.chg_curr_a - 8.7f) < 0.01f);
    CHECK(t.chg_w == 465.0f && t.pv_relay == 1 && t.chg_state == 3 && t.chg_ok);
    r[11] = 7; Telemetry t2; decode_chg(r, t2); CHECK(t2.pv_relay == -1);
}

int main() {
    test_baseline_no_event(); test_flap_and_stable(); test_short_run_boundary(); test_relay_unknown_uses_power();
    test_energy_and_gap_cap(); test_day_rollover(); test_unsynced_time(); test_nan_inputs(); test_json_size(); test_decode_chg_fields();
    printf("test_gs_stats: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
