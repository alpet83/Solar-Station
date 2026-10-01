// Адаптер Arduino/ESP32 для gs_core: «вежливый» мастер Modbus RTU на RS485 T-CAN485.
// Слайсы C1/C2. Wi-Fi/OTA/MQTT — отдельные слайсы (S*), здесь только ядро + шина.
//
// Пины T-CAN485 заданы по схеме платы и ПОДЛЕЖАТ ПРОВЕРКЕ на железе (переопределяются -D).
#include <Arduino.h>
#include <esp_task_wdt.h>

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
static const uint32_t kIdleBeforeTxMs = 20;     // тишина на шине перед нашим запросом
static const uint32_t kIdleWaitMaxMs = 1500;    // максимум ждём тишины (занят второй мастер)
static const uint32_t kRespTimeoutMs = 400;
static const uint32_t kWdtSeconds = 30;

static gs::Supervisor g_sv;
static gs::Telemetry g_tel;
static uint32_t g_stat_ok = 0, g_stat_err = 0, g_stat_foreign = 0, g_stat_busy = 0;

// --- safe-mode: счётчик «коротких» загрузок в RTC-памяти -------------------------------------
RTC_NOINIT_ATTR static uint32_t rtc_magic;
RTC_NOINIT_ATTR static uint32_t rtc_short_boots;
static bool g_safe_mode = false;

static void init_boot_counter() {
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_POWERON || rtc_magic != 0xC0FFEE11u) { rtc_magic = 0xC0FFEE11u; rtc_short_boots = 0; }
    else if (rr != ESP_RST_SW) { ++rtc_short_boots; }            // паника/WDT/brownout
    g_safe_mode = rtc_short_boots >= 3;
}

// --- RS485 ---------------------------------------------------------------------------------
static void rx_mode() { digitalWrite(GS_RS485_DE, LOW); }

static void drain() { while (Serial1.available()) Serial1.read(); }

// Ждём тишину на шине (нас не должно быть там, где идёт чужой обмен).
static bool wait_idle() {
    uint32_t start = millis(), last_rx = millis();
    while (millis() - start < kIdleWaitMaxMs) {
        if (Serial1.available()) { drain(); last_rx = millis(); }
        else if (millis() - last_rx >= kIdleBeforeTxMs) return true;
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
}

// Принимаем кадры до таймаута; чужие/битые отбрасываем, берём первый валидный ответ.
template <typename Check>
static bool rx_valid(size_t expect_len, Check check) {
    uint8_t buf[256]; size_t n = 0; uint32_t last = millis(), start = last;
    while (millis() - start < kRespTimeoutMs) {
        while (Serial1.available() && n < sizeof(buf)) { buf[n++] = (uint8_t)Serial1.read(); last = millis(); }
        if (n >= expect_len && check(buf, n)) return true;
        if (n && millis() - last > 8) {                          // межкадровая пауза
            if (check(buf, n)) return true;
            ++g_stat_foreign; n = 0;                              // чужой/битый кадр — выбрасываем
        }
        delay(1);
    }
    return false;
}

static bool read_block(uint8_t fn, uint16_t addr, uint16_t cnt, uint16_t* regs) {
    uint8_t q[8];
    if (!gs::build_read(q, sizeof q, gs::kInverterSlave, fn, addr, cnt)) return false;
    if (!wait_idle()) return false;
    tx(q, 8);
    bool ok = rx_valid(gs::read_response_len(cnt), [&](const uint8_t* b, size_t n) {
        return gs::parse_read(b, n, gs::kInverterSlave, fn, cnt, regs) == gs::Rx::Ok;
    });
    ok ? ++g_stat_ok : ++g_stat_err;
    return ok;
}

static bool write_ongrid(bool connect) {
    uint8_t q[8];
    if (!gs::build_write_ongrid(q, sizeof q, gs::kInverterSlave, connect)) return false;
    if (!wait_idle()) return false;
    tx(q, 8);
    return rx_valid(8, [&](const uint8_t* b, size_t n) {
        return gs::parse_write_echo(b, n, gs::kInverterSlave, gs::kRegOngridSwitch, connect ? 1 : 0) == gs::Rx::Ok;
    });
}

static void poll_inverter() {
    static uint16_t rt[gs::kBlockRtCount], chg[gs::kBlockChgCount], cfg[gs::kBlockCfgCount];
    g_tel = gs::Telemetry();
    if (read_block(0x04, gs::kBlockRtAddr, gs::kBlockRtCount, rt)) gs::decode_rt(rt, g_tel);
    if (read_block(0x04, gs::kBlockChgAddr, gs::kBlockChgCount, chg)) gs::decode_chg(chg, g_tel);
    if (read_block(0x03, gs::kBlockCfgAddr, gs::kBlockCfgCount, cfg)) gs::decode_cfg(cfg, g_tel);
}

void setup() {
    Serial.begin(115200);
    init_boot_counter();
    pinMode(GS_PIN_5V_EN, OUTPUT); digitalWrite(GS_PIN_5V_EN, HIGH);
    pinMode(GS_RS485_SE, OUTPUT);  digitalWrite(GS_RS485_SE, HIGH);
    pinMode(GS_RS485_DE, OUTPUT);  rx_mode();
    Serial1.begin(GS_BAUD, SERIAL_8N1, GS_RS485_RX, GS_RS485_TX);

    esp_task_wdt_config_t wdt = {.timeout_ms = kWdtSeconds * 1000, .idle_core_mask = 0, .trigger_panic = true};
    esp_task_wdt_reconfigure(&wdt);
    esp_task_wdt_add(nullptr);
    Serial.printf("gs boot: reset=%d short_boots=%u safe=%d\n", (int)esp_reset_reason(), (unsigned)rtc_short_boots, g_safe_mode);
}

void loop() {
    static uint32_t next = 0;
    esp_task_wdt_reset();
    if ((int32_t)(millis() - next) < 0) { delay(5); return; }
    next = millis() + kPollPeriodMs;

    if (millis() > 60000 && rtc_short_boots) rtc_short_boots = 0;   // стабильно проработали — счётчик в ноль

    poll_inverter();
    gs::Input in = gs::make_input(g_tel, millis(), g_safe_mode, gs::kNaN, 0);
    gs::Decision d = g_sv.step(in);

    if (d.cmd != gs::Cmd::None) {
        bool ok = write_ongrid(d.cmd == gs::Cmd::Connect);
        Serial.printf("CMD %s reason=%d ok=%d\n", d.cmd == gs::Cmd::Connect ? "CONNECT" : "DISCONNECT", (int)d.reason, ok);
    }
    Serial.printf("v=%.1f/%.1f chg=%.1f load=%.0f reg=%d relay=%d flags=0x%04x rx ok/err/foreign/busy=%u/%u/%u/%u\n",
                  g_tel.pack_v, g_tel.pack_v_alt, g_tel.chg_curr_a, g_tel.load_w, g_tel.ongrid_reg, g_tel.relay_grid,
                  (unsigned)d.flags, g_stat_ok, g_stat_err, g_stat_foreign, g_stat_busy);
}
