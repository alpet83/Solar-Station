#include "gs_util.h"
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
    printf("%s\n", out);

    float f;
    CHECK(parse_ext_current("12.5", f) && f == 12.5f);
    CHECK(!parse_ext_current("-1", f) && !parse_ext_current("501", f) && !parse_ext_current("x", f) && !parse_ext_current("", f) && !parse_ext_current(nullptr, f));
    printf("util: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
