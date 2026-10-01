// gs_modbus — переносимый (без Arduino) слой Modbus RTU: CRC, построение запросов,
// строгая проверка ответов и декодирование телеметрии MUST PV1800 в gs::Input.
//
// Единственная разрешённая запись — регистр 20105 (ongrid_switch) значениями 0/1.
// Проверка whitelist делается ЗДЕСЬ, на уровне построения кадра, чтобы никакой код выше
// не мог сформировать другую запись.
#pragma once

#include <cstddef>
#include <cstdint>

#include "gs_core.h"

namespace gs {

constexpr uint8_t  kInverterSlave = 4;
constexpr uint16_t kRegOngridSwitch = 20105;

uint16_t crc16(const uint8_t* data, size_t len);

enum class Rx : uint8_t {
    Ok = 0,
    Short,        // кадр короче минимально возможного / не дослан
    BadCrc,
    WrongId,      // чужой slave id (в т.ч. кадр второго мастера/его ответа)
    WrongFunc,
    Exception,    // ответ-исключение Modbus
    WrongLen,     // длина данных не соответствует запросу
    WrongEcho,    // эхо записи не совпало с запросом
};

// Запрос чтения (функции 0x03/0x04). Возвращает длину кадра (8) или 0 при ошибке аргументов.
size_t build_read(uint8_t* out, size_t cap, uint8_t id, uint8_t fn, uint16_t addr, uint16_t count);

// Запрос записи одного регистра (0x06). Возвращает 0, если регистр/значение не из whitelist.
size_t build_write_ongrid(uint8_t* out, size_t cap, uint8_t id, bool connect);

// Разбор ответа на чтение. regs должен вместить count слов.
Rx parse_read(const uint8_t* buf, size_t len, uint8_t id, uint8_t fn, uint16_t count, uint16_t* regs);

// Разбор эхо-ответа записи 0x06.
Rx parse_write_echo(const uint8_t* buf, size_t len, uint8_t id, uint16_t addr, uint16_t value);

// Ожидаемая длина ответа на чтение count регистров.
constexpr size_t read_response_len(uint16_t count) { return 5u + 2u * count; }

// --- декодирование блоков MUST PV1800 ---
constexpr uint16_t kBlockRtAddr = 25201, kBlockRtCount = 79;   // реальное время
constexpr uint16_t kBlockChgAddr = 15201, kBlockChgCount = 22; // зарядный контроллер
constexpr uint16_t kBlockCfgAddr = 20101, kBlockCfgCount = 44; // управление (RW)

struct Telemetry {
    bool  rt_ok = false, chg_ok = false, cfg_ok = false;
    float pack_v = kNaN;       // 25205, x0.1
    float grid_v = kNaN;       // 25207, x0.1
    float load_w = kNaN;       // 25215
    int8_t relay_grid = -1;    // 25238
    float pack_v_alt = kNaN;   // 15206, x0.1
    float chg_curr_a = kNaN;   // 15207, x0.1
    int8_t ongrid_reg = -1;    // 20105
};

void decode_rt(const uint16_t* regs, Telemetry& t);    // 79 слов с 25201
void decode_chg(const uint16_t* regs, Telemetry& t);   // 22 слова с 15201
void decode_cfg(const uint16_t* regs, Telemetry& t);   // 44 слова с 20101

// Собирает Input для ядра. Данные считаются свежими, только если успешно прочитан блок
// реального времени (rt_ok). Остальные блоки опциональны: без них поля остаются NaN/-1.
Input make_input(const Telemetry& t, uint64_t now_ms, bool safe_mode,
                 float ext_charge_a, uint64_t ext_age_ms);

}  // namespace gs
