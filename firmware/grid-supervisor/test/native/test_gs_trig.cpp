#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "gs_trig.h"

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

// Стенд: watch + очередь + журнал; снимок собирается руками.
struct Rig {
    BmsWatch w; TriggerQueue q; BmsEventLog log; uint32_t t = 100000;
    BmsSnapshot s;
    Rig() { s.frames_alive = true; }
    void cells(uint16_t mn, uint16_t mx) { s.cells_valid = true; s.cell_min_mv = mn; s.cell_max_mv = mx; }
    void field(size_t i, const char* name, std::initializer_list<int> b) {
        WatchField& f = s.watch[i]; strncpy(f.name, name, 5); f.valid = true; f.len = (uint8_t)b.size();
        size_t k = 0; for (int x : b) f.data[k++] = (uint8_t)x; s.flags_valid = true;
    }
    void tick(uint32_t dt = 1000) { t += dt; w.update(s, t, q, &log, 1790000000); }
    std::vector<Trigger> drain() { std::vector<Trigger> v; Trigger x; while (q.pop(x)) v.push_back(x); return v; }
};

static void test_tags() {
    CHECK(!strcmp(trig_tag(TrigKind::CellLowEnter), "CELL_LOW"));
    CHECK(!strcmp(trig_tag(TrigKind::CellHardEnter), "CELL_HARD"));
    CHECK(!strcmp(trig_tag(TrigKind::BmsFlagsChanged), "BMS_FLAGS"));
    CHECK(!strcmp(trig_tag(TrigKind::BmsStale), "BMS_STALE"));
    CHECK(!strcmp(trig_tag(TrigKind::BmsBack), "BMS_BACK"));
    for (int k = 0; k < (int)TrigKind::Count_; ++k) CHECK(strcmp(trig_tag((TrigKind)k), "UNKNOWN") != 0);
    CHECK(trig_severity(TrigKind::CellHardEnter) == TrigSeverity::Alert);
    CHECK(trig_severity(TrigKind::CellLowEnter) == TrigSeverity::Warn);
    CHECK(trig_severity(TrigKind::BmsFlagsChanged) == TrigSeverity::Warn);
    CHECK(trig_severity(TrigKind::CellLowExit) == TrigSeverity::Info);
    CHECK(trig_severity(TrigKind::BmsBack) == TrigSeverity::Info);
    CHECK(!strcmp(trig_severity_name(TrigSeverity::Alert), "alert") && !strcmp(trig_severity_name(TrigSeverity::Info), "info"));
}

static void test_queue() {
    TriggerQueue q; Trigger t, o;
    CHECK(q.size() == 0 && !q.pop(o) && q.dropped() == 0);
    for (int i = 0; i < 3; ++i) { t.t_ms = 10 + i; CHECK(q.push(t) == (uint32_t)(i + 1)); }
    CHECK(q.size() == 3);
    CHECK(q.pop(o) && o.id == 1 && o.t_ms == 10);
    CHECK(q.pop(o) && o.id == 2 && q.pop(o) && o.id == 3 && !q.pop(o));
    // переполнение: вытесняются самые старые
    for (int i = 0; i < 37; ++i) { t.t_ms = i; q.push(t); }
    CHECK(q.size() == kTrigQueueCap && q.dropped() == 5);
    CHECK(q.pop(o) && o.t_ms == 5);                      // первые 5 (t_ms 0..4) потеряны
    size_t n = 1; uint32_t prev = o.id; while (q.pop(o)) { CHECK(o.id == prev + 1); prev = o.id; ++n; }
    CHECK(n == kTrigQueueCap && o.t_ms == 36);
    q.push(t); q.clear(); CHECK(q.size() == 0 && !q.pop(o));
}

