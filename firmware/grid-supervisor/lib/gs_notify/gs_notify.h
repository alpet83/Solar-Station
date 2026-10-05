// gs_notify — уведомления на HTTP-вебхук: чистая логика без Arduino (тесты: test/native/test_gs_notify.cpp).
//
// Слои:
//   * ШАБЛОНЫ: expand_template() подставляет ${host_name} ${event_message} ${event_value} ${event_tag} ${event_flags} ${event_time}
//     (имена и семантика как у встроенного вебхука прокси sbin/eybond_tap.py; значения URL-кодируются, как там).
//     validate_template()/validate_url(): неизвестный плейсхолдер = ошибка с именем.
//   * НАСТРОЙКИ: WebhookCfg + webhook_set() — единая валидация для консоли, /setup и POST /api/webhook.
//   * ОЧЕРЕДЬ: NotifyQueue — кольцо 16 событий, фильтр по серьёзности, подавление повторов, лимит скорости, повторы с backoff.
//     Время — uptime в мс (uint32, перескок через 49 дней безопасен). Не потокобезопасна: владелец (src/notify.cpp) держит mutex.
//   * ПРЕОБРАЗОВАНИЕ: Trigger (gs_trig) и события супервизора (флаги/команды/старт) -> NotifyEvent.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "gs_core.h"
#include "gs_trig.h"

namespace gs {

// ---- событие --------------------------------------------------------------------------------------------
enum : uint8_t { kSevInfo = 0, kSevWarn = 1, kSevAlert = 2 };
const char* notify_sev_name(uint8_t sev);              // "info" / "warn" / "alert"

struct NotifyEvent {
    uint32_t id = 0;               // сквозной номер (назначает очередь)
    uint8_t sev = kSevInfo;        // 0 info, 1 warn, 2 alert
    char tag[16] = {0};            // CELL_LOW, BOOT, CONNECT, ... (до 15 символов)
    char message[96] = {0};
    float value = NAN;             // NaN — нет значения (${event_value} пуст)
    char flags[64] = {0};          // текст для ${event_flags}
    int64_t epoch_s = -1;          // UTC; < 0 — время не было синхронизировано
    uint32_t uptime_ms = 0;        // время события, мс с загрузки
};

// Копирует src в dst[cap] с обрезкой и завершающим NUL.
void notify_copy(char* dst, size_t cap, const char* src);

// ---- шаблоны ----------------------------------------------------------------------------------------------
const char* const kPlaceholders[] = {"host_name", "event_message", "event_value", "event_tag", "event_flags", "event_time"};
constexpr size_t kPlaceholderCount = 6;

constexpr size_t kUrlMax = 255;        // длина URL-шаблона
constexpr size_t kBodyMax = 400;       // шаблон тела POST
constexpr size_t kCtypeMax = 64;
constexpr size_t kErrTextMax = 48;
constexpr size_t kExpandedMax = 1024;  // буфер для раскрытого URL / тела (сообщение 96 символов, все кодируются как %XX)

// URL-кодирование (как urllib.parse.quote(s, safe='')): без процентов остаются только A-Za-z0-9 - _ . ~. false — не влезло (out обрезан).
bool urlencode(const char* in, char* out, size_t cap);

// ${имя} — [A-Za-z0-9_]*; всё остальное (например "${a-b}", "$x") — обычный текст, как в прокси. true — все имена известны;
// иначе bad = первое неизвестное имя (для ${} — пустая строка).
bool validate_template(const char* tpl, char* bad, size_t bad_cap);

// URL-шаблон: http:// или https://, непустой хост, длина <= 255, без пробелов/управляющих, плейсхолдеры известны. err — причина.
bool validate_url(const char* url, char* err, size_t err_cap);
bool validate_method(const char* m);                    // "GET" | "POST" без учёта регистра

// ${event_time}: "YYYY-MM-DD HH:MM:SS" (UTC) или, если время не синхронизировано, "uptime+<сек>s".
void format_event_time(const NotifyEvent& ev, char* out, size_t cap);
// ${event_value}: число с 2 знаками или пусто для NaN/inf.
void format_event_value(float v, char* out, size_t cap);

// Раскрывает шаблон. false — неизвестный плейсхолдер или не влезло в cap (out при этом обрезан и завершён NUL).
bool expand_template(const char* tpl, const NotifyEvent& ev, const char* host, char* out, size_t cap, bool url_encode);

// "https://host" из URL (схема + хост без порта, userinfo, пути и query: токены ботов лежат именно там). Пусто, если не URL.
void url_origin(const char* url, char* out, size_t cap, bool with_scheme = true);

// ---- настройки --------------------------------------------------------------------------------------------
struct WebhookCfg {
    bool enabled = false;
    char url[kUrlMax + 1] = {0};
    char method[5] = "GET";
    char body[kBodyMax + 1] = {0};
    char ctype[kCtypeMax + 1] = "application/x-www-form-urlencoded";
    char err[kErrTextMax + 1] = {0};           // подстрока ответа, означающая сбой
    uint8_t timeout_s = 10;                    // 1..60
    uint8_t min_sev = kSevWarn;                // 0..2
    bool insecure = true;                      // https без проверки сертификата (компромисс LAN)
};
constexpr const char* kDefaultCtype = "application/x-www-form-urlencoded";

// Ключи: url method body ctype err tmo min en insecure (в NVS с префиксом "wh_"). Значение проверяется целиком, при ошибке cfg не меняется
// и err получает причину. Пустое значение: url -> очищает и выключает; body/err -> очищает; ctype -> по умолчанию.
bool webhook_set(WebhookCfg& cfg, const char* key, const char* val, char* err, size_t err_cap);
// Значение ключа в виде текста (для export и сохранения в NVS). false — неизвестный ключ.
bool webhook_get(const WebhookCfg& cfg, const char* key, char* out, size_t cap);
extern const char* const kWebhookKeys[];       // порядок: url method body ctype err tmo min insecure en (en последним: он проверяет url)
constexpr size_t kWebhookKeyCount = 9;

// ---- очередь ----------------------------------------------------------------------------------------------
constexpr size_t kNotifyQueueCap = 16;

struct NotifyPolicy {
    uint8_t min_sev = kSevWarn;                // события ниже отбрасываются (stats.filtered), кроме force
    uint32_t dedup_ms = 60000;                 // одинаковые tag+message — не чаще раза за окно (stats.suppressed)
    uint32_t rate_window_ms = 60000;           // общий лимит: не более rate_max попыток отправки за окно
    uint32_t rate_max = 10;
    uint32_t backoff_ms[3] = {5000, 30000, 120000};   // после 1-й и 2-й неудачи — повтор через 5 и 30 с; после 3-й событие
                                                      // отбрасывается (stats.failed), а очередь замолкает на 120 с
    uint8_t max_attempts = 3;
};

struct NotifyStats {
    uint32_t sent = 0;          // успешно отправлено
    uint32_t failed = 0;        // отброшено после max_attempts неудач
    uint32_t dropped = 0;       // вытеснено из переполненного кольца (и потеряно при занятом mutex)
    uint32_t suppressed = 0;    // повтор в окне подавления
    uint32_t filtered = 0;      // ниже min_sev
    bool have_last = false;     // была хоть одна попытка отправки
    bool last_ok = false;
    int last_http = 0;          // HTTP-код последней попытки (0 — ответа нет; отрицательный — код транспорта HTTPClient)
    char last_error[48] = {0};  // пусто при успехе
    uint32_t last_ms = 0;       // когда была последняя попытка
};

class NotifyQueue {
public:
    explicit NotifyQueue(const NotifyPolicy& p = NotifyPolicy()) : pol_(p) {}
    void set_policy(const NotifyPolicy& p) { pol_ = p; }
    const NotifyPolicy& policy() const { return pol_; }

