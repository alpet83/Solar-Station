#include "../../lib/gs_core/gs_core.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
using namespace gs;

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Sim {
    Supervisor sv; uint64_t t = 1000; int8_t reg = 1;
    float v = 52.0f, alt = 52.0f, chg = 0, ext = kNaN, load = 500, grid = 230;
    bool sample = true, safe = false; uint64_t ext_age = 0;
    Cmd last = Cmd::None; Reason lastr = Reason::None; uint32_t flags = 0;
    int n_conn = 0, n_disc = 0; uint64_t first_conn = 0, first_disc = 0;
    explicit Sim(const Config& c = Config()) : sv(c) {}
    Decision tick(uint64_t dt = 5000) {
        t += dt;
        Input in; in.now_ms = t; in.have_sample = sample; in.pack_v = v; in.pack_v_alt = alt;
        in.charge_a = chg; in.ext_charge_a = ext; in.ext_age_ms = ext_age; in.load_w = load;
        in.grid_v = grid; in.ongrid_reg = reg; in.safe_mode = safe;
        Decision d = sv.step(in); last = d.cmd; lastr = d.reason; flags = d.flags;
        if (d.cmd == Cmd::Connect) { if (!n_conn) first_conn = t; ++n_conn; reg = 1; }  // идеальный инвертор
        if (d.cmd == Cmd::Disconnect) { if (!n_disc) first_disc = t; ++n_disc; reg = 0; }
        return d;
    }
    void run(uint64_t ms) { for (uint64_t e = t + ms; t < e;) tick(); }
};

static Config fast() { Config c; c.hold_after_connect_ms = 60000; return c; }

static void test_config() {
    Config c; CHECK(c.validate());
    Config b = c; b.soft_on_v = 47; CHECK(!b.validate());
    b = c; b.full_v = 80; CHECK(!b.validate());
    b = c; b.hard_floor_v = kNaN; CHECK(!b.validate());
    b = c; b.soft_off_dwell_ms = 0; CHECK(!b.validate());
}

static void test_eff() {
    Config c;
    CHECK(effective_charge_a(c, 10, 20, 1000) == 10 + 0.9f * 20);
    CHECK(effective_charge_a(c, 10, 20, 61000) == 10);       // протухло
    CHECK(effective_charge_a(c, 10, kNaN, 0) == 10);
    CHECK(effective_charge_a(c, kNaN, -5, 0) == 0);
    CHECK(effective_charge_a(c, 0, 1000, 0) == 0.9f * 80);   // кламп
}

static void test_smooth_discharge() {
    Sim s; s.reg = 0; s.v = 51.4f; s.alt = 51.4f;
    uint64_t t0 = s.t;
    s.run(110000); CHECK(s.n_conn == 0);
    s.run(20000);  CHECK(s.n_conn == 1);
    CHECK(s.first_conn - t0 >= 120000 && s.first_conn - t0 <= 130000);
}

static void test_rebound() {
    Sim s; s.reg = 0; s.v = s.alt = 51.4f; s.run(60000);
    s.v = s.alt = 52.5f; s.run(10000);
    s.v = s.alt = 51.4f; s.run(100000);
    CHECK(s.n_conn == 0);   // таймер сбросился отскоком
}

static void test_hard_floor() {
    Sim s; s.reg = 0; s.v = s.alt = 47.0f;
    uint64_t t0 = s.t; s.run(10000); CHECK(s.n_conn == 0);
    s.run(15000); CHECK(s.n_conn == 1);
    CHECK(s.first_conn - t0 <= 25000);
    // safe-mode переопределяет dwell: подключаем сразу
    Sim q; q.reg = 0; q.safe = true; q.v = q.alt = 53.0f; q.tick(); CHECK(q.n_conn == 1); CHECK(q.lastr == Reason::SafeMode);
    // и во время ручной паузы
    Sim p; p.reg = 1; p.tick(); p.reg = 0; p.tick();     // внешнее отключение -> пауза
    CHECK(p.flags & F_MANUAL_OVERRIDE);
    p.v = p.alt = 51.0f; p.run(300000); CHECK(p.n_conn == 0);   // мягкий порог в паузе молчит
    p.v = p.alt = 47.0f; p.run(30000); CHECK(p.n_conn == 1);    // жёсткий — нет
}

