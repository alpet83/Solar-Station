#include "gs_cli.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace gs {

namespace {

const int kMaxTok = 5;
const size_t kTokCap = 80;

struct Tok { char s[kTokCap]; bool quoted; };

// 0 — ок, иначе код ошибки: 1 не закрыта кавычка, 2 токен слишком длинный, 3 слишком много токенов
int tokenize(const char* p, Tok* t, int& n) {
    n = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (n >= kMaxTok) return 3;
        size_t k = 0;
        Tok& tk = t[n];
        tk.quoted = false;
        if (*p == '"') {
            tk.quoted = true; ++p;
            for (;;) {
                if (!*p) return 1;
                if (*p == '"') { ++p; break; }
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) ++p;
                if (k + 1 >= kTokCap) return 2;
                tk.s[k++] = *p++;
            }
        } else {
            while (*p && *p != ' ' && *p != '\t') {
                if (k + 1 >= kTokCap) return 2;
                tk.s[k++] = *p++;
            }
        }
        tk.s[k] = 0;
        ++n;
    }
    return 0;
}

bool ieq(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

bool to_long(const char* s, long& v) {
    if (!*s) return false;
    char* e = nullptr;
    v = strtol(s, &e, 10);
    return e && *e == 0;
}

CliCmd fail(const char* msg) {
    CliCmd c; c.kind = CliKind::Error;
    strncpy(c.err, msg, sizeof c.err - 1);
    return c;
}

const size_t kWhValueMax = 400;

// Остаток строки как значение: "в кавычках" (с \" и \\) или как есть до конца строки (хвостовые пробелы отброшены).
// 0 — ок, иначе текст ошибки через msg.
bool parse_rest_value(const char* p, char* out, const char*& msg) {
    while (*p == ' ' || *p == '\t') ++p;
    size_t k = 0;
    if (*p == '"') {
        ++p;
        for (;;) {
            if (!*p) { msg = "unterminated quote"; return false; }
            if (*p == '"') { ++p; break; }
            if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) ++p;
            if (k >= kWhValueMax) { msg = "value too long (max 400)"; return false; }
            out[k++] = *p++;
        }
        while (*p == ' ' || *p == '\t') ++p;
        if (*p) { msg = "unexpected text after closing quote"; return false; }
    } else {
        size_t n = strlen(p);
        while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) --n;
        if (n > kWhValueMax) { msg = "value too long (max 400)"; return false; }
        memcpy(out, p, n); k = n;
    }
    out[k] = 0;
    return true;
}

// p — строка после слова "webhook".
CliCmd parse_webhook(const char* p) {
    CliCmd c; c.kind = CliKind::Webhook;
    static const char* usage = "usage: webhook [url <u>|method get|post|body <t>|ctype <t>|err <s>|timeout <1..60>|min info|warn|alert|insecure on|off|on|off|test]";
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p) { c.flag = 0; return c; }
    char sub[16]; size_t k = 0;
    while (*p && *p != ' ' && *p != '\t') { if (k + 1 >= sizeof sub) return fail(usage); sub[k++] = *p++; }
    sub[k] = 0;
    const char* rest = p;
    while (*rest == ' ' || *rest == '\t') ++rest;
    if (ieq(sub, "status") || ieq(sub, "test") || ieq(sub, "on") || ieq(sub, "off")) {
        if (*rest) return fail(usage);
        if (ieq(sub, "test")) { c.flag = 2; return c; }
        if (ieq(sub, "status")) { c.flag = 0; return c; }
        c.flag = 1; strcpy(c.a, "en"); strcpy(c.text, ieq(sub, "on") ? "1" : "0"); return c;
    }
    const char* key = nullptr;
    if (ieq(sub, "url")) key = "url";
    else if (ieq(sub, "body")) key = "body";
    else if (ieq(sub, "ctype")) key = "ctype";
    else if (ieq(sub, "err")) key = "err";
    if (key) {
        if (!*rest) return fail("value required (use \"\" to clear)");
        const char* msg = "";
        if (!parse_rest_value(rest, c.text, msg)) return fail(msg);
        c.flag = 1; strcpy(c.a, key);
        return c;
    }
    // однословные значения
    char v[40]; size_t n = 0;
    while (rest[n] && rest[n] != ' ' && rest[n] != '\t') { if (n + 1 >= sizeof v) return fail(usage); v[n] = rest[n]; ++n; }
    v[n] = 0;
    const char* after = rest + n;
    while (*after == ' ' || *after == '\t') ++after;
    if (!n || *after) return fail(usage);
    c.flag = 1;
    if (ieq(sub, "method")) {
        if (!ieq(v, "get") && !ieq(v, "post")) return fail("method must be get or post");
        strcpy(c.a, "method"); strcpy(c.text, ieq(v, "post") ? "POST" : "GET"); return c;
    }
    if (ieq(sub, "timeout") || ieq(sub, "tmo")) {
        long t = 0;
        if (!to_long(v, t) || t < 1 || t > 60) return fail("timeout must be 1..60 seconds");
        strcpy(c.a, "tmo"); strcpy(c.text, v); return c;
    }
    if (ieq(sub, "min")) {
        if (!ieq(v, "info") && !ieq(v, "warn") && !ieq(v, "alert")) return fail("min must be info, warn or alert");
        strcpy(c.a, "min"); for (size_t i = 0; v[i]; ++i) c.text[i] = (char)tolower((unsigned char)v[i]); return c;
    }
    if (ieq(sub, "insecure")) {
        if (!ieq(v, "on") && !ieq(v, "off")) return fail("insecure must be on or off");
        strcpy(c.a, "insecure"); strcpy(c.text, ieq(v, "on") ? "1" : "0"); return c;
    }
    return fail(usage);
}

