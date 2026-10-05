// gs_util — переносимые утилиты оболочки: конфиг <-> текст/JSON с валидацией, JSON состояния.
// Без Arduino-зависимостей, тестируется нативно.
#pragma once
#include <cstddef>
#include <cstdint>

#include "gs_core.h"
#include "gs_bus.h"
#include "gs_modbus.h"
#include "gs_can.h"

namespace gs {

// Список полей Config, доступных для настройки. Имена — ключи JSON; в NVS пишем по индексу ("c00"...),
// т.к. ключ NVS ограничен 15 символами.
struct CfgField { const char* key; enum Kind { F32, U32, U8 } kind; size_t off; };
extern const CfgField kCfgFields[];
extern const size_t kCfgFieldCount;

// Устанавливает поле по имени из строки. Результат применяется ТОЛЬКО если вся конфигурация
// после изменения проходит validate(); иначе cfg не меняется и возвращается false.
bool config_set(Config& cfg, const char* key, const char* value);

// Атомарное применение набора пар ключ/значение: все поля разбираются в кандидата и
// проверяются ОДНИМ вызовом validate(). При любой ошибке cfg не меняется. Это нужно и для
// загрузки из NVS (набор, недостижимый пошагово), и для сдвига нескольких порогов сразу.
bool config_apply_pairs(Config& cfg, const char* const* keys, const char* const* vals, size_t n);

// Читает значение поля в строку (для NVS/JSON). false — неизвестный ключ.
bool config_get(const Config& cfg, const char* key, char* out, size_t cap);

size_t config_to_json(const Config& cfg, char* out, size_t cap);

// Имена флагов решения через запятую, например "SOFT_LOW,HOLD".
size_t flags_to_str(uint32_t flags, char* out, size_t cap);

// Событие переключения (журнал в RAM, последние kEventCap). id растёт в пределах boot_id.
constexpr size_t kEventCap = 8;
struct Event { uint32_t id = 0; Cmd cmd = Cmd::None; Reason reason = Reason::None; int64_t epoch_s = -1; uint64_t uptime_ms = 0; float v = 0; bool ok = false; };
const char* reason_name(Reason r);

struct StateView {
    const Telemetry* tel; const Decision* dec; const Input* in;
    uint64_t uptime_ms; uint32_t seq; uint32_t boot_id; bool safe_mode;
    int64_t epoch_s;                 // -1 если время не синхронизировано
    bool paused; uint64_t pause_left_ms;
    uint32_t rx_ok, rx_err, rx_foreign, rx_busy, rx_exc;
    const Event* events = nullptr; size_t n_events = 0;     // от новых к старым
    bool sniff_only = false;                                // режим «только приём»: управление сетью выключено
    const BatteryBrief* bat = nullptr;                      // сводка батареи по CAN; nullptr = объект с null-полями
};
// JSON состояния. Время — UTC: epoch в "t" и ISO в "ts" (только если epoch_s >= 0).
size_t state_to_json(const StateView& s, char* out, size_t cap);

// JSON для /api/sniff: итог по ритму логгера, последние пачки и кадры (времена — «сколько мс
// назад» от now_ms, плюс UTC-метка, если известно epoch_now_s >= 0). 0 — не влезло в буфер.
size_t sniff_to_json(const BusSniffer& sn, const LoggerTracker& tr, uint64_t now_ms, int64_t epoch_now_s,
                     size_t n_frames, size_t n_bursts, char* out, size_t cap);

// JSON ответа /api/regs: {"addr":..,"count":..,"age_ms":..,"regs":[..]}. 0 — не влезло в буфер.
size_t regs_to_json(uint16_t addr, uint16_t count, uint64_t age_ms, const uint16_t* regs, char* out, size_t cap);

// Разбор значения внешнего тока ("12.5"): true и [0..500] при корректном вводе.
bool parse_ext_current(const char* s, float& out);

}  // namespace gs
