#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <driver/i2s.h>
#include "esp_wifi.h"  // to tune PHY / power-save

// Pin assignments for Heltec WiFi Kit 32 (Demo Board w/ LEDs and Buzzer)
#define MIC_SCK   33   // Position 15J
#define MIC_WS    25   // Position 15K
#define MIC_SDOUT 32   // Position 15L
#define I2S_PORT  I2S_NUM_0

// WiFi credentials are provided by include/secrets.h
#include "secrets.h"

// Raw TCP server for the continuous audio stream (avoids blocking)
WiFiServer streamServer(8080);

static void I2SSetup(void) {
    i2s_config_t i2s_config = {
        // For INMP441 microphone
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        // TODO: Drop sample rate for testing; consider turning up later:
        .sample_rate = 16000,
         .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,        // 32-slot (24-bit data left-justified)
         .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,        // TODO: Collect both channels for debugging
         .communication_format = I2S_COMM_FORMAT_STAND_I2S,   // 1
         .intr_alloc_flags = 0,  // Lower interrupt priority to avoid starving WiFi/lwIP tasks
         .dma_buf_count = 3,
         .dma_buf_len = 128,
     };
    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = MIC_SCK,                 // IIS_SCLK
        .ws_io_num = MIC_WS,                   // IIS_LCLK
        .data_out_num = I2S_PIN_NO_CHANGE,     // IIS_DSIN
        .data_in_num = MIC_SDOUT               // IIS_DOUT
    };

    // Install driver and check for errors
    esp_err_t res = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (res != ESP_OK) {
        Serial.printf("i2s_driver_install failed: %d\n", res);
        return;
    }

    res = i2s_set_pin(I2S_PORT, &pin_config);
    if (res != ESP_OK) {
        Serial.printf("i2s_set_pin failed: %d\n", res);
        return;
    }

    // Explicitly set standard clock
    // This ensures BCLK/WS are configured consistently
    res = i2s_set_clk(I2S_PORT, i2s_config.sample_rate, i2s_config.bits_per_sample, I2S_CHANNEL_STEREO);
    if (res != ESP_OK) {
        Serial.printf("i2s_set_clk failed: %d\n", res);
        // continue — driver is installed
    }
    i2s_zero_dma_buffer(I2S_PORT);

    // Startup self-test 
    {
        const int test_len = 64;
        int32_t test_buf[test_len];
        size_t bytes_read = 0;
        vTaskDelay(1000 / portTICK_PERIOD_MS); // "let clocks stabilize"
        esp_err_t r = i2s_read(I2S_PORT, test_buf, sizeof(test_buf), &bytes_read, 100 / portTICK_PERIOD_MS);
        Serial.printf("I2S self-test read bytes: %u\n", (unsigned)bytes_read);
        if (r == ESP_OK && bytes_read > 0) {
            int samples = bytes_read / sizeof(int32_t);
            int pairs = samples / 2;
            int dump = pairs < 4 ? pairs : 4;
            Serial.print("Self-test sample pairs (L,R): ");
            for (int i = 0; i < dump; ++i) {
                uint32_t l = (uint32_t)test_buf[i*2];
                uint32_t r = (uint32_t)test_buf[i*2 + 1];
                Serial.printf("[%08X,%08X] ", l, r);
            }
            Serial.println();
        } else {
            Serial.println("Self-test: no data or read failed");
        }
    }
}

