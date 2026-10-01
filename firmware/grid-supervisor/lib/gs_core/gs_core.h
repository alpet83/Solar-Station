// gs_core — чистая (без Arduino/ESP-IDF зависимостей) логика супервизора грид-переключения.
//
// Ядро НИЧЕГО не знает про Modbus, Wi-Fi и время железа: на вход подаётся снимок
// измерений и монотонное время в миллисекундах, на выходе — решение (команда или её
// отсутствие). Благодаря этому оно тестируется на ПК (см. test/native) без инвертора.
//
// Главный принцип (асимметрия безопасности):
//   * «ПОДКЛЮЧИТЬ к сети» (запись 20105=1) разрешено почти всегда: в любом режиме, в том числе
//     safe-mode и сразу после загрузки. Единственное исключение — «мягкий» порог (SoftLow)
//     молчит во время ручной паузы (человек сам отключил сеть); жёсткий порог, NoData и
//     safe-mode ручную паузу переопределяют;
//   * «ОТКЛЮЧИТЬ от сети» (20105=0) разрешено только при полной уверенности: свежие,
//     непротиворечивые данные, непрерывное выполнение условия в течение dwell, нет safe-mode,
//     нет ручной паузы, выдержана пауза после последнего подключения.
//   Любое сомнение смещает решение в сторону сети.
//
// Время везде — монотонные миллисекунды (uint64_t). Единицы: вольты, амперы, ватты.
#pragma once

#include <cstdint>

namespace gs {

// Команда, которую адаптер должен выполнить записью регистра 20105.
enum class Cmd : uint8_t {
    None = 0,
    Connect = 1,     // 20105 = 1 (ON-Grid, подключить сеть)
    Disconnect = 2,  // 20105 = 0 (OFF-Grid, работать от батареи)
};

// Причина выданной команды (для журнала событий и уведомлений).
enum class Reason : uint8_t {
    None = 0,
    HardFloor,   // напряжение ниже жёсткого порога
    SoftLow,     // напряжение ниже мягкого порога дольше dwell
    NoData,      // нет валидных измерений дольше no_data_timeout
    Charged,     // батарея заряжена и заряд достаточен для нагрузки
    SafeMode,    // safe-mode (петля перезагрузок): сеть подключаем немедленно
};

// Битовые флаги состояния (для дашборда/алертов). Не все приводят к команде.
enum Flag : uint32_t {
    F_HARD_LOW        = 1u << 0,   // напряжение <= hard_floor_v (таймер dwell может ещё идти)
    F_SOFT_LOW        = 1u << 1,   // напряжение <= soft_on_v
    F_NO_DATA         = 1u << 2,   // валидных данных нет дольше no_data_timeout
    F_STALE           = 1u << 3,   // данных нет в этом шаге (но таймаут ещё не истёк)
    F_DATA_DOUBTFUL   = 1u << 4,   // два регистра напряжения расходятся сильнее допуска
    F_MANUAL_PAUSE    = 1u << 5,   // автоматика на ручной паузе
    F_MANUAL_OVERRIDE = 1u << 6,   // в этом шаге замечено внешнее изменение 20105
    F_GRID_LOST_LOW   = 1u << 7,   // сети нет (grid_v), а заряд низкий: алерт, команды бессильны
    F_SAFE_MODE       = 1u << 8,   // safe-mode: отключение от сети запрещено
    F_CMD_PENDING     = 1u << 9,   // команда выдана, подтверждения (чтения 20105) пока нет
    F_CMD_RETRY       = 1u << 10,  // команда выдана повторно
    F_OFF_COND        = 1u << 11,  // условия отключения от сети выполняются (идёт dwell)
    F_HOLD            = 1u << 12,  // пауза после подключения ещё не истекла
    F_SINGLE_SRC      = 1u << 13,  // валиден только один из двух регистров напряжения
    F_TIME_JUMP       = 1u << 14,  // обнаружен скачок/откат времени или большой пропуск шагов
    F_RELAY_MISMATCH  = 1u << 16,  // регистр 20105 и физическое реле расходятся (только алерт)
    F_LOAD_UNKNOWN    = 1u << 15,  // нагрузка в этом шаге неизвестна: «заряжено» не засчитывается
};

struct Config {
    // --- пороги по напряжению сборки (В). Калибровка — docs/calibration.md ---
    float hard_floor_v = 48.0f;      // <= этого: немедленное подключение сети (после hard_dwell)
    float soft_on_v    = 51.5f;      // <= этого дольше soft_on_dwell: подключить сеть
    float soft_off_v   = 53.3f;      // >= этого И заряд >= pow_min дольше soft_off_dwell: отключить
    float full_v       = 54.5f;      // >= этого дольше soft_off_dwell: отключить без условия по мощности

