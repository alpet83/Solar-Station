// Пассивный CAN-сниффер батареи (TWAI, только приём): отдельная задача FreeRTOS, разбор кадров — lib/gs_can.
// В CAN ничего не передаётся: драйвер в режиме listen-only, очередь передачи отключена.
#pragma once
#include <Arduino.h>
#include "gs_can.h"
#include "gs_trig.h"

namespace canrx {

void begin();                        // читает скорость из NVS и запускает задачу
void set_speed(int kbps);            // 0 = выключить, 250 или 500; сохраняет в NVS, применяет задача CAN (счётчики сбрасываются)
void set_ack(bool on);              // true: подтверждать кадры (ACK); сохраняется в NVS (BMS без других узлов иначе повторяет кадры вечно)
bool ack();
void set_heartbeat(bool on);        // true: раз в секунду слать кадр 0x305 (8 нулей) как «жив» инвертора; включает ACK; сохраняется в NVS
bool heartbeat();
int speed();                         // заданная скорость (0 = выключен)
void brief(gs::BatteryBrief& out);   // сводка для /api/state
size_t api_json(char* out, size_t cap);          // JSON для /api/can; 0 — нет данных/не влезло
void status_line(char* out, size_t cap);         // строка для консоли

// ---- защита по ячейкам и триггеры BMS (слой «обработки» вызывается из цикла управления) ----
void set_watch_cfg(const gs::WatchCfg& c);       // пороги генератора триггеров (из gs::Config через gs::make_watch_cfg)
// Снимок для ядра под mutex'ом: cell_min_v — самая слабая ячейка, В (NaN: данных старше 10 с или протокол без ячеек);
// cells_seen — в этом запуске ячейки хоть раз были видны. false — mutex не получен (значения не тронуты).
bool cell_snapshot(float& cell_min_v, bool& cells_seen);
size_t drain_triggers(gs::Trigger* out, size_t max);   // выгребает очередь триггеров (от старых к новым); возвращает число
size_t events_json(char* out, size_t cap);             // JSON /api/bms_events (журнал последних 16 событий BMS); 0 — нет/не влезло

}  // namespace canrx
