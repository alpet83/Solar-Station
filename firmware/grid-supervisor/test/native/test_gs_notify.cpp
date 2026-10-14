#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "gs_notify.h"

using namespace gs;
static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static NotifyEvent mk(uint8_t sev, const char* tag, const char* msg, float v = NAN) {
    NotifyEvent e; e.sev = sev; notify_copy(e.tag, sizeof e.tag, tag); notify_copy(e.message, sizeof e.message, msg); e.value = v;
    return e;
}
static std::string exp(const char* tpl, const NotifyEvent& e, const char* host, bool enc, bool* ok = nullptr) {
    char out[1024]; bool r = expand_template(tpl, e, host, out, sizeof out, enc);
    if (ok) *ok = r;
    return out;
}

static void test_urlencode() {
    char o[64];
    CHECK(urlencode("AZaz09-_.~", o, sizeof o) && !strcmp(o, "AZaz09-_.~"));
    CHECK(urlencode("a b:c/d?e=f&g+h", o, sizeof o) && !strcmp(o, "a%20b%3Ac%2Fd%3Fe%3Df%26g%2Bh"));
    CHECK(urlencode("\xD0\x9F\xD1\x80", o, sizeof o) && !strcmp(o, "%D0%9F%D1%80"));      // UTF-8 побайтно
    CHECK(urlencode("", o, sizeof o) && o[0] == 0);
    CHECK(!urlencode("aaaa", o, 4) && !strcmp(o, "aaa"));                                   // не влезло: обрезано, но завершено
    CHECK(urlencode("aaa", o, 4) && !strcmp(o, "aaa"));
    CHECK(!urlencode("a b", o, 4) && !strcmp(o, "a"));                                      // %20 целиком не влезло
}

static void test_validate_template() {
    char bad[40];
    CHECK(validate_template("", bad, sizeof bad));
    CHECK(validate_template("plain text $x ${a-b} ${unclosed", bad, sizeof bad));            // как в прокси: не плейсхолдеры
    CHECK(validate_template("${host_name}${event_message}${event_value}${event_tag}${event_flags}${event_time}${fw_version}", bad, sizeof bad));
    CHECK(!validate_template("x=${event_tag}&y=${nope}&z=${bad2}", bad, sizeof bad) && !strcmp(bad, "nope"));
    CHECK(!validate_template("${}", bad, sizeof bad) && bad[0] == 0);
    CHECK(!validate_template("${Event_Tag}", bad, sizeof bad) && !strcmp(bad, "Event_Tag"));  // регистр важен
    CHECK(!validate_template("${event_tag_}", bad, sizeof bad));
    CHECK(!validate_template("${abcdefghijklmnopqrstuvwxyz_0123456789_long}", bad, 8) && strlen(bad) == 7);   // обрезка имени
    CHECK(validate_template("x", nullptr, 0));
}

static void test_validate_url() {
    char e[120];
    CHECK(validate_url("http://srv/reg_event.php?tag=${event_tag}&host=${host_name}", e, sizeof e));
    CHECK(validate_url("https://api.telegram.org/bot123:ABC/sendMessage?chat_id=1&text=${host_name}%20${event_tag}:%20${event_message}", e, sizeof e));
    CHECK(validate_url("http://192.168.1.5:8080/x", e, sizeof e));
    CHECK(!validate_url("ftp://x/y", e, sizeof e) && strstr(e, "http://"));
    CHECK(!validate_url("HTTP://x/y", e, sizeof e));                                          // как re.match в прокси: регистр схемы важен
    CHECK(!validate_url("https:/x", e, sizeof e));
    CHECK(!validate_url("http://", e, sizeof e) && strstr(e, "no host"));
    CHECK(!validate_url("http:///path", e, sizeof e) && strstr(e, "no host"));
    CHECK(!validate_url("http://x/a b", e, sizeof e) && strstr(e, "spaces"));
    CHECK(!validate_url("http://x/a\nb", e, sizeof e));
    CHECK(!validate_url("http://x/?a=${bogus}", e, sizeof e) && strstr(e, "${bogus}"));
    std::string ok = "http://h/"; ok.append(kUrlMax - ok.size(), 'a');
    CHECK(ok.size() == kUrlMax && validate_url(ok.c_str(), e, sizeof e));
    ok += "a";
    CHECK(!validate_url(ok.c_str(), e, sizeof e) && strstr(e, "too long"));
    CHECK(validate_method("get") && validate_method("POST") && validate_method("Post") && !validate_method("PUT") && !validate_method(""));
}

