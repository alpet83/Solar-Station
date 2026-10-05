// gs_cfghist — история изменений конфигурации ядра (gs::Config) в формате JSONL и восстановление из записи.
// Чистая логика без Arduino (нативные тесты: test/native/test_gs_cfghist.cpp). Хранение на SD — src/sdlog.cpp.
//
// Одна запись = одна строка JSON (заканчивается '\n'):
//   {"id":7,"t":1791230000,"src":"api","ch":{"cell_guard":[1,0],"soft_on_v":[3.04999995,3.0999999]},"cfg":{...полный снимок...}}
// ch — что изменилось ([было, стало], числа в том же текстовом виде, что в /api/config); cfg — полный снимок после изменения,
// по нему делается восстановление. В Config нет секретов (пороги и тайминги): пароли, Wi-Fi и URL вебхука в историю НЕ попадают.
#pragma once
#include <cstddef>
#include <cstdint>

#include "gs_core.h"

namespace gs {

constexpr size_t kCfgHistLineMax = 2048;   // максимальная длина строки (измерено: при изменении 18 полей ≈ 1,2 КБ)

// Строит строку записи. Если before и after совпадают — возвращает 0 (запись не нужна), кроме force (baseline: пустой ch).
// src — короткая метка источника ("api", "restore:7", "baseline"; недопустимые символы заменяются на '_'). 0 — не влезло.
size_t cfg_hist_line(const Config& before, const Config& after, int64_t epoch_s, uint32_t id, const char* src,
                     bool force, char* out, size_t cap);

struct CfgHistHeader { uint32_t id = 0; int64_t t = 0; char src[32] = {0}; };
// Разбирает заголовок (id, t, src) из строки записи. false — не похоже на запись.
bool cfg_hist_header(const char* line, size_t len, CfgHistHeader& h);

// Краткая запись для списка: {"id":..,"t":..,"src":"..","ch":{..}} (без полного снимка). 0 — не влезло или строка битая.
size_t cfg_hist_summary(const char* line, size_t len, char* out, size_t cap);

// Применяет снимок "cfg" из записи к cfg атомарно (все поля или ничего; проверка Config::validate()).
// Неизвестные ключи пропускаются (запись из другой версии прошивки). false — снимка нет/значения отвергнуты.
bool cfg_hist_apply(const char* line, size_t len, Config& cfg);

}  // namespace gs
