#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "gs_can.h"
#include "gs_util.h"

using namespace gs;
static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static CanFrame fr(uint32_t id, uint32_t t, std::initializer_list<int> b, bool ext = false) {
    CanFrame f; f.id = id; f.t_ms = t; f.ext = ext; f.dlc = (uint8_t)b.size();
    size_t i = 0; for (int x : b) f.data[i++] = (uint8_t)x;
    return f;
}
static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

// Грубая проверка JSON: скобки/кавычки сбалансированы, нет запятых перед закрывающими скобками.
static bool json_ok(const std::string& s) {
    int depth = 0; bool str = false; char prev = 0;
    for (char c : s) {
        if (str) { if (c == '"') str = false; prev = c; continue; }
        if (c == '"') str = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') { if (--depth < 0 || prev == ',') return false; }
        prev = c;
    }
    return depth == 0 && !str;
}

// Эталонные кадры (значения посчитаны вручную):
//  0x351: 53.2 В=532=0x0214, 50.0 А=500=0x01F4, 120.0 А=1200=0x04B0, 44.0 В=440=0x01B8
//  0x355: SOC 87=0x57, SOH 100=0x64
//  0x356: 52.34 В=5234=0x1472, -3.2 А=-32=0xFFE0, 25.3 °C=253=0x00FD
static const CanFrame k351 = fr(0x351, 1000, {0x14, 0x02, 0xF4, 0x01, 0xB0, 0x04, 0xB8, 0x01});
static const CanFrame k355 = fr(0x355, 1000, {0x57, 0x00, 0x64, 0x00});
static const CanFrame k356 = fr(0x356, 1000, {0x72, 0x14, 0xE0, 0xFF, 0xFD, 0x00});
static const CanFrame k359 = fr(0x359, 1000, {0x06, 0x01, 0x80, 0x08, 0x03, 'P', 'N'});
static const CanFrame k35c = fr(0x35C, 1000, {0xC0, 0x00});
static const CanFrame k35e = fr(0x35E, 1000, {'P', 'Y', 'L', 'O', 'N', ' ', ' ', ' '});

static void test_decode() {
    PylonBattery b;
    CHECK(b.feed(k351) && b.limits.have);
    CHECK(near(b.limits.charge_v, 53.2f) && near(b.limits.charge_a, 50.0f) && near(b.limits.discharge_a, 120.0f) && near(b.limits.discharge_v_min, 44.0f));
    CHECK(b.feed(k355) && b.soc.soc == 87 && b.soc.soh == 100);
    CHECK(b.feed(k356) && near(b.pack.v, 52.34f) && near(b.pack.i, -3.2f) && near(b.pack.temp_c, 25.3f));
    CHECK(b.feed(k359) && b.prot.prot1 == 0x06 && b.prot.prot2 == 0x01 && b.prot.alarm1 == 0x80 && b.prot.alarm2 == 0x08 && b.prot.modules == 3 && b.prot.pn);
    CHECK(b.feed(k35c) && b.req.charge_en && b.req.discharge_en && !b.req.force1 && !b.req.force2 && !b.req.full_req && b.req.raw == 0xC0);
    CHECK(b.feed(k35e) && !strcmp(b.mfr.name, "PYLON"));

    // положительный ток и отрицательная температура: 12.0 А = 120 = 0x0078, -5.0 °C = -50 = 0xFFCE
    CHECK(b.feed(fr(0x356, 2000, {0x72, 0x14, 0x78, 0x00, 0xCE, 0xFF})) && near(b.pack.i, 12.0f) && near(b.pack.temp_c, -5.0f) && b.pack.t_ms == 2000);
    // флаги: только заряд; разряд запрещён, принудительный заряд 1 и полный заряд
    CHECK(b.feed(fr(0x35C, 2000, {0x80, 0x00})) && b.req.charge_en && !b.req.discharge_en);
    CHECK(b.feed(fr(0x35C, 2000, {0x28, 0x00})) && !b.req.charge_en && !b.req.discharge_en && b.req.force1 && b.req.full_req && !b.req.force2);
    CHECK(b.feed(fr(0x35C, 2000, {0x10, 0x00})) && b.req.force2);
    // 0x351 без поля нижнего напряжения разряда (dlc 6)
    CHECK(b.feed(fr(0x351, 2000, {0x14, 0x02, 0xF4, 0x01, 0xB0, 0x04})) && std::isnan(b.limits.discharge_v_min));
    // 0x359 короткий, без 'PN'
    CHECK(b.feed(fr(0x359, 2000, {0, 0, 0, 0, 1})) && !b.prot.pn && b.prot.modules == 1);
    // имя: непечатные/кавычки заменяются, пробелы справа убираются
    CHECK(b.feed(fr(0x35E, 2000, {'A', '"', 0x01, 'B', 0, 0, 0, 0})) && !strcmp(b.mfr.name, "A??B"));

    // короткие кадры, чужие ID и расширенный формат не разбираются
    PylonBattery e;
    CHECK(!e.feed(fr(0x351, 1, {1, 2, 3, 4, 5})));
    CHECK(!e.feed(fr(0x355, 1, {1, 2, 3})));
    CHECK(!e.feed(fr(0x356, 1, {1, 2, 3, 4, 5})));
    CHECK(!e.feed(fr(0x359, 1, {1, 2, 3, 4})));
    CHECK(!e.feed(fr(0x35C, 1, {})));
    CHECK(!e.feed(fr(0x305, 1, {0, 0, 0, 0, 0, 0, 0, 0})));
    CHECK(!e.feed(fr(0x351, 1, {0x14, 0x02, 0xF4, 0x01, 0xB0, 0x04, 0xB8, 0x01}, true)));
    CHECK(!e.limits.have && !e.soc.have && !e.pack.have && !e.prot.have && !e.req.have && !e.mfr.have);
}