static void test_cells_hysteresis() {
    Rig r;
    r.cells(3300, 3350); r.tick(); CHECK(r.drain().empty());
    r.cells(3051, 3101); r.tick(); CHECK(r.drain().empty());                  // 3051 > 3050
    r.cells(3050, 3100); r.tick();
    auto v = r.drain(); CHECK(v.size() == 1 && v[0].kind == TrigKind::CellLowEnter && v[0].value == 3050 && v[0].aux == 3100);
    r.cells(3040, 3090); r.tick(); r.cells(3079, 3129); r.tick(); CHECK(r.drain().empty());   // гистерезис: ещё не вышли
    r.cells(3080, 3130); r.tick();
    v = r.drain(); CHECK(v.size() == 1 && v[0].kind == TrigKind::CellLowExit);
    // дребезг вокруг порога входа не плодит событий выхода
    r.cells(3050, 3100); r.tick(); r.cells(3060, 3110); r.tick(); r.cells(3049, 3099); r.tick();
    v = r.drain(); CHECK(v.size() == 1 && v[0].kind == TrigKind::CellLowEnter);
    // жёсткий порог (2900): вход и выход +30
    r.cells(2900, 2950); r.tick(); v = r.drain(); CHECK(v.size() == 1 && v[0].kind == TrigKind::CellHardEnter);
    r.cells(2929, 2979); r.tick(); CHECK(r.drain().empty());
    r.cells(2930, 2980); r.tick(); v = r.drain(); CHECK(v.size() == 1 && v[0].kind == TrigKind::CellHardExit);
    // сразу и low, и hard при первом же кадре: два триггера (low, затем hard)
    Rig r2; r2.cells(2800, 3300); r2.tick(); v = r2.drain();
    CHECK(v.size() == 3 && v[0].kind == TrigKind::CellLowEnter && v[1].kind == TrigKind::CellHardEnter && v[2].kind == TrigKind::CellDeltaHigh);
    // пороги из конфига
    Rig r3; WatchCfg c; c.cell_warn_mv = 3200; r3.w.set_cfg(c); r3.cells(3190, 3200); r3.tick();
    v = r3.drain(); CHECK(v.size() == 1 && v[0].kind == TrigKind::CellLowEnter);
}

static void test_delta() {
    Rig r; r.cells(3300, 3400); r.tick(); CHECK(r.drain().empty());           // ровно 100 — не «> 100»
    r.cells(3300, 3401); r.tick(); auto v = r.drain(); CHECK(v.size() == 1 && v[0].kind == TrigKind::CellDeltaHigh && v[0].value == 101);
    r.cells(3300, 3371); r.tick(); CHECK(r.drain().empty());                  // 71 — ещё выше 70
    r.cells(3300, 3369); r.tick(); v = r.drain(); CHECK(v.size() == 1 && v[0].kind == TrigKind::CellDeltaOk && v[0].value == 69);
    r.cells(3300, 3400); r.tick(); CHECK(r.drain().empty());
    // кривые данные (max < min) не ломают
    r.cells(3400, 3300); r.tick(); CHECK(r.drain().empty());
    // невалидные ячейки состояние не трогают
    r.cells(3300, 3500); r.tick(); CHECK(r.drain().size() == 1);
    r.s.cells_valid = false; r.tick(); r.tick(); CHECK(r.drain().empty());
    r.cells(3300, 3500); r.tick(); CHECK(r.drain().empty());                  // всё ещё «высокий разброс», повторного входа нет
}

static void test_flags_baseline_and_change() {
    Rig r;
    r.field(0, "f312", {0, 0, 0, 0, 1, 0, 0, 0x10}); r.tick(); r.tick(); CHECK(r.drain().empty());   // baseline
    r.field(0, "f312", {0, 0, 0, 0, 1, 0, 0, 0x12}); r.tick();
    auto v = r.drain(); CHECK(v.size() == 1 && v[0].kind == TrigKind::BmsFlagsChanged);
    CHECK(v[0].value == 1 && v[0].aux == ((0u << 8) | 0x80));
    // 8 байт: полная запись «f312 00 00 .. -> .. » (55 знаков) не влезает, остаётся компактная
    CHECK(!strcmp(v[0].text, "f312 0000000001000010->0000000001000012"));
    CHECK(strlen(v[0].text) <= 47);
    // повтор того же значения не даёт события
    r.tick(6000); r.tick(6000); CHECK(r.drain().empty());
    // короткое поле: разворачиваем с пробелами
    r.field(1, "f319", {0xC0, 5, 6}); r.tick(); r.drain();
    r.field(1, "f319", {0xC0, 5, 7}); r.tick();
    v = r.drain(); CHECK(v.size() == 1 && !strcmp(v[0].text, "f319 c0 05 06 -> c0 05 07") && v[0].aux == ((1u << 8) | 0x4));
    // журнал получил те же события
    CHECK(r.log.size() == 2 && r.log.at(0).id == v[0].id && !strcmp(r.log.at(0).tag, "BMS_FLAGS"));
    CHECK(r.log.at(0).epoch_s == 1790000000);
}