static void test_expand() {
    NotifyEvent ev = mk(kSevWarn, "CELL_LOW", "cell low: min 3040 mV (thr 3050)", 3040.0f);
    notify_copy(ev.flags, sizeof ev.flags, "CELL_LOW|SOFT_LOW");
    ev.epoch_s = 1790000000; ev.uptime_ms = 123456;
    CHECK(exp("${host_name}", ev, "gs-1", true) == "gs-1");
    CHECK(exp("${event_tag}", ev, "h", true) == "CELL_LOW");
    CHECK(exp("[${fw_version}]", ev, "h", true) == "[]");              // до старта версия пуста
    notify_set_fw_version("0.1v-abc1234");
    CHECK(exp("v=${fw_version}", ev, "h", true) == "v%3D0.1v-abc1234" || exp("v=${fw_version}", ev, "h", true) == "v=0.1v-abc1234");
    notify_set_fw_version("");
    CHECK(exp("${event_value}", ev, "h", true) == "3040.00");
    CHECK(exp("${event_time}", ev, "h", false) == "2026-09-21 14:13:20");
    CHECK(exp("${event_time}", ev, "h", true) == "2026-09-21%2014%3A13%3A20");
    CHECK(exp("${event_flags}", ev, "h", true) == "CELL_LOW%7CSOFT_LOW");
    CHECK(exp("t=${event_tag}&m=${event_message}", ev, "h", true) == "t=CELL_LOW&m=cell%20low%3A%20min%203040%20mV%20%28thr%203050%29");
    CHECK(exp("${event_message}", ev, "h", false) == "cell low: min 3040 mV (thr 3050)");     // без кодирования — как есть
    CHECK(exp("a${host_name}b${host_name}c", ev, "H", true) == "aHbHc");
    CHECK(exp("100%${event_tag}$${event_tag}${x", ev, "h", true, nullptr) == "100%CELL_LOW$CELL_LOW${x");   // % и $ вне плейсхолдера — текст
    ev.value = NAN;
    CHECK(exp("v=${event_value}.", ev, "h", true) == "v=.");
    ev.value = INFINITY;
    CHECK(exp("v=${event_value}.", ev, "h", true) == "v=.");
    ev.value = -0.004f;
    CHECK(exp("${event_value}", ev, "h", true) == "-0.00");
    ev.value = 52.456f;
    CHECK(exp("${event_value}", ev, "h", true) == "52.46");
    ev.epoch_s = -1; ev.uptime_ms = 125999;
    CHECK(exp("${event_time}", ev, "h", false) == "uptime+125s");
    CHECK(exp("${event_time}", ev, "h", true) == "uptime%2B125s");
    bool ok = true;
    CHECK(exp("a${nope}b", ev, "h", true, &ok) == "a" && !ok);                                // неизвестное имя -> ошибка
    char small[16]; CHECK(!expand_template("0123456789abcdefXYZ", ev, "h", small, sizeof small, true) && strlen(small) == 15);
    CHECK(!expand_template("${event_message}", mk(1, "T", "very long message very long message"), "h", small, sizeof small, false) && strlen(small) < 16);
    CHECK(expand_template("", ev, "h", small, sizeof small, true) && small[0] == 0);
    CHECK(!expand_template("x", ev, "h", small, 0, true));
    // худший случай: сообщение 95 символов из спецсимволов -> 285 символов в URL-кодировании
    NotifyEvent big = mk(2, "T", std::string(95, '"').c_str());
    CHECK(exp("u=${event_message}&v=${event_message}", big, "h", true, &ok).size() == 2 + 285 + 3 + 285 && ok);
    // host == nullptr не падает
    CHECK(exp("[${host_name}]", ev, nullptr, true) == "[]");
}

static void test_origin() {
    char o[100];
    url_origin("https://api.telegram.org/bot123:SECRET/sendMessage?chat_id=1", o, sizeof o); CHECK(!strcmp(o, "https://api.telegram.org"));
    url_origin("http://user:pw@host.lan:8080/p?q=1#f", o, sizeof o); CHECK(!strcmp(o, "http://host.lan:8080"));
    url_origin("http://host.lan?token=abc", o, sizeof o); CHECK(!strcmp(o, "http://host.lan"));
    url_origin("https://h/x", o, sizeof o, false); CHECK(!strcmp(o, "h"));
    url_origin("garbage", o, sizeof o); CHECK(o[0] == 0);
    url_origin("https://api.telegram.org/x", o, 12); CHECK(!strcmp(o, "https://api"));
    CHECK(!strstr(o, "bot"));
}

