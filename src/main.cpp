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

WebServer server(80);  // Set port

static void I2SSetup(void)
{
    i2s_config_t i2s_config = {
        // For INMP441 microphone
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 44100,
        // Use 32-bit slot width so MCLK/BCLK/WS remain accurate without
        // forcing a non-standard MCLK multiple. INMP441 is 24-bit data
        // left-justified inside a 32-bit slot.
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,        // 32-slot (24-bit data left-justified)
        // Use stereo (right+left) format while debugging so we can detect
        // which channel the microphone places its data (some mics use right).
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,       // stereo interleaved
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,   // 1
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2,            // (1<<2)
        .dma_buf_count = 3,
        .dma_buf_len = 300,
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

    // Explicitly set the standard clock: sample_rate, bits_per_sample and channels.
    // This ensures BCLK/WS are configured consistently for the chosen slot width.
    res = i2s_set_clk(I2S_PORT, i2s_config.sample_rate, i2s_config.bits_per_sample, I2S_CHANNEL_STEREO);
    if (res != ESP_OK) {
        Serial.printf("i2s_set_clk failed: %d\n", res);
        // continue — driver is installed; logging will show if reads are zero.
    }

    i2s_zero_dma_buffer(I2S_PORT);

    // quick self-test: read a small buffer and dump to Serial so we can verify real data
    {
        const int test_len = 64;
        int32_t test_buf[test_len];
        size_t bytes_read = 0;
        vTaskDelay(10 / portTICK_PERIOD_MS); // let clocks stabilize
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


// Function to send the WAV header to the client
void sendWavHeader() {
    // 16-bit PCM, mono, 44100 Hz
    byte header[44] = {
        0x52, 0x49, 0x46, 0x46, // RIFF
        0xFF, 0xFF, 0xFF, 0xFF, // File size, placeholder to be replaced
        0x57, 0x41, 0x56, 0x45, // WAVE
        0x66, 0x6D, 0x74, 0x20, // fmt 
        0x10, 0x00, 0x00, 0x00, // Subchunk1Size (16 for PCM)
        0x01, 0x00,             // AudioFormat (PCM = 1)
        0x01, 0x00,             // NumChannels (Mono = 1)
        0x44, 0xAC, 0x00, 0x00, // SampleRate (44100 Hz)
        0x88, 0x58, 0x01, 0x00, // ByteRate (44100 * 1 * 16/8 = 88200)
        0x02, 0x00,             // BlockAlign (1 * 16/8 = 2)
        0x10, 0x00,             // BitsPerSample (16)
        0x64, 0x61, 0x74, 0x61, // data
        0xFF, 0xFF, 0xFF, 0xFF  // Subchunk2Size (data size, placeholder to be replaced)
    };
    // send raw bytes (may contain NULs) directly to the client socket
    WiFiClient client = server.client();
    if (client && client.connected()) {
        client.write(header, sizeof(header));
    }
}


void handleAudioStream() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "audio/wav", "");
 
    sendWavHeader();
 
    const int i2s_read_len = 1024;
    // read into 32-bit words (driver returns 32-bit per slot)
    int32_t i2s_read_buff[i2s_read_len];
 
    while (true) {
        size_t bytes_read = 0;
        esp_err_t i2s_read_result = i2s_read(I2S_PORT, (void*) i2s_read_buff, i2s_read_len * sizeof(int32_t), &bytes_read, portMAX_DELAY);
 
        if (i2s_read_result == ESP_OK && bytes_read > 0) {
            // Log bytes read and a small stereo sample dump (Left,Right pairs)
            Serial.printf("I2S read bytes: %u\n", (unsigned)bytes_read);
            int samples = bytes_read / sizeof(int32_t); // total slot samples
            int pairs = samples / 2;                    // stereo pairs
            int dump = pairs < 8 ? pairs : 8;
            Serial.print("Sample pairs (L,R): ");
            for (int i = 0; i < dump; ++i) {
                uint32_t left = (uint32_t)i2s_read_buff[i*2];
                uint32_t right = (uint32_t)i2s_read_buff[i*2 + 1];
                Serial.printf("[%08X,%08X] ", left, right);
            }
            Serial.println();
 
            // Convert stereo 32-bit (left-justified 24-in-32) -> mono 16-bit (left channel)
            int out_count = pairs; // one mono sample per pair
            // allocate on stack if size reasonable; otherwise use a static buffer or malloc
            int16_t outbuf[512]; // ensure large enough for i2s_read_len=1024 -> pairs=512
            if (out_count > (int)(sizeof(outbuf)/sizeof(outbuf[0]))) {
                out_count = (int)(sizeof(outbuf)/sizeof(outbuf[0])); // clamp to buffer
            }
            for (int i = 0; i < out_count; ++i) {
                int32_t left = i2s_read_buff[i*2];
                // INMP441 provides 24-bit left-justified in 32 bits; shift right 8 to get 16-bit
                int16_t s16 = (int16_t)(left >> 8);
                outbuf[i] = s16;
            }
            size_t bytes_to_send = out_count * sizeof(int16_t);
 
            // debug dump first few 16-bit samples
            Serial.print("Output 16-bit samples: ");
            int dump16 = out_count < 8 ? out_count : 8;
            for (int i = 0; i < dump16; ++i) {
                Serial.printf("%04X ", (uint16_t)outbuf[i]);
            }
            Serial.println();
 
            // send converted 16-bit mono audio bytes to client socket
            WiFiClient client = server.client();
            if (client && client.connected()) {
                size_t written = 0;
                const uint8_t* src = (const uint8_t*)outbuf;
                while (written < bytes_to_send) {
                    int w = client.write(src + written, bytes_to_send - written);
                    if (w <= 0) break;
                    written += w;
                }
            }
         }
 
         if (!server.client().connected()) {
             Serial.println("Client Disconnected");
             break;
         }
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
    const unsigned long connect_timeout_ms = 180UL * 1000UL; // 180 seconds per attempt
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

    // Register HTTP handler and start server
    server.on("/stream", HTTP_GET, handleAudioStream);
    server.begin();
}

void loop() {
    // All work is done in the audio stream handler
}