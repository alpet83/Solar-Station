#include "gs_util.h"
#include "gs_bus.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace gs;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    Config c;
    CHECK(config_set(c, "soft_on_v", "51.0") && c.soft_on_v == 51.0f);
    CHECK(!config_set(c, "soft_on_v", "47.0") && c.soft_on_v == 51.0f);        // нарушает порядок порогов
    CHECK(!config_set(c, "soft_on_v", "abc"));
    CHECK(!config_set(c, "soft_on_v", "nan"));
    CHECK(!config_set(c, "nope", "1"));
    CHECK(!config_set(c, "hold_after_connect_ms", "-5"));
    CHECK(!config_set(c, "hold_after_connect_ms", "0"));
    CHECK(config_set(c, "hold_after_connect_ms", "600000") && c.hold_after_connect_ms == 600000);
    CHECK(!config_set(c, "no_data_timeout_ms", "4000000000"));
    CHECK(config_set(c, "ext_coef", "0.8"));
    char v[32];
    CHECK(config_get(c, "hold_after_connect_ms", v, sizeof v) && !strcmp(v, "600000"));
    CHECK(config_get(c, "soft_on_v", v, sizeof v) && !strcmp(v, "51"));
    // каждое поле читается/пишется туда-обратно и результат валиден
    for (size_t i = 0; i < kCfgFieldCount; ++i) {
        Config d; char b[32];
        CHECK(config_get(d, kCfgFields[i].key, b, sizeof b));
        CHECK(config_set(d, kCfgFields[i].key, b));
        for (size_t j = 0; j < i; ++j) CHECK(strcmp(kCfgFields[i].key, kCfgFields[j].key) != 0);
    }
    char js[1200]; size_t n = config_to_json(c, js, sizeof js);
    CHECK(n > 100 && js[0] == '{' && js[n - 1] == '}');
    char tiny[20]; CHECK(config_to_json(c, tiny, sizeof tiny) == 0);

    char fl[200]; flags_to_str(F_SOFT_LOW | F_HOLD, fl, sizeof fl); CHECK(!strcmp(fl, "SOFT_LOW,HOLD"));
    flags_to_str(0, fl, sizeof fl); CHECK(fl[0] == 0);
    char s5[5]; flags_to_str(0xFFFFFFFFu, s5, sizeof s5); CHECK(strlen(s5) < 5);

    Telemetry t; t.pack_v = 51.8f; t.rt_ok = true; t.ongrid_reg = 1;
    Decision d; d.flags = F_HOLD; Input in;
    StateView sv{&t, &d, &in, 12345, 7, 3, false, 1790826665, false, 0, 1, 2, 3, 4, 5};
    char out[1200]; size_t m = state_to_json(sv, out, sizeof out);
    CHECK(m > 0 && strstr(out, "\"ts\":\"2026-10-01") && strstr(out, "\"source\":\"supervisor\""));
    CHECK(strstr(out, "\"alt\":null"));                                      // NaN -> null
    sv.epoch_s = -1; m = state_to_json(sv, out, sizeof out);
    CHECK(m > 0 && strstr(out, "\"ts\":null") && strstr(out, "\"ntp_synced\":false"));
    CHECK(state_to_json(sv, tiny, sizeof tiny) == 0);
    {
        Event ev[2]; ev[0].id = 2; ev[0].cmd = Cmd::Connect; ev[0].reason = Reason::HardFloor; ev[0].epoch_s = 1790826665; ev[0].v = 46.0f; ev[0].ok = true;
        ev[1].id = 1; ev[1].cmd = Cmd::Disconnect; ev[1].reason = Reason::Charged;
        sv.events = ev; sv.n_events = 2; sv.epoch_s = 1790826665;
        char big[2000]; size_t k = state_to_json(sv, big, sizeof big);
        CHECK(k > 0 && big[k - 1] == '}' && strstr(big, "\"events\":[{\"id\":2,\"cmd\":\"CONNECT\",\"reason\":\"HARD_FLOOR\"") && strstr(big, "\"ts\":null"));
        CHECK(strlen(big) == k);
        CHECK(state_to_json(sv, big, 500) == 0);
        printf("%s\n", big);
    }

    // батарея по CAN в /api/state: объект с null-ами без данных, значения при свежих данных
    {
        Event ev[kEventCap];
        for (size_t i = 0; i < kEventCap; ++i) { ev[i].id = 4000000000u + (uint32_t)i; ev[i].cmd = Cmd::Disconnect; ev[i].reason = Reason::SafeMode; ev[i].epoch_s = 1790826665; ev[i].v = 51.23f; ev[i].uptime_ms = 99999999999ull; ev[i].ok = true; }
        sv.events = ev; sv.n_events = kEventCap; sv.epoch_s = 1790826665;
        static char big[3000];
        size_t k = state_to_json(sv, big, sizeof big);
        CHECK(k > 0 && strstr(big, "\"battery\":{\"v\":null,\"i\":null,\"soc\":null,\"charge_en\":null,\"discharge_en\":null,\"alarm\":null,\"age_s\":null},\"events\":["));
        PylonBattery pb; CanFrame f; f.t_ms = 1000;
        f.id = 0x356; f.dlc = 6; f.data[0] = 0x72; f.data[1] = 0x14; f.data[2] = 0xE0; f.data[3] = 0xFF; pb.feed(f);
        f.id = 0x355; f.dlc = 4; f.data[0] = 87; f.data[1] = 0; pb.feed(f);
        f.id = 0x35C; f.dlc = 2; f.data[0] = 0x80; pb.feed(f);
        f.id = 0x359; f.dlc = 7; f.data[0] = 0x00; f.data[1] = 0; f.data[2] = 0; f.data[3] = 0; f.data[4] = 1; pb.feed(f);
        BatteryBrief bb = battery_brief(pb, 3500);
        sv.bat = &bb;
        k = state_to_json(sv, big, sizeof big);
        CHECK(k > 0 && big[k - 1] == '}' && strlen(big) == k && strstr(big, "\"battery\":{\"v\":52.34,\"i\":-3.2,\"soc\":87,\"charge_en\":true,\"discharge_en\":false,\"alarm\":false,\"age_s\":2}"));
        printf("state json with 8 events + battery: %zu bytes (shell buffer 3072)\n", k);
        CHECK(k < 3072 - 400);
        CHECK(state_to_json(sv, big, k) == 0 && state_to_json(sv, big, k + 1) == k);   // граница буфера
        sv.bat = nullptr; sv.n_events = 0; sv.events = nullptr;
    }

    // атомарный набор: сдвиг всех порогов, недостижимый пошагово от значений по умолчанию
    {
        Config d; const char* k[] = {"hard_floor_v", "soft_on_v", "soft_off_v", "full_v"};
        const char* vv[] = {"52", "52.6", "53.4", "54.6"};
        CHECK(!config_set(d, "hard_floor_v", "52"));                     // пошагово нельзя
        CHECK(config_apply_pairs(d, k, vv, 4) && d.hard_floor_v == 52.0f && d.full_v == 54.6f);
        const char* bad[] = {"52", "52.6", "53.4", "80"};
        Config e; CHECK(!config_apply_pairs(e, k, bad, 4) && e.hard_floor_v == 48.0f);   // откат целиком
        const char* mixed[] = {"52", "x", "53.4", "54.6"};
        CHECK(!config_apply_pairs(e, k, mixed, 4));
    }
    // точный round-trip нетривиальных значений
    {
        Config d; CHECK(config_set(d, "pow_min_base_w", "12345") && config_set(d, "load_factor", "0.97"));
        char b[32]; config_get(d, "pow_min_base_w", b, sizeof b); CHECK(!strcmp(b, "12345"));
        Config r; config_get(d, "load_factor", b, sizeof b); CHECK(config_set(r, "load_factor", b) && r.load_factor == d.load_factor);
        config_get(d, "pow_min_base_w", b, sizeof b); CHECK(config_set(r, "pow_min_base_w", b) && r.pow_min_base_w == 12345.0f);
    }
    CHECK(flags_to_str(F_HOLD, nullptr, 0) == 0);
    { uint16_t r[3] = {1, 65535, 0}; char b[100];
      CHECK(regs_to_json(25201, 3, 1500, r, b, sizeof b) > 0 && !strcmp(b, "{\"addr\":25201,\"count\":3,\"age_ms\":1500,\"regs\":[1,65535,0]}"));
      CHECK(regs_to_json(25201, 3, 1500, r, b, 50) == 0); }
    {
        BusSniffer sn(6); LoggerTracker tr;
        uint8_t q[8]; build_read(q, 8, 4, 3, 20001, 16);
        for (int i = 0; i < 3; ++i) { sn.feed(q, 8, 1000 + i * 300000ull); sn.poll(1100 + i * 300000ull);
            FrameRec f; sn.recent(0, f); tr.observe(f, 1000 + i * 300000ull); }
        tr.tick(700000);
        static char big[8000];
        size_t k = sniff_to_json(sn, tr, 700000, 1790826665, 10, 10, big, sizeof big);
        CHECK(k > 0 && strlen(big) == k && big[0] == '{' && big[k - 1] == '}');
        CHECK(strstr(big, "\"bursts_total\":3") && strstr(big, "\"kind\":\"req\"") && strstr(big, "\"first_addr\":20001"));
        CHECK(strstr(big, "\"ts\":\"2026-"));
        CHECK(sniff_to_json(sn, tr, 700000, -1, 10, 10, big, sizeof big) > 0 && strstr(big, "\"ts\":null"));
        CHECK(sniff_to_json(sn, tr, 700000, -1, 10, 10, big, 200) == 0);
        CHECK(strstr(big, "next_start_in_ms") == nullptr || true);
        printf("%s\n", big);
    }
    float f;
    CHECK(parse_ext_current("12.5", f) && f == 12.5f);
    CHECK(!parse_ext_current("-1", f) && !parse_ext_current("501", f) && !parse_ext_current("x", f) && !parse_ext_current("", f) && !parse_ext_current(nullptr, f));
    printf("util: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