static void test_cfg() {
    WebhookCfg c; char e[160];
    CHECK(!c.enabled && !strcmp(c.method, "GET") && c.timeout_s == 10 && c.min_sev == kSevWarn && c.insecure && !strcmp(c.ctype, kDefaultCtype));
    CHECK(!webhook_set(c, "en", "1", e, sizeof e) && !strcmp(e, "set url first") && !c.enabled);
    CHECK(webhook_set(c, "url", "https://api.telegram.org/botX/sendMessage?chat_id=1&text=${event_tag}", e, sizeof e));
    CHECK(webhook_set(c, "en", "on", e, sizeof e) && c.enabled);
    CHECK(!webhook_set(c, "url", "ftp://x", e, sizeof e) && c.enabled && strstr(c.url, "telegram"));   // при ошибке без изменений
    CHECK(!webhook_set(c, "url", "http://x/${zzz}", e, sizeof e) && strstr(e, "${zzz}"));
    CHECK(webhook_set(c, "method", "post", e, sizeof e) && !strcmp(c.method, "POST"));
    CHECK(!webhook_set(c, "method", "put", e, sizeof e) && !strcmp(c.method, "POST"));
    CHECK(webhook_set(c, "body", "chat_id=1&text=${host_name} ${event_message}", e, sizeof e) && strstr(c.body, "chat_id"));
    CHECK(!webhook_set(c, "body", "x=${unknown}", e, sizeof e) && strstr(e, "${unknown}") && strstr(c.body, "chat_id"));
    CHECK(!webhook_set(c, "body", std::string(401, 'x').c_str(), e, sizeof e));
    CHECK(webhook_set(c, "body", std::string(400, 'x').c_str(), e, sizeof e) && strlen(c.body) == 400);
    CHECK(webhook_set(c, "body", "", e, sizeof e) && !c.body[0]);
    CHECK(webhook_set(c, "ctype", "application/json", e, sizeof e) && !strcmp(c.ctype, "application/json"));
    CHECK(!webhook_set(c, "ctype", "text/plain\r\nX-Evil: 1", e, sizeof e) && !strcmp(c.ctype, "application/json"));   // инъекция заголовка
    CHECK(webhook_set(c, "ctype", "", e, sizeof e) && !strcmp(c.ctype, kDefaultCtype));
    CHECK(webhook_set(c, "err", "#ERROR", e, sizeof e) && !strcmp(c.err, "#ERROR"));
    CHECK(!webhook_set(c, "err", "a\nb", e, sizeof e));
    CHECK(!webhook_set(c, "err", std::string(49, 'x').c_str(), e, sizeof e));
    CHECK(webhook_set(c, "err", "", e, sizeof e) && !c.err[0]);
    CHECK(webhook_set(c, "tmo", "1", e, sizeof e) && c.timeout_s == 1);
    CHECK(webhook_set(c, "tmo", "60", e, sizeof e) && c.timeout_s == 60);
    CHECK(!webhook_set(c, "tmo", "0", e, sizeof e) && !webhook_set(c, "tmo", "61", e, sizeof e) && !webhook_set(c, "tmo", "x", e, sizeof e) && !webhook_set(c, "tmo", "", e, sizeof e) && !webhook_set(c, "tmo", "5s", e, sizeof e) && c.timeout_s == 60);
    CHECK(webhook_set(c, "min", "info", e, sizeof e) && c.min_sev == 0 && webhook_set(c, "min", "ALERT", e, sizeof e) && c.min_sev == 2 && webhook_set(c, "min", "1", e, sizeof e) && c.min_sev == 1);
    CHECK(!webhook_set(c, "min", "3", e, sizeof e) && !webhook_set(c, "min", "high", e, sizeof e) && c.min_sev == 1);
    CHECK(webhook_set(c, "insecure", "0", e, sizeof e) && !c.insecure && webhook_set(c, "insecure", "on", e, sizeof e) && c.insecure);
    CHECK(!webhook_set(c, "insecure", "maybe", e, sizeof e));
    CHECK(!webhook_set(c, "nope", "1", e, sizeof e) && strstr(e, "unknown key"));
    CHECK(webhook_set(c, "url", "", e, sizeof e) && !c.url[0] && !c.enabled);                 // очистка URL выключает
    CHECK(!webhook_set(c, "en", "yes please", e, sizeof e));
    // webhook_get: круговой обход через все ключи
    WebhookCfg a; webhook_set(a, "url", "http://h/p?x=${event_tag}", e, sizeof e); webhook_set(a, "method", "POST", e, sizeof e);
    webhook_set(a, "body", "b=${event_message}", e, sizeof e); webhook_set(a, "ctype", "text/plain", e, sizeof e); webhook_set(a, "err", "FAIL", e, sizeof e);
    webhook_set(a, "tmo", "7", e, sizeof e); webhook_set(a, "min", "alert", e, sizeof e); webhook_set(a, "insecure", "0", e, sizeof e); webhook_set(a, "en", "1", e, sizeof e);
    WebhookCfg b;
    for (size_t i = 0; i < kWebhookKeyCount; ++i) {
        char v[512]; CHECK(webhook_get(a, kWebhookKeys[i], v, sizeof v));
        CHECK(webhook_set(b, kWebhookKeys[i], v, e, sizeof e));
    }
    CHECK(b.enabled && !strcmp(b.url, a.url) && !strcmp(b.method, "POST") && !strcmp(b.body, a.body) && !strcmp(b.ctype, "text/plain") &&
          !strcmp(b.err, "FAIL") && b.timeout_s == 7 && b.min_sev == 2 && !b.insecure);
    char v[8]; CHECK(!webhook_get(a, "nope", v, sizeof v) && v[0] == 0);
    CHECK(!strcmp(kWebhookKeys[kWebhookKeyCount - 1], "en"));      // en последним: проверяет url
}

