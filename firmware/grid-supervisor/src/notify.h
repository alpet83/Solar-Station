// Уведомления на HTTP-вебхук (слой Arduino над lib/gs_notify): настройки в NVS ("wh_*"), очередь, отдельная
// задача FreeRTOS низкого приоритета. Цикл управления сюда только кладёт события (push) и никогда не ждёт.
#pragma once
#include <Arduino.h>
#include "gs_notify.h"

namespace notify {

void begin();                                      // читает настройки из NVS, создаёт задачу отправки (вызывать после shell::begin)

// Потокобезопасно, O(1): mutex берётся с таймаутом 0 — занят, событие считается потерянным (dropped). Выключенный вебхук событий не копит.
void push(const gs::NotifyEvent& ev);
// Тестовое событие TEST (обходит фильтр серьёзности и подавление повторов). false — вебхук выключен/не настроен (причина в err).
bool test(char* err, size_t err_cap);

gs::WebhookCfg config();                           // копия настроек (содержит URL с секретами — только для консоли/export)
// Атомарно применяет набор ключей (url method body ctype err tmo min insecure en): валидация как в gs_notify, при любой ошибке
// ничего не меняется; успех — запись в NVS (wh_<ключ>) и немедленное применение. Порядок ключей не важен ("en" проверяется последним).
bool set_many(const char* const* keys, const char* const* vals, size_t n, char* err, size_t err_cap);
inline bool set(const char* key, const char* val, char* err, size_t err_cap) { return set_many(&key, &val, 1, err, err_cap); }
void reload_host();                                // имя хоста для ${host_name} (NVS "host") изменилось

struct Status { gs::WebhookCfg cfg; gs::NotifyStats st; size_t queue_len = 0; uint32_t now_ms = 0; };
bool status(Status& out);                          // false — mutex не получен
// GET /api/webhook: БЕЗ секретов (только хост из URL). 0 — не влезло/нет данных.
size_t status_json(char* out, size_t cap);

}  // namespace notify