static void test_alarm_names() {
    // Protection 1: бит1 перенапряжение, бит2 недонапряжение, бит3 перегрев, бит4 переохлаждение, бит7 ток разряда
    CHECK(!strcmp(pylon_prot_name(0, 1), "Cell or module over-voltage"));
    CHECK(!strcmp(pylon_prot_name(0, 2), "Cell or module under-voltage"));
    CHECK(!strcmp(pylon_prot_name(0, 3), "Cell or module over-temperature"));
    CHECK(!strcmp(pylon_prot_name(0, 4), "Cell or module under-temperature"));
    CHECK(!strcmp(pylon_prot_name(0, 7), "Discharge over-current"));
    CHECK(!strcmp(pylon_prot_name(1, 0), "Charge over-current"));
    CHECK(!strcmp(pylon_prot_name(1, 3), "System error") && !strcmp(pylon_prot_name(1, 7), "System error"));
    CHECK(!strcmp(pylon_alarm_name(0, 7), "Discharge high current"));
    CHECK(!strcmp(pylon_alarm_name(1, 0), "Charge high current"));
    CHECK(!strcmp(pylon_alarm_name(1, 3), "Internal communication fail"));
    CHECK(pylon_prot_name(0, 0) == nullptr && pylon_prot_name(0, 8) == nullptr && pylon_prot_name(2, 0) == nullptr && pylon_alarm_name(0, -1) == nullptr);
}

static void test_stale_and_brief() {
    PylonBattery b;
    BatteryBrief r = battery_brief(b, 5000);                       // кадров не было
    CHECK(std::isnan(r.v) && std::isnan(r.i) && std::isnan(r.soc) && r.charge_en < 0 && r.discharge_en < 0 && r.alarm < 0 && r.age_s < 0);
    b.feed(k356); b.feed(k355); b.feed(k35c); b.feed(k359);        // все с t=1000
    r = battery_brief(b, 1000 + 10000);                            // ровно 10 с — ещё свежее
    CHECK(near(r.v, 52.34f) && near(r.soc, 87) && r.charge_en == 1 && r.discharge_en == 1 && r.alarm == 1 && r.age_s == 10);
    r = battery_brief(b, 1000 + 10001);                            // старше 10 с — всё недостоверно
    CHECK(std::isnan(r.v) && std::isnan(r.i) && std::isnan(r.soc) && r.charge_en < 0 && r.alarm < 0 && r.age_s == 10);
    // разные возрасты: свежим остаётся только 0x356
    b.feed(fr(0x356, 20000, {0x72, 0x14, 0, 0, 0, 0}));
    r = battery_brief(b, 25000);
    CHECK(near(r.v, 52.34f) && std::isnan(r.soc) && r.charge_en < 0 && r.age_s == 5);
    // без аварий
    b.feed(fr(0x359, 24000, {0, 0, 0, 0, 1, 'P', 'N'}));
    CHECK(battery_brief(b, 25000).alarm == 0);
    // переход через 2^32 мс и «время из будущего»
    CHECK(can_age_ms(0xFFFFFFF0u, 0x10u) == 0x20);
    CHECK(can_age_ms(100, 90) == 0);
    CHECK(can_fresh(true, 0xFFFFFFF0u, 0x10u) && !can_fresh(false, 0, 0));
    char s[200];
    CHECK(battery_brief_json(battery_brief(b, 25000), s, sizeof s) > 0 && json_ok(s));
}