static void test_no_data() {
    Sim s; s.reg = 0; s.v = s.alt = 53.0f; s.sample = false;
    s.run(100000); CHECK(s.n_conn == 0);
    s.run(30000);  CHECK(s.n_conn == 1);
    // данные вернулись, батарея полна, но отключение — не раньше dwell + hold
    s.sample = true; s.v = s.alt = 54.8f; s.chg = 50; s.load = 300;
    uint64_t t0 = s.t; s.run(280000); CHECK(s.n_disc == 0);
    s.run(700000); CHECK(s.n_disc == 1); CHECK(s.first_disc - t0 >= 900000 - 5000);
}

static void test_no_data_at_boot() {
    Sim s; s.reg = -1; s.sample = false;       // регистр неизвестен, данных нет
    s.run(125000); CHECK(s.n_conn == 1);       // connect даже при неизвестном регистре
}

static void test_disconnect_conditions() {
    Sim s(fast()); s.reg = 1; s.v = s.alt = 53.5f; s.chg = 30; s.load = 300;   // 30А*53.5 = 1600 Вт
    uint64_t t0 = s.t; s.run(290000); CHECK(s.n_disc == 0);
    s.run(30000); CHECK(s.n_disc == 1);
    CHECK(s.first_disc - t0 >= 300000);
    // прерывание сбрасывает таймер
    Sim r(fast()); r.reg = 1; r.v = r.alt = 53.5f; r.chg = 30; r.load = 300;
    r.run(200000); r.chg = 1; r.run(5000); r.chg = 30; r.run(200000); CHECK(r.n_disc == 0);
    r.run(120000); CHECK(r.n_disc == 1);
    // мощности не хватает
    Sim w; w.reg = 1; w.v = w.alt = 53.5f; w.chg = 5; w.load = 1500; w.run(900000); CHECK(w.n_disc == 0);
    // но full_v отключает без условия по мощности
    w.v = w.alt = 54.6f; w.run(320000); CHECK(w.n_disc == 1);
}

static void test_doubtful() {
    Sim s; s.reg = 1; s.v = 53.5f; s.alt = 55.5f; s.chg = 30; s.run(900000);
    CHECK(s.n_disc == 0); CHECK(s.flags & F_DATA_DOUBTFUL);
    // но подключение не блокируется: v_used = min
    Sim t; t.reg = 0; t.v = 51.0f; t.alt = 53.0f; t.run(130000); CHECK(t.n_conn == 1);
}

static void test_alt_zero() {
    Sim s(fast()); s.reg = 1; s.v = 53.5f; s.alt = 0; s.chg = 30; s.load = 300;   // alt=0 игнорируется
    s.run(330000); CHECK(s.n_disc == 1);
}

static void test_manual_override() {
    Sim s; s.reg = 1; s.tick();                    // принято без паузы
    CHECK(!(s.flags & F_MANUAL_PAUSE));
    s.reg = 0; s.tick(); CHECK(s.flags & F_MANUAL_OVERRIDE); CHECK(s.flags & F_MANUAL_PAUSE);
    s.v = s.alt = 51.0f; s.run(3000000); CHECK(s.n_conn == 0);     // почти час паузы
    s.run(700000); CHECK(s.n_conn == 1);                           // пауза кончилась -> SoftLow
}

static void test_safe_mode() {
    Sim s; s.safe = true; s.reg = 1; s.v = s.alt = 54.8f; s.chg = 50; s.run(1200000);
    CHECK(s.n_disc == 0); CHECK(s.flags & F_SAFE_MODE);
    Sim t; t.safe = true; t.reg = 0; t.v = t.alt = 51.0f; t.run(130000); CHECK(t.n_conn == 1);
}

static void test_ext_loss() {
    // заряд только внешний: потеря ext -> не отключаться (при v < full)
    Sim s(fast()); s.reg = 1; s.v = s.alt = 53.6f; s.chg = 0; s.ext = 40; s.load = 300;
    s.run(330000); CHECK(s.n_disc == 1);
    Sim t; t.reg = 1; t.v = t.alt = 53.6f; t.chg = 0; t.ext = 40; t.ext_age = 120000; t.load = 300;
    t.run(900000); CHECK(t.n_disc == 0);
    t.v = t.alt = 54.6f; t.run(320000); CHECK(t.n_disc == 1);
}

static void test_pending_retry() {
    Sim s; s.reg = 0; s.v = s.alt = 51.0f;
    // «неисправный» инвертор: команда не меняет регистр
    int cmds = 0; uint64_t t0 = s.t;
    while (s.t - t0 < 200000) {
        s.t += 5000; Input in; in.now_ms = s.t; in.have_sample = true; in.pack_v = in.pack_v_alt = 51.0f;
        in.ongrid_reg = 0; Decision d = s.sv.step(in); if (d.cmd == Cmd::Connect) ++cmds;
    }
    CHECK(cmds >= 3 && cmds <= 8);   // повторы раз в ~10с после 120с, не каждые 5с
}


