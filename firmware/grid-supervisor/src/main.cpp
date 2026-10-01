// Адаптер Arduino/ESP32 для gs_core: «вежливый» мастер Modbus RTU на RS485 T-CAN485.
// Слайсы C1/C2. Wi-Fi/OTA/MQTT — отдельные слайсы (S*), здесь только ядро + шина.
//
// Пины T-CAN485 заданы по схеме платы и ПОДЛЕЖАТ ПРОВЕРКЕ на железе (переопределяются -D).
#include <Arduino.h>
#include <esp_random.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <cstring>

#include "gs_core.h"
#include "gs_modbus.h"

#ifndef GS_RS485_RX
#define GS_RS485_RX 21
#endif
#ifndef GS_RS485_TX
#define GS_RS485_TX 22
#endif
#ifndef GS_RS485_DE       // управление направлением (DE и /RE вместе)
#define GS_RS485_DE 17
#endif
#ifndef GS_RS485_SE       // питание/enable трансивера на T-CAN485
#define GS_RS485_SE 19
#endif
#ifndef GS_PIN_5V_EN
#define GS_PIN_5V_EN 16
#endif
#ifndef GS_BAUD
#define GS_BAUD 19200
#endif

static const uint32_t kPollPeriodMs = 5000;
static const uint32_t kIdleBeforeTxMs = 100;     // тишина на шине перед нашим запросом (+ случайная добавка)
static const uint32_t kAfterForeignReqMs = 900;  // после чужого запроса к инвертору ждём его ответ и паузу
static const uint32_t kIdleWaitMaxMs = 3000;     // максимум ждём окна (занят второй мастер)
static const uint32_t kFirstByteTimeoutMs = 800; // ожидание начала ответа
static const uint32_t kFrameGapMs = 12;          // пауза, означающая конец кадра (T3.5 при 19200 ~ 2 мс)
static const uint32_t kBlockSpacingMs = 300;     // как у pv1800_ctrl.py: пауза между запросами
static const uint32_t kPollBudgetMs = 15000;     // общий лимит опроса за цикл
static const uint32_t kWdtSeconds = 30;
static const uint32_t kStableBootMs = 600000;    // столько работаем без аварийных перезагрузок -> счётчик в ноль

static gs::Supervisor g_sv;
static gs::Telemetry g_tel;
static uint32_t g_stat_ok = 0, g_stat_err = 0, g_stat_foreign = 0, g_stat_busy = 0, g_stat_exc = 0;

// --- safe-mode: счётчик «коротких» загрузок в RTC-памяти -------------------------------------
RTC_NOINIT_ATTR static uint32_t rtc_magic;
RTC_NOINIT_ATTR static uint32_t rtc_short_boots;
static bool g_safe_mode = false;

static void init_boot_counter() {
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_POWERON || rtc_magic != 0xC0FFEE11u) { rtc_magic = 0xC0FFEE11u; rtc_short_boots = 0; }
    else if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
             rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT) {
        ++rtc_short_boots;                       // только аварийные причины (не EN-кнопка и не DTR монитора)
    }
    g_safe_mode = rtc_short_boots >= 3;
}

// --- RS485 ---------------------------------------------------------------------------------
static void rx_mode() { digitalWrite(GS_RS485_DE, LOW); }

static void drain() { while (Serial1.available()) Serial1.read(); }

// Ждём окно на шине. Любой чужой трафик сбрасывает ожидание; если виден валидный запрос
// другого мастера к нашему инвертору, ждём ещё и его ответ (инвертор отвечает с задержкой).
static bool wait_idle() {
    uint8_t seen[64]; size_t n = 0;
    uint32_t start = millis(), last_rx = millis(), need = kIdleBeforeTxMs + (esp_random() % 50);
    while (millis() - start < kIdleWaitMaxMs) {
        esp_task_wdt_reset();
        if (Serial1.available()) {
            while (Serial1.available()) {
                uint8_t c = (uint8_t)Serial1.read();
                if (n == sizeof seen) { memmove(seen, seen + 1, n - 1); --n; }
                seen[n++] = c;
            }
            last_rx = millis();
            need = gs::scan_has_request(seen, n, gs::kInverterSlave) ? kAfterForeignReqMs
                                                                      : kIdleBeforeTxMs + (esp_random() % 50);
        } else if (millis() - last_rx >= need) {
            return true;
        }
        delay(1);
    }
    ++g_stat_busy;
    return false;
}

static void tx(const uint8_t* f, size_t n) {
    digitalWrite(GS_RS485_DE, HIGH);
    Serial1.write(f, n);
    Serial1.flush();
    rx_mode();
    delayMicroseconds(300);
    drain();                                  // собственное эхо при включённом приёмнике
}

// Принимаем всё до паузы/таймаута и ищем валидный ответ сканером (мусор/склейки допустимы).
enum class RxRes : uint8_t { Ok, Timeout, Exception };

