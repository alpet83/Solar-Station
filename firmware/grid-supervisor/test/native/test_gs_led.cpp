#include <cstdio>
#include "gs_led.h"
using namespace gs;
static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool is(const Rgb& c, uint8_t r, uint8_t g, uint8_t b) { return c.r == r && c.g == g && c.b == b; }

int main() {
    LedStatus s;
    CHECK(is(led_color(s, 500), 255, 255, 255));              // белый
    CHECK(is(led_color(s, 1100), 0, 0, 0));                   // пауза
    CHECK(is(led_color(s, 1500), 255, 0, 0));                 // оффлайн = красный
    CHECK(is(led_color(s, 2900), 0, 0, 255));                 // синий
    CHECK(led_segment(7000 + 500) == 0 && led_segment(6900) == 4);
    s.net = NetState::Connected;
    CHECK(is(led_color(s, 1500), 255, 170, 0));               // подключено, запросов нет = жёлтый
    s.api_recent = true;
    CHECK(is(led_color(s, 1500), 0, 255, 0));                 // опрашивается = зелёный
    s.mqtt_bad = true;
    CHECK(is(led_color(s, 1500), 255, 80, 0));                // MQTT-проблема = оранжевый
    s.net = NetState::Ap; CHECK(is(led_color(s, 1500), 255, 0, 200));
    s.net = NetState::Offline; CHECK(is(led_color(s, 1500), 255, 0, 0));
    // инвертор
    s.inv = InvState::NoLink;
    for (uint32_t u = 0; u < 3000; u += 250) CHECK(is(led_color(s, 3600 + u), 255, 0, 0));   // красный все 3 с
    s.inv = InvState::OkGrid; CHECK(is(led_color(s, 3600), 0, 255, 0) && is(led_color(s, 6599), 0, 255, 0));
    s.inv = InvState::OkOff;  CHECK(is(led_color(s, 5000), 0, 220, 200));
    s.inv = InvState::Sniff;  CHECK(is(led_color(s, 5000), 120, 0, 255));
    s.inv = InvState::Problem;
    CHECK(is(led_color(s, 3600 + 100), 255, 0, 0));           // 1-е мигание красное
    CHECK(is(led_color(s, 3600 + 400), 0, 0, 0));             // пауза
    CHECK(is(led_color(s, 3600 + 700), 255, 80, 0));          // 2-е оранжевое
    CHECK(is(led_color(s, 3600 + 1300), 255, 0, 0));          // 3-е красное
    int blinks = 0; bool prev = false;
    for (uint32_t u = 0; u < 3000; u += 10) { bool on = !is(led_color(s, 3600 + u), 0, 0, 0); if (on && !prev) ++blinks; prev = on; }
    CHECK(blinks == 5);
    CHECK(is(led_color(s, 6700), 0, 0, 0));                   // хвост цикла погашен
    printf("led: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
