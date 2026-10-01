// Заглушка: слой Arduino/ESP32 (Modbus, Wi-Fi, OTA) будет добавлен слайсами C1+/S*.
// Пока прошивка только доказывает, что ядро gs_core собирается под ESP32.
#include <Arduino.h>
#include "gs_core.h"

static gs::Supervisor g_sv;

void setup() { Serial.begin(115200); }

void loop() {
    gs::Input in;
    in.now_ms = millis();
    gs::Decision d = g_sv.step(in);  // без данных: через no_data_timeout выдаст Connect
    if (d.cmd != gs::Cmd::None) Serial.printf("cmd=%d reason=%d\n", (int)d.cmd, (int)d.reason);
    delay(5000);
}
