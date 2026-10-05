// gs_can — пассивный разбор CAN-кадров батареи (Seplos BMS 3.0, протокол по умолчанию —
// Pylontech-совместимый, 500 кбит/с). Только чистая логика без Arduino: тестируется нативно.
// Драйвер TWAI (listen-only) — в src/can_rx.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

namespace gs {

struct CanFrame {
    uint32_t id = 0;            // 11-битный или 29-битный идентификатор
    uint8_t dlc = 0;
    uint8_t data[8] = {0};
    uint32_t t_ms = 0;          // время приёма (мс с загрузки, 32 бита: разности считаем по модулю 2^32)
    bool ext = false;           // расширенный (29 бит) формат
};

constexpr size_t kCanMaxIds = 24;          // размер таблицы статистики по ID
constexpr uint32_t kCanStaleMs = 10000;    // кадр старше — значения недостоверны

// Возраст по модулю 2^32; «время из будущего» (гонка между задачами) считаем нулём.
uint32_t can_age_ms(uint32_t t_ms, uint32_t now_ms);

struct CanIdStat {
    uint32_t id = 0; bool ext = false;
    uint32_t count = 0;
    uint32_t last_ms = 0;
    uint32_t period_ms = 0;     // сглаженная оценка периода; 0 пока кадров меньше двух
    uint8_t dlc = 0; uint8_t data[8] = {0};
};

// Таблица статистики по ID (отсортирована по ID). Новые ID сверх kCanMaxIds не заводятся, их кадры
// считаются в overflow().
class CanStats {
public:
    void on_frame(const CanFrame& f);
    size_t size() const { return n_; }
    const CanIdStat& at(size_t i) const { return t_[i]; }
    const CanIdStat* find(uint32_t id, bool ext) const;
    uint32_t total() const { return total_; }
    uint32_t overflow() const { return overflow_; }
    void clear() { *this = CanStats(); }
private:
    CanIdStat t_[kCanMaxIds];
    size_t n_ = 0;
    uint32_t total_ = 0, overflow_ = 0;
};

// ---- Pylontech-совместимые кадры (11-битные ID) -------------------------------------------------
struct PylonLimits { bool have = false; uint32_t t_ms = 0; float charge_v = 0, charge_a = 0, discharge_a = 0, discharge_v_min = 0; };   // 0x351
struct PylonSoc    { bool have = false; uint32_t t_ms = 0; int soc = 0, soh = 0; };                                                   // 0x355
struct PylonPack   { bool have = false; uint32_t t_ms = 0; float v = 0, i = 0, temp_c = 0; };                                         // 0x356; i > 0 — заряд
struct PylonProt   { bool have = false; uint32_t t_ms = 0; uint8_t prot1 = 0, prot2 = 0, alarm1 = 0, alarm2 = 0, modules = 0; bool pn = false; };   // 0x359
struct PylonReq    { bool have = false; uint32_t t_ms = 0; uint8_t raw = 0; bool charge_en = false, discharge_en = false, force1 = false, force2 = false, full_req = false; };   // 0x35C
struct PylonMfr    { bool have = false; uint32_t t_ms = 0; char name[9] = {0}; };                                                     // 0x35E

struct PylonBattery {
    PylonLimits limits; PylonSoc soc; PylonPack pack; PylonProt prot; PylonReq req; PylonMfr mfr;
    // true — кадр известен и разобран (короткие кадры игнорируются)
    bool feed(const CanFrame& f);
};

inline bool can_fresh(bool have, uint32_t t_ms, uint32_t now_ms) { return have && can_age_ms(t_ms, now_ms) <= kCanStaleMs; }

// Имя бита (0..7) байта 0/1 Protection (prot1, prot2) или Alarm (alarm1, alarm2); nullptr — бит не определён.
const char* pylon_prot_name(int byte, int bit);
const char* pylon_alarm_name(int byte, int bit);

// Краткая сводка для /api/state; NaN / -1 — нет свежих данных.
struct BatteryBrief {
    float v, i, soc;
    int8_t charge_en, discharge_en, alarm;    // -1 нет данных
    int32_t age_s;                             // возраст самого свежего кадра, с; -1 — кадров не было
    BatteryBrief();
};
BatteryBrief battery_brief(const PylonBattery& b, uint32_t now_ms);
// {"v":..,"i":..,"soc":..,"charge_en":..,"discharge_en":..,"alarm":..,"age_s":..}; 0 — не влезло.
size_t battery_brief_json(const BatteryBrief& b, char* out, size_t cap);

// Состояние драйвера для /api/can (заполняет src/can_rx.cpp).
struct CanDriverInfo {
    bool enabled = false; uint16_t speed_kbps = 0;
    bool ack = false;                          // true: узел подтверждает кадры (ACK, режим normal без очереди передачи); false: listen-only
    const char* state = "off";                 // off / running / stopped / bus_off / recovering
    uint32_t rx_msgs = 0, rx_missed = 0, bus_errors = 0, bus_off_count = 0, rx_err_counter = 0;
};
// Полный JSON /api/can. 0 — не влезло в буфер. Для 24 ID с запасом хватает kCanJsonMax.
constexpr size_t kCanJsonMax = 4800;
size_t can_to_json(const CanDriverInfo& d, const CanStats& st, const PylonBattery& b, uint32_t now_ms, char* out, size_t cap);

}  // namespace gs
