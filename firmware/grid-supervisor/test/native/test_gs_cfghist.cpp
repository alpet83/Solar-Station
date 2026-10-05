#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "gs_cfghist.h"
#include "gs_util.h"

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

static bool same(const Config& a, const Config& b) {
    for (size_t i = 0; i < kCfgFieldCount; ++i) {
        char x[24], y[24]; config_get(a, kCfgFields[i].key, x, sizeof x); config_get(b, kCfgFields[i].key, y, sizeof y);
        if (strcmp(x, y)) return false;
    }
    return true;
}

static void test_no_change() {
    Config a; char out[kCfgHistLineMax];
    CHECK(cfg_hist_line(a, a, 1000, 1, "api", false, out, sizeof out) == 0);
    size_t n = cfg_hist_line(a, a, 1000, 1, "baseline", true, out, sizeof out);    // baseline: без изменений, но запись нужна
    CHECK(n > 0 && out[n - 1] == '\n' && strstr(out, "\"ch\":{}") != nullptr);
    std::string s(out, n - 1); CHECK(json_ok(s));
}

static void test_line_and_roundtrip() {
    Config a, b; b.soft_on_v = 50.8f; b.cell_guard = 0; b.hard_dwell_ms = 12345;
    char out[kCfgHistLineMax];
    size_t n = cfg_hist_line(a, b, 1791230000, 7, "api", false, out, sizeof out);
    CHECK(n > 0);
    std::string s(out, n); CHECK(s.back() == '\n'); s.pop_back(); CHECK(json_ok(s) && s.find('\n') == std::string::npos);
    CHECK(strstr(out, "\"cell_guard\":[1,0]") != nullptr && strstr(out, "\"hard_dwell_ms\":[") != nullptr);
    CfgHistHeader h; CHECK(cfg_hist_header(out, n, h) && h.id == 7 && h.t == 1791230000 && !strcmp(h.src, "api"));
    Config c;                                         // восстановление поверх значений по умолчанию → получаем b
    CHECK(cfg_hist_apply(out, n, c) && same(c, b));
    Config d = b; d.soft_on_v = 50.2f; d.soft_off_v = 53.0f;
    CHECK(cfg_hist_apply(out, n, d) && same(d, b));   // и поверх другого состояния
    char sm[512]; size_t k = cfg_hist_summary(out, n, sm, sizeof sm);
    CHECK(k > 0 && json_ok(sm) && strstr(sm, "\"cfg\"") == nullptr && strstr(sm, "\"id\":7") != nullptr && strstr(sm, "cell_guard") != nullptr);
}

static void test_float_exact() {
    Config a, b; b.cell_soft_on_v = 3.07f; b.load_factor = 1.0f / 3.0f;
    char out[kCfgHistLineMax]; size_t n = cfg_hist_line(a, b, 1, 1, "api", false, out, sizeof out);
    Config c; CHECK(cfg_hist_apply(out, n, c));
    CHECK(c.cell_soft_on_v == b.cell_soft_on_v && c.load_factor == b.load_factor);   // float без потерь (%.9g)
}

static void test_apply_rejects() {
    Config a, b; b.soft_on_v = 50.8f; char out[kCfgHistLineMax];
    size_t n = cfg_hist_line(a, b, 1, 1, "api", false, out, sizeof out);
    // повреждённое значение: cell_guard = 9 → validate() отвергает, конфиг не меняется
    std::string s(out, n);
    size_t p = s.find("\"cfg\":"); size_t q = s.find("\"cell_guard\":", p);
    CHECK(q != std::string::npos);
    s[q + 13] = '9';
    Config c; Config before = c;
    CHECK(!cfg_hist_apply(s.c_str(), s.size(), c) && same(c, before));
    { const char* nl = "{\"id\":1,\"t\":1,\"src\":\"x\",\"ch\":{}}\n"; CHECK(!cfg_hist_apply(nl, strlen(nl), c)); }          // нет снимка
    CHECK(!cfg_hist_apply("garbage", 7, c));
    CfgHistHeader h; CHECK(!cfg_hist_header("garbage", 7, h) && !cfg_hist_header("", 0, h) && !cfg_hist_header(nullptr, 0, h));
}

static void test_unknown_keys_skipped() {
    // запись из другой версии: лишний ключ в cfg пропускается, известные применяются
    std::string line = "{\"id\":3,\"t\":9,\"src\":\"api\",\"ch\":{},\"cfg\":{\"future_key\":5,\"cell_guard\":0,\"soft_on_v\":50.8}}\n";
    Config c; CHECK(cfg_hist_apply(line.c_str(), line.size(), c) && c.cell_guard == 0 && fabsf(c.soft_on_v - 50.8f) < 1e-4f);
}

static void test_src_sanitized_and_size() {
    Config a, b;
    // все поля меняются: худший случай по длине
    b.hard_floor_v = 44.0f; b.soft_on_v = 3.11f; b.soft_off_v = 3.45f; b.full_v = 3.5f; b.hard_dwell_ms = 99999; b.soft_on_dwell_ms = 99999;
    b.soft_off_dwell_ms = 99999; b.no_data_timeout_ms = 99999; b.manual_pause_ms = 99999; b.hold_after_connect_ms = 99999; b.cmd_retry_ms = 99999;
    b.pow_min_base_w = 777; b.load_factor = 1.23f; b.ext_coef = 0.77f; b.ext_ttl_ms = 99999; b.ext_max_a = 55; b.charge_max_a = 77; b.step_gap_ms = 99999;
    char out[kCfgHistLineMax];
    size_t n = cfg_hist_line(a, b, 4102444799LL, 4000000000u, "restore:7\"},{\"x", false, out, sizeof out);
    CHECK(n > 0 && n < kCfgHistLineMax);
    std::string s(out, n - 1); CHECK(json_ok(s));
    CHECK(strstr(out, "restore:7___") != nullptr);                                        // кавычки и скобки заменены
    printf("cfg history worst-ish line: %zu bytes (max %zu)\n", n, kCfgHistLineMax);
    char tiny[100]; CHECK(cfg_hist_line(a, b, 1, 1, "api", false, tiny, sizeof tiny) == 0);   // не влезло → 0
    char small[400]; CHECK(cfg_hist_line(a, b, 1, 1, "api", false, small, sizeof small) == 0);
}

int main() {
    test_no_change(); test_line_and_roundtrip(); test_float_exact(); test_apply_rejects(); test_unknown_keys_skipped(); test_src_sanitized_and_size();
    printf("test_gs_cfghist: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
