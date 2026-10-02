// Адаптер Arduino/ESP32 для gs_core: «вежливый» мастер Modbus RTU на RS485 T-CAN485.
// Слайсы C1/C2. Wi-Fi/OTA/MQTT — отдельные слайсы (S*), здесь только ядро + шина.
//
// Пины T-CAN485 заданы по схеме платы и ПОДЛЕЖАТ ПРОВЕРКЕ на железе (переопределяются -D).
#include <Arduino.h>
#include <esp_random.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <cstring>

#include "gs_bus.h"
#include "gs_core.h"
#include "gs_led.h"
#include <atomic>
#include "gs_modbus.h"
#include "shell.h"
#include <ctime>

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
#ifndef GS_PIN_LED          // WS2812 на T-CAN485 (GPIO4 по схеме платы — проверить)
#define GS_PIN_LED 4
#endif
#ifndef GS_LED_BRIGHT       // 0..255: общая яркость (индикатор не должен слепить ночью)
#define GS_LED_BRIGHT 40
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
static gs::RegCache g_cache;
// Пассивный разбор шины и трекер ритма логгера: используются только из задачи ядра.
static gs::BusSniffer g_sn(6);
static gs::LoggerTracker g_trk;
static const uint32_t kPollPeriodActiveMs = 3000;   // между пачками логгера опрашиваем чаще
static const uint32_t kPollNeedMs = 3500;           // сколько тишины нужно для одного цикла опроса
static std::atomic<uint8_t> g_inv_state{(uint8_t)gs::InvState::NoLink};   // для светодиода
static std::atomic<int64_t> g_last_rt_ok_ms{-1};
static char g_sniff_json[9000];
static size_t g_sniff_len = 0;
static SemaphoreHandle_t g_sniff_mtx;
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

static uint64_t up_ms() { return (uint64_t)(esp_timer_get_time() / 1000); }

// Всё принятое с шины (кроме собственного эха) отдаём сборщику кадров; закрытый кадр — трекеру.
static void bus_in(const uint8_t* p, size_t n) {
    const uint64_t now = up_ms();
    if (n && g_sn.feed(p, n, now)) {
        const gs::FrameRec& f = g_sn.last();
        g_trk.observe(f, now - (uint32_t)((uint32_t)now - f.t_ms));
    }
}
static void bus_tick() {
    const uint64_t now = up_ms();
    if (g_sn.poll(now)) {
        const gs::FrameRec& f = g_sn.last();
        g_trk.observe(f, now - (uint32_t)((uint32_t)now - f.t_ms));
    }
    g_trk.tick(now);
}
// Слушаем шину, пока ждём: иначе переполнится буфер UART и кадры логгера потеряются.
static void bus_listen() {
    uint8_t tmp[64];
    for (;;) {
        size_t n = 0;
        while (n < sizeof tmp && Serial1.available()) tmp[n++] = (uint8_t)Serial1.read();
        if (!n) break;
        bus_in(tmp, n);
    }
    bus_tick();
}
static void bus_delay(uint32_t ms) {
    const uint32_t t0 = millis();
    while (millis() - t0 < ms) { esp_task_wdt_reset(); bus_listen(); delay(1); }
}

static size_t sniff_provider(char* out, size_t cap) {
    size_t n = 0;
    if (g_sniff_mtx && xSemaphoreTake(g_sniff_mtx, pdMS_TO_TICKS(100))) {
        if (g_sniff_len && g_sniff_len < cap) { memcpy(out, g_sniff_json, g_sniff_len + 1); n = g_sniff_len; }
        xSemaphoreGive(g_sniff_mtx);
    }
    return n;
}
static void sniff_rebuild() {
    static char tmp[sizeof g_sniff_json];
    time_t tt = time(nullptr);
    size_t n = gs::sniff_to_json(g_sn, g_trk, up_ms(), tt > 1700000000 ? (int64_t)tt : -1, 24, 24, tmp, sizeof tmp);
    if (n && xSemaphoreTake(g_sniff_mtx, pdMS_TO_TICKS(100))) {
        memcpy(g_sniff_json, tmp, n + 1); g_sniff_len = n; xSemaphoreGive(g_sniff_mtx);
    }
}

