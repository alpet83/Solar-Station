// gs_util — переносимые утилиты оболочки: конфиг <-> текст/JSON с валидацией, JSON состояния.
// Без Arduino-зависимостей, тестируется нативно.
#pragma once
#include <cstddef>
#include <cstdint>

#include "gs_core.h"
#include "gs_modbus.h"

namespace gs {

// Список полей Config, доступных для настройки. Имена — ключи JSON; в NVS пишем по индексу ("c00"...),
// т.к. ключ NVS ограничен 15 символами.
struct CfgField { const char* key; enum Kind { F32, U32 } kind; size_t off; };
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

struct StateView {
    const Telemetry* tel; const Decision* dec; const Input* in;
    uint64_t uptime_ms; uint32_t seq; uint32_t boot_id; bool safe_mode;
    int64_t epoch_s;                 // -1 если время не синхронизировано
    bool paused; uint64_t pause_left_ms;
    uint32_t rx_ok, rx_err, rx_foreign, rx_busy, rx_exc;
};
// JSON состояния. Время — UTC: epoch в "t" и ISO в "ts" (только если epoch_s >= 0).
size_t state_to_json(const StateView& s, char* out, size_t cap);

// Разбор значения внешнего тока ("12.5"): true и [0..500] при корректном вводе.
bool parse_ext_current(const char* s, float& out);

}  // namespace gs