static void test_stats() {
    CanStats st;
    st.on_frame(fr(0x356, 1000, {1}));
    CHECK(st.size() == 1 && st.at(0).count == 1 && st.at(0).period_ms == 0);
    st.on_frame(fr(0x356, 2000, {2, 3}));
    CHECK(st.at(0).count == 2 && st.at(0).period_ms == 1000 && st.at(0).dlc == 2 && st.at(0).data[0] == 2 && st.at(0).data[1] == 3);
    st.on_frame(fr(0x356, 3000, {2}));
    CHECK(st.at(0).period_ms == 1000);
    st.on_frame(fr(0x356, 4400, {2}));                           // 1400 -> (3*1000+1400+2)/4 = 1100
    CHECK(st.at(0).period_ms == 1100);
    // сортировка по ID, стандартные и расширенные раздельно
    st.on_frame(fr(0x351, 4500, {1}));
    st.on_frame(fr(0x359, 4500, {1}));
    st.on_frame(fr(0x356, 4500, {1}, true));
    CHECK(st.size() == 4 && st.at(0).id == 0x351 && st.at(1).id == 0x356 && !st.at(1).ext && st.at(2).id == 0x359 && st.at(3).ext);
    CHECK(st.find(0x356, true) && st.find(0x356, true)->count == 1 && st.find(0x356, false)->count == 4 && !st.find(0x400, false));
    CHECK(st.total() == 7 && st.overflow() == 0);
    // период через границу 2^32
    CanStats w;
    w.on_frame(fr(0x100, 0xFFFFFF00u, {})); w.on_frame(fr(0x100, 0x00000100u, {}));
    CHECK(w.at(0).period_ms == 0x200);
    // переполнение таблицы: 24 ID, остальные считаются отдельно, известные продолжают обновляться
    CanStats o;
    for (uint32_t i = 0; i < kCanMaxIds; ++i) o.on_frame(fr(0x400 + i, 100, {1}));
    CHECK(o.size() == kCanMaxIds && o.overflow() == 0);
    o.on_frame(fr(0x7FF, 200, {1})); o.on_frame(fr(0x100, 200, {1}));
    CHECK(o.size() == kCanMaxIds && o.overflow() == 2 && o.total() == kCanMaxIds + 2 && !o.find(0x7FF, false));
    o.on_frame(fr(0x400, 300, {9}));
    CHECK(o.find(0x400, false)->count == 2 && o.at(0).id == 0x400);
    o.clear(); CHECK(o.size() == 0 && o.total() == 0);
}

static CanDriverInfo drv() {
    CanDriverInfo d; d.enabled = true; d.speed_kbps = 500; d.state = "running";
    d.rx_msgs = 123; d.rx_missed = 1; d.bus_errors = 2; d.bus_off_count = 3; d.rx_err_counter = 4;
    return d;
}

