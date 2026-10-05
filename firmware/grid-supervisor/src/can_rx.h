// Пассивный CAN-сниффер батареи (TWAI, только приём): отдельная задача FreeRTOS, разбор кадров — lib/gs_can.
// В CAN ничего не передаётся: драйвер в режиме listen-only, очередь передачи отключена.
#pragma once
#include <Arduino.h>
#include "gs_can.h"

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

}  // namespace canrx
