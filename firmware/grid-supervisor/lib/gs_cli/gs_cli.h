// Консоль настройки по Serial: чистый разбор строк команд (без Arduino) — тестируется на хосте.
// Исполнение команд (NVS, Wi-Fi) — в оболочке (src/shell.cpp).
#pragma once
#include <cstddef>

namespace gs {

enum class CliKind { None, Help, Status, Nets, NetSet, NetDel, Host, ApPass, AdminPass, Mode, Logger, Log, Can, Reboot, Scan, Export, ExtToken, Webhook, Error };

struct CliCmd {
    CliKind kind = CliKind::None;
    int net = 0;               // NetSet/NetDel: номер сети 1..max_nets
    char a[80] = {0};          // SSID / имя хоста / пароль / слово режима
    char b[80] = {0};          // пароль сети (пусто = открытая сеть)
    long num = 0;              // Logger: период, с; Can: скорость, кбит/с (0 = выключен)
    int flag = -1;             // Mode: 0 = normal, 1 = sniff, -1 = только показать; Log: 0/1, -1 = показать; Can: 1 = задать (num), -1 = показать
    char text[416] = {0};      // Webhook: значение (url/шаблон тела/...), до 400 символов; строка целиком, пробелы сохраняются
    char err[64] = {0};        // Error: текст
};

// Webhook: flag 0 — показать статус; 1 — задать ключ a (url method body ctype err tmo min insecure en) значением text; 2 — тестовое событие.
// Значение — остаток строки как есть (можно в кавычках с \" и \\; пустое — ""). Полная проверка (URL, плейсхолдеры) — в gs_notify.

// Разбор одной строки. Строка без команды (пустая) даёт CliKind::None.
CliCmd cli_parse(const char* line, int max_nets);

// Сборка строки из потока байт: \r и \n завершают строку, BS/DEL стирают, длинная строка
// отбрасывается целиком (overflowed() = true к её концу).
class CliLine {
public:
    // true — строка готова (line()); пустые строки не возвращаются.
    bool feed(char c);
    const char* line() const { return buf_; }
    bool overflowed() const { return over_; }      // сбрасывается на следующем feed после завершения
private:
    static const size_t kCap = 1024;
    char buf_[kCap + 1] = {0};
    size_t n_ = 0;
    bool skip_ = false, over_ = false, done_ = false;
};

// Вход в режим «только приём» запрещён, если сеть заведомо отключена (20105=0): в нём защит нет.
// Если данных нет вовсе (-1), входить можно: передавать на шину всё равно нечего.
bool sniff_entry_ok(int ongrid_reg);

const char* cli_help_text();

}  // namespace gs