static void test_json() {
    CanStats st; PylonBattery b;
    for (const CanFrame* f : {&k351, &k355, &k356, &k359, &k35c, &k35e}) { st.on_frame(*f); b.feed(*f); }
    st.on_frame(fr(0x305, 1000, {0, 0, 0, 0, 0, 0, 0, 0}));
    static char js[kCanJsonMax];
    size_t n = can_to_json(drv(), st, b, 1500, js, sizeof js);
    CHECK(n > 0 && strlen(js) == n && json_ok(js));
    CHECK(strstr(js, "\"speed_kbps\":500") && strstr(js, "\"state\":\"running\"") && strstr(js, "\"rx_msgs\":123") && strstr(js, "\"rx_missed\":1") &&
          strstr(js, "\"bus_errors\":2") && strstr(js, "\"bus_off_count\":3"));
    CHECK(strstr(js, "{\"id\":\"0x351\",\"ext\":false,\"count\":1,\"period_ms\":null,\"age_ms\":500,\"dlc\":8,\"data\":\"1402F401B004B801\"}"));
    CHECK(strstr(js, "\"id\":\"0x305\""));                       // неизвестный ID виден в таблице
    CHECK(strstr(js, "\"charge_v_max\":53.2,\"charge_a_max\":50.0,\"discharge_a_max\":120.0,\"discharge_v_min\":44.0,\"soc\":87,\"soh\":100,\"v\":52.34,\"i\":-3.2,\"temp_c\":25.3"));
    CHECK(strstr(js, "\"charge_en\":true,\"discharge_en\":true,\"force_charge\":false") && strstr(js, "\"req_raw\":\"0xC0\""));
    CHECK(strstr(js, "\"alarm\":true,\"protect\":[\"Cell or module under-voltage\",\"Cell or module over-voltage\",\"Charge over-current\"]"));
    CHECK(strstr(js, "\"alarms\":[\"Discharge high current\",\"Internal communication fail\"]"));
    CHECK(strstr(js, "\"prot_raw\":\"06 01 80 08\",\"modules\":3") && strstr(js, "\"mfr\":\"PYLON\""));
    CHECK(strstr(js, "\"age_ms\":{\"limits\":500,") && strstr(js, "\"stale\":{\"limits\":false,"));

    // через 10.5 с данные недостоверны: значения и списки аварий -> null, возраст остаётся
    n = can_to_json(drv(), st, b, 11500, js, sizeof js);
    CHECK(n > 0 && json_ok(js));
    CHECK(strstr(js, "\"soc\":null,\"soh\":null,\"v\":null,\"i\":null,\"temp_c\":null") && strstr(js, "\"charge_en\":null,\"discharge_en\":null,\"force_charge\":null"));
    CHECK(strstr(js, "\"alarm\":null,\"protect\":null,\"alarms\":null,\"prot_raw\":null") && strstr(js, "\"mfr\":null,\"age_ms\""));
    CHECK(strstr(js, "\"age_ms\":{\"limits\":10500,") && strstr(js, "\"stale\":{\"limits\":true,"));

    // пустое состояние: ни кадров, драйвер выключен
    CanDriverInfo off; off.state = "off";
    n = can_to_json(off, CanStats(), PylonBattery(), 0, js, sizeof js);
    CHECK(n > 0 && json_ok(js) && strstr(js, "\"enabled\":false") && strstr(js, "\"ids\":[]") && strstr(js, "\"age_ms\":{\"limits\":null,") && strstr(js, "\"stale\":{\"limits\":null,"));

    // неизвестные биты получают имена вида P1.b5 / A2.b1; нулевые — пустые списки
    PylonBattery x; x.feed(fr(0x359, 1000, {0x21, 0x00, 0x00, 0x02, 1, 'P', 'N'}));
    n = can_to_json(drv(), CanStats(), x, 1000, js, sizeof js);
    CHECK(n > 0 && json_ok(js) && strstr(js, "\"protect\":[\"P1.b5\",\"P1.b0\"]") && strstr(js, "\"alarms\":[\"A2.b1\"]"));
    x.feed(fr(0x359, 1000, {0, 0, 0, 0, 1, 'P', 'N'}));
    can_to_json(drv(), CanStats(), x, 1000, js, sizeof js);
    CHECK(strstr(js, "\"alarm\":false,\"protect\":[],\"alarms\":[]"));

    // худший случай по размеру: 24 ID, расширенные, все биты аварий, все флаги
    CanStats big; PylonBattery bb;
    for (uint32_t i = 0; i < kCanMaxIds; ++i) big.on_frame(fr(0x1FFFF000u + i, 1000 + i, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, true));
    for (const CanFrame* f : {&k351, &k355, &k356, &k35c, &k35e}) bb.feed(*f);
    bb.feed(fr(0x359, 1000, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 'P', 'N'}));
    bb.feed(fr(0x35C, 1000, {0xF8, 0}));
    CanDriverInfo bd = drv(); bd.rx_msgs = bd.rx_missed = bd.bus_errors = bd.bus_off_count = bd.rx_err_counter = 4294967295u; bd.state = "recovering";
    n = can_to_json(bd, big, bb, 2000, js, sizeof js);
    printf("can json worst case: %zu bytes (buffer %zu)\n", n, (size_t)kCanJsonMax);
    CHECK(n > 0 && json_ok(js) && n < kCanJsonMax - 300);

    // любой буфер короче результата -> 0 и пустая строка, без записи за границу (ASan)
    n = can_to_json(drv(), st, b, 1500, js, sizeof js);
    bool all0 = true;
    for (size_t cap = 0; cap < n + 1; ++cap) {
        std::string tmp(cap + 1, 'x'); tmp[cap] = '#';           // канарейка за буфером
        size_t r = can_to_json(drv(), st, b, 1500, &tmp[0], cap);
        if (cap <= n ? r != 0 : r != n) all0 = false;
        if (tmp[cap] != '#') all0 = false;
        if (cap && tmp[0] != 0) all0 = false;
    }
    CHECK(all0);
    CHECK(can_to_json(drv(), st, b, 1500, js, n + 1) == n);     // ровно впритык
    // то же для краткой сводки
    char bj[200]; BatteryBrief br = battery_brief(b, 1500);
    size_t m = battery_brief_json(br, bj, sizeof bj);
    CHECK(m > 0 && !strcmp(bj, "{\"v\":52.34,\"i\":-3.2,\"soc\":87,\"charge_en\":true,\"discharge_en\":true,\"alarm\":true,\"age_s\":0,\"i_abs\":null,\"protocol\":\"pylon\"}"));
    bool all1 = true;
    for (size_t cap = 0; cap <= m; ++cap) if (battery_brief_json(br, bj, cap) != 0) all1 = false;
    CHECK(all1);
    CHECK(battery_brief_json(BatteryBrief(), bj, sizeof bj) > 0 &&
          !strcmp(bj, "{\"v\":null,\"i\":null,\"soc\":null,\"charge_en\":null,\"discharge_en\":null,\"alarm\":null,\"age_s\":null,\"i_abs\":null,\"protocol\":null}"));
}