// ---- очередь --------------------------------------------------------------------------------------------------
static void test_filter_dedup() {
    NotifyQueue q;
    CHECK(!q.push(mk(kSevInfo, "BOOT", "b"), 1000) && q.stats().filtered == 1 && q.size() == 0);           // по умолчанию min = warn
    CHECK(q.push(mk(kSevWarn, "A", "m"), 1000) && q.size() == 1);
    CHECK(!q.push(mk(kSevWarn, "A", "m"), 1000 + 59999) && q.stats().suppressed == 1);                       // повтор внутри 60 с
    CHECK(q.push(mk(kSevWarn, "A", "m2"), 1100) && q.push(mk(kSevWarn, "B", "m"), 1100));                    // другая комбинация — сразу
    CHECK(q.push(mk(kSevWarn, "A", "m"), 1000 + 60000) && q.size() == 4);                                    // окно истекло
    CHECK(!q.push(mk(kSevAlert, "A", "m"), 1000 + 60001) && q.stats().suppressed == 2);                      // подавление не зависит от sev
    // tag+message: перестановка не склеивается ("AB"+"C" != "A"+"BC")
    CHECK(q.push(mk(kSevWarn, "AB", "C"), 70000) && q.push(mk(kSevWarn, "A", "BC"), 70000));
    // force обходит всё
    NotifyQueue f;
    CHECK(f.push(mk(kSevInfo, "TEST", "t"), 0, true) && f.push(mk(kSevInfo, "TEST", "t"), 1, true) && f.size() == 2 && f.stats().filtered == 0 && f.stats().suppressed == 0);
    // min_sev = alert
    NotifyPolicy p; p.min_sev = kSevAlert; NotifyQueue a(p);
    CHECK(!a.push(mk(kSevWarn, "W", "w"), 0) && a.push(mk(kSevAlert, "X", "x"), 0));
    p.min_sev = kSevInfo; a.set_policy(p); CHECK(a.push(mk(kSevInfo, "I", "i"), 0));
    // dedup_ms = 0 выключает подавление
    p.dedup_ms = 0; NotifyQueue n(p);
    CHECK(n.push(mk(1, "A", "m"), 0) && n.push(mk(1, "A", "m"), 0) && n.stats().suppressed == 0);
    // больше kSeenCap разных ключей: самая старая запись вытесняется, подавление работает для свежих
    NotifyQueue s;
    char m[16];
    for (int i = 0; i < 20; ++i) { snprintf(m, sizeof m, "m%d", i); CHECK(s.push(mk(1, "T", m), (uint32_t)(i * 10))); }
    CHECK(!s.push(mk(1, "T", "m19"), 300) && s.stats().suppressed == 1);
    CHECK(s.push(mk(1, "T", "m0"), 300));         // самая первая уже забыта (кольцо повторов на 16)
}

static void test_ring_and_ids() {
    NotifyQueue q;
    for (int i = 0; i < 16; ++i) { char m[16]; snprintf(m, sizeof m, "m%d", i); CHECK(q.push(mk(1, "T", m), 0)); }
    CHECK(q.size() == 16 && q.stats().dropped == 0);
    CHECK(q.push(mk(1, "T", "m16"), 0) && q.size() == 16 && q.stats().dropped == 1);                         // вытеснение самого старого
    CHECK(q.push(mk(1, "T", "m17"), 0) && q.stats().dropped == 2);
    NotifyEvent e;
    CHECK(q.take(1000, e) && !strcmp(e.message, "m2") && e.id == 3);                                         // m0, m1 вытеснены; id сквозные с 1
    q.done(true, 1000, 200, "");
    CHECK(q.take(1000, e) && !strcmp(e.message, "m3") && e.id == 4);
    q.note_lost(); CHECK(q.stats().dropped == 3);
}