    // --- времена (мс) ---
    uint32_t hard_dwell_ms       = 15000;    // фильтр одиночных выбросов; в safe-режиме тоже действует
    uint32_t soft_on_dwell_ms    = 120000;
    uint32_t soft_off_dwell_ms   = 300000;
    uint32_t no_data_timeout_ms  = 120000;   // T: сколько терпим отсутствие валидных данных
    uint32_t manual_pause_ms     = 3600000;  // пауза автоматики после внешнего изменения 20105
    uint32_t hold_after_connect_ms = 900000; // после подключения не отключаем минимум это время
    uint32_t cmd_retry_ms        = 10000;    // повтор неподтверждённой команды

    // --- расчёт «достаточности заряда» (аналог $pow_min и $chg_pow из batmon.php) ---
    float pow_min_base_w = 640.0f;
    float load_factor    = 0.97f;
    float ext_coef       = 0.9f;             // доля внешнего тока (Epever), которую учитываем
    uint32_t ext_ttl_ms  = 60000;            // время жизни внешнего значения
    float ext_max_a      = 80.0f;            // кламп внешнего значения
    float charge_max_a   = 150.0f;           // собственный ток выше этого — мусор, считаем 0
    uint32_t step_gap_ms = 30000;            // пропуск между шагами больше этого сбрасывает «отключать»

    // --- валидация измерений ---
    float v_valid_min        = 30.0f;
    float v_valid_max        = 70.0f;
    float v_mismatch_max     = 1.5f;         // допустимое расхождение двух регистров напряжения
    float grid_present_v_min = 150.0f;       // ниже: сети считаем нет

    // Проверка осмысленности конфигурации (используется загрузчиком NVS: при false —
    // откат на значения по умолчанию, а не работа с произвольными порогами).
    bool validate() const;
};

constexpr float kNaN = __builtin_nanf("");

struct Input {
    uint64_t now_ms = 0;      // монотонное время
    bool     have_sample = false;  // в этом шаге свежий опрос блока 25201 удался (CRC/длина/id проверены)
    float    pack_v = kNaN;     // voltages.battery
    float    pack_v_alt = kNaN; // PV_charger.batt_volt (0 или NaN — «нет данных»)
    float    charge_a = kNaN;   // ток зарядного контроллера инвертора (PV_charger.chg_curr)
    float    ext_charge_a = kNaN;// последнее значение от Pi (ток Epever), NaN — не поступало
    uint64_t ext_age_ms = 0;       // возраст внешнего значения
    float    load_w = kNaN;     // мощность нагрузки инвертора
    float    grid_v = kNaN;     // напряжение сети
    int8_t   relay_grid = -1;      // физическое реле сети (25238): -1 неизвестно, 0, 1
    int8_t   ongrid_reg = -1;      // прочитанное значение 20105: -1 неизвестно, 0, 1
    bool     safe_mode = false;    // включён safe-mode (петля перезагрузок)

};

struct Decision {
    Cmd      cmd = Cmd::None;
    Reason   reason = Reason::None;
    uint32_t flags = 0;
    float    v_used = 0.0f;         // напряжение, по которому принято решение (минимум валидных)
    float    eff_charge_a = 0.0f;   // эффективный зарядный ток
    float    pow_min_w = 0.0f;      // требуемая зарядная мощность для отключения
};

// Эффективный зарядный ток: ток контроллера инвертора + ext_coef * внешний ток.
// Устаревшее/некорректное внешнее значение даёт 0 — потеря источника смещает решение
// в «зарядка меньше» = не отключаться от сети.
float effective_charge_a(const Config& cfg, float charge_a, float ext_a, uint64_t ext_age_ms);

class Supervisor {
public:
    explicit Supervisor(const Config& cfg = Config()) : cfg_(cfg) {}

    // Один шаг логики. Вызывать регулярно (раз в цикл опроса), даже если опрос не удался
    // (have_sample = false): по отсутствию данных ядро само отсчитывает no_data_timeout.
    Decision step(const Input& in);

    const Config& config() const { return cfg_; }

    // Для дашборда/отладки.
    uint64_t pause_until_ms() const { return pause_until_ms_; }

private:
    static constexpr int64_t kInactive = -1;

    Config cfg_;

    bool     started_ = false;
    uint64_t start_ms_ = 0;

    // валидные измерения
    uint64_t last_valid_ms_ = 0;
    float    last_load_w_ = 0.0f;
    float    last_v_ = 0.0f;

    // таймеры условий («с какого момента непрерывно выполняется»)
    int64_t  hard_since_ = kInactive;
    int64_t  soft_since_ = kInactive;
    int64_t  off_since_  = kInactive;

    // состояние регистра 20105 и команд
    int8_t   last_reg_ = -1;
    Cmd      pending_ = Cmd::None;
    uint64_t pending_since_ = 0;
    uint8_t  retries_ = 0;                    // повторы неподтверждённой команды (для backoff)
    int64_t  last_cmd_conn_ms_ = kInactive;   // когда выдавали Connect / Disconnect
    int64_t  last_cmd_disc_ms_ = kInactive;
    bool     have_last_step_ = false;
    uint64_t last_step_ms_ = 0;
    int64_t  last_connect_ms_ = kInactive;

    uint64_t pause_until_ms_ = 0;
};

}  // namespace gs