// ---- регрессии по результатам ревью ----
static void test_time_backwards() {
    Sim s; s.t = 4294967000ull; s.reg = 0; s.v = s.alt = 51.0f; s.run(60000);
    s.t = 1000;                      // millis() обнулился
    s.run(130000); CHECK(s.n_conn == 1);
    Sim h; h.t = 4294967000ull; h.reg = 0; h.v = h.alt = 47.0f; h.tick(); h.t = 500; h.run(30000); CHECK(h.n_conn == 1);
}

static void test_reboot_loop_safe_mode() {
    int conn = 0;
    for (int i = 0; i < 10; ++i) {              // каждая «загрузка» — новый Supervisor, safe_mode после пары перезагрузок
        Supervisor sv; Input in; in.now_ms = 1000; in.have_sample = true; in.pack_v = in.pack_v_alt = 47.0f;
        in.ongrid_reg = 0; in.safe_mode = (i >= 2);
        if (sv.step(in).cmd == Cmd::Connect) ++conn;
    }
    CHECK(conn == 8);
}

static void test_own_overlapping_commands() {
    Config c = fast();
    Sim s(c); s.reg = 1; s.v = s.alt = 53.5f; s.chg = 30; s.load = 300; s.run(400000);
    CHECK(s.n_disc == 1);
    // Disconnect выдан, инвертор его уже применил (reg=0), но данных нет -> NoData-Connect, затем
    // reg=0 наблюдается: изменение наше, паузы быть не должно.
    s.sample = false; s.run(125000);
    CHECK(s.n_conn >= 1);
    CHECK(!(s.flags & F_MANUAL_PAUSE));
}

static void test_stale_pending_is_not_confirmation() {
    Sim s(fast()); s.reg = 0; s.v = s.alt = 51.0f;
    // Connect выдан, но «потерялся»
    uint64_t t0 = s.t;
    while (s.t - t0 < 130000) {
        s.t += 5000; Input in; in.now_ms = s.t; in.have_sample = true; in.pack_v = in.pack_v_alt = 51.0f; in.ongrid_reg = 0;
        s.sv.step(in);
    }
    // напряжение восстановилось, часы идут
    s.v = s.alt = 53.5f; s.chg = 30; s.load = 300; s.reg = 0; s.run(7200000);   // 2 ч в off-grid без дёрганья
    CHECK(s.n_conn == 0);
    s.reg = 1; s.tick();             // человек включил сеть: это внешнее изменение
    CHECK(s.flags & F_MANUAL_OVERRIDE);
    s.run(3000000); CHECK(s.n_disc == 0);
}

static void test_step_gap() {
    Sim s(fast()); s.reg = 1; s.v = s.alt = 53.5f; s.chg = 30; s.load = 300;
    s.run(200000); s.tick(600000);   // 10 минут тишины
    CHECK(s.n_disc == 0); CHECK(s.flags & F_TIME_JUMP);
    s.run(250000); CHECK(s.n_disc == 0);
    s.run(100000); CHECK(s.n_disc == 1);
}

static void test_boot_hold() {
    Sim s; s.reg = 1; s.v = s.alt = 53.5f; s.chg = 30; s.load = 300;   // конфиг по умолчанию: hold 15 мин
    s.run(800000); CHECK(s.n_disc == 0); CHECK(s.flags & F_HOLD);
    s.run(200000); CHECK(s.n_disc == 1);
}

static void test_garbage_inputs() {
    Sim s(fast()); s.reg = 1; s.v = s.alt = 53.3f; s.chg = 6553.5f; s.load = 3000; s.run(900000); CHECK(s.n_disc == 0);
    Sim n(fast()); n.reg = 1; n.v = n.alt = 53.5f; n.chg = 30; n.load = kNaN; n.run(900000); CHECK(n.n_disc == 0);
    CHECK(n.flags & F_LOAD_UNKNOWN);
    // но по full_v с двумя источниками — отключаем и без нагрузки
    Sim f(fast()); f.reg = 1; f.v = f.alt = 54.6f; f.load = kNaN; f.run(400000); CHECK(f.n_disc == 1);
}

static void test_single_source_full() {
    Sim s(fast()); s.reg = 1; s.v = 54.7f; s.alt = 0; s.chg = 0; s.load = 300; s.run(900000);
    CHECK(s.n_disc == 0); CHECK(s.flags & F_SINGLE_SRC);   // один источник: full_v не засчитывается
}

