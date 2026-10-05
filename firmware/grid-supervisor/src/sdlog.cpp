#include "sdlog.h"

#include "gs_cfghist.h"

#include <SD.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cstring>
#include <new>

// Пины SD на LilyGo T-CAN485 (по схеме/примерам производителя; перед первым применением сверить с платой). Переопределяются -D.
#ifndef GS_SD_MISO
#define GS_SD_MISO 2
#endif
#ifndef GS_SD_MOSI
#define GS_SD_MOSI 15
#endif
#ifndef GS_SD_SCLK
#define GS_SD_SCLK 14
#endif
#ifndef GS_SD_CS
#define GS_SD_CS 13
#endif

namespace sdlog {

static constexpr uint32_t kSampleMs = 10000;       // период строк метрик
static constexpr uint32_t kRetryMs = 300000;       // повтор монтирования при отсутствии карты
static constexpr uint32_t kFirstTryMs = 20000;     // первая попытка после загрузки (дать Wi-Fi/HTTP стартовать)
// Буферы малы намеренно: после монтирования карты у платы остаётся мало кучи (замер: ~40 КБ свободно из ~87 КБ без карты).
static constexpr size_t kMetBuf = 2048, kEvBuf = 1024, kWork = 1536;
static constexpr uint32_t kEvRotateBytes = 1000000;
static constexpr uint32_t kMaxDays = 90, kMinFreePct = 10;

static SemaphoreHandle_t g_mtx = nullptr;          // защищает буферы
static SemaphoreHandle_t g_sd_mtx = nullptr;       // защищает обращения к карте (задача и веб)
static SPIClass g_spi(HSPI);
static gs::BatchLog* g_met = nullptr;              // создаются при первом успешном монтировании
static gs::BatchLog* g_ev = nullptr;
static char* g_work = nullptr;
static gs::RateLimit g_rate(120, 20);
static volatile bool g_mounted = false;
static uint32_t g_ev_rate_dropped = 0, g_last_day = 0, g_last_retention_ms = 0;
static char g_pre[4][200]; static uint8_t g_pre_n = 0;   // события до появления карты (BOOT и т.п.): первые 4 попадут в журнал при монтировании
static char g_err[48] = "";
static uint64_t g_total = 0, g_used = 0;
static uint32_t g_heap_mount = 0;

static void set_err(const char* e) { strncpy(g_err, e, sizeof g_err - 1); g_err[sizeof g_err - 1] = 0; }

// Имя записи каталога без пути (разные версии ядра возвращают с путём или без).
static const char* base_name(const char* n) { const char* s = strrchr(n, '/'); return s ? s + 1 : n; }

static bool mount_card() {
    const uint32_t heap0 = ESP.getFreeHeap();
    g_spi.begin(GS_SD_SCLK, GS_SD_MISO, GS_SD_MOSI, GS_SD_CS);
    if (!SD.begin(GS_SD_CS, g_spi, 4000000, "/sd", 2)) { set_err("no card"); g_spi.end(); return false; }
    if (SD.cardType() == CARD_NONE) { set_err("no card"); SD.end(); g_spi.end(); return false; }
    if (!SD.exists("/m") && !SD.mkdir("/m")) { set_err("mkdir /m failed"); SD.end(); g_spi.end(); return false; }
    if (!g_met) {   // буферы выделяются только если карта реально есть
        char* mb = new (std::nothrow) char[kMetBuf];
        char* eb = new (std::nothrow) char[kEvBuf];
        g_work = new (std::nothrow) char[kWork];
        if (!mb || !eb || !g_work) { set_err("no heap"); SD.end(); g_spi.end(); delete[] mb; delete[] eb; delete[] g_work; g_work = nullptr; return false; }
        gs::LogCfg ec; ec.flush_bytes = 1; ec.flush_ms = 0;          // события — при ближайшем проходе задачи
        gs::LogCfg mc; mc.flush_bytes = 1024; mc.flush_ms = 600000;  // порция ~1 КБ (≈3 мин) или 10 минут
        xSemaphoreTake(g_mtx, portMAX_DELAY);
        g_met = new gs::BatchLog(mb, kMetBuf, mc);
        g_ev = new gs::BatchLog(eb, kEvBuf, ec);
        for (uint8_t i = 0; i < g_pre_n; ++i) g_ev->add(0, g_pre[i], millis());
        g_pre_n = 0;
        xSemaphoreGive(g_mtx);
    }
    g_total = SD.totalBytes(); g_used = SD.usedBytes();
    set_err("");
    g_heap_mount = (heap0 > ESP.getFreeHeap()) ? heap0 - ESP.getFreeHeap() : 0;   // сколько кучи съело монтирование (с буферами)
    return true;
}

static void unmount_card(const char* why) {
    set_err(why);
    g_mounted = false;
    SD.end(); g_spi.end();
}

// Дописывает порцию в файл (создаёт и пишет заголовок, если файла нет). true — записано.
static bool append_file(const char* path, const char* header, const char* data, size_t n) {
    const bool existed = SD.exists(path);
    File f = SD.open(path, FILE_APPEND);
    if (!f) return false;
    bool ok = true;
    if (!existed && header && *header) ok = f.print(header) == strlen(header);
    if (ok) ok = f.write((const uint8_t*)data, n) == n;
    f.close();                                                     // close фиксирует данные и запись каталога
    return ok;
}

static size_t count_lines(const char* d, size_t n) { size_t c = 0; for (size_t i = 0; i < n; ++i) if (d[i] == '\n') ++c; return c; }

static void rotate_events() {
    File f = SD.open("/m/events.log", FILE_READ);
    if (!f) return;
    const uint32_t sz = f.size(); f.close();
    if (sz < kEvRotateBytes) return;
    SD.remove("/m/events.old");
    SD.rename("/m/events.log", "/m/events.old");
}

static void retention() {
    gs::LogFile* files = new (std::nothrow) gs::LogFile[200];       // в куче на время вызова (статическое ОЗУ дефицитно)
    if (!files) return;
    size_t n = 0;
    File root = SD.open("/m");
    if (!root) { delete[] files; return; }
    for (File e = root.openNextFile(); e && n < 200; e = root.openNextFile()) {
        if (!e.isDirectory()) { snprintf(files[n].name, sizeof files[n].name, "%s", base_name(e.name())); files[n].size = e.size(); ++n; }
        e.close();
    }
    root.close();
    g_total = SD.totalBytes(); g_used = SD.usedBytes();
    const uint32_t free_pct = g_total ? (uint32_t)((g_total - g_used) * 100 / g_total) : 100;
    size_t del[16];
    const size_t k = gs::select_old(files, n, kMaxDays, free_pct, kMinFreePct, del, 16);
    for (size_t i = 0; i < k; ++i) { char p[40]; snprintf(p, sizeof p, "/m/%s", files[del[i]].name); SD.remove(p); }
    delete[] files;
}

// Один проход сброса. force — принудительно (перед OTA/перезагрузкой). Вызывать с взятым g_sd_mtx.
static void flush_pass(bool force) {
    if (!g_mounted || !g_met) return;
    bool drain = force;
    for (int round = 0; round < 8 && g_mounted; ++round) {         // метрики могут лежать в нескольких сутках / не влезть в рабочий буфер
        uint32_t day = 0; size_t n;
        xSemaphoreTake(g_mtx, portMAX_DELAY);
        n = g_met->take(millis(), drain, g_work, kWork, day);
        xSemaphoreGive(g_mtx);
        if (!n) break;
        drain = true;                                              // уже решили сбрасывать: доводим остаток
        char path[32];
        if (!gs::day_path(day, path, sizeof path)) break;
        const bool ok = append_file(path, gs::kMetricsHeader, g_work, n);
        xSemaphoreTake(g_mtx, portMAX_DELAY);
        g_met->note_flush(ok, n, count_lines(g_work, n));
        xSemaphoreGive(g_mtx);
        if (!ok) { unmount_card("write failed"); return; }
        if (day != g_last_day) { g_last_day = day; retention(); g_last_retention_ms = millis(); }
    }
    if (g_ev) {
        uint32_t day = 0; size_t n;
        xSemaphoreTake(g_mtx, portMAX_DELAY);
        n = g_ev->take(millis(), force, g_work, kWork, day);
        xSemaphoreGive(g_mtx);
        if (n) {
            rotate_events();
            const bool ok = append_file("/m/events.log", "", g_work, n);
            xSemaphoreTake(g_mtx, portMAX_DELAY);
            g_ev->note_flush(ok, n, count_lines(g_work, n));
            xSemaphoreGive(g_mtx);
            if (!ok) { unmount_card("write failed"); return; }
        }
    }
    if (millis() - g_last_retention_ms > 3600000UL) { g_last_retention_ms = millis(); retention(); }   // раз в час (место/срок)
}

// ---- история конфигурации -----------------------------------------------------------------------
static const char* const kCfgFile = "/m/cfg_history.jsonl";
static constexpr uint32_t kCfgRotateBytes = 200000;
static CfgProvider g_cfg_provider = nullptr;
static uint32_t g_cfg_next_id = 0;                 // 0 — ещё не определён (сканируем файл при первом обращении)

// Построчное чтение файла (строки длиннее lcap пропускаются). fn(line, len) → false прерывает обход. true — файл открыт.
template <typename F>
static bool for_each_line(const char* path, char* lbuf, size_t lcap, F fn) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint8_t chunk[256]; size_t n = 0; bool over = false, stop = false;
    for (int r; !stop && (r = f.read(chunk, sizeof chunk)) > 0;) {
        for (int i = 0; i < r && !stop; ++i) {
            const char c = (char)chunk[i];
            if (c == '\n') { if (!over && n && !fn(lbuf, n)) stop = true; n = 0; over = false; }
            else if (n < lcap) lbuf[n++] = c;
            else over = true;
        }
    }
    f.close();
    return true;
}