static void test_debounce_merge() {
    Rig r; r.field(0, "f312", {0, 0, 0, 0, 0, 0, 0, 0x10}); r.tick();
    r.field(0, "f312", {0, 0, 0, 0, 0, 0, 0, 0x11}); r.tick();               // первое изменение — сразу
    auto v = r.drain(); CHECK(v.size() == 1 && !strcmp(v[0].text, "f312 0000000000000010->0000000000000011"));
    r.field(0, "f312", {0, 0, 0, 0, 0, 0, 0, 0x13}); r.tick(1000); CHECK(r.drain().empty());   // окно 5 с
    r.field(0, "f312", {0, 0, 0, 0, 0, 0, 0, 0x17}); r.tick(1000); CHECK(r.drain().empty());
    r.tick(2000); CHECK(r.drain().empty());                                    // прошло 4 с
    r.tick(1000);                                                              // 5 с с прошлого события
    v = r.drain(); CHECK(v.size() == 1);
    CHECK(v.size() == 1 && !strcmp(v[0].text, "f312 0000000000000011->0000000000000017"));    // old — последний отправленный, new — актуальный
    // туда-обратно внутри окна: событий нет
    r.field(0, "f312", {0, 0, 0, 0, 0, 0, 0, 0x20}); r.tick(1000);
    r.field(0, "f312", {0, 0, 0, 0, 0, 0, 0, 0x17}); r.tick(10000); CHECK(r.drain().empty());
    // разные поля не мешают друг другу
    r.field(0, "f312", {0, 0, 0, 0, 0, 0, 0, 0x18}); r.field(1, "f320", {1, 2}); r.tick(10000); r.drain();
    r.field(0, "f312", {0, 0, 0, 0, 0, 0, 0, 0x19}); r.field(1, "f320", {1, 3}); r.tick(10000);
    CHECK(r.drain().size() == 2);
}

static void test_text_bounds() {
    // длинное имя + 8 байт: обрезка, без выхода за text[48]
    Trigger t; memset(t.text, 'Z', sizeof t.text);
    Rig r; r.field(0, "abcde", {1, 2, 3, 4, 5, 6, 7, 8}); r.tick();
    r.field(0, "abcde", {8, 7, 6, 5, 4, 3, 2, 1}); r.tick();
    auto v = r.drain(); CHECK(v.size() == 1 && strlen(v[0].text) < sizeof v[0].text);
    CHECK(!strcmp(v[0].text, "abcde 0102030405060708->0807060504030201"));
    // крайние значения ячеек
    Rig q; q.cells(0, 65535); q.tick(); v = q.drain(); CHECK(v.size() == 3);
    for (auto& x : v) CHECK(strlen(x.text) < sizeof x.text);
    // поле с чрезмерной длиной игнорируется
    Rig b; b.field(0, "bad", {1}); b.s.watch[0].len = 9; b.tick(); b.tick(); CHECK(b.drain().empty());
}

static void test_stale_back() {
    Rig r;
    r.s.frames_alive = false; r.tick(); r.tick(); CHECK(r.drain().empty());   // кадров ещё не было — не «пропали»
    r.s.frames_alive = true; r.field(0, "f312", {1, 2}); r.tick(); CHECK(r.drain().empty());   // первое появление — без BmsBack
    r.s.frames_alive = false; r.s.watch[0].valid = false; r.tick(5000);
    auto v = r.drain(); CHECK(v.size() == 1 && v[0].kind == TrigKind::BmsStale && r.w.stale());
    r.tick(5000); r.tick(5000); CHECK(r.drain().empty());                     // один раз
    r.s.frames_alive = true; r.field(0, "f312", {1, 3}); r.tick(5000);      // значение изменилось за паузу, но это baseline
    v = r.drain(); CHECK(v.size() == 1 && v[0].kind == TrigKind::BmsBack && !r.w.stale());
    r.tick(); CHECK(r.drain().empty());
    r.field(0, "f312", {1, 4}); r.tick(); v = r.drain(); CHECK(v.size() == 1 && v[0].kind == TrigKind::BmsFlagsChanged);
    r.w.reset(); r.field(0, "f312", {9, 9}); r.tick(); CHECK(r.drain().empty());   // после reset снова baseline
}

