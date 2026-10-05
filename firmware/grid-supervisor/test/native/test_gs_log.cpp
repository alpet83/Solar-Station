#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "gs_log.h"

using namespace gs;
static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_batching_by_size() {
    char buf[8192]; BatchLog l(buf, sizeof buf); char out[8192]; uint32_t day = 0;
    char line[64]; memset(line, 'a', 59); line[59] = 0;       // ~60 байт + метка
    uint32_t t = 0; size_t n = 0;
    for (int i = 0; i < 100; ++i) { l.add(20000, line, t); t += 1000; if ((n = l.take(t, false, out, sizeof out, day)) > 0) break; }
    CHECK(n > 0 && day == 20000); out[n] = 0;
    CHECK(n >= 4096 && n <= 4200);                               // порция около 4 КБ (метки не считаются в данных)
    CHECK(out[0] == 'a' && out[58] == 'a' && out[59] == '\n' && out[n - 1] == '\n' && strstr(out, "|") == nullptr);
    CHECK(l.pending_bytes() == 0 || l.pending_bytes() < 4096);
}

static void test_batching_by_time() {
    char buf[8192]; BatchLog l(buf, sizeof buf); char out[8192]; uint32_t day;
    l.add(20000, "x,1", 1000);
    CHECK(l.take(1000 + 599999, false, out, sizeof out, day) == 0);       // 10 мин не прошло
    size_t n = l.take(1000 + 600000, false, out, sizeof out, day);
    CHECK(n == 4 && !memcmp(out, "x,1\n", 4));
    CHECK(l.take(2000000, true, out, sizeof out, day) == 0);               // пусто — нечего
}

static void test_force_and_split_days() {
    char buf[8192]; BatchLog l(buf, sizeof buf); char out[8192]; uint32_t day = 0;
    l.add(100, "a", 0); l.add(100, "b", 10); l.add(101, "c", 20); l.add(101, "d", 30);
    size_t n = l.take(40, false, out, sizeof out, day);                      // граница суток — порция готова сразу
    CHECK(n == 4 && day == 100 && !memcmp(out, "a\nb\n", 4));
    CHECK(l.take(41, false, out, sizeof out, day) == 0);                    // остаток ждёт своей очереди
    n = l.take(42, true, out, sizeof out, day);
    CHECK(n == 4 && day == 101 && !memcmp(out, "c\nd\n", 4) && l.pending_bytes() == 0);
}

static void test_drop_oldest() {
    char buf[256]; BatchLog l(buf, sizeof buf); char out[256]; uint32_t day = 0;
    char line[64];
    for (int i = 0; i < 20; ++i) { snprintf(line, sizeof line, "line%02d-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", i); CHECK(l.add(5, line, i)); }
    CHECK(l.stats().dropped > 0 && l.pending_bytes() <= sizeof buf);
    size_t n = l.take(100, true, out, sizeof out, day);
    CHECK(n > 0 && strstr(out, "line19") != nullptr && strstr(out, "line00") == nullptr);   // самые новые остались
    char big[200]; memset(big, 'z', 199); big[199] = 0;
    CHECK(!l.add(5, big, 0));                                                // длиннее cap/2 — отброшена
}

static void test_note_flush() {
    char buf[1024]; BatchLog l(buf, sizeof buf);
    l.note_flush(true, 100, 2); l.note_flush(false, 50, 3);
    CHECK(l.stats().flushes == 1 && l.stats().bytes == 100 && l.stats().errors == 1 && l.stats().dropped == 3);
}

static void test_rate_limit() {
    RateLimit r(60, 3);                       // 60/час = 1 в минуту, всплеск 3
    CHECK(r.allow(0) && r.allow(0) && r.allow(0) && !r.allow(0));
    CHECK(!r.allow(30000));                   // через 30 с ещё нет
    CHECK(r.allow(60000));                    // через минуту токен набежал
    // частые вызовы не теряют дробную часть: 600 вызовов по 100 мс = 60 с -> один токен
    RateLimit q(60, 1); q.allow(0); int got = 0;
    for (int i = 1; i <= 650; ++i) if (q.allow(i * 100)) ++got;
    CHECK(got == 1);
}