static void cfg_scan_ids(char* lbuf, size_t lcap) {
    if (g_cfg_next_id) return;
    uint32_t mx = 0;
    for_each_line(kCfgFile, lbuf, lcap, [&](const char* l, size_t n) { gs::CfgHistHeader h; if (gs::cfg_hist_header(l, n, h) && h.id > mx) mx = h.id; return true; });
    g_cfg_next_id = mx + 1;
}

// Вызывать с взятым g_sd_mtx и смонтированной картой. lbuf — буфер ≥ kCfgHistLineMax.
static bool cfg_record_locked(const gs::Config& before, const gs::Config& after, int64_t epoch_s, const char* src, bool force, char* lbuf) {
    cfg_scan_ids(lbuf, gs::kCfgHistLineMax);
    const size_t n = gs::cfg_hist_line(before, after, epoch_s, g_cfg_next_id, src, force, lbuf, gs::kCfgHistLineMax);
    if (!n) return true;                                           // изменений нет — не ошибка
    File f = SD.open(kCfgFile, FILE_READ);
    if (f) { const uint32_t sz = f.size(); f.close(); if (sz > kCfgRotateBytes) { SD.remove("/m/cfg_history.old"); SD.rename(kCfgFile, "/m/cfg_history.old"); } }
    if (!append_file(kCfgFile, "", lbuf, n)) return false;
    ++g_cfg_next_id;
    return true;
}