bool valid_pass(const char* s) { size_t n = strlen(s); return n >= 8 && n <= 63; }

}  // namespace

CliCmd cli_parse(const char* line, int max_nets) {
    {   // комментарий — до разбора на токены (в нём может быть что угодно)
        const char* q = line;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q == '#') return CliCmd();
    }
    {   // webhook: значения длиннее токена и с пробелами — разбираем остаток строки целиком
        const char* q = line;
        while (*q == ' ' || *q == '\t') ++q;
        if (!strncasecmp(q, "webhook", 7) && (q[7] == 0 || q[7] == ' ' || q[7] == '\t')) return parse_webhook(q + 7);
    }
    Tok t[kMaxTok]; int n = 0;
    int r = tokenize(line, t, n);
    if (r == 1) return fail("unterminated quote");
    if (r == 2) return fail("argument too long");
    if (r == 3) return fail("too many arguments");
    CliCmd c;
    if (n == 0) return c;
    const char* cmd = t[0].s;

    if (ieq(cmd, "help") || ieq(cmd, "?")) { c.kind = CliKind::Help; return c; }
    if (ieq(cmd, "status")) { c.kind = CliKind::Status; return c; }
    if (ieq(cmd, "nets")) { c.kind = CliKind::Nets; return c; }
    if (ieq(cmd, "reboot")) { c.kind = CliKind::Reboot; return c; }
    if (ieq(cmd, "scan")) { c.kind = CliKind::Scan; return c; }
    if (ieq(cmd, "export")) { c.kind = CliKind::Export; return c; }
    if (ieq(cmd, "token")) {
        if (n != 2) return fail("usage: token <16..63 chars>");
        size_t len = strlen(t[1].s);
        if (len < 16 || len > 63) return fail("token must be 16..63 chars");
        c.kind = CliKind::ExtToken; strcpy(c.a, t[1].s); return c;
    }

    if (ieq(cmd, "net")) {
        long k = 0;
        if (n < 3 || !to_long(t[1].s, k) || k < 1 || k > max_nets) return fail("usage: net <1..N> <ssid> [password] | net <1..N> del");
        c.net = (int)k;
        if (n == 3 && !t[2].quoted && ieq(t[2].s, "del")) { c.kind = CliKind::NetDel; return c; }
        if (n > 4) return fail("too many arguments (quote values with spaces)");
        if (!t[2].s[0] || strlen(t[2].s) > 32) return fail("ssid must be 1..32 chars");
        if (n == 4 && t[3].s[0] && !valid_pass(t[3].s)) return fail("password must be 8..63 chars (omit for an open network)");
        c.kind = CliKind::NetSet;
        strcpy(c.a, t[2].s);
        if (n == 4) strcpy(c.b, t[3].s);
        return c;
    }
    if (ieq(cmd, "host")) {
        if (n != 2) return fail("usage: host <name>");
        size_t len = strlen(t[1].s);
        if (len < 1 || len > 32) return fail("hostname must be 1..32 chars");
        for (size_t i = 0; i < len; ++i)
            if (!(isalnum((unsigned char)t[1].s[i]) || t[1].s[i] == '-')) return fail("hostname: letters, digits, '-' only");
        c.kind = CliKind::Host; strcpy(c.a, t[1].s); return c;
    }
    if (ieq(cmd, "ap") || ieq(cmd, "admin")) {
        if (n != 2) return fail(ieq(cmd, "ap") ? "usage: ap <password>" : "usage: admin <password>");
        if (!valid_pass(t[1].s)) return fail("password must be 8..63 chars");
        c.kind = ieq(cmd, "ap") ? CliKind::ApPass : CliKind::AdminPass; strcpy(c.a, t[1].s); return c;
    }
    if (ieq(cmd, "mode")) {
        c.kind = CliKind::Mode;
        if (n == 1) return c;
        if (n != 2) return fail("usage: mode [sniff|normal]");
        if (ieq(t[1].s, "sniff")) c.flag = 1;
        else if (ieq(t[1].s, "normal")) c.flag = 0;
        else return fail("usage: mode [sniff|normal]");
        return c;
    }
    if (ieq(cmd, "logger")) {
        long v = 0;
        if (n != 2 || !to_long(t[1].s, v) || v < 30 || v > 900) return fail("usage: logger <30..900> (seconds)");
        c.kind = CliKind::Logger; c.num = v; return c;
    }
    if (ieq(cmd, "log")) {
        c.kind = CliKind::Log;
        if (n == 1) return c;
        if (n == 2 && ieq(t[1].s, "on")) { c.flag = 1; return c; }
        if (n == 2 && ieq(t[1].s, "off")) { c.flag = 0; return c; }
        return fail("usage: log [on|off]");
    }
    if (ieq(cmd, "can")) {
        c.kind = CliKind::Can;
        if (n == 1) return c;
        if (n == 2 && ieq(t[1].s, "off")) { c.flag = 1; c.num = 0; return c; }
        if (n == 2 && !t[1].quoted && (!strcmp(t[1].s, "100") || !strcmp(t[1].s, "125") || !strcmp(t[1].s, "250") || !strcmp(t[1].s, "500"))) { c.flag = 1; c.num = atol(t[1].s); return c; }
        return fail("usage: can [off|100|125|250|500]");
    }
    return fail("unknown command, type 'help'");
}

