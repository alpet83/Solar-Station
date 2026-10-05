#include <cstdio>
#include <cstring>
#include <string>
#include "gs_cli.h"

using namespace gs;
static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool is_err(const CliCmd& c) { return c.kind == CliKind::Error; }

static void test_simple() {
    CHECK(cli_parse("", 4).kind == CliKind::None);
    CHECK(cli_parse("   \t ", 4).kind == CliKind::None);
    CHECK(cli_parse("help", 4).kind == CliKind::Help);
    CHECK(cli_parse("HELP", 4).kind == CliKind::Help);
    CHECK(cli_parse("?", 4).kind == CliKind::Help);
    CHECK(cli_parse("status", 4).kind == CliKind::Status);
    CHECK(cli_parse("nets", 4).kind == CliKind::Nets);
    CHECK(cli_parse("reboot", 4).kind == CliKind::Reboot);
    CHECK(is_err(cli_parse("frobnicate", 4)));
}

static void test_net() {
    CliCmd c = cli_parse("net 2 HomeWiFi secret123", 4);
    CHECK(c.kind == CliKind::NetSet && c.net == 2 && !strcmp(c.a, "HomeWiFi") && !strcmp(c.b, "secret123"));
    c = cli_parse("net 1 OpenNet", 4);
    CHECK(c.kind == CliKind::NetSet && c.net == 1 && !strcmp(c.a, "OpenNet") && c.b[0] == 0);
    c = cli_parse("net 3 \"My Home Wi-Fi\" \"pass with space\"", 4);
    CHECK(c.kind == CliKind::NetSet && !strcmp(c.a, "My Home Wi-Fi") && !strcmp(c.b, "pass with space"));
    c = cli_parse("net 3 \"a\\\"b\\\\c\" \"12345678\"", 4);
    CHECK(c.kind == CliKind::NetSet && !strcmp(c.a, "a\"b\\c"));
    c = cli_parse("net 4 del", 4);
    CHECK(c.kind == CliKind::NetDel && c.net == 4);
    c = cli_parse("net 4 DEL", 4);
    CHECK(c.kind == CliKind::NetDel);
    c = cli_parse("net 4 \"del\"", 4);                 // SSID, буквально названный del
    CHECK(c.kind == CliKind::NetSet && !strcmp(c.a, "del"));
    c = cli_parse("net 4 del 12345678", 4);            // SSID del с паролем
    CHECK(c.kind == CliKind::NetSet && !strcmp(c.a, "del"));
    CHECK(is_err(cli_parse("net 0 x", 4)));
    CHECK(is_err(cli_parse("net 5 x", 4)));
    CHECK(is_err(cli_parse("net x y", 4)));
    CHECK(is_err(cli_parse("net 1", 4)));
    CHECK(is_err(cli_parse("net 1 ssid short", 4)));                       // пароль < 8
    CHECK(is_err(cli_parse("net 1 ssid 123456789012345678901234567890123456789012345678901234567890123456", 4)));   // > 63
    CHECK(is_err(cli_parse("net 1 123456789012345678901234567890123", 4)));   // ssid 33
    CHECK(cli_parse("net 1 123456789012345678901234567890xy", 4).kind == CliKind::NetSet);   // ssid 32
    CHECK(is_err(cli_parse("net 1 a b c", 4)));
    CHECK(is_err(cli_parse("net 1 \"unterminated", 4)));
    CHECK(is_err(cli_parse("net 1 \"\"", 4)));                              // пустой ssid
    c = cli_parse("net 1 ssid \"\"", 4);                                   // пустой пароль = открытая
    CHECK(c.kind == CliKind::NetSet && c.b[0] == 0);
    c = cli_parse("net 2 s p4ssw0rd", 2);
    CHECK(c.kind == CliKind::NetSet);
    CHECK(is_err(cli_parse("net 3 s", 2)));
}

static void test_other() {
    CliCmd c = cli_parse("host my-pi-1", 4);
    CHECK(c.kind == CliKind::Host && !strcmp(c.a, "my-pi-1"));
    CHECK(is_err(cli_parse("host bad_name", 4)));
    CHECK(is_err(cli_parse("host", 4)));
    CHECK(is_err(cli_parse("host 123456789012345678901234567890123", 4)));
    c = cli_parse("ap 12345678", 4);
    CHECK(c.kind == CliKind::ApPass && !strcmp(c.a, "12345678"));
    CHECK(is_err(cli_parse("ap 1234567", 4)));
    CHECK(cli_parse("admin longenough", 4).kind == CliKind::AdminPass);
    CHECK(is_err(cli_parse("admin x", 4)));
    CHECK(cli_parse("mode", 4).kind == CliKind::Mode && cli_parse("mode", 4).flag == -1);
    CHECK(cli_parse("mode sniff", 4).flag == 1);
    CHECK(cli_parse("mode normal", 4).flag == 0);
    CHECK(is_err(cli_parse("mode active", 4)));
    c = cli_parse("logger 90", 4);
    CHECK(c.kind == CliKind::Logger && c.num == 90);
    CHECK(is_err(cli_parse("logger 10", 4)));
    CHECK(is_err(cli_parse("logger 901", 4)));
    CHECK(is_err(cli_parse("logger abc", 4)));
    CHECK(cli_parse("log", 4).flag == -1);
    CHECK(cli_parse("log off", 4).flag == 0);
    CHECK(cli_parse("log on", 4).flag == 1);
    CHECK(is_err(cli_parse("log maybe", 4)));
    CHECK(is_err(cli_parse("a b c d e f g", 4)));            // > 5 токенов
    CHECK(is_err(cli_parse("net 1 ssid 12345678 extra extra2", 4)));
}