static void test_validate_bounds() {
    Config c;
    c.no_data_timeout_ms = 4000000000u; CHECK(!c.validate()); c = Config();
    c.soft_on_dwell_ms = 4000000000u; CHECK(!c.validate()); c = Config();
    c.soft_off_dwell_ms = 1; CHECK(!c.validate()); c = Config();
    c.hold_after_connect_ms = 0; CHECK(!c.validate()); c = Config();
    c.v_mismatch_max = 1e9f; CHECK(!c.validate()); c = Config();
    c.ext_ttl_ms = 0; CHECK(!c.validate()); c = Config();
    c.pow_min_base_w = 0; CHECK(!c.validate());
}

static void test_connect_over_pending_disconnect() {
    Sim s(fast()); s.reg = 1; s.v = s.alt = 53.5f; s.chg = 30; s.load = 300;
    for (int i = 0; i < 200 && !s.n_disc; ++i) s.tick();
    CHECK(s.n_disc == 1);
    // инвертор ещё не применил Disconnect (reg остаётся 1), а напряжение рухнуло
    s.reg = 1; s.v = s.alt = 47.0f; s.n_conn = 0;
    s.run(25000); CHECK(s.n_conn >= 1);
}

static void test_cmd_reason_at_issue() {
    Sim s; s.reg = 0; s.v = s.alt = 47.0f;
    Decision d; for (int i = 0; i < 10 && d.cmd == Cmd::None; ++i) d = s.tick();
    CHECK(d.cmd == Cmd::Connect); CHECK(d.reason == Reason::HardFloor);
    Sim n; n.reg = 0; n.sample = false; n.v = n.alt = 53;
    Decision e; for (int i = 0; i < 40 && e.cmd == Cmd::None; ++i) e = n.tick();
    CHECK(e.cmd == Cmd::Connect); CHECK(e.reason == Reason::NoData);
}

static std::vector<std::vector<double>> load_csv(const char* p) {
    std::vector<std::vector<double>> r; std::ifstream f(p); std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        std::vector<double> row; std::stringstream ss(line); std::string c;
        while (std::getline(ss, c, ',')) row.push_back(atof(c.c_str()));
        if (row.size() >= 7) r.push_back(row);
    }
    return r;
}

static void test_incident_replay(const char* path) {
    auto rows = load_csv(path);
    if (rows.empty()) { printf("SKIP replay: %s not found\n", path); return; }
    Supervisor sv; int8_t reg = (int8_t)rows[0][6];
    uint64_t t0 = (uint64_t)rows[0][0]; size_t idx = 0;
    bool conn = false, disc_low = false; uint64_t t_conn = 0;
    uint64_t t_end = (uint64_t)rows.back()[0];
    for (uint64_t t = t0; t <= t_end; t += 5) {
        while (idx + 1 < rows.size() && (uint64_t)rows[idx + 1][0] <= t) ++idx;
        auto& r = rows[idx];
        Input in; in.now_ms = (t - t0) * 1000 + 1000; in.have_sample = true;
        in.pack_v = (float)r[1]; in.pack_v_alt = (float)r[2]; in.charge_a = (float)r[3];
        in.load_w = (float)r[4]; in.grid_v = (float)r[5]; in.ongrid_reg = reg;
        Decision d = sv.step(in);
        if (d.cmd == Cmd::Connect) { reg = 1; if (!conn) { conn = true; t_conn = t; } }
        if (d.cmd == Cmd::Disconnect) { reg = 0; if (d.v_used < sv.config().soft_off_v) disc_low = true; }
    }
    printf("replay: rows=%zu first connect at +%llus (conn=%d)\n", rows.size(), (unsigned long long)(t_conn - t0), conn);
    CHECK(!disc_low);
    if (rows[0][1] <= 48.0 && rows[0][6] == 0) CHECK(conn && t_conn - t0 <= 25);
}

int main(int argc, char** argv) {
    test_config(); test_eff(); test_smooth_discharge(); test_rebound(); test_hard_floor();
    test_no_data(); test_no_data_at_boot(); test_disconnect_conditions(); test_doubtful();
    test_alt_zero(); test_manual_override(); test_safe_mode(); test_ext_loss(); test_pending_retry();
    test_time_backwards(); test_reboot_loop_safe_mode(); test_own_overlapping_commands();
    test_stale_pending_is_not_confirmation(); test_step_gap(); test_boot_hold(); test_garbage_inputs();
    test_single_source_full(); test_validate_bounds(); test_connect_over_pending_disconnect(); test_cmd_reason_at_issue();
    test_incident_replay(argc > 1 ? argv[1] : "test/native/fixtures/incident_2026-10-01.csv");
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
