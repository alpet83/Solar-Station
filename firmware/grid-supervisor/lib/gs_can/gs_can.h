// gs_can — пассивный разбор CAN-кадров батареи (Seplos BMS 3.0). Три декодера:
//  - PylonBattery: Pylontech-совместимый протокол (11-битные ID 0x351…, 500 кбит/с по мануалу);
//  - SeplosBattery: реальный протокол, наблюдаемый на проводе: 100 кбит/с, 29-битные ID 0x000301xx…0x000305xx.
//    Его раскладка — ГИПОТЕЗА по значениям, документации нет (см. README, confidence = "guess");
//  - Seplos500Battery: режим BMS «0. Pylon CAN» на самом деле: 500 кбит/с, СТАНДАРТНЫЕ ID 0x311…0x322, u16 BIG-endian,
//    каждый кадр раз в секунду. Раскладка — по значениям, документации нет (confidence = "partial": см. README).
// Только чистая логика без Arduino: тестируется нативно.
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

// ---- наблюдаемый протокол Seplos на 100 кбит/с (29-битные ID, little-endian, ~1 Гц) ------------------
// ВСЕ раскладки ниже — предположения по значениям на проводе, не подтверждены документацией.
constexpr uint32_t kSeplosIdBase = 0x00030000;   // ID = 0x000300NN; NN = 0x01 / 0x02 / 0x04 / 0x05
struct SeplosLimits { bool have = false; uint32_t t_ms = 0; float charge_v = 0, charge_a = 0, discharge_v_min = 0, discharge_a = 0; };   // 0x…0100: 4 × u16, шаг 0,1
struct SeplosProt   { bool have = false; uint32_t t_ms = 0; uint8_t raw[8] = {0}; bool alarm = false; };        // 0x…0200: раскладка битов неизвестна; alarm = есть ненулевой байт
struct SeplosPack   { bool have = false; uint32_t t_ms = 0; float v = 0, i_abs = 0, temp_c = 0; };            // 0x…0400: U 0,1 В; |I| 0,1 А (знак НЕ известен); temp_c — предположительно °C
struct SeplosSoc    { bool have = false; uint32_t t_ms = 0; int soc = 0, soh = 0, soc10 = 0; };               // 0x…0500: SOC %, SOH % (вероятно), SOC в 0,1 % (вероятно)

struct SeplosBattery {
    SeplosLimits limits; SeplosProt prot; SeplosPack pack; SeplosSoc soc;
    // true — кадр узнан и разобран; чужие/короткие/стандартные кадры игнорируются
    bool feed(const CanFrame& f);
    uint32_t last_ms() const;                  // время самого свежего разобранного кадра (если any())
    bool any() const { return limits.have || prot.have || pack.have || soc.have; }
};

// ---- наблюдаемый протокол Seplos на 500 кбит/с (режим BMS «0. Pylon CAN»; стандартные ID 0x311…0x322, BIG-endian, 1 Гц) ----
// Уровни уверенности (по значениям на проводе; документации нет):
//   УВЕРЕННО: 0x315–0x318 (напряжения 16 ячеек, мВ), 0x313 байты 0–1 (U пакета) и 6–7 (SOC, SOH), 0x319 байты 1–4 (макс./мин. ячейка);
//   СРЕДНЕ:   0x313 ток (знак НЕ подтверждён) и температура, 0x311 предел заряда 57,6 В;
//   ГИПОТЕЗА (guess): 0x311 токи, 0x314 ёмкость/циклы, 0x322 температуры; сырьё без расшифровки: 0x312, 0x320, прочие байты.
constexpr uint32_t kSeplos5IdFirst = 0x311, kSeplos5IdLast = 0x322;
struct Sep5Cells  { bool have[4] = {false, false, false, false}; uint32_t t_ms[4] = {0, 0, 0, 0}; uint16_t mv[16] = {0}; };   // 0x315..0x318: по 4 ячейки, u16 BE, мВ (УВЕРЕННО)
struct Sep5Pack   { bool have = false; uint32_t t_ms = 0; float v = 0, i_raw = 0, temp_c = 0; int soc = 0, soh = 0; };       // 0x313: U 0,01 В (уверенно); I int16 0,1 А знак НЕ подтверждён (средне); T int16 0,1 °C (средне); SOC, SOH % (уверенно)
struct Sep5MinMax { bool have = false; uint32_t t_ms = 0; uint16_t max_mv = 0, min_mv = 0; uint8_t b0 = 0, idx[2] = {0, 0}; };    // 0x319: байт0 = 0xC0 (неизвестно); max/min мВ (УВЕРЕННО); байты 5–6 — индексы (неизвестно)
struct Sep5Limits { bool have = false; uint32_t t_ms = 0; float charge_v_limit = 0, charge_a = 0, discharge_a = 0; uint16_t unk = 0; };   // 0x311: 57,6 В (средне); два тока 0,1 А (guess, порядок «V, I, I»); байты 6–7 неизвестны
struct Sep5Cap    { bool have = false; uint32_t t_ms = 0; float remaining_ah = 0, full_ah = 0; uint16_t counter = 0, cycles = 0; };   // 0x314: u16 0,01 А·ч ×2, счётчик (неизвестен), циклы — ВСЁ guess
struct Sep5Temps  { bool have = false; uint32_t t_ms = 0; float max_c = 0, min_c = 0; uint8_t unk[4] = {0}; };                      // 0x322: макс./мин. температура 0,1 °C (guess); байты 4–7 неизвестны
struct Sep5Raw    { bool have = false; uint32_t t_ms = 0; uint8_t raw[8] = {0}; };                                                // 0x312 (флаги состояния/аварий?) и 0x320 — сырые байты

