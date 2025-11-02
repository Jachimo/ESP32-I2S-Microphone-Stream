#include <Arduino.h>
#include "config.h"
#include "i2s_manager.h"
#include "wifi_manager.h"
#include "streamer.h"

void setup() {
    Serial.begin(115200);
    I2SSetup();
    I2SSelfTest();

    bool ok = wifi_connect_blocking();
    if (!ok) {
        Serial.println("WiFi not connected; continuing but streamer may not be reachable.");
    }

    // start streamer task (server created inside streamer)
    start_streamer();
}

void loop() {
    // keep main loop minimal; streamer runs in its own task
    delay(10);
}
