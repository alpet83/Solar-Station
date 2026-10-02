// Оболочка: NVS-настройки, Wi-Fi (STA или AP для первичной настройки), NTP, веб-сервер с
// basic-auth, JSON-состояние, OTA. Ядро и шина от неё не зависят: оболочка только читает
// снимок состояния и вносит валидированные изменения конфигурации.
#pragma once
#include <Arduino.h>
#include "gs_core.h"
#include "gs_modbus.h"
#include "gs_util.h"

namespace shell {

// Снимок состояния, публикуемый задачей ядра (под мьютексом).
struct Snapshot {
    gs::Telemetry tel; gs::Decision dec; gs::Input in;
    uint64_t uptime_ms = 0; uint32_t seq = 0; bool safe_mode = false;
    bool paused = false; uint64_t pause_left_ms = 0; bool sniff_only = false;
    uint32_t rx_ok = 0, rx_err = 0, rx_foreign = 0, rx_busy = 0, rx_exc = 0;
    gs::RegCache cache;
    gs::Event events[gs::kEventCap]; size_t n_events = 0;     // от новых к старым
};

void begin(uint32_t boot_id);
void loop();                                   // вызывать из loop(): обслуживание HTTP/Wi-Fi

void publish(const Snapshot& s);               // задача ядра публикует снимок
bool take_config(gs::Config& out);             // true, если конфигурация изменилась (однократно)
gs::Config load_config();                      // из NVS, при невалидности — значения по умолчанию
bool ext_charge(float& a, uint64_t& age_ms);   // последнее внешнее значение тока от Pi (без блокировок)
int take_manual();                             // -1 нет, 0 = OFF-grid, 1 = ON-grid (команда оператора, однократно)
bool ota_active();                             // идёт загрузка образа: ядру не передавать на шину
uint64_t heartbeat_age_ms();                   // сколько оболочка не вызывала loop()
void mark_healthy(uint32_t publishes);         // ядро сообщает число успешных циклов (критерий валидности OTA-образа)

bool sniff_only();                             // режим «только приём» (в NVS, переживает перезагрузку)
// Провайдер JSON для /api/sniff: копирует готовый текст в буфер, возвращает длину (0 — нет данных).
typedef size_t (*SniffProvider)(char* out, size_t cap);
void set_sniff_provider(SniffProvider p);

}  // namespace shell