// Streaming task — runs as a separate per-client FreeRTOS task
//  Accepts a client, sends a WAV header, then streams
static void streamingTask(void *pvParameters) {
    (void)pvParameters;

    // match DMA buffer size and reduce latency
    const int i2s_read_len = 128;
    int32_t i2s_read_buff[i2s_read_len];

    // WAV header used earlier (16000 Hz, 16-bit mono)
    byte streamingWavHeader[44] = {
        0x52,0x49,0x46,0x46, 0xFF,0xFF,0xFF,0xFF,
        0x57,0x41,0x56,0x45,
        0x66,0x6D,0x74,0x20,
        0x10,0x00,0x00,0x00,
        0x01,0x00,
        0x01,0x00,
        0x80,0x3E,0x00,0x00, // SampleRate (16000) little-endian
        0x00,0x7D,0x00,0x00, // ByteRate (32000) little-endian
        0x02,0x00,
        0x10,0x00,
        0x64,0x61,0x74,0x61,
        0xFF,0xFF,0xFF,0xFF
    };

    while (true) {
        WiFiClient client = streamServer.available();
        if (!client) {
            vTaskDelay(50 / portTICK_PERIOD_MS);
            continue;
        }

        Serial.printf("Streamer: client connected from %s\n", client.remoteIP().toString().c_str());
        client.setNoDelay(true);

        // Read and discard request headers (robust)
        unsigned long total_deadline = millis() + 1000;
        bool firstLineLogged = false;
        bool headers_done = false;
        while (client.connected() && millis() < total_deadline && !headers_done) {
            while (client.available()) {
                String line = client.readStringUntil('\n');
                if (line.endsWith("\r")) line.remove(line.length() - 1);
                if (!firstLineLogged) {
                    Serial.printf("Streamer: request first line: %s\n", line.c_str());
                    firstLineLogged = true;
                }
                if (line.length() == 0) { headers_done = true; break; }
            }
            if (!headers_done) vTaskDelay(5 / portTICK_PERIOD_MS);
        }
        if (!firstLineLogged) Serial.println("Streamer: no request headers received (timeout), proceeding to respond");

        // Send response headers + WAV header
        client.print("HTTP/1.1 200 OK\r\n");
        client.print("Content-Type: audio/wav\r\n");
        client.print("Cache-Control: no-cache\r\n");
        client.print("Connection: keep-alive\r\n");
        client.print("\r\n");
        client.write(streamingWavHeader, sizeof(streamingWavHeader));
        client.flush();
        Serial.println("Streamer: response headers & WAV header sent, entering stream loop");

        // TODO: REMOVE ME LATER
        // Immediate sanity-test: send a short known non-zero PCM block
        {
            const int TEST_SAMPLES = 128;
            int16_t testbuf[TEST_SAMPLES];

            // simple ramp / DC-like pattern: non-zero values that are easy to spot
            for (int i = 0; i < TEST_SAMPLES; ++i) {
                testbuf[i] = (int16_t)((i & 0xFF) - 128) * 64;
            }

            const uint8_t* tsrc = (const uint8_t*)testbuf;
            size_t tbytes = TEST_SAMPLES * sizeof(int16_t);
            size_t twritten = 0;
            unsigned long tdeadline = millis() + 1000;
            while (twritten < tbytes && client && client.connected()) {
                size_t chunk = tbytes - twritten;
                if (chunk > 64) chunk = 64;
                int w = client.write(tsrc + twritten, chunk);
                if (w > 0) {
                    twritten += (size_t)w;
                } else {
                    vTaskDelay(2 / portTICK_PERIOD_MS);
                    if (millis() > tdeadline) break;
                }
            }
            client.flush();
            Serial.printf("Streamer: test pattern sent (%u/%u bytes)\n", (unsigned)twritten, (unsigned)tbytes);
        }
        // End sanity-test block

        // Stream loop: convert 32-bit stereo -> 16-bit mono (left), send in small chunks
        bool firstAudioSent = false;
        int consecutiveEmptySends = 0;

        while (true) {
            if (!client || !client.connected()) break;

            size_t bytes_read = 0;
            esp_err_t r = i2s_read(I2S_PORT, i2s_read_buff, i2s_read_len * sizeof(int32_t), &bytes_read, 200 / portTICK_PERIOD_MS);
            
            if (r != ESP_OK || bytes_read == 0) {
                // no data this cycle; allow other tasks to run
                vTaskDelay(5 / portTICK_PERIOD_MS);
                continue;
            }

            // convert
            int samples = bytes_read / sizeof(int32_t);
            int pairs = samples / 2;
            if (pairs <= 0) continue;

            const int MAX_OUT = 64; // 64 samples -> 128 bytes per chunk
            int16_t outbuf[MAX_OUT];
            int to_convert = pairs < MAX_OUT ? pairs : MAX_OUT;
            for (int i = 0; i < to_convert; ++i) {
                int32_t left = i2s_read_buff[i * 2];
                outbuf[i] = (int16_t)(left >> 8);
            }
            size_t bytes_to_send = (size_t)to_convert * sizeof(int16_t);

            // write small fixed-size chunks reliably (do NOT rely solely on availableForWrite)
            const uint8_t* src = (const uint8_t*)outbuf;
            size_t written = 0;
            unsigned long write_deadline = millis() + 1000; // allow up to 1s to write this chunk
            while (written < bytes_to_send && client && client.connected()) {
                size_t chunk = bytes_to_send - written;
                if (chunk > 64) chunk = 64; // write <=64 bytes per attempt
                int w = client.write(src + written, chunk);
                if (w > 0) {
                    written += (size_t)w;
                } else {
                    // no progress; yield and retry until deadline
                    vTaskDelay(2 / portTICK_PERIOD_MS);
                    if (millis() > write_deadline) break;
                }
            }

            if (written > 0) {
                consecutiveEmptySends = 0;
                if (!firstAudioSent) {
                    Serial.printf("Streamer: first audio block sent (%u bytes)\n", (unsigned)written);
                    firstAudioSent = true;
                }
            } else {
                consecutiveEmptySends++;
                // If we repeatedly can't write, break the client to allow reconnect
                if (consecutiveEmptySends >= 10) {
                    Serial.println("Streamer: unable to write audio for multiple attempts — closing client");
                    break;
                }
            }

            // occasional debug: show bytes_read / written
            static unsigned long dbg_ts = 0;
            if (millis() - dbg_ts > 2000) {
                dbg_ts = millis();
                Serial.printf("Streamer debug: bytes_read=%u, last_written=%u\n", (unsigned)bytes_read, (unsigned)written);
            }

            // give WiFi stack a chance
            vTaskDelay(1 / portTICK_PERIOD_MS);
        }

        // Ensure client is closed and log it
        if (client) {
            client.stop();
        }
        Serial.println("Streamer: client disconnected");
        // brief pause before accepting next client
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// Helper: map WiFi.status() to readable text
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

void setup() {
    Serial.begin(115200);
    I2SSetup();

    Serial.println();
    Serial.println("=== WiFi connect sequence ===");
    Serial.printf("Attempting to connect to SSID: '%s'\n", ssid);
    Serial.printf("Device MAC: %s\n", WiFi.macAddress().c_str());

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    // Do a single scan up-front so we can report whether the AP is visible.
    Serial.println("Performing initial WiFi scan...");
    int n = WiFi.scanNetworks();
    bool ssid_found = false;
    if (n == 0) {
        Serial.println("  No networks found in initial scan.");
    } else {
        for (int i = 0; i < n; ++i) {
            String foundSsid = WiFi.SSID(i);
            int rssi = WiFi.RSSI(i);
            String enc = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "secure";
            Serial.printf("  %d: %s (%d dBm) %s\n", i, foundSsid.c_str(), rssi, enc.c_str());
            if (foundSsid == ssid) {
                Serial.printf("    -> Target SSID '%s' FOUND (RSSI=%d dBm)\n", foundSsid.c_str(), rssi);
                ssid_found = true;
            }
        }
    }
    WiFi.scanDelete();
    if (!ssid_found) {
        Serial.printf("Warning: target SSID '%s' not seen in scan. It may be hidden or out of range.\n", ssid);
    }

    const int max_retries = 300;
    const unsigned long connect_timeout_ms = 180UL * 1000UL; // TODO: 180 seconds per attempt while debugging
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
            WiFi.disconnect(true, true); // disconnect and erase AP info to force a fresh attempt
            delay(500);
        }
    }

    if (connected && WiFi.status() == WL_CONNECTED) {
        Serial.println("Connected to WiFi");
        Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
        Serial.printf("Subnet: %s\n", WiFi.subnetMask().toString().c_str());
        Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());

        // Improve link performance: disable WiFi power-save and enable common PHYs.
        esp_err_t err;

        // Disable WiFi power save (modem sleep)
        err = esp_wifi_set_ps(WIFI_PS_NONE);
        Serial.printf("esp_wifi_set_ps(WIFI_PS_NONE) -> %d\n", (int)err);

        // Also tell Arduino wrapper to disable sleep (best-effort)
        WiFi.setSleep(false);

        // Enable 11b/11g/11n protocols explicitly so HT is available
        err = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
        Serial.printf("esp_wifi_set_protocol(STA, 11B|11G|11N) -> %d\n", (int)err);

        // Print AP info (channel, RSSI, auth) to help debug PHY negotiation
        wifi_ap_record_t apinfo;
        if (esp_wifi_sta_get_ap_info(&apinfo) == ESP_OK) {
            Serial.printf("AP info: SSID=%s, primary_chan=%d, rssi=%d, authmode=%d\n",
                apinfo.ssid, apinfo.primary, apinfo.rssi, apinfo.authmode);
        } else {
            Serial.println("esp_wifi_sta_get_ap_info() failed");
        }

        // If all else fails, raise tx power:
        //esp_wifi_set_max_tx_power(78);  // 78 = ~19.5dBm
    } else {
         Serial.printf("Final WiFi status after retries: %s (0x%02X)\n", wifiStatusStr(WiFi.status()), (int)WiFi.status());
         Serial.println("Giving up connecting to WiFi. Check SSID/password, visibility, and AP signal.");
     }

    // Start the raw stream server on :8080
    streamServer.begin();
    Serial.println("Stream server listening on port 8080");
    xTaskCreatePinnedToCore(streamingTask, "streamingTask", 8192, NULL, 1, NULL, 1);
}

void loop() {
    // small delay/yield
    delay(1);
}