// Ждём окно на шине. Любой чужой трафик сбрасывает ожидание; если виден валидный запрос
// другого мастера к нашему инвертору, ждём ещё и его ответ (инвертор отвечает с задержкой).
static bool wait_idle() {
    uint8_t seen[64]; size_t n = 0;
    uint32_t start = millis(), last_rx = millis(), need = kIdleBeforeTxMs + (esp_random() % 50);
    while (millis() - start < kIdleWaitMaxMs) {
        esp_task_wdt_reset();
        if (Serial1.available()) {
            uint8_t tmp[64]; size_t k = 0;
            while (Serial1.available() && k < sizeof tmp) {
                uint8_t c = (uint8_t)Serial1.read();
                tmp[k++] = c;
                if (n == sizeof seen) { memmove(seen, seen + 1, n - 1); --n; }
                seen[n++] = c;
            }
            bus_in(tmp, k);
            last_rx = millis();
            need = gs::scan_has_request(seen, n, gs::kInverterSlave) ? kAfterForeignReqMs
                                                                      : kIdleBeforeTxMs + (esp_random() % 50);
        } else if (millis() - last_rx >= need) {
            return true;
        }
        bus_tick();
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
        { const size_t n0 = n;
          while (Serial1.available() && n < sizeof buf) { buf[n++] = (uint8_t)Serial1.read(); last = millis(); }
          if (n > n0) bus_in(buf + n0, n - n0); }
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
        if (r == RxRes::Ok) { ++g_stat_ok; g_cache.store(addr, cnt, regs, (uint64_t)(esp_timer_get_time() / 1000)); return true; }
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

// Решения принимаются только по блокам, прочитанным в ЭТОМ цикле: пропущенный (из-за логгера)
// или неудавшийся опрос = нет данных; ядро само сбрасывает выдержку «заряжено» и считает no_data.
static void poll_inverter(bool do_poll) {
    static uint16_t rt[gs::kBlockRtCount], chg[gs::kBlockChgCount], cfg[gs::kBlockCfgCount];
    g_tel = gs::Telemetry();
    if (!do_poll) return;
    const uint32_t t0 = millis();
    if (read_block(gs::kBlockRtAddr, gs::kBlockRtCount, rt)) { gs::decode_rt(rt, g_tel); g_last_rt_ok_ms.store((int64_t)up_ms()); }
    bus_delay(kBlockSpacingMs);
    if (millis() - t0 < kPollBudgetMs && read_block(gs::kBlockChgAddr, gs::kBlockChgCount, chg)) gs::decode_chg(chg, g_tel);
    bus_delay(kBlockSpacingMs);
    if (millis() - t0 < kPollBudgetMs && read_block(gs::kBlockCfgAddr, gs::kBlockCfgCount, cfg)) gs::decode_cfg(cfg, g_tel);
}

// Медленные блоки (идентификация, калибровка, параметры заряда): нужны только кэшу /api/regs для
// pv1800_ctrl.py; читаются при старте и затем раз в 5 минут, при неудаче — повтор через 30 с.
static void poll_slow() {
    static uint32_t next = 0;
    static const struct { uint16_t addr, cnt; } blocks[] = {{20000, 16}, {10001, 8}, {10101, 23}};
    if ((int32_t)(millis() - next) < 0) return;
    static uint16_t tmp[gs::kMaxBlockRegs];
    bool all = true;
    for (auto& b : blocks) {
        bus_delay(kBlockSpacingMs);
        if (!g_trk.may_transmit(up_ms(), 2500).ok) { all = false; break; }   // каждый блок отдельно вне пачки логгера
        all &= read_block(b.addr, b.cnt, tmp);
    }
    next = millis() + (all ? 300000u : 30000u);
}

// Задача ядра: опрос инвертора, решение, команда, публикация снимка. Работает независимо
// от оболочки: зависание Wi-Fi/HTTP на неё не влияет (своя задача, свой WDT).
static void gs_task(void*) {
    esp_task_wdt_add(nullptr);
    uint32_t next = millis();
    uint32_t seq = 0, ev_id = 0;
    gs::Event events[gs::kEventCap]; size_t n_events = 0;
    for (;;) {
        esp_task_wdt_reset();
        if ((int32_t)(millis() - next) < 0) { bus_listen(); delay(2); continue; }
        next = millis() + (g_trk.active_window(up_ms()) ? kPollPeriodActiveMs : kPollPeriodMs);

        const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
        if (now_ms > kStableBootMs) { rtc_short_boots = 0; g_safe_mode = false; }   // стабильно проработали — выходим из safe-mode

        gs::Config nc;
        if (shell::take_config(nc)) g_sv.set_config(nc);
        { int mc = shell::take_manual(); if (mc >= 0 && !shell::sniff_only()) g_sv.manual_request(mc ? gs::Cmd::Connect : gs::Cmd::Disconnect, now_ms); }

        // safe-mode после аварийных перезагрузок отменяет режим приёма: защиты (возврат в сеть) важнее
        const bool sniff_only = shell::sniff_only() && !g_safe_mode;
        if (sniff_only || shell::ota_active()) {
            g_tel = gs::Telemetry();                       // на шину не передаём
        } else {
            const gs::Gate gate = g_trk.may_transmit(now_ms, kPollNeedMs);
            poll_inverter(gate.ok);
            if (gate.ok) poll_slow();
        }
        float ext_a = gs::kNaN; uint64_t ext_age = 0;
        float a; uint64_t age;
        if (shell::ext_charge(a, age)) { ext_a = a; ext_age = age; }
        gs::Input in = gs::make_input(g_tel, now_ms, g_safe_mode, ext_a, ext_age);
        gs::Decision d;
        if (!sniff_only) d = g_sv.step(in);            // в режиме приёма ядро не шагает: команд нет

        if (d.cmd != gs::Cmd::None) {
            // во время OTA (стирание flash блокирует кэш и UART ISR) на шину не передаём: ядро повторит команду
            bool ok = !shell::ota_active() && !sniff_only && write_ongrid(d.cmd == gs::Cmd::Connect);
            gs::Event e; e.id = ++ev_id; e.cmd = d.cmd; e.reason = d.reason; e.uptime_ms = now_ms; e.v = d.v_used; e.ok = ok;
            time_t tt = time(nullptr); e.epoch_s = tt > 1700000000 ? (int64_t)tt : -1;
            for (size_t i = gs::kEventCap - 1; i > 0; --i) events[i] = events[i - 1];
            events[0] = e; if (n_events < gs::kEventCap) ++n_events;
            Serial.printf("CMD %s reason=%d ok=%d\n", d.cmd == gs::Cmd::Connect ? "CONNECT" : "DISCONNECT", (int)d.reason, ok);
        }
        Serial.printf("v=%.1f/%.1f chg=%.1f load=%.0f reg=%d relay=%d flags=0x%04x rx ok/err/foreign/busy/exc=%u/%u/%u/%u/%u\n",
                      g_tel.pack_v, g_tel.pack_v_alt, g_tel.chg_curr_a, g_tel.load_w, g_tel.ongrid_reg, g_tel.relay_grid,
                      (unsigned)d.flags, g_stat_ok, g_stat_err, g_stat_foreign, g_stat_busy, g_stat_exc);

        {   // состояние инвертора для светодиода: связь = блок реального времени читался за последние 45 с
            const int64_t last = g_last_rt_ok_ms.load();
            const bool link = last >= 0 && (int64_t)now_ms - last <= 45000;
            const uint32_t problem_mask = gs::F_HARD_LOW | gs::F_SOFT_LOW | gs::F_NO_DATA | gs::F_DATA_DOUBTFUL | gs::F_GRID_LOST_LOW |
                                          gs::F_SAFE_MODE | gs::F_CMD_RETRY | gs::F_RELAY_MISMATCH | gs::F_TIME_JUMP | gs::F_SINGLE_SRC;
            gs::InvState st;
            if (sniff_only) st = gs::InvState::Sniff;
            else if (!link) st = gs::InvState::NoLink;
            else if ((d.flags & problem_mask) || g_safe_mode) st = gs::InvState::Problem;
            else {
                const int on = g_tel.ongrid_reg >= 0 ? g_tel.ongrid_reg : (g_tel.relay_grid >= 0 ? g_tel.relay_grid : 1);
                st = on ? gs::InvState::OkGrid : gs::InvState::OkOff;
            }
            g_inv_state.store((uint8_t)st);
        }
        static shell::Snapshot s;
        s.tel = g_tel; s.dec = d; s.in = in; s.uptime_ms = now_ms; s.seq = ++seq; s.safe_mode = g_safe_mode; s.sniff_only = sniff_only;
        s.paused = (d.flags & gs::F_MANUAL_PAUSE) != 0;
        s.pause_left_ms = s.paused ? g_sv.pause_until_ms() - now_ms : 0;
        s.cache = g_cache;
        for (size_t i = 0; i < n_events; ++i) s.events[i] = events[i];
        s.n_events = n_events;
        s.rx_ok = g_stat_ok; s.rx_err = g_stat_err; s.rx_foreign = g_stat_foreign; s.rx_busy = g_stat_busy; s.rx_exc = g_stat_exc;
        sniff_rebuild();
        shell::publish(s);
        shell::mark_healthy(seq);
        // оболочка зависла надолго — перезапуск (SW-сброс не считается аварийным; ядро безопасно стартует в сторону сети)
        if (shell::heartbeat_age_ms() > 900000) { Serial.println("shell stalled -> restart"); delay(100); ESP.restart(); }
    }
}

// Светодиод: отдельная задача низкого приоритета, не влияет на ядро и не блокируется оболочкой.
static void led_task(void*) {
    gs::Rgb last; bool first = true;
    for (;;) {
        gs::LedStatus st;
        switch (shell::link_state()) {
            case shell::Link::Connected: st.net = gs::NetState::Connected; break;
            case shell::Link::Ap: st.net = gs::NetState::Ap; break;
            default: st.net = gs::NetState::Offline;
        }
        st.api_recent = shell::api_age_ms() < 60000;
        st.mqtt_bad = shell::mqtt_bad();
        st.inv = (gs::InvState)g_inv_state.load();
        gs::Rgb c = gs::led_color(st, (uint32_t)(esp_timer_get_time() / 1000));
        if (first || !(c == last)) {
            neopixelWrite(GS_PIN_LED, (uint8_t)((c.r * GS_LED_BRIGHT) / 255), (uint8_t)((c.g * GS_LED_BRIGHT) / 255), (uint8_t)((c.b * GS_LED_BRIGHT) / 255));
            last = c; first = false;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

void setup() {
    Serial.begin(115200);
    init_boot_counter();
    pinMode(GS_PIN_5V_EN, OUTPUT); digitalWrite(GS_PIN_5V_EN, HIGH);
    pinMode(GS_RS485_SE, OUTPUT);  digitalWrite(GS_RS485_SE, HIGH);
    pinMode(GS_RS485_DE, OUTPUT);  rx_mode();
    Serial1.setRxBufferSize(1024);
    Serial1.begin(GS_BAUD, SERIAL_8N1, GS_RS485_RX, GS_RS485_TX);
    g_sniff_mtx = xSemaphoreCreateMutex();
    // собственные запросы pv1800_ctrl.py (режим наблюдателя) — не ритм логгера
    static const struct { uint16_t a, c; } own[] = {{25201, 79}, {15201, 22}, {20101, 44}, {20000, 16}, {10001, 8}, {10101, 23}};
    for (auto& o : own) g_trk.ignore(0x03, o.a, o.c);
    shell::set_sniff_provider(sniff_provider);

    esp_task_wdt_config_t wdt = {.timeout_ms = kWdtSeconds * 1000, .idle_core_mask = 0, .trigger_panic = true};
    if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
    // loop() (оболочка) намеренно НЕ в TWDT: её зависание не должно перезагружать ядро.

    g_sv.set_config(shell::load_config());
    shell::begin(esp_random());
    Serial.printf("gs boot: reset=%d short_boots=%u safe=%d\n", (int)esp_reset_reason(), (unsigned)rtc_short_boots, g_safe_mode);
    xTaskCreatePinnedToCore(gs_task, "gs_core", 8192, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(led_task, "gs_led", 3072, nullptr, 1, nullptr, 0);
}

void loop() {
    shell::loop();
    delay(2);
}
