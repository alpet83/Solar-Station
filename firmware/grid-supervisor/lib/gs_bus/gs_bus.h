// gs_bus — пассивный анализ RS485: разбор кадров Modbus RTU «на лету» и трекер ритма
// второго мастера (даталоггер/облако). Без Arduino-зависимостей, тестируется нативно.
//
// Идея сожительства: логгер раз в ~5 минут шлёт пачку запросов к инвертору. Трекер
// группирует чужие запросы в пачки, выучивает период и фазу и отвечает на вопрос
// «можно ли мне сейчас передавать?»: нельзя во время пачки, в короткой паузе после неё и в
// предсказанном окне перед следующей пачкой. Между пачками плата может опрашивать чаще.
// Предсказание — подсказка: критичные команды (подключить сеть) опираются только на
// наблюдаемую активность шины.
#pragma once
#include <cstddef>
#include <cstdint>

#include "gs_modbus.h"

namespace gs {

enum class FrameKind : uint8_t { Junk = 0, Request, Response, Exception, WriteEcho };

struct FrameRec {
    uint32_t t_ms = 0;       // начало кадра (аптайм, мс, усечено до 32 бит)
    uint16_t dur_ms = 0;
    uint16_t len = 0;
    FrameKind kind = FrameKind::Junk;
    uint8_t id = 0, fn = 0;
    uint16_t addr = 0;       // Request/WriteEcho: адрес; Response: 0
    uint16_t count = 0;      // Request: число регистров; WriteEcho: значение; Response: число регистров
    bool crc_ok = false;
};

// Сборщик кадров по паузе на шине. Вызывать poll() каждую итерацию и feed() для принятых
// байтов; обе функции возвращают true, если закрыли кадр (он в last() и в кольце).
class BusSniffer {
public:
    static constexpr size_t kRing = 64;
    static constexpr size_t kBuf = 300;

    explicit BusSniffer(uint32_t gap_ms = 6) : gap_ms_(gap_ms) {}

    bool feed(const uint8_t* p, size_t n, uint64_t now_ms);
    bool poll(uint64_t now_ms);

    const FrameRec& last() const { return last_; }
    // i = 0 — самый новый. false, если записи нет.
    bool recent(size_t i, FrameRec& out) const;
    size_t count() const { return n_ < kRing ? n_ : kRing; }
    uint32_t total() const { return n_; }
    uint32_t junk() const { return junk_; }
    // Байты последнего закрытого кадра (копия: буфер приёма уже может быть перезаписан).
    const uint8_t* last_bytes() const { return last_buf_; }

private:
    bool close(uint64_t now_ms);
    uint32_t gap_ms_;
    uint8_t buf_[kBuf];
    size_t len_ = 0;
    uint64_t first_ms_ = 0, last_byte_ms_ = 0;
    FrameRec ring_[kRing];
    uint32_t n_ = 0, junk_ = 0;
    FrameRec last_;
    uint8_t last_buf_[kBuf];
};

// Пассивный разбор чужого опроса: пары «запрос чтения → ответ» с адресом id превращаются в
// телеметрию без единой передачи в шину. Нужен в режиме приёма, чтобы видеть напряжения.
class PassiveTelemetry {
public:
    static constexpr uint32_t kPairMs = 1500;   // ответ должен прийти не позже, чем через столько после запроса
    static constexpr uint32_t kFreshMs = 30000; // старше — считаем данные устаревшими
    explicit PassiveTelemetry(uint8_t id = 4) : id_(id) {}
    void set_id(uint8_t id) { id_ = id; }
    // f/data — только что закрытый кадр (BusSniffer::last()/last_bytes()).
    void on_frame(const FrameRec& f, const uint8_t* data, uint64_t now_ms);
    // Телеметрия только из свежих блоков; устаревшие поля остаются NaN/-1.
    Telemetry get(uint64_t now_ms) const;
private:
    uint8_t id_;
    bool have_req_ = false; uint16_t req_addr_ = 0, req_cnt_ = 0; uint64_t req_ms_ = 0;
    Telemetry rt_, chg_, cfg_;                  // каждый блок декодируется в свою копию
    uint64_t rt_ms_ = 0, chg_ms_ = 0, cfg_ms_ = 0;
};

// Разбор одного буфера как кадра (экспортируется для тестов).
FrameRec classify_frame(const uint8_t* b, size_t len);

struct LoggerCfg {
    uint8_t  target_id = 4;
    uint32_t period_ms = 300000;       // ожидаемый период пачек
    uint32_t period_tol_ms = 20000;    // допуск попадания в ритм
    uint32_t burst_gap_ms = 3000;      // запросы ближе этого — одна пачка
    uint32_t post_guard_ms = 1500;     // тишина после последнего чужого запроса
    uint32_t pre_quiet_ms = 8000;      // молчим за столько до предсказанного начала
    uint32_t min_burst_ms = 4000;      // нижняя оценка длины пачки для окна
    uint32_t burst_max_ms = 90000;     // пачка длиннее — не логгер: перестаём считать её запретом
    uint32_t fast_gap_ms = 15000;      // «активное» окно, если до тишины больше этого
};

struct Gate {
    enum Why : uint8_t { None = 0, InBurst, Predicted } why = None;
    bool ok = true;
    uint32_t wait_ms = 0;              // сколько ждать до снятия запрета
};

struct BurstRec {
    uint32_t start_ms = 0;             // аптайм начала (мс, усечено)
    uint32_t dur_ms = 0;
    uint16_t n_req = 0;
    uint16_t first_addr = 0;
    bool in_phase = false;
};

class LoggerTracker {
public:
    static constexpr size_t kBursts = 288;       // ~сутки при периоде 5 мин
    static constexpr size_t kIgnore = 12;