void set_cfg_provider(CfgProvider p) { g_cfg_provider = p; }

bool cfg_record(const gs::Config& before, const gs::Config& after, int64_t epoch_s, const char* src) {
    if (!g_mounted || !g_sd_mtx) return false;
    char* lbuf = new (std::nothrow) char[gs::kCfgHistLineMax];
    if (!lbuf) return false;
    bool ok = false;
    if (xSemaphoreTake(g_sd_mtx, pdMS_TO_TICKS(3000)) == pdTRUE) {
        if (g_mounted) { ok = cfg_record_locked(before, after, epoch_s, src, false, lbuf); if (!ok) unmount_card("cfg write failed"); }
        xSemaphoreGive(g_sd_mtx);
    }
    delete[] lbuf;
    return ok;
}

bool cfg_list_json(char* out, size_t cap, size_t max_items) {
    if (!g_mounted || !g_sd_mtx || cap < 64) return false;
    char* lbuf = new (std::nothrow) char[gs::kCfgHistLineMax];
    if (!lbuf) return false;
    bool ok = false;
    if (xSemaphoreTake(g_sd_mtx, pdMS_TO_TICKS(2000)) == pdTRUE) {
        if (g_mounted) {
            size_t total = 0;
            for_each_line(kCfgFile, lbuf, gs::kCfgHistLineMax, [&](const char*, size_t) { ++total; return true; });
            const size_t skip = total > max_items ? total - max_items : 0;
            size_t idx = 0, n = (size_t)snprintf(out, cap, "{\"items\":["), emitted = 0; bool trunc = false;
            for_each_line(kCfgFile, lbuf, gs::kCfgHistLineMax, [&](const char* l, size_t len) {
                if (idx++ < skip) return true;
                char sm[640]; size_t k = gs::cfg_hist_summary(l, len, sm, sizeof sm);
                if (!k) return true;
                if (n + k + 24 >= cap) { trunc = true; return false; }
                if (emitted++) out[n++] = ',';
                memcpy(out + n, sm, k); n += k;
                return true;
            });
            n += (size_t)snprintf(out + n, cap - n, "]%s}", trunc ? ",\"truncated\":true" : "");
            ok = n < cap;
        }
        xSemaphoreGive(g_sd_mtx);
    }
    delete[] lbuf;
    return ok;
}

