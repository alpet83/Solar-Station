#include "gs_cli.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

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

bool valid_pass(const char* s) { size_t n = strlen(s); return n >= 8 && n <= 63; }

}  // namespace

CliCmd cli_parse(const char* line, int max_nets) {
    {   // комментарий — до разбора на токены (в нём может быть что угодно)
        const char* q = line;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q == '#') return CliCmd();
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
        if (n == 2 && !t[1].quoted && (!strcmp(t[1].s, "125") || !strcmp(t[1].s, "250") || !strcmp(t[1].s, "500"))) { c.flag = 1; c.num = atol(t[1].s); return c; }
        return fail("usage: can [off|125|250|500]");
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
        "  can [off|125|250|500]         show / set CAN sniffer (listen-only) speed, kbit/s (saved)\n"
        "  reboot\n";
}

}  // namespace gs