struct Seplos500Battery {
    Sep5Cells cells; Sep5Pack pack; Sep5MinMax mm; Sep5Limits limits; Sep5Cap cap; Sep5Temps temps; Sep5Raw flags /*0x312*/, unk320;
    // true — кадр узнан и разобран; расширенные, короткие (dlc < 8) и чужие ID игнорируются
    bool feed(const CanFrame& f);
    uint32_t last_ms() const;                  // время самого свежего разобранного кадра (если any())
    bool any() const;
    // Ячейка i (0..15) годна: её кадр получен и не старше kCanStaleMs.
    bool cell_ok(size_t i, uint32_t now_ms) const;
    // Мин./макс. напряжение ячейки, мВ: из 0x319 (если свеж), иначе по массиву, если все 16 ячеек свежие. false — нет данных.
    bool cell_range(uint32_t now_ms, int* min_mv, int* max_mv) const;
};

inline bool can_fresh(bool have, uint32_t t_ms, uint32_t now_ms) { return have && can_age_ms(t_ms, now_ms) <= kCanStaleMs; }

// Имя бита (0..7) байта 0/1 Protection (prot1, prot2) или Alarm (alarm1, alarm2); nullptr — бит не определён.
const char* pylon_prot_name(int byte, int bit);
const char* pylon_alarm_name(int byte, int bit);

// Краткая сводка для /api/state; NaN / -1 — нет свежих данных.
struct BatteryBrief {
    float v, i, soc;
    float i_abs;                               // |I| без знака (Seplos: знак неизвестен, i = NaN)
    int32_t cell_min_mv, cell_max_mv, cell_delta_mv;   // только seplos_500k; -1 — нет данных
    const char* protocol;                      // "pylon" / "seplos_100k" / "seplos_500k" / nullptr — какой декодер получал кадры последним
    int8_t charge_en, discharge_en, alarm;    // -1 нет данных
    int32_t age_s;                             // возраст самого свежего кадра, с; -1 — кадров не было
    BatteryBrief();
};
BatteryBrief battery_brief(const PylonBattery& b, uint32_t now_ms);
// Для двух декодеров: берётся тот, чей кадр пришёл позже (при равенстве — Pylon).
BatteryBrief battery_brief(const PylonBattery& b, const SeplosBattery& s, uint32_t now_ms);
// Для трёх декодеров: берётся тот, чей кадр пришёл позже (при равенстве приоритет Pylon, затем seplos_100k).
BatteryBrief battery_brief(const PylonBattery& b, const SeplosBattery& s, const Seplos500Battery& s5, uint32_t now_ms);
// {"v":..,"i":..,"soc":..,"charge_en":..,"discharge_en":..,"alarm":..,"age_s":..,"i_abs":..,"protocol":..,"cell_min_mv":..,"cell_max_mv":..,"cell_delta_mv":..}; 0 — не влезло.
size_t battery_brief_json(const BatteryBrief& b, char* out, size_t cap);

// Состояние драйвера для /api/can (заполняет src/can_rx.cpp).
struct CanDriverInfo {
    bool enabled = false; uint16_t speed_kbps = 0;
    bool hb = false;                           // шлём «жив» инвертора: кадр 0x305 из восьми нулей раз в секунду (только если включено вручную)
    bool ack = false;                          // true: узел подтверждает кадры (ACK, режим normal без очереди передачи); false: listen-only
    const char* state = "off";                 // off / running / stopped / bus_off / recovering
    uint32_t rx_msgs = 0, rx_missed = 0, bus_errors = 0, bus_off_count = 0, rx_err_counter = 0;
};
// Полный JSON /api/can. 0 — не влезло в буфер. Для 24 ID с запасом хватает kCanJsonMax.
constexpr size_t kCanJsonMax = 5200;
size_t can_to_json(const CanDriverInfo& d, const CanStats& st, const PylonBattery& b, uint32_t now_ms, char* out, size_t cap);
size_t can_to_json(const CanDriverInfo& d, const CanStats& st, const PylonBattery& b, const SeplosBattery& s, uint32_t now_ms, char* out, size_t cap);
size_t can_to_json(const CanDriverInfo& d, const CanStats& st, const PylonBattery& b, const SeplosBattery& s, const Seplos500Battery& s5, uint32_t now_ms, char* out, size_t cap);

}  // namespace gs
