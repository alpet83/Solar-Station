#include "stats.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "gs_stats.h"

namespace stats {

static gs::PvStats g_pv;
static SemaphoreHandle_t g_mtx = nullptr;

void begin() { if (!g_mtx) g_mtx = xSemaphoreCreateMutex(); }

void feed(const gs::Telemetry& t, uint32_t now_ms, int64_t epoch_s, float pack_v) {
    if (!g_mtx || !t.chg_ok) return;
    gs::PvSample s;
    s.t_ms = now_ms; s.epoch_s = epoch_s; s.pv_v = t.pv_v; s.chg_a = t.chg_curr_a; s.chg_w = t.chg_w; s.pack_v = pack_v; s.pv_relay = t.pv_relay;
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    g_pv.feed(s);
    xSemaphoreGive(g_mtx);
}

size_t json(char* buf, size_t cap) {
    if (!g_mtx) return 0;
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    size_t n = g_pv.to_json((uint32_t)millis(), buf, cap);
    xSemaphoreGive(g_mtx);
    return n;
}

}  // namespace stats
