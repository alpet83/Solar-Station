// sdlog — запись логов на microSD (FAT32): метрики пакетами, события, веб-доступ к файлам (/api/log).
// Логика буфера/имён/ротации — lib/gs_log (нативные тесты). Здесь: SPI/SD, задача низкого приоритета, mutex.
// Карты может не быть: тогда ничего не выделяется, раз в 5 минут повторяется попытка монтирования, управление не страдает.
#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include "gs_log.h"

namespace sdlog {

void begin();                                                    // запускает задачу (монтирование — внутри, не блокирует)
// Метрики: вызывается из цикла управления часто; запись строки — не чаще раза в 10 с.
void sample(const gs::MetricsRow& row, int64_t epoch_s, uint32_t now_ms);
// Событие (редкое): "epoch,текст" в events.log; частота ограничена (120/час, всплеск 20). Звать можно из любой задачи.
void event(int64_t epoch_s, uint32_t now_ms, const char* text);
// Синхронный сброс буферов на карту (перед OTA/перезагрузкой); до ~3 с. Без карты — сразу возвращается.
void flush_now();
// JSON для GET /api/log (состояние, статистика, список файлов); 0 — не влезло.
size_t status_json(char* out, size_t cap);
// Отдаёт файл клиенту (имя проверяется gs::valid_log_name). false — файла/карты нет (ответ не отправлен).
bool stream_file(WebServer& srv, const char* name);

}  // namespace sdlog
