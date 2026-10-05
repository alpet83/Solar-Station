// gs_log — пакетная запись логов на SD (FAT32): буфер строк, политика сброса, имена файлов, ротация, формат строки метрик.
// Чистая логика без Arduino (нативные тесты: test/native/test_gs_log.cpp). Файловая система и задача — src/sdlog.cpp.
//
// Принципы (сберечь ресурс карты и не мешать управлению):
//   * строки копятся в ОЗУ и пишутся порциями: когда накопилось >= flush_bytes (по умолчанию 4096) или самая старая
//     строка ждёт >= flush_ms (по умолчанию 10 минут), либо принудительно (перед OTA/перезагрузкой);
//   * одна порция — строки одних суток (граница суток режет порцию), файл на сутки: YYYYMMDD.csv (UTC);
//   * карты нет/запись упала — буфер переполняется «вытеснением самых старых строк», управление не страдает;
//   * события (редкие) идут в отдельный экземпляр с быстрым сбросом и ограничением частоты.
// BatchLog не потокобезопасен: владелец (src/sdlog.cpp) берёт mutex на add()/take(), а сама запись на карту идёт вне mutex.
#pragma once
#include <cstddef>
#include <cstdint>

namespace gs {

struct LogCfg {
    size_t   flush_bytes = 4096;
    uint32_t flush_ms = 600000;
};

struct LogStatsC {
    uint32_t lines = 0, dropped = 0, flushes = 0, bytes = 0, errors = 0;
};

class BatchLog {
public:
    // buf — рабочий буфер владельца (cap байт). Ничего не выделяет сам.
    BatchLog(char* buf, size_t cap, const LogCfg& c = LogCfg()) : buf_(buf), cap_(cap), cfg_(c) {}
    // Добавляет строку (без '\n': он добавляется). day — номер суток (epoch/86400), 0 = время неизвестно.
    // Не помещается — вытесняются самые старые строки. Слишком длинная строка (> cap/2) отбрасывается.
    bool add(uint32_t day, const char* line, uint32_t now_ms);
    // Если пора (или force) — копирует в out порцию строк одних суток (без служебных меток), удаляет её из буфера
    // и возвращает длину (>0), day_out — сутки порции. 0 — сбрасывать нечего/рано.
    size_t take(uint32_t now_ms, bool force, char* out, size_t out_cap, uint32_t& day_out);
    void note_flush(bool ok, size_t bytes, size_t lines);   // результат записи (для статистики)
    size_t pending_bytes() const { return len_; }
    const LogStatsC& stats() const { return st_; }
private:
    void drop_first_line();
    char* buf_; size_t cap_, len_ = 0;
    LogCfg cfg_;
    uint32_t first_ms_ = 0;
    LogStatsC st_;
};

// Ограничитель частоты (token bucket): per_hour событий в час, всплеск до burst.
class RateLimit {
public:
    RateLimit(uint32_t per_hour = 60, uint32_t burst = 10) : rate_(per_hour), burst_(burst), tokens_x1000_(burst * 1000ull) {}
    bool allow(uint32_t now_ms);
private:
    uint32_t rate_, burst_; uint64_t tokens_x1000_; uint64_t carry_ = 0; uint32_t last_ms_ = 0; bool init_ = false;
};

// ---- имена файлов ----
// Путь суточного файла метрик "/m/YYYYMMDD.csv" (UTC); day == 0 → "/m/nosync.csv". Возвращает длину или 0.
size_t day_path(uint32_t day, char* out, size_t cap);
// Имя разрешено для скачивания через веб: "YYYYMMDD.csv" (цифры), "nosync.csv", "events.log", "events.old". Без путей и "..".
bool valid_log_name(const char* name);

// ---- ротация ----
struct LogFile { char name[16]; uint32_t size = 0; };
// Выбор файлов на удаление: суточные CSV старше max_days (по имени, относительно самого нового), затем, пока свободно
// меньше min_free_pct %, самые старые суточные (но не менее 2 файлов остаётся). Возвращает число индексов в out_idx.
size_t select_old(const LogFile* files, size_t n, uint32_t max_days, uint32_t free_pct, uint32_t min_free_pct, size_t* out_idx, size_t out_cap);

// ---- строка метрик ----
extern const char kMetricsHeader[];
struct MetricsRow {
    float pack_v, alt_v, grid_v, load_w, chg_a, chg_w, pv_v;   // NaN → пустое поле
    int pv_relay, relay_grid, ongrid_reg;                      // -1 → пусто
    float bms_v, bms_soc;
    int cell_min_mv;
    MetricsRow();
};
// "epoch,pack_v,alt_v,grid_v,load_w,chg_a,chg_w,pv_v,pv_relay,relay_grid,ongrid_reg,bms_v,bms_soc,cell_min_mv" (без '\n').
size_t format_metrics(int64_t epoch_s, const MetricsRow& r, char* out, size_t cap);

}  // namespace gs