static void test_rate_limit() {
    NotifyQueue q; NotifyEvent e;
    for (int i = 0; i < 15; ++i) { char m[16]; snprintf(m, sizeof m, "m%d", i); q.push(mk(1, "T", m), 0); }
    int got = 0;
    uint32_t t = 100000;
    for (; got < 15; ++got) { if (!q.take(t, e)) break; q.done(true, t, 200, ""); t += 1000; }          // по 1 событию в секунду
    CHECK(got == 10 && q.size() == 5);                                                                      // 11-е ждёт: 10 попыток за последние 60 с
    CHECK(!q.take(t + 40000, e));                                                                           // окно ещё не освободилось (t0 + 60 с = 160000, t = 110000)
    CHECK(!q.take(100000 + 59999, e));
    CHECK(q.take(100000 + 60000, e) && !strcmp(e.message, "m10"));                                          // освободилась первая попытка
    q.done(true, 160000, 200, "");
    CHECK(!q.take(160000, e));                                                                              // вторая освободится в 161000
    CHECK(q.take(161000, e) && !strcmp(e.message, "m11"));
    q.done(true, 161000, 200, ""); CHECK(q.stats().sent == 12);
    CHECK(q.stats().dropped == 0);                                                                          // остаток не теряется
    // кастомный лимит
    NotifyPolicy p; p.rate_max = 2; p.rate_window_ms = 1000; NotifyQueue r(p);
    for (int i = 0; i < 4; ++i) { char m[16]; snprintf(m, sizeof m, "x%d", i); r.push(mk(1, "T", m), 0); }
    CHECK(r.take(0, e)); r.done(true, 0, 200, ""); CHECK(r.take(0, e)); r.done(true, 0, 200, ""); CHECK(!r.take(999, e) && r.take(1000, e));
}

static void test_retry_backoff() {
    NotifyQueue q; NotifyEvent e;
    q.push(mk(1, "A", "first"), 0); q.push(mk(1, "B", "second"), 0);
    CHECK(q.take(1000, e) && !strcmp(e.message, "first") && q.inflight());
    CHECK(!q.take(1000, e));                                                                                // пока отправка не завершена — ничего
    CHECK(q.size() == 2);                                                                                   // выданное + одно в кольце
    q.done(false, 1000, 503, "HTTP 503");
    CHECK(!q.inflight() && q.size() == 2 && q.stats().failed == 0 && q.stats().last_http == 503 && !q.stats().last_ok && !strcmp(q.stats().last_error, "HTTP 503"));
    CHECK(!q.take(1000 + 4999, e));                                                                         // backoff 5 с
    CHECK(q.take(1000 + 5000, e) && !strcmp(e.message, "first") && e.id == 1);                              // то же событие, порядок сохранён
    q.done(false, 6000, -1, "connection refused");
    CHECK(q.stats().last_http == -1 && !strcmp(q.stats().last_error, "connection refused"));
    CHECK(!q.take(6000 + 29999, e));                                                                        // backoff 30 с
    CHECK(q.take(6000 + 30000, e) && !strcmp(e.message, "first"));
    q.done(false, 36000, 500, "HTTP 500");                                                                  // третья неудача: отбрасываем
    CHECK(q.stats().failed == 1 && q.stats().sent == 0 && q.size() == 1);
    CHECK(!q.take(36000 + 119999, e));                                                                      // пауза 120 с перед следующим событием
    CHECK(q.take(36000 + 120000, e) && !strcmp(e.message, "second") && e.id == 2);
    q.done(true, 156000, 200, "");
    CHECK(q.stats().sent == 1 && q.stats().last_ok && q.stats().last_error[0] == 0 && q.size() == 0);
    // успех после одной неудачи сбрасывает счётчик попыток
    NotifyQueue r;
    r.push(mk(1, "A", "x"), 0); r.push(mk(1, "A", "y"), 0);
    CHECK(r.take(0, e)); r.done(false, 0, 0, "timeout"); CHECK(r.take(5000, e)); r.done(true, 5000, 204, "");
    CHECK(r.take(5000, e) && !strcmp(e.message, "y")); r.done(false, 5000, 0, ""); CHECK(!r.take(5000 + 4999, e) && r.take(10000, e));
    CHECK(!strcmp(r.stats().last_error, "send failed"));                                                    // пустая причина заменяется
    // событие, пришедшее во время ожидания повтора, встаёт в очередь и не теряется
    r.done(false, 10000, 0, "x"); r.push(mk(1, "C", "z"), 10001); CHECK(r.size() == 2);
    // done без take игнорируется
    NotifyQueue d; d.done(true, 0, 200, ""); CHECK(d.stats().sent == 0 && !d.stats().have_last);
    CHECK(q.stats().have_last);
}