// ---- Seplos 100 кбит/с (29-битные ID, гипотезы по наблюдаемым кадрам) --------------------------------
static const CanFrame s100 = fr(0x00030100, 5000, {0x40, 0x02, 0x08, 0x07, 0xD0, 0x01, 0x08, 0x07}, true);
static const CanFrame s200 = fr(0x00030200, 5000, {0, 0, 0, 0, 0, 0, 0, 0}, true);
static const CanFrame s400 = fr(0x00030400, 5000, {0x1B, 0x02, 0x6F, 0x00, 0x1D, 0x00, 0x00, 0x00}, true);
static const CanFrame s500 = fr(0x00030500, 5000, {0x46, 0x00, 0x60, 0x00, 0xC1, 0x02, 0x00, 0x00}, true);

static void test_seplos() {
    SeplosBattery b;
    CHECK(b.feed(s100) && b.feed(s200) && b.feed(s400) && b.feed(s500));
    CHECK(near(b.limits.charge_v, 57.6f) && near(b.limits.charge_a, 180.0f) && near(b.limits.discharge_v_min, 46.4f) && near(b.limits.discharge_a, 180.0f));
    CHECK(near(b.pack.v, 53.9f) && near(b.pack.i_abs, 11.1f) && near(b.pack.temp_c, 29.0f));
    CHECK(b.soc.soc == 70 && b.soc.soh == 96 && b.soc.soc10 == 705);
    CHECK(b.prot.have && !b.prot.alarm && b.last_ms() == 5000 && b.any());

    // аварии: любой ненулевой байт 0x…0200
    CHECK(b.feed(fr(0x00030200, 6000, {0, 0, 0, 0, 0, 0, 0, 0x04}, true)) && b.prot.alarm && b.prot.raw[7] == 4 && b.prot.t_ms == 6000);
    // чужие/короткие/стандартные кадры не разбираются
    SeplosBattery e;
    CHECK(!e.feed(fr(0x00030300, 1, {1, 2, 3, 4, 5, 6, 7, 8}, true)));      // неизвестный ID семейства
    CHECK(!e.feed(fr(0x00040400, 1, {1, 2, 3, 4, 5, 6, 7, 8}, true)));
    CHECK(!e.feed(fr(0x00030401, 1, {1, 2, 3, 4, 5, 6, 7, 8}, true)));
    CHECK(!e.feed(fr(0x00030400, 1, {1, 2, 3, 4, 5}, true)));               // короткий
    CHECK(!e.feed(fr(0x00030100, 1, {1, 2, 3, 4, 5, 6, 7}, true)));
    CHECK(!e.feed(fr(0x00030400, 1, {0x1B, 0x02, 0x6F, 0x00, 0x1D, 0x00, 0, 0}, false)));   // не расширенный
    CHECK(!e.feed(fr(0x351, 1, {0x14, 0x02, 0xF4, 0x01, 0xB0, 0x04, 0xB8, 0x01})) && !e.any());
    // и Pylon не принимает кадры Seplos
    PylonBattery p; CHECK(!p.feed(s400) && !p.pack.have);

    // brief: v, soc, alarm; i = NaN, i_abs; charge_en/discharge_en неизвестны
    PylonBattery none;
    BatteryBrief r = battery_brief(none, b, 7000);
    CHECK(near(r.v, 53.9f) && near(r.soc, 70) && std::isnan(r.i) && near(r.i_abs, 11.1f) && r.charge_en < 0 && r.discharge_en < 0 && r.alarm == 1 && r.age_s == 1);
    CHECK(r.protocol && !strcmp(r.protocol, "seplos_100k"));
    char bj[220];
    CHECK(battery_brief_json(r, bj, sizeof bj) > 0 && json_ok(bj) &&
          !strcmp(bj, "{\"v\":53.90,\"i\":null,\"soc\":70,\"charge_en\":null,\"discharge_en\":null,\"alarm\":true,\"age_s\":1,\"i_abs\":11.1,\"protocol\":\"seplos_100k\"}"));
    // stale после 10 с (кадры 0x200 — 6000, остальные 5000)
    r = battery_brief(none, b, 5000 + 10000);
    CHECK(near(r.v, 53.9f) && r.alarm == 1);
    r = battery_brief(none, b, 5000 + 10001);
    CHECK(std::isnan(r.v) && std::isnan(r.soc) && std::isnan(r.i_abs) && r.alarm == 1 && r.age_s == 9);
    r = battery_brief(none, b, 6000 + 10001);
    CHECK(r.alarm < 0 && r.age_s == 10 && r.protocol);

    // /api/can
    CanStats st; for (const CanFrame* f : {&s100, &s200, &s400, &s500}) st.on_frame(*f);
    CanDriverInfo d; d.enabled = true; d.speed_kbps = 100; d.ack = true; d.state = "running";
    static char js[kCanJsonMax];
    size_t n = can_to_json(d, st, none, b, 7000, js, sizeof js);
    CHECK(n > 0 && json_ok(js));
    CHECK(strstr(js, "\"protocol\":\"seplos_100k\",\"confidence\":\"guess\",\"charge_v_max\":57.6,\"charge_a_max\":180.0,\"discharge_a_max\":180.0,\"discharge_v_min\":46.4,"));
    CHECK(strstr(js, "\"soc\":70,\"soh\":96,\"soc10\":705,\"v\":53.90,\"i\":null,\"i_abs\":11.1,\"temp_c\":29,"));
    CHECK(strstr(js, "\"charge_en\":null,\"discharge_en\":null") && strstr(js, "\"alarm\":true") && strstr(js, "\"prot_raw\":\"00 00 00 00 00 00 00 04\""));
    CHECK(strstr(js, "\"id\":\"0x00030400\",\"ext\":true"));
    printf("seplos /api/can json: %zu bytes (kCanJsonMax %zu)\n", n, (size_t)kCanJsonMax);
    // stale: значения null, alarm null
    n = can_to_json(d, st, none, b, 20000, js, sizeof js);
    CHECK(n > 0 && json_ok(js) && strstr(js, "\"v\":null,\"i\":null,\"i_abs\":null") && strstr(js, "\"alarm\":null") && strstr(js, "\"prot_raw\":null") && strstr(js, "\"pack\":true"));
    // граница буфера
    bool ok = true;
    std::string tmp(n + 2, 'x'); tmp[n + 1] = '#';
    ok = can_to_json(d, st, none, b, 20000, &tmp[0], n) == 0 && can_to_json(d, st, none, b, 20000, &tmp[0], n + 1) == n && tmp[n + 1] == '#';
    CHECK(ok);
    // Pylon-блок несёт protocol "pylon" и null-поля Seplos
    PylonBattery pb; pb.feed(k356);
    n = can_to_json(d, st, pb, SeplosBattery(), 1500, js, sizeof js);
    CHECK(n > 0 && json_ok(js) && strstr(js, "\"protocol\":\"pylon\",\"confidence\":null,\"soc10\":null,\"i_abs\":null"));
    n = can_to_json(d, st, none, SeplosBattery(), 1500, js, sizeof js);
    CHECK(n > 0 && json_ok(js) && strstr(js, "\"protocol\":null,\"confidence\":null"));
}