bool CliLine::feed(char c) {
    if (done_) { n_ = 0; buf_[0] = 0; over_ = false; done_ = false; }
    if (c == '\r' || c == '\n') {
        if (skip_) { skip_ = false; over_ = true; n_ = 0; buf_[0] = 0; done_ = true; return false; }
        if (n_ == 0) return false;
        buf_[n_] = 0; done_ = true;
        return true;
    }
    if (skip_) return false;
    if (c == 0x08 || c == 0x7f) { if (n_) buf_[--n_] = 0; return false; }
    if ((unsigned char)c < 0x20 && c != '\t') return false;
    if (n_ >= kCap) { skip_ = true; return false; }
    buf_[n_++] = c; buf_[n_] = 0;
    return false;
}

bool sniff_entry_ok(int ongrid_reg) { return ongrid_reg != 0; }

const char* cli_help_text() {
    return
        "commands:\n"
        "  status                      network state now\n"
        "  nets                        configured Wi-Fi networks (1 = main, others = backup)\n"
        "  net <1..4> <ssid> [pass]    set a network; no pass = open network; quote values with spaces\n"
        "  net <1..4> del              remove a network\n"
        "  scan                        list visible Wi-Fi networks (check the SSID spelling / 2.4 GHz)\n"
        "  export                      print all settings as commands (save to a file to restore later)\n"
        "  host <name>                 hostname (applies after reboot)\n"
        "  ap <password>               setup access point password (>= 8 chars)\n"
        "  admin <password>            web admin password (>= 8 chars)\n"
        "  token <16..63 chars>        token for /api/ext (Pi sender)\n"
        "  # text                      comment (ignored)\n"
        "  mode [sniff|normal]         show / set receive-only mode (saved)\n"
        "  logger <30..900>            expected cloud logger period, seconds\n"
        "  log [on|off]                periodic telemetry line on this console\n"
        "  can [off|100|125|250|500]         show / set CAN sniffer (listen-only) speed, kbit/s (saved)\n"
        "  webhook                     webhook notifications: status (URL secrets are not shown)\n"
        "  webhook url <url>           http(s) URL template; ${host_name} ${event_message} ${event_value} ${event_tag} ${event_flags} ${event_time}\n"
        "  webhook method get|post     request method (default get)\n"
        "  webhook body <template>     POST body template (values are URL-encoded); \"\" clears\n"
        "  webhook ctype <type>        POST Content-Type (default application/x-www-form-urlencoded)\n"
        "  webhook err <text>          response containing this text counts as a failure; \"\" clears\n"
        "  webhook timeout <1..60>     request timeout, seconds (default 10)\n"
        "  webhook min info|warn|alert minimum severity to send (default warn)\n"
        "  webhook insecure on|off     https without certificate check (default on)\n"
        "  webhook on|off              enable / disable\n"
        "  webhook test                queue a TEST event (always sent)\n"
        "  reboot\n";
}

}  // namespace gs