static RxRes rx_read(uint8_t fn, uint16_t cnt, uint16_t* regs) {
    uint8_t buf[320]; size_t n = 0;
    uint32_t start = millis(), last = start;
    while (true) {
        esp_task_wdt_reset();
        while (Serial1.available() && n < sizeof buf) { buf[n++] = (uint8_t)Serial1.read(); last = millis(); }
        if (n) {
            gs::Rx r = gs::scan_read(buf, n, gs::kInverterSlave, fn, cnt, regs);
            if (r == gs::Rx::Ok) return RxRes::Ok;
            if (r == gs::Rx::Exception) return RxRes::Exception;
            if (millis() - last > kFrameGapMs) { ++g_stat_foreign; return RxRes::Timeout; }
        } else if (millis() - start > kFirstByteTimeoutMs) {
            return RxRes::Timeout;
        }
        if (n >= sizeof buf) { ++g_stat_foreign; return RxRes::Timeout; }
        delay(1);
    }
}

// Одна транзакция чтения: до двух попыток, второй раз после паузы со случайной добавкой.
static bool read_block(uint16_t addr, uint16_t cnt, uint16_t* regs) {
    uint8_t q[8];
    if (!gs::build_read(q, sizeof q, gs::kInverterSlave, 0x03, addr, cnt)) return false;   // holding registers, как у pv1800_ctrl.py
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt) delay(kBlockSpacingMs + (esp_random() % 200));
        if (!wait_idle()) continue;
        tx(q, 8);
        RxRes r = rx_read(0x03, cnt, regs);
        if (r == RxRes::Ok) { ++g_stat_ok; return true; }
        if (r == RxRes::Exception) { ++g_stat_exc; break; }      // повтор бессмысленен
    }
    ++g_stat_err;
    return false;
}

// Запись только отправляется (до 3 попыток дойти до шины). Подтверждение — ТОЛЬКО чтением
// 20105 в ядре: эхо ответа совпадает с запросом байт в байт и не отличимо от нашего эха.
static bool write_ongrid(bool connect) {
    uint8_t q[8];
    if (!gs::build_write_ongrid(q, sizeof q, gs::kInverterSlave, connect)) return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (wait_idle()) { tx(q, 8); delay(150); drain(); return true; }
    }
    return false;
}

static void poll_inverter() {
    static uint16_t rt[gs::kBlockRtCount], chg[gs::kBlockChgCount], cfg[gs::kBlockCfgCount];
    g_tel = gs::Telemetry();
    const uint32_t t0 = millis();
    if (read_block(gs::kBlockRtAddr, gs::kBlockRtCount, rt)) gs::decode_rt(rt, g_tel);
    delay(kBlockSpacingMs);
    if (millis() - t0 < kPollBudgetMs && read_block(gs::kBlockChgAddr, gs::kBlockChgCount, chg)) gs::decode_chg(chg, g_tel);
    delay(kBlockSpacingMs);
    if (millis() - t0 < kPollBudgetMs && read_block(gs::kBlockCfgAddr, gs::kBlockCfgCount, cfg)) gs::decode_cfg(cfg, g_tel);
}

void setup() {
    Serial.begin(115200);
    init_boot_counter();
    pinMode(GS_PIN_5V_EN, OUTPUT); digitalWrite(GS_PIN_5V_EN, HIGH);
    pinMode(GS_RS485_SE, OUTPUT);  digitalWrite(GS_RS485_SE, HIGH);
    pinMode(GS_RS485_DE, OUTPUT);  rx_mode();
    Serial1.begin(GS_BAUD, SERIAL_8N1, GS_RS485_RX, GS_RS485_TX);

    esp_task_wdt_config_t wdt = {.timeout_ms = kWdtSeconds * 1000, .idle_core_mask = 0, .trigger_panic = true};
    if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
    if (esp_task_wdt_add(nullptr) != ESP_OK) Serial.println("WDT: add failed");
    Serial.printf("gs boot: reset=%d short_boots=%u safe=%d\n", (int)esp_reset_reason(), (unsigned)rtc_short_boots, g_safe_mode);
}

void loop() {
    static uint32_t next = 0;
    esp_task_wdt_reset();
    if ((int32_t)(millis() - next) < 0) { delay(5); return; }
    next = millis() + kPollPeriodMs;

    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    if (now_ms > kStableBootMs) { rtc_short_boots = 0; g_safe_mode = false; }   // стабильно проработали — выходим из safe-mode

    poll_inverter();
    gs::Input in = gs::make_input(g_tel, now_ms, g_safe_mode, gs::kNaN, 0);
    gs::Decision d = g_sv.step(in);

    if (d.cmd != gs::Cmd::None) {
        bool ok = write_ongrid(d.cmd == gs::Cmd::Connect);
        Serial.printf("CMD %s reason=%d ok=%d\n", d.cmd == gs::Cmd::Connect ? "CONNECT" : "DISCONNECT", (int)d.reason, ok);
    }
    Serial.printf("v=%.1f/%.1f chg=%.1f load=%.0f reg=%d relay=%d flags=0x%04x rx ok/err/foreign/busy/exc=%u/%u/%u/%u/%u\n",
                  g_tel.pack_v, g_tel.pack_v_alt, g_tel.chg_curr_a, g_tel.load_w, g_tel.ongrid_reg, g_tel.relay_grid,
                  (unsigned)d.flags, g_stat_ok, g_stat_err, g_stat_foreign, g_stat_busy, g_stat_exc);
}
