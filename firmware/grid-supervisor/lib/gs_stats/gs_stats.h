// gs_stats — статистика включений PV: когда реле PV замыкается/размыкается, сколько держится включение,
// суточные итоги (первое/последнее включение, «устойчивый» заряд, число коротких включений, энергия заряда).
// Чистая логика без Arduino (нативные тесты: test/native/test_gs_stats.cpp). Данные только в ОЗУ;
// запись на SD — отдельный слайс. Потокобезопасность — на владельце (src/stats.cpp).
//
// Определение «PV включена»: реле PV (15212) == 1; если реле неизвестно — мощность заряда >= min_w_on.
// «Устойчивое включение» — непрерывно включено не менее stable_s секунд; более короткие включения
// считаются «неуверенными» (short_runs) — это и есть щёлканье реле утром/вечером.
// Разрешение по времени ограничено периодом чужого опроса блока 15201: клики быстрее периода не видны.
#pragma once
#include <cstddef>
#include <cstdint>

namespace gs {

struct PvSample {
    uint32_t t_ms = 0;        // монотонное время, мс
    int64_t  epoch_s = -1;    // UTC, секунды; -1 если время не синхронизировано
    float    pv_v = 0, chg_a = 0, chg_w = 0, pack_v = 0;   // NaN допустим
    int8_t   pv_relay = -1;   // 0/1, -1 неизвестно
};

struct PvStatsCfg {
    uint32_t stable_s = 600;  // включение не короче этого — «устойчивое»
    float    min_w_on = 10.0f;// порог мощности заряда, если реле PV неизвестно
    uint32_t max_dt_s = 300;  // максимальный шаг интегрирования энергии (при пропуске данных не «дорисовываем»)
};

struct PvEvent {
    int64_t  epoch_s = 0;     // 0 — время неизвестно
    uint32_t t_ms = 0;
    uint8_t  on = 0;          // 1 — включилась, 0 — выключилась
    uint32_t run_s = 0;       // для выключения: сколько длилось включение
    float    pv_v = 0, chg_a = 0, chg_w = 0, pack_v = 0;
};

struct PvDay {
    uint32_t day = 0;         // epoch/86400 (UTC), 0 — время неизвестно
    bool     used = false;
    uint16_t on_edges = 0;    // сколько раз PV включалась
    uint16_t short_runs = 0;  // из них коротких (< stable_s) — «неуверенные»
    int64_t  first_on = 0, last_off = 0;                 // эпохи, 0 — не было
    int64_t  stable_start = 0, stable_end = 0;           // первое устойчивое включение: начало; конец последнего устойчивого
    float    start_pv_v = 0, start_chg_w = 0;            // значения в момент начала первого устойчивого включения
    float    max_pv_v = 0, max_chg_w = 0, chg_wh = 0;
    uint32_t samples = 0;
};

constexpr size_t kPvEvents = 32;
constexpr size_t kPvDays = 8;     // текущий + 7 предыдущих

class PvStats {
public:
    explicit PvStats(const PvStatsCfg& c = PvStatsCfg()) : cfg_(c) {}
    void set_config(const PvStatsCfg& c) { cfg_ = c; }
    const PvStatsCfg& config() const { return cfg_; }
    void feed(const PvSample& s);                 // один свежий разобранный блок 15201
    bool pv_on() const { return have_ && on_; }
    uint32_t event_total() const { return (uint32_t)n_ev_; }   // сколько событий создано всего (для обнаружения новых)
    size_t event_count() const { return n_ev_ < kPvEvents ? n_ev_ : kPvEvents; }
    bool event(size_t i, PvEvent& out) const;     // i = 0 — самое новое
    size_t day_count() const { return n_days_ < kPvDays ? n_days_ : kPvDays; }
    bool day(size_t i, PvDay& out) const;         // i = 0 — текущий/самый новый
    uint32_t total_samples() const { return total_; }
    uint32_t run_s(uint32_t now_ms) const;        // длительность текущего состояния (вкл/выкл), с
    // JSON: {"cfg":..,"now":..,"days":[..],"events":[..]}. Возвращает длину или 0, если не влезло.
    size_t to_json(uint32_t now_ms, char* buf, size_t cap) const;
private:
    PvDay& cur();
    void push_day(uint32_t day);
    void add_event(const PvEvent& e);
    PvStatsCfg cfg_;
    bool have_ = false, on_ = false, stable_ = false;
    uint32_t since_ms_ = 0, last_ms_ = 0;
    int64_t since_epoch_ = 0;
    float run_pv_v_ = 0, run_chg_w_ = 0;
    uint32_t total_ = 0;
    PvEvent ev_[kPvEvents]; size_t n_ev_ = 0;
    PvDay days_[kPvDays]; size_t n_days_ = 0;
};

}  // namespace gs