static void test_mixed() {
    // смесь двух протоколов: обе ветки кормятся одними кадрами, активной считается та, чей кадр позже
    PylonBattery p; SeplosBattery s; CanStats st;
    const CanFrame seq[] = {k351, s100, k356, s400, fr(0x123, 1, {1}), fr(0x00031234, 1, {1, 2}, true), s500, k355, s200, fr(0x351, 1, {1, 2, 3}, true)};
    for (const CanFrame& f : seq) { st.on_frame(f); p.feed(f); s.feed(f); }
    CHECK(st.size() == 10 && p.limits.have && p.pack.have && s.limits.have && s.pack.have);
    CHECK(st.find(0x00031234, true) && st.find(0x123, false));            // неизвестные ID — в таблице
    BatteryBrief r = battery_brief(p, s, 5500);                          // Pylon-кадры t=1000, Seplos t=5000 -> Seplos
    CHECK(r.protocol && !strcmp(r.protocol, "seplos_100k") && near(r.v, 53.9f));
    p.feed(fr(0x356, 6000, {0x72, 0x14, 0xE0, 0xFF, 0xFD, 0x00}));       // Pylon свежее
    r = battery_brief(p, s, 6500);
    CHECK(r.protocol && !strcmp(r.protocol, "pylon") && near(r.v, 52.34f) && near(r.i, -3.2f));
    static char js[kCanJsonMax];
    CanDriverInfo d; d.enabled = true; d.speed_kbps = 500;
    CHECK(can_to_json(d, st, p, s, 6500, js, sizeof js) > 0 && json_ok(js) && strstr(js, "\"protocol\":\"pylon\""));
    // Pylon-кадр со стандартным ID 0x351 не уходит в Seplos и наоборот
    SeplosBattery s2; PylonBattery p2;
    CHECK(p2.feed(k351) && !s2.feed(k351) && s2.feed(s100) && !p2.feed(s100));
    // худший случай размера: 24 ID + Seplos
    CanStats full;
    for (uint32_t i = 0; i < 24; ++i) full.on_frame(fr(0x00030000 + (i << 8) + 0x00, 1, {1, 2, 3, 4, 5, 6, 7, 8}, true));
    size_t n = can_to_json(d, full, p, s, 6500, js, sizeof js);
    CHECK(n > 0 && n < kCanJsonMax - 300);
    printf("can json, 24 ids + battery: %zu bytes (kCanJsonMax %zu)\n", n, (size_t)kCanJsonMax);
}

int main() {
    test_decode();
    test_alarm_names();
    test_stale_and_brief();
    test_stats();
    test_json();
    test_seplos();
    test_mixed();
    printf("can: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
