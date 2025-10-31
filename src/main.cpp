#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <driver/i2s.h>

// Pin assignments for Heltec WiFi Kit 32 (Demo Board w/ LEDs and Buzzer)
#define MIC_SCK   33   // Position 15J
#define MIC_WS    25   // Position 15K
#define MIC_SDOUT 32   // Position 15L
#define I2S_PORT  I2S_NUM_0

// WiFi credentials are provided by include/secrets.h
#include "secrets.h"

// Convenience redirect
WebServer server(80);  
// Raw TCP server for the continuous audio stream (avoids blocking)
WiFiServer streamServer(8080);

static void I2SSetup(void) {
    i2s_config_t i2s_config = {
        // For INMP441 microphone
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        // TODO: Drop sample rate for testing; consider turning up later:
        .sample_rate = 16000,
         // INMP441 is 24-bit data left-justified inside a 32-bit slot.
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

void handleAudioStream() {
    // Redirect :80 clients to raw stream server on port 8080.
    String location = String("http://") + WiFi.localIP().toString() + ":8080/stream";
    server.sendHeader("Location", location);
    server.send(302, "text/plain", "Redirecting to stream server");
}

// Streaming task — runs as a separate per-client FreeRTOS task
//  Accepts a client, sends a WAV header, then streams
static void streamingTask(void *pvParameters) {
    (void)pvParameters;
    const int i2s_read_len = 256; // "smaller read improves latency"
    int32_t i2s_read_buff[i2s_read_len];

    // TODO: Replace hardcoded WAV header
    byte wavHeader[44] = {
        0x52,0x49,0x46,0x46, 0xFF,0xFF,0xFF,0xFF,
        0x57,0x41,0x56,0x45,
        0x66,0x6D,0x74,0x20,
        0x10,0x00,0x00,0x00,
        0x01,0x00,
        0x01,0x00,
        0x40,0x1F,0x00,0x00, // SampleRate (16000) -> 0x00001F40
        0x00,0x7D,0x00,0x00, // ByteRate (16000 * 1 * 16/8 = 32000 -> 0x00007D00) little-endian
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

        // TODO: Potential VLC behavior workaround — read request headers
        // Wait briefly for request data to arrive (avoid infinite block)
        unsigned long wait_deadline = millis() + 200;
        while (!client.available() && millis() < wait_deadline && client.connected()) {
            vTaskDelay(1 / portTICK_PERIOD_MS);
        }

        // --- Robustly read and discard the client's HTTP request headers ---
        unsigned long total_deadline = millis() + 1000; // wait up to 1s for headers
        String requestLine;
        bool firstLineLogged = false;
        bool headers_done = false;
        while (client.connected() && millis() < total_deadline && !headers_done) {
            while (client.available()) {
                String line = client.readStringUntil('\n'); // reads up to '\n'
                // trim trailing CR for easier checks
                if (line.endsWith("\r")) line.remove(line.length() - 1);
                if (!firstLineLogged) {
                    requestLine = line;
                    Serial.printf("Streamer: request first line: %s\n", requestLine.c_str());
                    firstLineLogged = true;
                }
                // end of headers: blank line
                if (line.length() == 0) {
                    headers_done = true;
                    break;
                }
                // continue reading remaining header lines
            }
            if (!headers_done) vTaskDelay(5 / portTICK_PERIOD_MS);
        }
        if (!firstLineLogged) {
            Serial.println("Streamer: no request headers received (timeout), proceeding to respond");
        } else if (!headers_done) {
            Serial.println("Streamer: header read timed out but proceeding to respond");
        }

        // Send HTTP headers + raw WAV header (no Content-Length; keep connection open)
        client.print("HTTP/1.1 200 OK\r\n");
        client.print("Content-Type: audio/wav\r\n");
        client.print("Cache-Control: no-cache\r\n");
        client.print("Connection: keep-alive\r\n");
        client.print("\r\n");
        
        // WAV header: sample rate = 16000 (0x00003E80 -> bytes 0x80,0x3E,0x00,0x00)
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
        client.write(streamingWavHeader, sizeof(streamingWavHeader));
         // Ensure header bytes are flushed to client
         client.flush();
         Serial.println("Streamer: response headers & WAV header sent, entering stream loop");
 
         // Stream loop
         while (client && client.connected()) {
            size_t bytes_read = 0;
            // bounded timeout so this task yields occasionally
            esp_err_t r = i2s_read(I2S_PORT, i2s_read_buff, i2s_read_len * sizeof(int32_t), &bytes_read, 100 / portTICK_PERIOD_MS);
            if (r != ESP_OK || bytes_read == 0) {
                // no data this cycle; let other tasks run
                vTaskDelay(5 / portTICK_PERIOD_MS);
                continue;
            }

            // Convert interleaved stereo 32-bit -> mono 16-bit (left channel)
            int samples = bytes_read / sizeof(int32_t);
            int pairs = samples / 2;
            if (pairs <= 0) continue;

            // Allocate small temp buffer on stack (pairs <= 128 here)
            int16_t outbuf[128];
            int out_count = pairs;

            // TODO: Simplify this abomination 
            if (out_count > (int)(sizeof(outbuf)/sizeof(outbuf[0]))) out_count = (int)(sizeof(outbuf)/sizeof(outbuf[0]));

            for (int i = 0; i < out_count; ++i) {
                int32_t left = i2s_read_buff[i*2];
                int16_t s16 = (int16_t)(left >> 8);
                outbuf[i] = s16;
            }
            size_t bytes_to_send = out_count * sizeof(int16_t);

            // send in small chunks, yield if socket buffer full
            const uint8_t* src = (const uint8_t*)outbuf;
            size_t sent = 0;
            unsigned long deadline = millis() + 200;
            while (sent < bytes_to_send && client && client.connected()) {
                int canWrite = client.availableForWrite();
                if (canWrite <= 0) {
                    // let stacks run
                    vTaskDelay(1 / portTICK_PERIOD_MS);
                    if (millis() > deadline) break;
                    continue;
                }
                size_t chunk = (size_t)canWrite;
                if (chunk > bytes_to_send - sent) chunk = bytes_to_send - sent;
                int w = client.write(src + sent, chunk);
                if (w <= 0) break;
                sent += (size_t)w;
            }

            // TODO: Debugging
            static bool firstAudioSent = false;
            if (!firstAudioSent && sent > 0) {
                Serial.printf("Streamer: first audio block sent (%u bytes)\n", (unsigned)sent);
                firstAudioSent = true;
            }
             // yield to allow WiFi and other tasks to run
             taskYIELD();
         }

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
    } else {
        Serial.printf("Final WiFi status after retries: %s (0x%02X)\n", wifiStatusStr(WiFi.status()), (int)WiFi.status());
        Serial.println("Giving up connecting to WiFi. Check SSID/password, visibility, and AP signal.");
    }

    // Register HTTP handler (redirect)
    server.on("/stream", HTTP_GET, handleAudioStream);
    server.begin();

    // Start the raw stream server
    streamServer.begin();
    Serial.println("Stream server listening on port 8080");
    xTaskCreatePinnedToCore(streamingTask, "streamingTask", 8192, NULL, 1, NULL, 1);
}

void loop() {
    // Call handleClient() so WebServer processes incoming HTTP requests
    // Without this the server won't respond.
    server.handleClient();

    // small delay/yield
    delay(1);
}