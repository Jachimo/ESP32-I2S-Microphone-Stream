#include "wifi_manager.h"
#include "config.h"
#include <WiFi.h>
#include <Arduino.h>
#include "esp_wifi.h"

static const char* wifiStatusStr(wl_status_t s) {
    switch (s) {
        case WL_IDLE_STATUS:      return "WL_IDLE_STATUS";
        case WL_NO_SSID_AVAIL:    return "WL_NO_SSID_AVAIL";
        case WL_SCAN_COMPLETED:   return "WL_SCAN_COMPLETED";
        case WL_CONNECTED:        return "WL_CONNECTED";
        case WL_CONNECT_FAILED:   return "WL_CONNECT_FAILED";
        case WL_CONNECTION_LOST:  return "WL_CONNECTION_LOST";
        case WL_DISCONNECTED:     return "WL_DISCONNECTED";
        default:                  return "WL_UNKNOWN";
    }
}

bool wifi_connect_blocking(void) {
    Serial.println();
    Serial.println("=== WiFi connect sequence ===");
    Serial.printf("Attempting to connect to SSID: '%s'\n", ssid);
    Serial.printf("Device MAC: %s\n", WiFi.macAddress().c_str());

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    // single scan to see if SSID visible
    Serial.println("Performing initial WiFi scan...");
    int n = WiFi.scanNetworks();
    bool ssid_found = false;
    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            String foundSsid = WiFi.SSID(i);
            int rssi = WiFi.RSSI(i);
            String enc = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "secure";
            Serial.printf("  %d: %s (%d dBm) %s\n", i, foundSsid.c_str(), rssi, enc.c_str());
            if (foundSsid == ssid) ssid_found = true;
        }
    } else {
        Serial.println("  No networks found in initial scan.");
    }
    WiFi.scanDelete();
    if (!ssid_found) {
        Serial.printf("Warning: target SSID '%s' not seen in scan. It may be hidden or out of range.\n", ssid);
    }

    const int max_retries = 30;
    const unsigned long connect_timeout_ms = 175UL * 1000UL;
    bool connected = false;

    for (int attempt = 1; attempt <= max_retries && !connected; ++attempt) {
        Serial.printf("Connect attempt %d/%d: calling WiFi.begin()\n", attempt, max_retries);
        WiFi.begin(ssid, password);

        unsigned long start_ms = millis();
        while (millis() - start_ms < connect_timeout_ms) {
            wl_status_t st = WiFi.status();
            unsigned long elapsed_s = (millis() - start_ms) / 1000;
            Serial.printf("  WiFi status: %s (0x%02X), elapsed %lus\n", wifiStatusStr(st), (int)st, elapsed_s);
            if (st == WL_CONNECTED) {
                connected = true;
                break;
            }
            delay(1000);
        }
        if (!connected) {
            Serial.printf("  Attempt %d timed out after %lus. Disconnecting and retrying...\n", attempt, connect_timeout_ms / 1000UL);
            WiFi.disconnect(true, true);
            delay(500);
        }
    }

    if (connected && WiFi.status() == WL_CONNECTED) {
        Serial.println("Connected to WiFi");
        Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());

        // disable power save, enable protocols
        esp_err_t err;
        err = esp_wifi_set_ps(WIFI_PS_NONE);
        Serial.printf("esp_wifi_set_ps(WIFI_PS_NONE) -> %d\n", (int)err);
        WiFi.setSleep(false);
        err = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
        Serial.printf("esp_wifi_set_protocol(STA, 11B|11G|11N) -> %d\n", (int)err);

        wifi_ap_record_t apinfo;
        if (esp_wifi_sta_get_ap_info(&apinfo) == ESP_OK) {
            Serial.printf("AP info: SSID=%s, primary_chan=%d, rssi=%d, authmode=%d\n",
                apinfo.ssid, apinfo.primary, apinfo.rssi, apinfo.authmode);
        } else {
            Serial.println("esp_wifi_sta_get_ap_info() failed");
        }
        return true;
    }

    Serial.printf("Final WiFi status after retries: %s (0x%02X)\n", wifiStatusStr(WiFi.status()), (int)WiFi.status());
    return false;
}