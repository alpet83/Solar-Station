#include "gs_modbus.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace gs;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<uint8_t> resp(uint8_t id, uint8_t fn, const std::vector<uint16_t>& r) {
    std::vector<uint8_t> f{id, fn, (uint8_t)(r.size() * 2)};
    for (auto w : r) { f.push_back(w >> 8); f.push_back(w & 0xFF); }
    uint16_t c = crc16(f.data(), f.size()); f.push_back(c & 0xFF); f.push_back(c >> 8);
    return f;
}

int main() {
    // известный вектор CRC: 01 03 00 00 00 0A -> C5 CD (лежит как CD? low first: C5 CD)
    const uint8_t v[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
    CHECK(crc16(v, 6) == 0xCDC5);

    uint8_t q[16];
    CHECK(build_read(q, 16, 4, 0x04, 25201, 79) == 8);
    CHECK(q[0] == 4 && q[1] == 4 && q[2] == 0x62 && q[3] == 0x71 && q[5] == 79);
    CHECK(crc16(q, 8) == 0);                           // CRC кадра с CRC даёт 0
    CHECK(build_read(q, 16, 4, 0x06, 1, 1) == 0);      // не чтение
    CHECK(build_read(q, 16, 4, 0x03, 1, 0) == 0);
    CHECK(build_read(q, 16, 4, 0x03, 1, 126) == 0);
    CHECK(build_read(q, 4, 4, 0x03, 1, 1) == 0);

    // запись: только 20105 = 0/1
    CHECK(build_write_ongrid(q, 16, 4, true) == 8);
    CHECK(q[1] == 0x06 && q[2] == 0x4E && q[3] == 0x89 && q[4] == 0 && q[5] == 1 && crc16(q, 8) == 0);
    CHECK(build_write_ongrid(q, 16, 4, false) == 8 && q[5] == 0);
    CHECK(build_write_ongrid(q, 4, 4, true) == 0);

    // разбор чтения
    std::vector<uint16_t> r(5), out(5);
    for (int i = 0; i < 5; ++i) r[i] = 500 + i;
    auto ok = resp(4, 0x04, r);
    CHECK(parse_read(ok.data(), ok.size(), 4, 0x04, 5, out.data()) == Rx::Ok && out[3] == 503);
    auto bad = ok; bad[4] ^= 1;
    CHECK(parse_read(bad.data(), bad.size(), 4, 0x04, 5, out.data()) == Rx::BadCrc);
    auto wid = resp(1, 0x04, r);
    CHECK(parse_read(wid.data(), wid.size(), 4, 0x04, 5, out.data()) == Rx::WrongId);
    auto wfn = resp(4, 0x03, r);
    CHECK(parse_read(wfn.data(), wfn.size(), 4, 0x04, 5, out.data()) == Rx::WrongFunc);
    auto sh = resp(4, 0x04, {1, 2});
    CHECK(parse_read(sh.data(), sh.size(), 4, 0x04, 5, out.data()) == Rx::WrongLen);
    CHECK(parse_read(ok.data(), 3, 4, 0x04, 5, out.data()) == Rx::Short);
    CHECK(parse_read(ok.data(), ok.size() - 2, 4, 0x04, 5, out.data()) == Rx::Short);   // оборванный кадр
    std::vector<uint8_t> ex{4, 0x84, 2}; uint16_t c = crc16(ex.data(), 3); ex.push_back(c & 255); ex.push_back(c >> 8);
    CHECK(parse_read(ex.data(), ex.size(), 4, 0x04, 5, out.data()) == Rx::Exception);
    // «ответ» второго мастера — его запрос с нашим id: функция та же, длина не та
    uint8_t foreign[8]; build_read(foreign, 8, 4, 0x04, 25201, 79);
    CHECK(parse_read(foreign, 8, 4, 0x04, 5, out.data()) != Rx::Ok);

    // эхо записи
    uint8_t w[8]; build_write_ongrid(w, 8, 4, true);
    CHECK(parse_write_echo(w, 8, 4, 20105, 1) == Rx::Ok);
    CHECK(parse_write_echo(w, 8, 4, 20105, 0) == Rx::WrongEcho);
    w[7] ^= 1; CHECK(parse_write_echo(w, 8, 4, 20105, 1) == Rx::BadCrc);

    // декодирование
    std::vector<uint16_t> rt(79, 0), chg(22, 0), cfg(44, 0);
    rt[4] = 518; rt[6] = 2301; rt[14] = 640; rt[37] = 1; chg[5] = 523; chg[6] = 255; cfg[4] = 1;
    Telemetry t; decode_rt(rt.data(), t); decode_chg(chg.data(), t); decode_cfg(cfg.data(), t);
    CHECK(t.pack_v > 51.79f && t.pack_v < 51.81f && t.grid_v > 230.0f && t.load_w == 640 && t.relay_grid == 1);
    CHECK(t.pack_v_alt > 52.29f && t.pack_v_alt < 52.31f && t.chg_curr_a > 25.4f && t.chg_curr_a < 25.6f && t.ongrid_reg == 1);
    Input in = make_input(t, 12345, false, kNaN, 0);
    CHECK(in.have_sample && in.ongrid_reg == 1 && in.now_ms == 12345);
    // без блока реального времени данные не свежие, даже если остальное прочитано
    Telemetry u; decode_chg(chg.data(), u); decode_cfg(cfg.data(), u);
    CHECK(!make_input(u, 1, false, kNaN, 0).have_sample);
    // неизвестное значение регистра 20105
    cfg[4] = 7; Telemetry z; decode_cfg(cfg.data(), z); CHECK(z.ongrid_reg == -1);

    printf("modbus: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