static void test_names() {
    char p[32];
    CHECK(day_path(0, p, sizeof p) && !strcmp(p, "/m/nosync.csv"));
    CHECK(day_path(20731, p, sizeof p) && !strcmp(p, "/m/20261005.csv"));   // 2026-10-05
    CHECK(day_path(19782, p, sizeof p) && !strcmp(p, "/m/20240229.csv"));   // високосный день
    CHECK(day_path(1, p, sizeof p) && !strcmp(p, "/m/19700102.csv"));
    CHECK(!day_path(5, p, 10));
    CHECK(valid_log_name("20261005.csv") && valid_log_name("events.log") && valid_log_name("events.old") && valid_log_name("nosync.csv"));
    CHECK(!valid_log_name("../x") && !valid_log_name("") && !valid_log_name("2026100.csv") && !valid_log_name("20261005.csvx")
          && !valid_log_name("/m/20261005.csv") && !valid_log_name("2026100a.csv") && !valid_log_name(nullptr) && !valid_log_name("config.json"));
}

static LogFile mk(const char* n, uint32_t sz = 1000) { LogFile f; snprintf(f.name, sizeof f.name, "%s", n); f.size = sz; return f; }

static void test_retention() {
    LogFile fs[6] = { mk("20261003.csv"), mk("events.log"), mk("20261001.csv"), mk("20261005.csv"), mk("20261002.csv"), mk("20261004.csv") };
    size_t del[8];
    size_t n = select_old(fs, 6, 3, 50, 10, del, 8);                          // оставить 3 суточных: удалить 01 и 02
    CHECK(n == 2 && !strcmp(fs[del[0]].name, "20261001.csv") && !strcmp(fs[del[1]].name, "20261002.csv"));
    n = select_old(fs, 6, 90, 50, 10, del, 8); CHECK(n == 0);                   // в пределах срока и места хватает
    n = select_old(fs, 6, 90, 5, 10, del, 8);                                 // мало места: один самый старый
    CHECK(n == 1 && !strcmp(fs[del[0]].name, "20261001.csv"));
    LogFile two[2] = { mk("20261001.csv"), mk("20261002.csv") };
    CHECK(select_old(two, 2, 90, 1, 10, del, 8) == 0);                        // минимум 2 файла остаётся
    CHECK(select_old(fs, 6, 3, 50, 10, del, 1) == 1);                         // ограничение out_cap
    LogFile ev[1] = { mk("events.log") }; CHECK(select_old(ev, 1, 1, 0, 10, del, 8) == 0);   // события не трогаем
}

static void test_metrics() {
    MetricsRow r; char out[200];
    r.pack_v = 53.123f; r.grid_v = 237.4f; r.load_w = 759; r.chg_a = 0; r.pv_relay = 0; r.relay_grid = 1; r.cell_min_mv = 3281;
    size_t n = format_metrics(1791221098LL, r, out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "1791221098,53.12,,237.4,759,0.0,,,0,1,,,,3281"));
    int commas = 0; for (char* c = out; *c; ++c) if (*c == ',') ++commas;
    int hc = 0; for (const char* c = kMetricsHeader; *c; ++c) if (*c == ',') ++hc;
    CHECK(commas == hc);                                                       // число полей совпадает с заголовком
    MetricsRow z; z.pack_v = INFINITY; n = format_metrics(-1, z, out, sizeof out); CHECK(n > 0 && !strncmp(out, "0,,", 3));
    char small[10]; CHECK(format_metrics(1, r, small, sizeof small) == 0);
    char mid[30]; CHECK(format_metrics(1791221098LL, r, mid, sizeof mid) == 0);   // не влезло -> 0, без переполнения
}

int main() {
    test_batching_by_size(); test_batching_by_time(); test_force_and_split_days(); test_drop_oldest(); test_note_flush();
    test_rate_limit(); test_names(); test_retention(); test_metrics();
    printf("test_gs_log: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
