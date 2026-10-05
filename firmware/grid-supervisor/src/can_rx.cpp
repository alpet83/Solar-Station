#include "can_rx.h"

#include <Preferences.h>
#include <driver/twai.h>
#include <esp_timer.h>
#include <atomic>

// Пины T-CAN485 по схеме платы и примерам LilyGo; ПОДЛЕЖАТ ПРОВЕРКЕ на железе (переопределяются -D).
#ifndef GS_CAN_TX
#define GS_CAN_TX 27
#endif
#ifndef GS_CAN_RX
#define GS_CAN_RX 26
#endif
#ifndef GS_CAN_SE           // IO23 = CAN_SE (Rs у SN65HVD230): LOW — нормальный режим, HIGH — standby
#define GS_CAN_SE 23
#endif
#ifndef GS_CAN_SE_ON        // уровень CAN_SE для работы трансивера (проверить на железе)
#define GS_CAN_SE_ON LOW
#endif
#ifndef GS_CAN_SE_OFF
#define GS_CAN_SE_OFF HIGH
#endif

namespace canrx {

static std::atomic<int> g_want{500};         // заданная скорость, кбит/с (0 = выключено)
static std::atomic<bool> g_ack{false};      // подтверждать кадры (ACK) вместо чистого listen-only
static bool g_cur_ack = false;
static std::atomic<bool> g_hb{false};       // раз в секунду слать 0x305 (имитация «жив» инвертора), по умолчанию выключено
static bool g_cur_hb = false;
static uint32_t g_hb_ms = 0;
static int g_cur = -1;                       // применённая задачей (-1 — ещё ничего)
static bool g_installed = false;
static SemaphoreHandle_t g_mtx;
static gs::CanStats g_stats;
static gs::PylonBattery g_bat;             // Pylon-совместимый протокол (11-бит ID)
static gs::SeplosBattery g_sep;           // реальный протокол Seplos 100 кбит/с (29-бит ID, гипотеза)
static gs::Seplos500Battery g_sep5;       // «Pylon CAN» режим BMS на деле: 500 кбит/с, стандартные ID 0x311…0x322, big-endian (гипотеза)
static gs::CanDriverInfo g_info;
static uint32_t g_base_missed = 0, g_base_errs = 0;     // счётчики прежней установки драйвера (после переустановки после сбоя)

static uint32_t ms32() { return (uint32_t)(esp_timer_get_time() / 1000); }

static bool drv_install(int kbps) {
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)GS_CAN_TX, (gpio_num_t)GS_CAN_RX, (g_ack.load() || g_hb.load()) ? TWAI_MODE_NORMAL : TWAI_MODE_LISTEN_ONLY);
    g.rx_queue_len = 32;
    g.tx_queue_len = g_hb.load() ? 2 : 0;    // очередь передачи нужна только для «жив» 0x305; без него передавать нечем
    twai_timing_config_t t250 = TWAI_TIMING_CONFIG_250KBITS();
    twai_timing_config_t t500 = TWAI_TIMING_CONFIG_500KBITS();
    twai_timing_config_t t125 = TWAI_TIMING_CONFIG_125KBITS();
    twai_timing_config_t t100 = TWAI_TIMING_CONFIG_100KBITS();
    twai_timing_config_t t = kbps == 100 ? t100 : kbps == 125 ? t125 : kbps == 250 ? t250 : t500;
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
    if (twai_start() != ESP_OK) { twai_driver_uninstall(); return false; }
    return true;
}

static void drv_remove() {
    if (!g_installed) return;
    twai_stop();
    twai_driver_uninstall();
    g_installed = false;
}

// Применить заданную скорость: переустановка драйвера, сброс статистики.
static void apply(int want) {
    drv_remove();
    pinMode(GS_CAN_SE, OUTPUT);
    digitalWrite(GS_CAN_SE, want ? GS_CAN_SE_ON : GS_CAN_SE_OFF);
    if (want) g_installed = drv_install(want);
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    g_stats.clear(); g_bat = gs::PylonBattery(); g_sep = gs::SeplosBattery(); g_sep5 = gs::Seplos500Battery();
    g_base_missed = g_base_errs = 0;
    g_info = gs::CanDriverInfo();
    g_info.enabled = want != 0; g_info.speed_kbps = (uint16_t)want; g_info.ack = g_ack.load() || g_hb.load(); g_info.hb = g_hb.load();
    g_info.state = !want ? "off" : g_installed ? "running" : "error";
    xSemaphoreGive(g_mtx);
    g_cur = want; g_cur_ack = g_ack.load(); g_cur_hb = g_hb.load();
    Serial.printf("can: %s%s\n", !want ? "off" : g_installed ? (want == 100 ? "listen-only 100 kbit/s" : want == 125 ? "listen-only 125 kbit/s" : want == 250 ? "listen-only 250 kbit/s" : "listen-only 500 kbit/s") : "driver install FAILED, will retry", want && g_cur_ack ? " (+ACK)" : "");
}