static CanFrame fr(uint32_t id, uint32_t t, std::initializer_list<int> b, bool ext = false) {
    CanFrame f; f.id = id; f.t_ms = t; f.ext = ext; f.dlc = (uint8_t)b.size();
    size_t i = 0; for (int x : b) f.data[i++] = (uint8_t)x;
    return f;
}

static void test_snapshot_and_noise() {
    PylonBattery pb; SeplosBattery sb; Seplos500Battery s5;
    BmsWatch w; TriggerQueue q; BmsEventLog log;
    auto step = [&](uint32_t now) { w.update(make_bms_snapshot(pb, sb, s5, now), now, q, &log); };
    // 500k: набор кадров. 0x315 — ячейки, 0x319 — min/max
    auto feed500 = [&](uint32_t t, int cur_lo, int volt_lo, int b312, int cnt) {
        s5.feed(fr(0x312, t, {0, 0, 0, 0, 1, 0, 0, b312}));
        s5.feed(fr(0x313, t, {0x14, volt_lo, 0, cur_lo, 0, 0xFA, 50, 99}));        // напряжение/ток/температура — шум
        s5.feed(fr(0x314, t, {0, 1, 0, 2, 0, cnt, 0, 5}));                          // счётчик — шум
        s5.feed(fr(0x319, t, {0xC0, 0x0C, 0xE4, 0x0C, 0x80, 3, 7, 0}));           // max 3300, min 3200
    };
    s5.feed(fr(0x320, 1000, {1, 2, 3, 4, 5, 6, 7, 8}));
    s5.feed(fr(0x322, 1000, {0, 0xFA, 0, 0xF0, 1, 2, 3, 4}));
    s5.feed(fr(0x311, 1000, {2, 0x40, 0, 0x64, 0, 0x64, 0xAB, 0xCD}));
    feed500(1000, 5, 0x10, 0x10, 1); step(1000); CHECK(q.size() == 0);
    CHECK(make_bms_snapshot(pb, sb, s5, 1000).cells_valid);
    // шум: ток, напряжение, температура, счётчик 0x314 -> событий нет
    feed500(2000, 9, 0x20, 0x10, 2); step(2000); feed500(3000, 77, 0x30, 0x10, 3); step(3000); CHECK(q.size() == 0);
    // 0x322: байты 0–3 (температура) — шум; байты 6–7 повторяют SOC (0x48/0x49/0x3f/0x3e менялись по мере разряда) — тоже шум;
    // наблюдаются только байты 4–5
    s5.feed(fr(0x322, 3500, {1, 0xFA, 1, 0xF0, 1, 2, 3, 4})); step(3500); CHECK(q.size() == 0);
    s5.feed(fr(0x322, 3600, {0, 0xFA, 0, 0xF0, 1, 2, 0x48, 0x49})); step(3600); CHECK(q.size() == 0);
    s5.feed(fr(0x322, 3700, {0, 0xFA, 0, 0xF0, 1, 2, 0x3f, 0x3e})); step(3700); CHECK(q.size() == 0);
    s5.feed(fr(0x322, 4000, {0, 0xFA, 0, 0xF0, 1, 9, 3, 4})); step(4000);
    Trigger t; CHECK(q.pop(t) && !strcmp(t.text, "f322 01 02 -> 01 09") && t.aux == ((3u << 8) | 2u));
    // 0x312 меняется
    feed500(9000, 5, 0x10, 0x12, 4); step(9000);
    CHECK(q.pop(t) && t.kind == TrigKind::BmsFlagsChanged && !strcmp(t.text, "f312 0000000001000010->0000000001000012"));
    // редкие кадры подтверждаем теми же значениями (свежесть 10 с)
    s5.feed(fr(0x320, 9400, {1, 2, 3, 4, 5, 6, 7, 8})); s5.feed(fr(0x322, 9400, {0, 0xFA, 0, 0xF0, 1, 9, 3, 4}));
    s5.feed(fr(0x311, 9400, {2, 0x40, 0, 0x64, 0, 0x64, 0xAB, 0xCD}));
    // 0x319: min/max шумят — нет; индекс — да
    s5.feed(fr(0x319, 9500, {0xC0, 0x0C, 0xE3, 0x0C, 0x81, 3, 7, 0})); step(9500); CHECK(q.size() == 0);
    s5.feed(fr(0x319, 15000, {0xC0, 0x0C, 0xE3, 0x0C, 0x81, 4, 7, 0})); step(15000); CHECK(q.pop(t) && !strcmp(t.text, "f319 c0 03 07 -> c0 04 07"));
    // 0x311 байты 6–7; токи байтов 2–5 — не наблюдаем
    s5.feed(fr(0x311, 16000, {2, 0x40, 0, 0x65, 0, 0x65, 0xAB, 0xCD})); step(16000); CHECK(q.size() == 0);
    s5.feed(fr(0x311, 17000, {2, 0x40, 0, 0x65, 0, 0x65, 0xAB, 0xCE})); step(17000); CHECK(q.pop(t) && !strcmp(t.text, "f311 ab cd -> ab ce"));
    // 0x320
    s5.feed(fr(0x320, 18000, {1, 2, 3, 4, 5, 6, 7, 9})); step(18000); CHECK(q.pop(t) && !strcmp(t.text, "f320 0102030405060708->0102030405060709"));
    CHECK(q.size() == 0);

    // Pylon: 0x359 (protect/alarm) и 0x35C (req)
    PylonBattery p2; Seplos500Battery e5; SeplosBattery e1;
    BmsWatch w2; TriggerQueue q2;
    auto st2 = [&](uint32_t now) { w2.update(make_bms_snapshot(p2, e1, e5, now), now, q2); };
    p2.feed(fr(0x359, 1000, {0, 0, 0, 0, 3, 'P', 'N'})); p2.feed(fr(0x35C, 1000, {0xC0, 0}));
    p2.feed(fr(0x356, 1000, {0x72, 0x14, 0xE0, 0xFF, 0xFD, 0x00}));
    st2(1000); CHECK(q2.size() == 0);
    p2.feed(fr(0x356, 2000, {0x70, 0x14, 0x00, 0x00, 0xFE, 0x00})); st2(2000); CHECK(q2.size() == 0);     // напряжение/ток — шум
    p2.feed(fr(0x359, 3000, {0, 0x08, 0x80, 0, 3, 'P', 'N'})); st2(3000);
    CHECK(q2.pop(t) && !strcmp(t.text, "p359 00 00 00 00 -> 00 08 80 00"));
    p2.feed(fr(0x35C, 4000, {0x80, 0})); st2(4000);
    CHECK(q2.pop(t) && !strcmp(t.text, "p35c c0 -> 80"));

    // 100k: prot_raw 0x00030200
    SeplosBattery s1; PylonBattery p3; Seplos500Battery e3; BmsWatch w3; TriggerQueue q3;
    auto st3 = [&](uint32_t now) { w3.update(make_bms_snapshot(p3, s1, e3, now), now, q3); };
    s1.feed(fr(0x00030200, 1000, {0, 0, 0, 0, 0, 0, 0, 0}, true)); st3(1000);
    s1.feed(fr(0x00030400, 2000, {0x02, 0x00, 0x10, 0, 0x14, 0, 0, 0}, true)); st3(2000); CHECK(q3.size() == 0);
    s1.feed(fr(0x00030200, 3000, {0, 0, 0, 0, 0, 0, 0, 4}, true)); st3(3000);
    CHECK(q3.pop(t) && t.kind == TrigKind::BmsFlagsChanged && !strcmp(t.text, "a200 0000000000000000->0000000000000004"));

    // кадры пропали: свежесть 10 с -> BmsStale из снимка
    feed500(20000, 5, 0x10, 0x12, 4); step(20000); while (q.pop(t)) {}
    step(25000); CHECK(q.size() == 0);
    step(31000); CHECK(q.pop(t) && t.kind == TrigKind::BmsStale);
    step(40000); CHECK(q.size() == 0);
}

