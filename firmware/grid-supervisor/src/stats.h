// stats — обёртка над gs::PvStats с мьютексом: feed вызывает цикл управления, JSON читает HTTP-задача.
#pragma once
#include <cstddef>
#include <cstdint>
#include "gs_modbus.h"

namespace stats {
void begin();
void feed(const gs::Telemetry& t, uint32_t now_ms, int64_t epoch_s, float pack_v);   // t.chg_ok обязателен
size_t json(char* buf, size_t cap);
}
