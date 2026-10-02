// gs_led — логика световой индикации (RGB-светодиод WS2812). Без Arduino-зависимостей.
// Цикл 7 с: белый 1 с (начало) · сеть 1 с · синий 1 с · состояние инвертора 3 с · пауза; между
// сегментами короткие паузы, чтобы цвета не сливались.
#pragma once
#include <cstdint>

namespace gs {

struct Rgb { uint8_t r = 0, g = 0, b = 0; bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; } };

enum class NetState : uint8_t { Offline = 0, Ap, Connected };
enum class InvState : uint8_t { NoLink = 0, Problem, OkGrid, OkOff, Sniff };

struct LedStatus {
    NetState net = NetState::Offline;
    bool api_recent = false;      // за последнюю минуту были запросы к API (опрос Pi/HA)
    bool mqtt_bad = false;        // проблемы с отправкой MQTT (на плате MQTT пока нет — всегда false)
    InvState inv = InvState::NoLink;
};

constexpr uint32_t kLedCycleMs = 7000;

// Цвет в момент t_ms (любое число, берётся по модулю цикла). Яркость масштабирует адаптер.
Rgb led_color(const LedStatus& s, uint32_t t_ms);

// Сегмент цикла (для тестов/отладки): 0 белый, 1 сеть, 2 синий, 3 инвертор, 4 пауза/промежуток.
int led_segment(uint32_t t_ms);

}  // namespace gs