static void test_wraparound() {
    // uptime около переполнения uint32 (49 дней): сравнения времени безопасны
    NotifyQueue q; NotifyEvent e;
    const uint32_t t0 = 0xFFFFFFFFu - 2000;
    CHECK(q.push(mk(1, "A", "m"), t0));
    CHECK(!q.push(mk(1, "A", "m"), t0 + 30000));                                                            // 28 с после перескока: ещё подавляется
    CHECK(q.push(mk(1, "A", "m"), t0 + 60000));
    CHECK(q.take(t0, e)); q.done(false, t0, 0, "x");
    CHECK(!q.take(t0 + 4999, e) && q.take(t0 + 5000, e));                                                   // повтор через перескок
}

// ---- преобразования ---------------------------------------------------------------------------------------------
static void test_from_trigger() {
    Trigger t; t.kind = TrigKind::CellLowEnter; t.t_ms = 100000; t.value = 3040; t.aux = 3300;
    strcpy(t.text, "cell low: min 3040 mV (thr 3050), max 3300");
    NotifyEvent e = notify_from_trigger(t, 110000, 1790000000);
    CHECK(e.sev == kSevWarn && !strcmp(e.tag, "CELL_LOW") && !strcmp(e.message, t.text) && e.value == 3040.0f && e.flags[0] == 0);
    CHECK(e.epoch_s == 1790000000 - 10 && e.uptime_ms == 100000);
    CHECK(notify_from_trigger(t, 110000, -1).epoch_s == -1);
    CHECK(notify_from_trigger(t, 90000, 1790000000).epoch_s == 1790000000);                                  // «будущее» не уводит время назад
    // все виды: тег и серьёзность те же, что у gs_trig, и тег влезает без обрезки
    for (int k = 0; k < (int)TrigKind::Count_; ++k) {
        Trigger x; x.kind = (TrigKind)k; strcpy(x.text, "t");
        NotifyEvent y = notify_from_trigger(x, 0, -1);
        CHECK(!strcmp(y.tag, trig_tag(x.kind)) && y.sev == (uint8_t)trig_severity(x.kind) && strlen(trig_tag(x.kind)) <= 15);
    }
    Trigger f; f.kind = TrigKind::BmsFlagsChanged; f.value = 2; f.aux = (3u << 8) | 0x05; strcpy(f.text, "f322 01 02 -> 01 09");
    e = notify_from_trigger(f, 0, -1);
    CHECK(e.sev == kSevWarn && !std::isfinite(e.value) && !strcmp(e.flags, "f3 mask=0x05") && !strcmp(e.tag, "BMS_FLAGS"));
    Trigger s; s.kind = TrigKind::BmsStale; strcpy(s.text, "no BMS frames for >10 s");
    CHECK(!std::isfinite(notify_from_trigger(s, 0, -1).value));
    Trigger b; b.kind = TrigKind::BmsBack; b.value = 42; CHECK(notify_from_trigger(b, 0, -1).value == 42.0f && notify_from_trigger(b, 0, -1).sev == kSevInfo);
    Trigger h; h.kind = TrigKind::CellHardEnter; CHECK(notify_from_trigger(h, 0, -1).sev == kSevAlert);
    // текст триггера 47 символов целиком помещается в message[96]
    Trigger l; l.kind = TrigKind::BmsFlagsChanged; memset(l.text, 'x', 47); l.text[47] = 0;
    CHECK(strlen(notify_from_trigger(l, 0, -1).message) == 47);
}

static std::vector<NotifyEvent> flags(SupervisorNotifier& sn, uint32_t f, float v = 50.0f) {
    NotifyEvent out[24]; size_t n = sn.on_flags(f, v, 5000, 1790000000, out, 24);
    return std::vector<NotifyEvent>(out, out + n);
}

