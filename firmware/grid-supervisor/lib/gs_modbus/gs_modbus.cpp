#include "gs_modbus.h"

namespace gs {

uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int b = 0; b < 8; ++b) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

static void put_crc(uint8_t* f, size_t n) {
    uint16_t c = crc16(f, n);
    f[n] = c & 0xFF;          // Modbus RTU: CRC младшим байтом вперёд
    f[n + 1] = c >> 8;
}

static bool crc_ok(const uint8_t* b, size_t len) {
    return len >= 4 && crc16(b, len - 2) == (uint16_t)(b[len - 2] | (b[len - 1] << 8));
}

size_t build_read(uint8_t* out, size_t cap, uint8_t id, uint8_t fn, uint16_t addr, uint16_t count) {
    if (cap < 8 || (fn != 0x03 && fn != 0x04) || count == 0 || count > 125) return 0;
    out[0] = id; out[1] = fn;
    out[2] = addr >> 8; out[3] = addr & 0xFF;
    out[4] = count >> 8; out[5] = count & 0xFF;
    put_crc(out, 6);
    return 8;
}

size_t build_write_ongrid(uint8_t* out, size_t cap, uint8_t id, bool connect) {
    if (cap < 8) return 0;
    const uint16_t addr = kRegOngridSwitch, val = connect ? 1 : 0;
    out[0] = id; out[1] = 0x06;
    out[2] = addr >> 8; out[3] = addr & 0xFF;
    out[4] = val >> 8; out[5] = val & 0xFF;
    put_crc(out, 6);
    return 8;
}

Rx parse_read(const uint8_t* b, size_t len, uint8_t id, uint8_t fn, uint16_t count, uint16_t* regs) {
    if (len < 5) return Rx::Short;
    if (!crc_ok(b, len)) return (len < read_response_len(count)) ? Rx::Short : Rx::BadCrc;
    if (b[0] != id) return Rx::WrongId;
    if (b[1] == (fn | 0x80)) return Rx::Exception;
    if (b[1] != fn) return Rx::WrongFunc;
    if (len != read_response_len(count) || b[2] != 2u * count) return Rx::WrongLen;
    for (uint16_t i = 0; i < count; ++i) regs[i] = (uint16_t)((b[3 + 2 * i] << 8) | b[4 + 2 * i]);
    return Rx::Ok;
}

Rx parse_write_echo(const uint8_t* b, size_t len, uint8_t id, uint16_t addr, uint16_t value) {
    if (len < 8) return Rx::Short;
    if (!crc_ok(b, len)) return Rx::BadCrc;
    if (b[0] != id) return Rx::WrongId;
    if (b[1] == 0x86) return Rx::Exception;
    if (b[1] != 0x06) return Rx::WrongFunc;
    if (len != 8) return Rx::WrongLen;
    const uint16_t a = (uint16_t)((b[2] << 8) | b[3]), v = (uint16_t)((b[4] << 8) | b[5]);
    return (a == addr && v == value) ? Rx::Ok : Rx::WrongEcho;
}

void decode_rt(const uint16_t* r, Telemetry& t) {
    t.pack_v = r[4] * 0.1f;
    t.grid_v = r[6] * 0.1f;
    t.load_w = (float)r[14];
    t.relay_grid = r[37] ? 1 : 0;
    t.rt_ok = true;
}

void decode_chg(const uint16_t* r, Telemetry& t) {
    t.pack_v_alt = r[5] * 0.1f;
    t.chg_curr_a = r[6] * 0.1f;
    t.chg_ok = true;
}

void decode_cfg(const uint16_t* r, Telemetry& t) {
    t.ongrid_reg = (r[4] == 1) ? 1 : (r[4] == 0 ? 0 : -1);   // иное значение = неизвестно
    t.cfg_ok = true;
}

Input make_input(const Telemetry& t, uint64_t now_ms, bool safe_mode, float ext_a, uint64_t ext_age_ms) {
    Input in;
    in.now_ms = now_ms;
    in.have_sample = t.rt_ok;
    in.safe_mode = safe_mode;
    in.ext_charge_a = ext_a;
    in.ext_age_ms = ext_age_ms;
    if (t.rt_ok) { in.pack_v = t.pack_v; in.grid_v = t.grid_v; in.load_w = t.load_w; }
    if (t.chg_ok) { in.pack_v_alt = t.pack_v_alt; in.charge_a = t.chg_curr_a; }
    if (t.cfg_ok) in.ongrid_reg = t.ongrid_reg;
    return in;
}

}  // namespace gs