static void test_new() {
    CHECK(cli_parse("scan", 4).kind == CliKind::Scan);
    CHECK(cli_parse("export", 4).kind == CliKind::Export);
    CHECK(cli_parse("# comment net 1 x y", 4).kind == CliKind::None);
    CHECK(cli_parse("   #x", 4).kind == CliKind::None);
    CHECK(cli_parse("\"#\"", 4).kind == CliKind::Error);                 // кавычками # не комментарий
    CliCmd c = cli_parse("token 0123456789abcdef", 4);
    CHECK(c.kind == CliKind::ExtToken && !strcmp(c.a, "0123456789abcdef"));
    CHECK(is_err(cli_parse("token short", 4)));
    CHECK(is_err(cli_parse("token", 4)));
}

static void test_can() {
    CliCmd c = cli_parse("can", 4);
    CHECK(c.kind == CliKind::Can && c.flag == -1);
    c = cli_parse("can off", 4);
    CHECK(c.kind == CliKind::Can && c.flag == 1 && c.num == 0);
    c = cli_parse("CAN OFF", 4);
    CHECK(c.kind == CliKind::Can && c.flag == 1 && c.num == 0);
    c = cli_parse("can 250", 4);
    CHECK(c.kind == CliKind::Can && c.flag == 1 && c.num == 250);
    c = cli_parse("can 500", 4);
    CHECK(c.kind == CliKind::Can && c.flag == 1 && c.num == 500);
    CHECK(is_err(cli_parse("can 111", 4)));
    CHECK(is_err(cli_parse("can 0500", 4)));
    CHECK(is_err(cli_parse("can on", 4)));
    CHECK(is_err(cli_parse("can 500 250", 4)));
    CHECK(is_err(cli_parse("can \"500\"", 4)));
}