static void test_supervisor_flags() {
    SupervisorNotifier sn;
    CHECK(flags(sn, 0).empty());
    CHECK(flags(sn, F_HOLD | F_OFF_COND | F_STALE | F_MANUAL_PAUSE | F_CMD_PENDING).empty());              // ненаблюдаемые флаги — без событий
    auto v = flags(sn, F_HARD_LOW | F_HOLD, 47.5f);
    CHECK(v.size() == 1 && !strcmp(v[0].tag, "HARD_LOW") && v[0].sev == kSevAlert && v[0].value == 47.5f && !strcmp(v[0].flags, "HARD_LOW|HOLD") && v[0].epoch_s == 1790000000 && v[0].uptime_ms == 5000);
    CHECK(flags(sn, F_HARD_LOW).empty() && flags(sn, F_HARD_LOW | F_SOFT_LOW).size() == 1);                // фронт: только новое
    v = flags(sn, F_HARD_LOW | F_SOFT_LOW);  CHECK(v.empty());
    v = flags(sn, F_SOFT_LOW);
    CHECK(v.size() == 1 && !strcmp(v[0].tag, "HARD_LOW_OK") && v[0].sev == kSevInfo);                      // выход — info *_OK
    v = flags(sn, 0);
    CHECK(v.size() == 1 && !strcmp(v[0].tag, "SOFT_LOW_OK") && v[0].sev == kSevInfo);
    // все девять: вход и выход, теги влезают (<= 15) и различны, серьёзность по ТЗ
    struct E { uint32_t bit; const char* tag; const char* ok; uint8_t sev; } exp9[] = {
        {F_HARD_LOW, "HARD_LOW", "HARD_LOW_OK", kSevAlert}, {F_CELL_HARD, "CELL_HARD", "CELL_HARD_OK", kSevAlert}, {F_NO_DATA, "NO_DATA", "NO_DATA_OK", kSevAlert},
        {F_GRID_LOST_LOW, "GRID_LOST_LOW", "GRID_LOST_OK", kSevAlert}, {F_SAFE_MODE, "SAFE_MODE", "SAFE_MODE_OK", kSevAlert}, {F_CELL_BLOCK, "CELL_BLOCK", "CELL_BLOCK_OK", kSevWarn},
        {F_RELAY_MISMATCH, "RELAY_MISMATCH", "RELAY_MISM_OK", kSevWarn}, {F_CELL_LOW, "CELL_LOW", "CELL_LOW_OK", kSevWarn}, {F_SOFT_LOW, "SOFT_LOW", "SOFT_LOW_OK", kSevWarn}};
    for (auto& x : exp9) {
        SupervisorNotifier s;
        auto in = flags(s, x.bit); auto out = flags(s, 0);
        CHECK(in.size() == 1 && !strcmp(in[0].tag, x.tag) && in[0].sev == x.sev && strstr(in[0].message, "supervisor:") && strlen(in[0].message) < 95);
        CHECK(out.size() == 1 && !strcmp(out[0].tag, x.ok) && out[0].sev == kSevInfo && strstr(out[0].message, "supervisor:"));
        CHECK(strlen(x.tag) <= 15 && strlen(x.ok) <= 15);
        CHECK(strcmp(in[0].message, out[0].message) != 0);
    }
    // несколько фронтов за один шаг, сразу вход и выход
    SupervisorNotifier m;
    v = flags(m, F_NO_DATA | F_CELL_LOW | F_SOFT_LOW);
    CHECK(v.size() == 3);
    v = flags(m, F_NO_DATA | F_CELL_BLOCK);
    CHECK(v.size() == 3);                                                                                    // CELL_BLOCK вход, CELL_LOW_OK, SOFT_LOW_OK
    // дребезг: каждый фронт — событие (подавление — дело очереди)
    SupervisorNotifier d;
    size_t cnt = 0; for (int i = 0; i < 6; ++i) cnt += flags(d, (i & 1) ? 0u : (uint32_t)F_SOFT_LOW).size();
    CHECK(cnt == 6);
    // SAFE_MODE с первого шага после старта
    SupervisorNotifier sm; v = flags(sm, F_SAFE_MODE); CHECK(v.size() == 1 && v[0].sev == kSevAlert);
    // cap меньше числа фронтов: остаток придёт в следующем вызове
    SupervisorNotifier c; NotifyEvent o[2];
    CHECK(c.on_flags(F_HARD_LOW | F_NO_DATA | F_SOFT_LOW, 0, 0, -1, o, 2) == 2);
    CHECK(c.on_flags(F_HARD_LOW | F_NO_DATA | F_SOFT_LOW, 0, 0, -1, o, 2) == 1 && !strcmp(o[0].tag, "SOFT_LOW"));
    CHECK(c.on_flags(F_HARD_LOW | F_NO_DATA | F_SOFT_LOW, 0, 0, -1, o, 2) == 0);
    CHECK(c.on_flags(0, 0, 0, -1, nullptr, 0) == 0);
    c.reset(); CHECK(c.prev_flags() == 0);
    // NaN-напряжение: событие без значения
    SupervisorNotifier nn; NotifyEvent o1[4]; nn.on_flags(F_NO_DATA, NAN, 0, -1, o1, 4); CHECK(!std::isfinite(o1[0].value));
}

