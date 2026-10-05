// gs_trig — генерация «триггеров» от асинхронных кадров BMS и журнал событий BMS.
// Чистая логика без Arduino (тестируется нативно, test/native/test_gs_trig.cpp).
//
// Два слоя:
//   * ГЕНЕРАЦИЯ: BmsWatch::update() вызывается задачей CAN после каждого кадра (и раз в ~0.5 с по таймеру,
//     чтобы поймать «кадры пропали»). Сравнивает снимок BmsSnapshot с предыдущим состоянием и кладёт
//     в TriggerQueue события ПО ПЕРЕХОДАМ, а не по каждому кадру.
//   * ОБРАБОТКА: цикл управления раз в итерацию выгребает очередь (pop) и делает с триггерами что нужно
//     (сейчас — печать в Serial; дальше — вебхук). Очередь и журнал не потокобезопасны сами по себе:
//     владелец (src/can_rx.cpp) защищает их своим mutex'ом.
#pragma once
#include <cstddef>
#include <cstdint>

#include "gs_can.h"
#include "gs_core.h"

namespace gs {

enum class TrigKind : uint8_t {
    CellLowEnter = 0,   // самая слабая ячейка <= cell_warn_mv
    CellLowExit,        // ячейка поднялась выше cell_warn_mv + гистерезис
    CellHardEnter,      // самая слабая ячейка <= cell_hard_mv
    CellHardExit,
    CellDeltaHigh,      // разброс ячеек (max - min) стал > delta_hi_mv
    CellDeltaOk,        // разброс упал ниже delta_ok_mv
    BmsFlagsChanged,    // изменилось «знаковое» сырое поле BMS (флаги/аварии/индексы)
    BmsStale,           // свежих кадров BMS нет дольше kCanStaleMs (один раз)
    BmsBack,            // кадры BMS вернулись
    Count_
};

enum class TrigSeverity : uint8_t { Info = 0, Warn, Alert };

const char* trig_tag(TrigKind k);                  // CELL_LOW, CELL_LOW_OK, CELL_HARD, CELL_HARD_OK, CELL_DELTA, CELL_DELTA_OK, BMS_FLAGS, BMS_STALE, BMS_BACK
TrigSeverity trig_severity(TrigKind k);
const char* trig_severity_name(TrigSeverity s);    // "info" / "warn" / "alert"

struct Trigger {
    uint32_t id = 0;            // сквозной номер (назначает очередь при push)
    TrigKind kind = TrigKind::BmsFlagsChanged;
    uint32_t t_ms = 0;          // время события (мс с загрузки, 32 бита)
    float value = 0;            // ячейки: мВ (min ячейка / разброс); флаги: число изменившихся байт; Stale/Back: секунды
    uint32_t aux = 0;           // ячейки: max ячейка, мВ; флаги: (индекс поля << 8) | маска изменившихся байт
    char text[48] = {0};
};

// ---- очередь ------------------------------------------------------------------------------------
constexpr size_t kTrigQueueCap = 32;

// Кольцо на 32 записи. Переполнение вытесняет САМЫЕ СТАРЫЕ (счётчик dropped).
class TriggerQueue {
public:
    uint32_t push(Trigger t);               // назначает id (с 1), возвращает его
    bool pop(Trigger& out);                 // самый старый; false — пусто
    size_t size() const { return n_; }
    uint32_t dropped() const { return dropped_; }
    uint32_t last_id() const { return next_id_ - 1; }
    void clear() { head_ = n_ = 0; }
private:
    Trigger q_[kTrigQueueCap];
    size_t head_ = 0, n_ = 0;
    uint32_t next_id_ = 1, dropped_ = 0;
};

// ---- журнал событий BMS (для /api/bms_events) -------------------------------------------------------
constexpr size_t kBmsLogCap = 16;
struct BmsEvent {
    uint32_t id = 0; TrigKind kind = TrigKind::BmsFlagsChanged; uint32_t t_ms = 0; int64_t epoch_s = -1;
    const char* tag = "";       // статическая строка trig_tag()
    float value = 0; char text[48] = {0};
};
class BmsEventLog {
public:
    void add(const Trigger& t, int64_t epoch_s);   // epoch_s < 0 — время не синхронизировано
    size_t size() const { return n_; }
    const BmsEvent& at(size_t i) const;            // 0 — самое новое
    uint32_t total() const { return total_; }      // сколько событий записано за всё время (включая вытесненные)
    void clear() { head_ = n_ = 0; }
private:
    BmsEvent e_[kBmsLogCap];
    size_t head_ = 0, n_ = 0;                      // head_ — индекс следующей записи (кольцо)
    uint32_t total_ = 0;
};

// {"total":..,"now_ms":..,"events":[{"id":..,"t_ms":..,"age_s":..,"ts":"UTC"|null,"tag":"..","sev":"..","value":..,"text":".."}]}, от новых к старым.
// 0 — не влезло в буфер.
size_t bms_events_to_json(const BmsEventLog& log, uint32_t now_ms, char* out, size_t cap);

// ---- генератор ----------------------------------------------------------------------------------------
constexpr size_t kWatchMax = 8;

// Одно «знаковое» сырое поле BMS: имя, длина и байты. valid — поле свежее.
struct WatchField { char name[6] = {0}; uint8_t len = 0; uint8_t data[8] = {0}; bool valid = false; };

struct BmsSnapshot {
    bool cells_valid = false; uint16_t cell_min_mv = 0, cell_max_mv = 0;   // свежие данные по ячейкам
    bool flags_valid = false;                                              // хотя бы одно поле watch[] свежее
    WatchField watch[kWatchMax];
    bool frames_alive = false;                                             // кадры BMS приходили в последние kCanStaleMs
};

struct WatchCfg {
    uint16_t cell_warn_mv = 3050;      // вход CellLow (<=)
    uint16_t cell_hard_mv = 2900;      // вход CellHard (<=)
    uint16_t hyst_mv = 30;             // выход: порог + hyst_mv
    uint16_t delta_hi_mv = 100;        // CellDeltaHigh: > этого
    uint16_t delta_ok_mv = 70;         // CellDeltaOk: < этого
    uint32_t debounce_ms = 5000;       // одно поле не чаще раза в это время
};
// Пороги ячеек берутся из Config (В -> мВ с округлением): warn = cell_soft_on_v, hard = cell_hard_floor_v.
WatchCfg make_watch_cfg(const Config& cfg);

// Снимок из трёх декодеров. Слоты watch[]: 0 f312, 1 f320, 2 f319 (байт 0 и индексы), 3 f322 (байты 4–7), 4 f311 (байты 6–7)
// — seplos_500k; 5 a200 (prot_raw 0x00030200) — seplos_100k; 6 p359 (prot1,prot2,alarm1,alarm2), 7 p35c (req.raw) — Pylon.
// Шумные поля (ток, напряжения, температуры, ёмкость, счётчик 0x314) НЕ наблюдаются.
BmsSnapshot make_bms_snapshot(const PylonBattery& b, const SeplosBattery& s, const Seplos500Battery& s5, uint32_t now_ms);

class BmsWatch {
public:
    explicit BmsWatch(const WatchCfg& c = WatchCfg()) : cfg_(c) {}
    void set_cfg(const WatchCfg& c) { cfg_ = c; }
    const WatchCfg& cfg() const { return cfg_; }
    // Сравнивает снимок с состоянием, эмитит триггеры в q (и, если log != nullptr, копию в журнал).
    void update(const BmsSnapshot& s, uint32_t now_ms, TriggerQueue& q, BmsEventLog* log = nullptr, int64_t epoch_s = -1);
    void reset() { *this = BmsWatch(cfg_); }       // забыть всё (например, смена скорости CAN): следующие данные — baseline
    bool stale() const { return stale_; }
private:
    struct Field { bool base = false, emitted = false; uint8_t len = 0; uint8_t sent[8] = {0}, cur[8] = {0}; uint32_t last_emit_ms = 0; };
    void emit(TriggerQueue& q, BmsEventLog* log, int64_t epoch_s, Trigger t);
    WatchCfg cfg_;
    Field f_[kWatchMax];
    bool low_ = false, hard_ = false, delta_hi_ = false;
    bool ever_alive_ = false, stale_ = false;
    uint32_t stale_since_ms_ = 0;
};

}  // namespace gs