// Раз в ~0.5 с: состояние шины и счётчики; bus-off -> автоперезапуск.
static void housekeeping() {
    twai_status_info_t si;
    if (twai_get_status_info(&si) != ESP_OK) return;
    const char* st = "running";
    bool bus_off = false;
    switch (si.state) {
    case TWAI_STATE_BUS_OFF: st = "bus_off"; bus_off = true; break;
    case TWAI_STATE_RECOVERING: st = "recovering"; break;
    case TWAI_STATE_STOPPED: st = "stopped"; break;
    default: break;
    }
    if (bus_off) { twai_initiate_recovery(); Serial.println("can: bus-off, recovery"); }
    else if (si.state == TWAI_STATE_STOPPED) twai_start();           // восстановление завершено (или драйвер остановлен)
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    g_info.state = st;
    if (bus_off) ++g_info.bus_off_count;
    g_info.rx_missed = g_base_missed + si.rx_missed_count + si.rx_overrun_count;
    g_info.bus_errors = g_base_errs + si.bus_error_count;
    g_info.rx_err_counter = si.rx_error_counter;
    xSemaphoreGive(g_mtx);
}

static void can_task(void*) {
    uint32_t hk = 0, retry = 0;
    for (;;) {
        const int want = g_want.load();
        if (want != g_cur || g_ack.load() != g_cur_ack || g_hb.load() != g_cur_hb) { apply(want); retry = millis(); }
        if (!want) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        if (!g_installed) {                                             // установка не удалась: повтор раз в 5 с
            vTaskDelay(pdMS_TO_TICKS(200));
            if (millis() - retry > 5000) { retry = millis(); g_installed = drv_install(want); if (g_installed) { xSemaphoreTake(g_mtx, portMAX_DELAY); g_info.state = "running"; xSemaphoreGive(g_mtx); } }
            continue;
        }
        if (g_hb.load() && g_cur_hb && millis() - g_hb_ms >= 1000) {     // «жив» инвертора: Pylon/Seplos 0x305, 8 нулевых байт
            g_hb_ms = millis();
            twai_message_t hb = {}; hb.identifier = 0x305; hb.data_length_code = 8;
            twai_transmit(&hb, 0);
        }
        twai_message_t m;
        while (twai_receive(&m, pdMS_TO_TICKS(100)) == ESP_OK) {
            if (m.rtr) continue;                                        // удалённые запросы данных не несут
            gs::CanFrame f;
            f.id = m.identifier; f.ext = m.extd; f.t_ms = ms32();
            f.dlc = m.data_length_code > 8 ? 8 : m.data_length_code;
            memcpy(f.data, m.data, f.dlc);
            xSemaphoreTake(g_mtx, portMAX_DELAY);
            g_stats.on_frame(f); g_bat.feed(f); g_sep.feed(f); g_sep5.feed(f); ++g_info.rx_msgs;
            xSemaphoreGive(g_mtx);
            if (millis() - hk >= 500) break;
        }
        if (millis() - hk >= 500) { hk = millis(); housekeeping(); }
    }
}

void begin() {
    g_mtx = xSemaphoreCreateMutex();
    Preferences p; p.begin("gs", true);
    uint32_t v = p.getUInt("can", 500);
    p.end();
    { Preferences q; q.begin("gs", true); g_hb.store(q.getBool("canhb", false)); g_ack.store(q.getBool("canack", false)); q.end(); }
    g_want.store(v == 0 || v == 100 || v == 125 || v == 250 ? (int)v : 500);
    xTaskCreatePinnedToCore(can_task, "gs_can", 4096, nullptr, 2, nullptr, 0);
}

void set_speed(int kbps) {
    if (kbps != 0 && kbps != 100 && kbps != 125 && kbps != 250 && kbps != 500) return;
    Preferences p; p.begin("gs", false); p.putUInt("can", (uint32_t)kbps); p.end();
    g_want.store(kbps);
}

void set_ack(bool on) {
    Preferences p; p.begin("gs", false); p.putBool("canack", on); p.end();
    g_ack.store(on);
}
bool ack() { return g_ack.load(); }
void set_heartbeat(bool on) {
    Preferences p; p.begin("gs", false); p.putBool("canhb", on); p.end();
    g_hb.store(on);
}
bool heartbeat() { return g_hb.load(); }

int speed() { return g_want.load(); }

void brief(gs::BatteryBrief& out) {
    if (!g_mtx || !xSemaphoreTake(g_mtx, pdMS_TO_TICKS(50))) return;
    out = gs::battery_brief(g_bat, g_sep, g_sep5, ms32());
    xSemaphoreGive(g_mtx);
}

size_t api_json(char* out, size_t cap) {
    if (!g_mtx || !xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100))) return 0;
    size_t n = gs::can_to_json(g_info, g_stats, g_bat, g_sep, g_sep5, ms32(), out, cap);
    xSemaphoreGive(g_mtx);
    return n;
}

void status_line(char* out, size_t cap) {
    gs::CanDriverInfo d;
    if (g_mtx && xSemaphoreTake(g_mtx, pdMS_TO_TICKS(50))) { d = g_info; xSemaphoreGive(g_mtx); }
    const int want = g_want.load();
    if (!want) { snprintf(out, cap, "can: off"); return; }
    snprintf(out, cap, "can: %d kbit/s %s, state=%s rx=%lu missed=%lu bus_err=%lu bus_off=%lu",
             want, g_hb.load() ? "normal (ACK + heartbeat 0x305)" : g_ack.load() ? "normal (ACK)" : "listen-only", d.state, (unsigned long)d.rx_msgs, (unsigned long)d.rx_missed, (unsigned long)d.bus_errors, (unsigned long)d.bus_off_count);
}

}  // namespace canrx