static void test_log_and_json() {
    BmsEventLog log; Trigger t; TriggerQueue q;
    CHECK(log.size() == 0 && log.total() == 0);
    for (int i = 0; i < 20; ++i) { t.kind = i % 2 ? TrigKind::BmsFlagsChanged : TrigKind::CellLowEnter; t.t_ms = 1000 * (i + 1); t.value = (float)i; snprintf(t.text, sizeof t.text, "ev %d", i); t.id = q.push(t); log.add(t, i < 10 ? -1 : 1790000000 + i); }
    CHECK(log.size() == kBmsLogCap && log.total() == 20);
    CHECK(log.at(0).id == 20 && log.at(15).id == 5 && !strcmp(log.at(0).text, "ev 19"));
    CHECK(!strcmp(log.at(0).tag, "BMS_FLAGS") && !strcmp(log.at(1).tag, "CELL_LOW"));
    char js[4096]; size_t n = bms_events_to_json(log, 30000, js, sizeof js);
    CHECK(n > 0 && n == strlen(js) && json_ok(js));
    CHECK(strstr(js, "\"total\":20") && strstr(js, "\"tag\":\"BMS_FLAGS\"") && strstr(js, "\"sev\":\"warn\"") && strstr(js, "\"ts\":\"2026-"));
    CHECK(strstr(js, "\"ts\":null"));                      // записи без времени (i < 10) остались не в журнале? 16 новейших: i=4..19 -> есть i=4..9
    CHECK(strstr(js, "\"age_s\":10"));                      // событие 20 (t=20000), now=30000
    // граница буфера
    CHECK(bms_events_to_json(log, 30000, js, n) == 0 && js[0] == 0);
    CHECK(bms_events_to_json(log, 30000, js, n + 1) == n);
    CHECK(bms_events_to_json(log, 30000, js, 5) == 0);
    // пустой журнал и экранирование
    BmsEventLog e; n = bms_events_to_json(e, 5, js, sizeof js); CHECK(n && json_ok(js) && strstr(js, "\"events\":[]"));
    Trigger x; strcpy(x.text, "q\"u\\o\nte"); e.add(x, -1); bms_events_to_json(e, 5, js, sizeof js);
    CHECK(json_ok(js) && strstr(js, "q\\\"u\\\\o te"));
    // время «из будущего» (гонка) даёт age 0
    Trigger f; f.t_ms = 9000; BmsEventLog g; g.add(f, -1); bms_events_to_json(g, 8000, js, sizeof js); CHECK(strstr(js, "\"age_s\":0"));
}

