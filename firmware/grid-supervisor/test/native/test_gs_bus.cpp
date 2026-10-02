#include "gs_bus.h"
#include "gs_modbus.h"
#include <cstdio>
#include <vector>
using namespace gs;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<uint8_t> req(uint8_t id, uint8_t fn, uint16_t a, uint16_t n) {
    uint8_t b[8]; build_read(b, 8, id, fn, a, n); return {b, b + 8};
}
static std::vector<uint8_t> resp(uint8_t id, uint8_t fn, uint16_t n) {
    std::vector<uint8_t> f{id, fn, (uint8_t)(n * 2)};
    for (uint16_t i = 0; i < n; ++i) { f.push_back(0); f.push_back((uint8_t)i); }
    uint16_t c = crc16(f.data(), f.size()); f.push_back(c & 255); f.push_back(c >> 8); return f;
}

static void test_classify() {
    auto r = req(4, 3, 20001, 16);
    FrameRec f = classify_frame(r.data(), r.size());
    CHECK(f.kind == FrameKind::Request && f.addr == 20001 && f.count == 16 && f.crc_ok && f.id == 4);
    auto p = resp(4, 3, 16); f = classify_frame(p.data(), p.size());
    CHECK(f.kind == FrameKind::Response && f.count == 16);
    uint8_t w[8]; build_write_ongrid(w, 8, 4, true); f = classify_frame(w, 8);
    CHECK(f.kind == FrameKind::WriteEcho && f.addr == 20105 && f.count == 1);
    std::vector<uint8_t> e{4, 0x83, 2}; uint16_t c = crc16(e.data(), 3); e.push_back(c & 255); e.push_back(c >> 8);
    CHECK(classify_frame(e.data(), e.size()).kind == FrameKind::Exception);
    auto glued = std::vector<uint8_t>{0xFF, 0x00}; glued.insert(glued.end(), r.begin(), r.end()); glued.insert(glued.end(), p.begin(), p.end());
    f = classify_frame(glued.data(), glued.size()); CHECK(f.kind == FrameKind::Request && f.addr == 20001);
    auto bad = r; bad[7] ^= 1; CHECK(classify_frame(bad.data(), bad.size()).kind == FrameKind::Junk);
    uint8_t z[3] = {1, 2, 3}; CHECK(classify_frame(z, 3).kind == FrameKind::Junk);
}

static void test_sniffer() {
    BusSniffer s(6);
    auto r = req(4, 3, 25201, 79), p = resp(4, 3, 79);
    CHECK(!s.feed(r.data(), 4, 1000)); CHECK(!s.feed(r.data() + 4, 4, 1001));   // кадр пришёл кусками
    CHECK(!s.poll(1004)); CHECK(s.poll(1010));
    FrameRec f; CHECK(s.recent(0, f) && f.kind == FrameKind::Request && f.t_ms == 1000);
    CHECK(!s.feed(p.data(), p.size(), 1030));
    CHECK(s.feed(r.data(), r.size(), 1200));                                    // feed сам закрывает прошлый кадр по паузе
    CHECK(s.last().kind == FrameKind::Response && s.last().count == 79);
    CHECK(s.poll(1300) && s.total() == 3 && s.count() == 3);
    std::vector<uint8_t> big(700, 0x55); s.feed(big.data(), big.size(), 2000); s.poll(2100);
    CHECK(s.junk() >= 2);                                                         // переполнение режется на мусор
    for (int i = 0; i < 100; ++i) { s.feed(r.data(), 8, 3000 + i * 20); s.poll(3000 + i * 20 + 10); }
    CHECK(s.count() == BusSniffer::kRing && s.total() > 64);
}

// Логгер: пачка из n запросов с шагом step мс, начало в t0. Своих запросов Pi (25201x79) повторяет каждые 5 с.
static void feed_burst(LoggerTracker& tr, uint64_t t0, int n, uint32_t step, uint8_t id = 4) {
    for (int i = 0; i < n; ++i) {
        FrameRec f; f.kind = FrameKind::Request; f.id = id; f.fn = 3; f.addr = (uint16_t)(20001 + i); f.count = 16; f.crc_ok = true;
        tr.observe(f, t0 + (uint64_t)i * step);
    }
}