    // Принять событие. force (TEST) обходит min_sev и подавление повторов. true — поставлено в очередь.
    bool push(NotifyEvent ev, uint32_t now_ms, bool force = false);
    // Событие, которое можно отправлять прямо сейчас (учитывает лимит скорости, повторы и паузу). После true ОБЯЗАТЕЛЕН done().
    bool take(uint32_t now_ms, NotifyEvent& out);
    // Результат отправки события, выданного take(). http — код (0/<0 — ответа нет), err — причина при ok=false.
    void done(bool ok, uint32_t now_ms, int http, const char* err);
    // Потеряно вне очереди (например, занят mutex) — учитывается как dropped.
    void note_lost() { ++st_.dropped; }

    size_t size() const { return n_ + (retry_ ? 1u : 0u) + (inflight_ ? 1u : 0u); }
    bool inflight() const { return inflight_; }
    const NotifyStats& stats() const { return st_; }
    void clear();

private:
    struct Seen { bool used = false; uint32_t key = 0, at = 0; };
    static const size_t kSeenCap = 16;
    bool rate_ok(uint32_t now);
    NotifyPolicy pol_;
    NotifyEvent q_[kNotifyQueueCap];
    size_t head_ = 0, n_ = 0;
    NotifyEvent cur_;                              // выданное take() событие
    bool inflight_ = false, retry_ = false;        // retry_: cur_ ждёт повтора в retry_at_
    uint8_t attempts_ = 0;
    uint32_t retry_at_ = 0, blocked_until_ = 0; bool blocked_ = false;
    uint32_t att_[16] = {0}; size_t att_n_ = 0, att_head_ = 0;   // времена последних попыток (кольцо, rate_max <= 16)
    Seen seen_[kSeenCap];
    uint32_t next_id_ = 1;
    NotifyStats st_;
};

// ---- преобразование в NotifyEvent ------------------------------------------------------------------------------
// Триггер BMS. now_ms — текущий uptime, epoch_now_s — текущее UTC (< 0 — нет времени): время события = now - (now_ms - t.t_ms).
NotifyEvent notify_from_trigger(const Trigger& t, uint32_t now_ms, int64_t epoch_now_s);

const char* reason_name(Reason r);                // HARD_FLOOR, SOFT_LOW, NO_DATA, CHARGED, SAFE_MODE, MANUAL, NONE

// События супервизора. Состояние — флаги предыдущего шага (по фронту: вход -> событие, выход -> info <TAG>_OK).
class SupervisorNotifier {
public:
    // Вызывать на каждом шаге ядра с d.flags. Пишет события в out[cap], возвращает их число. Первый вызов сравнивает с «ничего нет».
    size_t on_flags(uint32_t flags, float v, uint32_t uptime_ms, int64_t epoch_s, NotifyEvent* out, size_t cap);
    // Команда CONNECT/DISCONNECT (ok — запись на шину удалась). false, если cmd == None.
    static bool on_command(Cmd cmd, Reason r, bool ok, float v, uint32_t uptime_ms, int64_t epoch_s, NotifyEvent& out);
    // Старт платы: info BOOT "boot rev=.. reset=..".
    static NotifyEvent boot(const char* rev, const char* reset_reason, uint32_t uptime_ms, int64_t epoch_s);
    void reset() { prev_ = 0; }
    uint32_t prev_flags() const { return prev_; }
private:
    uint32_t prev_ = 0;
};
// Имена наблюдаемых флагов: для ${event_flags} (например "HARD_LOW|CELL_LOW"); пусто — ничего.
void flags_text(uint32_t flags, char* out, size_t cap);

}  // namespace gs