bool cfg_find(uint32_t id, char* line, size_t cap, size_t& len) {
    if (!g_mounted || !g_sd_mtx || cap < gs::kCfgHistLineMax) return false;
    bool found = false;
    if (xSemaphoreTake(g_sd_mtx, pdMS_TO_TICKS(2000)) == pdTRUE) {
        if (g_mounted) {
            for_each_line(kCfgFile, line, cap, [&](const char* l, size_t n) {
                gs::CfgHistHeader h;
                if (gs::cfg_hist_header(l, n, h) && h.id == id) { len = n; found = true; return false; }
                return true;
            });
        }
        xSemaphoreGive(g_sd_mtx);
    }
    return found;
}


static void task(void*) {
    uint32_t next_try = kFirstTryMs;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        xSemaphoreTake(g_sd_mtx, portMAX_DELAY);
        if (!g_mounted && (int32_t)(millis() - next_try) >= 0) {
            next_try = millis() + kRetryMs;
            g_mounted = mount_card();
            if (g_mounted) {
                g_last_retention_ms = millis(); Serial.println("SD: mounted");
                if (g_cfg_provider && !SD.exists(kCfgFile)) {          // история пуста: первая запись — исходная конфигурация (для restore «к исходному»)
                    gs::Config c; char* lb = new (std::nothrow) char[gs::kCfgHistLineMax];
                    if (lb && g_cfg_provider(c)) { time_t tt = time(nullptr); cfg_record_locked(c, c, tt > 1700000000 ? (int64_t)tt : -1, "baseline", true, lb); }
                    delete[] lb;
                }
            }
        }
        if (g_mounted) flush_pass(false);
        xSemaphoreGive(g_sd_mtx);
    }
}

void begin() {
    g_mtx = xSemaphoreCreateMutex();
    g_sd_mtx = xSemaphoreCreateMutex();
    xTaskCreate(task, "sdlog", 6144, nullptr, 1, nullptr);
}

void sample(const gs::MetricsRow& row, int64_t epoch_s, uint32_t now_ms) {
    static uint32_t next = 0;
    if (!g_met || (int32_t)(now_ms - next) < 0) return;               // без карты — ничего не накапливаем
    next = now_ms + kSampleMs;
    char line[160];
    if (!gs::format_metrics(epoch_s, row, line, sizeof line)) return;
    const uint32_t day = epoch_s > 0 ? (uint32_t)(epoch_s / 86400) : 0;
    if (xSemaphoreTake(g_mtx, 0) != pdTRUE) return;                   // цикл управления не ждёт
    g_met->add(day, line, now_ms);
    xSemaphoreGive(g_mtx);
}