static void test_json_worst_case() {
    // 16 событий с самым длинным текстом/значениями должны влезать в буфер /api/bms_events (4096 в shell.cpp)
    BmsEventLog log; Trigger t; t.kind = TrigKind::BmsFlagsChanged; t.value = 65535; t.t_ms = 4294967295u;
    memset(t.text, 'x', 47); t.text[47] = 0;
    for (int i = 0; i < 40; ++i) { t.id = 4000000000u + i; log.add(t, 4102444799LL); }
    char js[4096]; size_t n = bms_events_to_json(log, 4294967295u, js, sizeof js);
    CHECK(n > 0 && json_ok(js) && n < 3600);
    printf("bms_events worst case: %zu bytes\n", n);
}

static void test_cfg() {
    Config c; WatchCfg w = make_watch_cfg(c);
    CHECK(w.cell_warn_mv == 3050 && w.cell_hard_mv == 2900 && w.hyst_mv == 30 && w.delta_hi_mv == 100 && w.delta_ok_mv == 70 && w.debounce_ms == 5000);
    c.cell_soft_on_v = 3.1f; c.cell_hard_floor_v = 2.95f; w = make_watch_cfg(c); CHECK(w.cell_warn_mv == 3100 && w.cell_hard_mv == 2950);
}

int main() {
    test_tags(); test_queue(); test_cells_hysteresis(); test_delta(); test_flags_baseline_and_change(); test_debounce_merge();
    test_text_bounds(); test_stale_back(); test_snapshot_and_noise(); test_log_and_json(); test_json_worst_case(); test_cfg();
    printf("test_gs_trig: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