    explicit LoggerTracker(const LoggerCfg& c = LoggerCfg()) : cfg_(c), period_(c.period_ms) {}

    // Запросы, которые считаем «своими» (например, блоки pv1800_ctrl.py): не считаются логгером.
    bool ignore(uint8_t fn, uint16_t addr, uint16_t count);

    // t_ms — 64-битное время начала кадра (аптайм). Свои/игнорируемые запросы не считаются.
    void observe(const FrameRec& f, uint64_t t_ms);
    void tick(uint64_t now_ms);

    Gate may_transmit(uint64_t now_ms, uint32_t need_ms) const;
    bool active_window(uint64_t now_ms) const;     // можно опрашивать чаще обычного

    // Задать ожидаемый период пачек (30..900 с); сбрасывает захват. false — значение вне диапазона.
    bool set_period(uint32_t ms);
    bool locked() const { return locked_; }
    uint32_t period_ms() const { return (uint32_t)period_; }
    uint32_t bursts_total() const { return total_; }
    uint32_t off_rhythm() const { return off_; }
    uint32_t ignored() const { return ignored_; }
    uint32_t burst_len_ms() const { return (uint32_t)len_est_; }
    bool next_start(uint64_t now_ms, uint64_t& t) const;   // ближайшее предсказанное начало
    size_t burst_count() const { return total_ < kBursts ? total_ : kBursts; }
    bool burst(size_t i, BurstRec& out) const;             // 0 — самая новая закрытая

private:
    uint64_t window_len() const;
    bool in_burst(uint64_t now_ms) const;
    void close_burst();

    LoggerCfg cfg_;
    struct Sig { uint8_t fn; uint16_t addr, count; } ign_[kIgnore];
    size_t n_ign_ = 0;

    bool open_ = false;
    uint64_t cur_start_ = 0, cur_last_ = 0;
    uint16_t cur_n_ = 0, cur_first_addr_ = 0;
    bool seen_any_ = false;
    uint64_t last_req_ = 0;

    bool have_ref_ = false, locked_ = false;
    uint64_t ref_ = 0, last_checked_ = 0;
    double period_;
    double len_est_ = 0;
    uint8_t inphase_ = 0, miss_ = 0;
    uint32_t total_ = 0, off_ = 0, ignored_ = 0;
    BurstRec ring_[kBursts];
};

}  // namespace gs