static void test_webhook() {
    CliCmd c = cli_parse("webhook", 4);
    CHECK(c.kind == CliKind::Webhook && c.flag == 0);
    CHECK(cli_parse("WEBHOOK status", 4).flag == 0 && cli_parse("  webhook   ", 4).flag == 0);
    CHECK(cli_parse("webhook test", 4).kind == CliKind::Webhook && cli_parse("webhook test", 4).flag == 2);
    c = cli_parse("webhook on", 4);
    CHECK(c.kind == CliKind::Webhook && c.flag == 1 && !strcmp(c.a, "en") && !strcmp(c.text, "1"));
    c = cli_parse("webhook OFF", 4);
    CHECK(c.flag == 1 && !strcmp(c.a, "en") && !strcmp(c.text, "0"));
    CHECK(is_err(cli_parse("webhook on now", 4)) && is_err(cli_parse("webhook test 1", 4)));
    // URL: остаток строки как есть, длиннее токена (80), с ${...} и '#'
    std::string url = "https://api.telegram.org/bot123456:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/sendMessage?chat_id=-100&text=${host_name}%20${event_tag}:%20${event_message}#x";
    CHECK(url.size() > 80);
    c = cli_parse(("webhook url " + url).c_str(), 4);
    CHECK(c.kind == CliKind::Webhook && c.flag == 1 && !strcmp(c.a, "url") && url == c.text);
    c = cli_parse(("webhook   url \t" + url + "   ").c_str(), 4);                // лишние пробелы вокруг
    CHECK(c.flag == 1 && url == c.text);
    c = cli_parse(("webhook url \"" + url + "\"").c_str(), 4);
    CHECK(c.flag == 1 && url == c.text);
    CHECK(is_err(cli_parse("webhook url", 4)));                                  // значение обязательно
    CHECK(is_err(cli_parse("webhook url \"http://x", 4)));                       // не закрыта кавычка
    CHECK(is_err(cli_parse("webhook url \"http://x\" tail", 4)));
    c = cli_parse("webhook url \"\"", 4);                                        // очистка
    CHECK(c.flag == 1 && !strcmp(c.a, "url") && c.text[0] == 0);
    // тело: пробелы сохраняются, кавычки внутри — экранированием
    c = cli_parse("webhook body chat_id=1&text=${host_name} ${event_message}", 4);
    CHECK(c.flag == 1 && !strcmp(c.a, "body") && !strcmp(c.text, "chat_id=1&text=${host_name} ${event_message}"));
    c = cli_parse("webhook body \"{\\\"a\\\": \\\"b\\\\c\\\"}\"", 4);
    CHECK(c.flag == 1 && !strcmp(c.text, "{\"a\": \"b\\c\"}"));
    c = cli_parse("webhook body {\"text\":\"x\"}", 4);                          // без кавычек в начале — как есть
    CHECK(c.flag == 1 && !strcmp(c.text, "{\"text\":\"x\"}"));
    c = cli_parse("webhook ctype application/json; charset=utf-8", 4);
    CHECK(c.flag == 1 && !strcmp(c.a, "ctype") && !strcmp(c.text, "application/json; charset=utf-8"));
    c = cli_parse("webhook err #ERROR", 4);                                      // # не в начале строки — не комментарий
    CHECK(c.flag == 1 && !strcmp(c.a, "err") && !strcmp(c.text, "#ERROR"));
    CHECK(cli_parse("webhook err \"\"", 4).text[0] == 0);
    std::string max(400, 'u'), over(401, 'u');
    CHECK(cli_parse(("webhook body " + max).c_str(), 4).flag == 1);
    CHECK(is_err(cli_parse(("webhook body " + over).c_str(), 4)));
    CHECK(is_err(cli_parse(("webhook body \"" + over + "\"").c_str(), 4)));
    c = cli_parse("webhook method POST", 4);
    CHECK(c.flag == 1 && !strcmp(c.a, "method") && !strcmp(c.text, "POST"));
    CHECK(!strcmp(cli_parse("webhook method get", 4).text, "GET"));
    CHECK(is_err(cli_parse("webhook method put", 4)) && is_err(cli_parse("webhook method", 4)) && is_err(cli_parse("webhook method get post", 4)));
    c = cli_parse("webhook timeout 25", 4);
    CHECK(c.flag == 1 && !strcmp(c.a, "tmo") && !strcmp(c.text, "25"));
    CHECK(cli_parse("webhook timeout 1", 4).flag == 1 && cli_parse("webhook timeout 60", 4).flag == 1);
    CHECK(is_err(cli_parse("webhook timeout 0", 4)) && is_err(cli_parse("webhook timeout 61", 4)) && is_err(cli_parse("webhook timeout x", 4)) && is_err(cli_parse("webhook timeout", 4)));
    c = cli_parse("webhook min ALERT", 4);
    CHECK(c.flag == 1 && !strcmp(c.a, "min") && !strcmp(c.text, "alert"));
    CHECK(!strcmp(cli_parse("webhook min info", 4).text, "info") && !strcmp(cli_parse("webhook min warn", 4).text, "warn"));
    CHECK(is_err(cli_parse("webhook min high", 4)) && is_err(cli_parse("webhook min", 4)) && is_err(cli_parse("webhook min 1", 4)));
    c = cli_parse("webhook insecure off", 4);
    CHECK(c.flag == 1 && !strcmp(c.a, "insecure") && !strcmp(c.text, "0") && !strcmp(cli_parse("webhook insecure on", 4).text, "1"));
    CHECK(is_err(cli_parse("webhook insecure maybe", 4)));
    CHECK(is_err(cli_parse("webhook frob", 4)) && is_err(cli_parse("webhook frob 1", 4)));
    CHECK(cli_parse("webhookx", 4).kind == CliKind::Error);                       // другое слово
    CHECK(cli_parse("# webhook on", 4).kind == CliKind::None);
    // строка вплоть до 1000 символов проходит сборщик строк целиком
    CliLine l; std::string long_line = "webhook url " + std::string(500, 'a') + "\n"; bool got = false;
    for (char ch : long_line) if (l.feed(ch)) got = true;
    CHECK(got && !l.overflowed() && strlen(l.line()) == 512);
}

static void test_line() {
    CliLine l;
    auto feed_all = [&](const char* s, std::string& out) {
        out.clear();
        for (; *s; ++s) if (l.feed(*s)) out = l.line();
    };
    std::string out;
    feed_all("status\r\n", out);
    CHECK(out == "status");
    feed_all("\n\r\n", out);                                  // пустые строки не возвращаются
    CHECK(out.empty());
    feed_all("abx\x7f\x08" "c\n", out);                      // BS/DEL стирают (оба стёрли x и b)
    CHECK(out == "ac");
    feed_all("a\nb\n", out);
    CHECK(out == "b");
    std::string big(1100, 'x');
    big += "\n";
    bool got = false, over = false;
    for (char ch : big) { if (l.feed(ch)) got = true; if (l.overflowed()) over = true; }
    CHECK(!got && over);
    feed_all("help\n", out);                                  // после переполнения работает
    CHECK(out == "help");
    CHECK(!l.overflowed());
    feed_all("a\x01" "b\n", out);                             // управляющие символы игнорируются
    CHECK(out == "ab");
}

static void test_sniff_policy() {
    CHECK(sniff_entry_ok(1));
    CHECK(sniff_entry_ok(-1));
    CHECK(!sniff_entry_ok(0));
}

int main() {
    test_simple(); test_net(); test_new(); test_can(); test_other(); test_line(); test_webhook(); test_sniff_policy();
    CHECK(strstr(cli_help_text(), "net <1..4>") != nullptr);
    CHECK(strstr(cli_help_text(), "can [off|100|125|250|500]") != nullptr);
    CHECK(strstr(cli_help_text(), "webhook test") != nullptr);
    printf("%d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