void event(int64_t epoch_s, uint32_t now_ms, const char* text) {
    if (!text || !g_mtx) return;
    char line[200];
    int n = snprintf(line, sizeof line, "%lld,", (long long)(epoch_s > 0 ? epoch_s : 0));
    for (const char* p = text; *p && n < (int)sizeof line - 1; ++p) line[n++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
    line[n] = 0;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (!g_rate.allow(now_ms)) ++g_ev_rate_dropped;      // ограничитель — под тем же mutex'ом (события идут из разных задач)
    else if (g_ev) g_ev->add(0, line, now_ms);
    else if (g_pre_n < 4) { memcpy(g_pre[g_pre_n], line, (size_t)n + 1); ++g_pre_n; }   // карты ещё нет: запоминаем первые события
    xSemaphoreGive(g_mtx);
}

void flush_now() {
    if (!g_mounted || !g_sd_mtx) return;
    if (xSemaphoreTake(g_sd_mtx, pdMS_TO_TICKS(3000)) != pdTRUE) return;
    flush_pass(true);
    xSemaphoreGive(g_sd_mtx);
}

size_t status_json(char* out, size_t cap) {
    if (!out || cap < 200) return 0;
    size_t n = 0;
    auto put = [&](const char* fmt, auto... a) { if (n >= cap) return; int r = snprintf(out + n, cap - n, fmt, a...); if (r < 0 || (size_t)r >= cap - n) n = cap; else n += (size_t)r; };
    gs::LogStatsC m, e; size_t pend = 0, epend = 0;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (g_met) { m = g_met->stats(); pend = g_met->pending_bytes(); }
        if (g_ev) { e = g_ev->stats(); epend = g_ev->pending_bytes(); }
        xSemaphoreGive(g_mtx);
    }
    put("{\"mounted\":%s,\"err\":\"%s\",\"total_mb\":%lu,\"free_mb\":%lu,\"heap_mount\":%lu,\"pending\":%lu,\"ev_pending\":%lu,"
        "\"metrics\":{\"lines\":%lu,\"flushes\":%lu,\"bytes\":%lu,\"dropped\":%lu,\"errors\":%lu},"
        "\"events\":{\"lines\":%lu,\"flushes\":%lu,\"bytes\":%lu,\"dropped\":%lu,\"rate_dropped\":%lu,\"errors\":%lu},\"files\":[",
        g_mounted ? "true" : "false", g_err, (unsigned long)(g_total >> 20), (unsigned long)((g_total - g_used) >> 20), (unsigned long)g_heap_mount, (unsigned long)pend, (unsigned long)epend,
        (unsigned long)m.lines, (unsigned long)m.flushes, (unsigned long)m.bytes, (unsigned long)m.dropped, (unsigned long)m.errors,
        (unsigned long)e.lines, (unsigned long)e.flushes, (unsigned long)e.bytes, (unsigned long)e.dropped, (unsigned long)g_ev_rate_dropped, (unsigned long)e.errors);
    if (g_mounted && g_sd_mtx && xSemaphoreTake(g_sd_mtx, pdMS_TO_TICKS(1500)) == pdTRUE) {
        if (g_mounted) {
            File root = SD.open("/m");
            int k = 0;
            for (File f = root ? root.openNextFile() : File(); f && k < 60; f = root.openNextFile()) {
                if (!f.isDirectory() && gs::valid_log_name(base_name(f.name()))) { put("%s{\"n\":\"%s\",\"s\":%lu}", k ? "," : "", base_name(f.name()), (unsigned long)f.size()); ++k; }
                f.close();
            }
            if (root) root.close();
        }
        xSemaphoreGive(g_sd_mtx);
    }
    put("]}");
    return n >= cap ? 0 : n;
}

bool stream_file(WebServer& srv, const char* name) {
    if (!gs::valid_log_name(name) || !g_mounted || !g_sd_mtx) return false;
    if (xSemaphoreTake(g_sd_mtx, pdMS_TO_TICKS(3000)) != pdTRUE) return false;
    bool ok = false;
    if (g_mounted) {
        char path[32]; snprintf(path, sizeof path, "/m/%s", name);
        File f = SD.open(path, FILE_READ);
        if (f) {
            // вручную кусками (streamFile у некоторых версий ядра обрывает ответ для SD-файлов)
            srv.sendHeader("Cache-Control", "no-store");
            srv.setContentLength(CONTENT_LENGTH_UNKNOWN);                     // chunked: длина заранее не нужна
            srv.send(200, strstr(name, ".csv") ? "text/csv" : "text/plain", "");
            uint8_t* chunk = (uint8_t*)malloc(1024);
            if (chunk) {
                for (int n; (n = f.read(chunk, 1024)) > 0;) srv.sendContent((const char*)chunk, (size_t)n);
                free(chunk);
            }
            srv.sendContent("");                                                // завершающий пустой чанк
            f.close(); ok = true;
        }
    }
    xSemaphoreGive(g_sd_mtx);
    return ok;
}

}  // namespace sdlog
