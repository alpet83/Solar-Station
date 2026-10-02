#include "gs_led.h"

namespace gs {

static const Rgb kOff{0, 0, 0}, kWhite{255, 255, 255}, kBlue{0, 0, 255}, kRed{255, 0, 0}, kOrange{255, 80, 0},
    kYellow{255, 170, 0}, kGreen{0, 255, 0}, kCyan{0, 220, 200}, kMagenta{255, 0, 200}, kViolet{120, 0, 255};

// границы сегментов: белый [0,1000) · пауза · сеть [1200,2200) · пауза · синий [2400,3400) · пауза · инвертор [3600,6600) · пауза
int led_segment(uint32_t t) {
    t %= kLedCycleMs;
    if (t < 1000) return 0;
    if (t >= 1200 && t < 2200) return 1;
    if (t >= 2400 && t < 3400) return 2;
    if (t >= 3600 && t < 6600) return 3;
    return 4;
}

static Rgb net_color(const LedStatus& s) {
    switch (s.net) {
        case NetState::Offline: return kRed;
        case NetState::Ap: return kMagenta;                 // режим настройки (точка доступа)
        case NetState::Connected: break;
    }
    if (s.mqtt_bad) return kOrange;
    return s.api_recent ? kGreen : kYellow;
}

static Rgb inv_color(const LedStatus& s, uint32_t u) {      // u — мс от начала сегмента (0..2999)
    switch (s.inv) {
        case InvState::NoLink: return kRed;                 // длинный красный
        case InvState::Problem: {                           // короткие мигания: красный, оранжевый, красный, ...
            const uint32_t k = u / 600, ph = u % 600;
            if (ph >= 300) return kOff;
            return (k & 1) ? kOrange : kRed;
        }
        case InvState::OkGrid: return kGreen;               // работаем от сети
        case InvState::OkOff: return kCyan;                 // автономно (сеть отключена)
        case InvState::Sniff: return kViolet;               // режим приёма: плата не управляет
    }
    return kOff;
}

Rgb led_color(const LedStatus& s, uint32_t t_ms) {
    switch (led_segment(t_ms)) {
        case 0: return kWhite;
        case 1: return net_color(s);
        case 2: return kBlue;
        case 3: return inv_color(s, (t_ms % kLedCycleMs) - 3600);
        default: return kOff;
    }
}

}  // namespace gs