static void test_flags_text() {
    char b[64];
    flags_text(0, b, sizeof b); CHECK(b[0] == 0);
    flags_text(F_HARD_LOW | F_CELL_BLOCK | F_TIME_JUMP, b, sizeof b); CHECK(!strcmp(b, "HARD_LOW|TIME_JUMP|CELL_BLOCK"));
    flags_text(0xFFFFFFFFu, b, sizeof b); CHECK(strlen(b) < sizeof b && strlen(b) > 40 && b[strlen(b) - 1] != '|');   // обрезается по целым именам
    flags_text(F_HARD_LOW, b, 5); CHECK(b[0] == 0);
    flags_text(F_HARD_LOW, b, 9); CHECK(!strcmp(b, "HARD_LOW"));
}

static void test_commands_boot() {
    NotifyEvent e;
    CHECK(!SupervisorNotifier::on_command(Cmd::None, Reason::None, true, 0, 0, -1, e));
    struct R { Reason r; const char* n; uint8_t sev; } rs[] = {{Reason::HardFloor, "HARD_FLOOR", kSevAlert}, {Reason::NoData, "NO_DATA", kSevAlert}, {Reason::SafeMode, "SAFE_MODE", kSevAlert},
        {Reason::SoftLow, "SOFT_LOW", kSevWarn}, {Reason::Charged, "CHARGED", kSevWarn}, {Reason::Manual, "MANUAL", kSevWarn}};
    for (auto& r : rs) {
        CHECK(SupervisorNotifier::on_command(Cmd::Connect, r.r, true, 47.25f, 7000, 1790000000, e));
        CHECK(!strcmp(e.tag, "CONNECT") && e.sev == r.sev && strstr(e.message, r.n) && e.value == 47.25f && e.uptime_ms == 7000 && e.epoch_s == 1790000000);
        CHECK(SupervisorNotifier::on_command(Cmd::Disconnect, r.r, true, 53.0f, 0, -1, e) && !strcmp(e.tag, "DISCONNECT") && e.sev == r.sev);
    }
    SupervisorNotifier::on_command(Cmd::Connect, Reason::HardFloor, true, 47.25f, 0, -1, e);
    CHECK(!strcmp(e.message, "grid CONNECT: reason=HARD_FLOOR, 47.2 V") || !strcmp(e.message, "grid CONNECT: reason=HARD_FLOOR, 47.3 V"));
    SupervisorNotifier::on_command(Cmd::Disconnect, Reason::Charged, false, NAN, 0, -1, e);
    CHECK(!strcmp(e.message, "grid DISCONNECT: reason=CHARGED, bus write FAILED") && !std::isfinite(e.value));
    CHECK(!strcmp(notify_reason_name(Reason::None), "NONE"));
    e = SupervisorNotifier::boot("abc1234", "SW", 1500, -1);
    CHECK(e.sev == kSevInfo && !strcmp(e.tag, "BOOT") && !strcmp(e.message, "boot ver=abc1234 reset=SW") && e.uptime_ms == 1500 && e.epoch_s == -1 && !std::isfinite(e.value));
    e = SupervisorNotifier::boot("", nullptr, 0, 5); CHECK(!strcmp(e.message, "boot ver=? reset=?"));
    e = SupervisorNotifier::boot(std::string(200, 'r').c_str(), "PANIC", 0, 5); CHECK(strlen(e.message) == 95);
    // событие BOOT проходит фильтр только при min=info
    NotifyQueue q; CHECK(!q.push(SupervisorNotifier::boot("r", "POWERON", 0, -1), 0));
    NotifyPolicy p; p.min_sev = kSevInfo; q.set_policy(p); CHECK(q.push(SupervisorNotifier::boot("r", "POWERON", 0, -1), 0));
}

static void test_end_to_end() {
    // шаблон Telegram GET, как в README: URL раскрывается, тег/сообщение кодируются
    NotifyEvent e = mk(kSevAlert, "CELL_HARD", "cell HARD: min 2890 mV (thr 2900), max 3300", 2890);
    char url[1024];
    CHECK(expand_template("https://api.telegram.org/bot1:T/sendMessage?chat_id=7&text=${host_name}%20${event_tag}:%20${event_message}", e, "gs-home", url, sizeof url, true));
    CHECK(!strcmp(url, "https://api.telegram.org/bot1:T/sendMessage?chat_id=7&text=gs-home%20CELL_HARD:%20cell%20HARD%3A%20min%202890%20mV%20%28thr%202900%29%2C%20max%203300"));
}

int main() {
    test_urlencode(); test_validate_template(); test_validate_url(); test_expand(); test_origin(); test_cfg();
    test_filter_dedup(); test_ring_and_ids(); test_rate_limit(); test_retry_backoff(); test_wraparound();
    test_from_trigger(); test_supervisor_flags(); test_flags_text(); test_commands_boot(); test_end_to_end();
    printf("test_gs_notify: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