static void test_tracker() {
    LoggerCfg c; LoggerTracker tr(c);
    tr.ignore(3, 25201, 79);
    FrameRec self; self.kind = FrameKind::Request; self.id = 4; self.fn = 3; self.addr = 25201; self.count = 79; self.crc_ok = true;
    for (int i = 0; i < 20; ++i) tr.observe(self, 1000 + i * 5000);
    CHECK(tr.ignored() == 20 && tr.bursts_total() == 0);                       // свои запросы не пачка

    const uint64_t base = 40000;
    // три пачки по 8 запросов с шагом 400 мс (длина ~2.8 с), период 300 с с джиттером
    feed_burst(tr, base, 8, 400); tr.tick(base + 20000);
    CHECK(!tr.locked() && tr.bursts_total() == 1);
    feed_burst(tr, base + 300500, 8, 400); tr.tick(base + 320000);
    CHECK(!tr.locked() && tr.bursts_total() == 2);
    feed_burst(tr, base + 599800, 8, 400); tr.tick(base + 620000);
    CHECK(tr.locked() && tr.period_ms() > 295000 && tr.period_ms() < 305000);

    BurstRec b; CHECK(tr.burst(0, b) && b.in_phase && b.n_req == 8 && b.first_addr == 20001);

    // следующая пачка ждётся около base+900000
    uint64_t nt; CHECK(tr.next_start(base + 620000, nt) && nt > base + 895000 && nt < base + 905000);
    Gate g = tr.may_transmit(base + 650000, 3000); CHECK(g.ok);               // середина паузы — можно
    CHECK(tr.active_window(base + 650000));
    g = tr.may_transmit(base + 899000, 3000); CHECK(!g.ok && g.why == Gate::Predicted && g.wait_ms > 0);   // перед пачкой молчим
    g = tr.may_transmit(base + 893000, 3000); CHECK(!g.ok);                  // учитывает длительность транзакции
    CHECK(!tr.active_window(base + 890000));
    g = tr.may_transmit(base + 920000, 3000); CHECK(g.ok);                    // после окна — снова можно

    // идущая пачка блокирует независимо от предсказания, плюс защитная пауза после неё
    feed_burst(tr, base + 900100, 8, 400);
    g = tr.may_transmit(base + 900100 + 1000, 100); CHECK(!g.ok && g.why == Gate::InBurst);
    g = tr.may_transmit(base + 900100 + 7 * 400 + 500, 100); CHECK(!g.ok);
    tr.tick(base + 910000); CHECK(tr.locked());

    // внеритмовая пачка (отладка из облака): гейтит, но не сбивает ритм
    feed_burst(tr, base + 1000000, 4, 300); tr.tick(base + 1010000);
    CHECK(tr.off_rhythm() == 1 && tr.locked());
    g = tr.may_transmit(base + 1000000 + 500, 100); CHECK(!g.ok);
    feed_burst(tr, base + 1200400, 8, 400); tr.tick(base + 1220000);          // ритм продолжается
    CHECK(tr.locked() && tr.off_rhythm() == 1);

    // три пропуска подряд — захват теряется
    for (uint64_t t = base + 1300000; t < base + 2500000; t += 5000) tr.tick(t);
    CHECK(!tr.locked());
    g = tr.may_transmit(base + 2500000, 3000); CHECK(g.ok);                   // без захвата — только реактивно
}

static void test_tracker_misc() {
    LoggerTracker tr;
    FrameRec f; f.kind = FrameKind::Request; f.id = 7; f.fn = 3; f.crc_ok = true;   // чужой slave id
    tr.observe(f, 1000); CHECK(tr.bursts_total() == 0 && tr.may_transmit(1100, 100).ok);
    f.id = 4; f.crc_ok = false; tr.observe(f, 1000); CHECK(tr.may_transmit(1100, 100).ok);
    f.crc_ok = true; f.kind = FrameKind::Response; tr.observe(f, 1000); CHECK(tr.may_transmit(1100, 100).ok);
    f.kind = FrameKind::WriteEcho; f.fn = 6; f.addr = 20105; tr.observe(f, 1000);     // чужая запись — тоже трафик
    CHECK(!tr.may_transmit(1100, 100).ok);
    CHECK(tr.may_transmit(1000 + 20000, 100).ok);
    tr.observe(f, 500);                                                             // время назад — игнор
    CHECK(tr.may_transmit(1000 + 20000, 100).ok);
    for (size_t i = 0; i < 20; ++i) tr.ignore(3, (uint16_t)i, 1);
    CHECK(!tr.ignore(3, 99, 1));                                                    // переполнение списка безопасно
}

int main() {
    test_classify(); test_sniffer(); test_tracker(); test_tracker_misc();
    printf("bus: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
