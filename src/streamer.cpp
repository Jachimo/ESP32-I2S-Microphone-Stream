#include "streamer.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <driver/i2s.h>

// create the server inside this module
static WiFiServer s_streamServer(8080);

static void streamingTask(void *pvParameters) {
    (void)pvParameters;
    const int i2s_read_len = AUDIO_DMA_BUF_LEN; // match DMA buffer
    int32_t i2s_read_buff[i2s_read_len];

    // WAV header (16000 Hz, 16-bit mono)
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

    s_streamServer.begin();
    Serial.println("Stream server listening on port 8080");

    while (true) {
        WiFiClient client = s_streamServer.available();
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

        // small immediate test pattern to verify wire
        {
            const int TEST_SAMPLES = 64;
            int16_t testbuf[TEST_SAMPLES];
            for (int i = 0; i < TEST_SAMPLES; ++i) testbuf[i] = (int16_t)((i & 0xff) - 128) * 64;
            client.write((const uint8_t*)testbuf, TEST_SAMPLES * sizeof(int16_t));
            client.flush();
            Serial.printf("Streamer: test pattern sent (%u bytes)\n", (unsigned)(TEST_SAMPLES * sizeof(int16_t)));
        }

        // Stream loop
        while (client && client.connected()) {
            size_t bytes_read = 0;
            esp_err_t r = i2s_read(I2S_PORT, i2s_read_buff, i2s_read_len * sizeof(int32_t), &bytes_read, 200 / portTICK_PERIOD_MS);
            if (r != ESP_OK || bytes_read == 0) {
                vTaskDelay(5 / portTICK_PERIOD_MS);
                continue;
            }

            int samples = bytes_read / sizeof(int32_t);
            if (samples <= 0) continue;

            // convert in blocks and write larger chunks
            const int MAX_OUT = 256;
            int to_convert = samples < MAX_OUT ? samples : MAX_OUT;
            int16_t outbuf[MAX_OUT];
            for (int i = 0; i < to_convert; ++i) {
                int32_t s32 = i2s_read_buff[i];
                outbuf[i] = (int16_t)(s32 >> 8);
            }
            size_t bytes_to_send = (size_t)to_convert * sizeof(int16_t);

            const uint8_t* src = (const uint8_t*)outbuf;
            size_t written = 0;
            unsigned long write_deadline = millis() + 1000;
            while (written < bytes_to_send && client && client.connected()) {
                size_t chunk = bytes_to_send - written;
                if (chunk > 1024) chunk = 1024;
                int w = client.write(src + written, chunk);
                if (w > 0) {
                    written += (size_t)w;
                } else {
                    vTaskDelay(1 / portTICK_PERIOD_MS);
                    if (millis() > write_deadline) break;
                }
            }

            // debug
            static unsigned long dbg_ts = 0;
            if (millis() - dbg_ts > 2000) {
                dbg_ts = millis();
                Serial.printf("Streamer debug: bytes_read=%u, last_written=%u\n", (unsigned)bytes_read, (unsigned)written);
            }

            if (written == 0) {
                static int stall_count = 0;
                if (++stall_count > 10) {
                    Serial.println("Streamer: write stalled repeatedly, closing client");
                    break;
                }
            }

            vTaskDelay(1 / portTICK_PERIOD_MS);
        }

        if (client) client.stop();
        Serial.println("Streamer: client disconnected");
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void start_streamer(void) {
    xTaskCreatePinnedToCore(streamingTask, "streamingTask", 8192, NULL, 1, NULL, 1);
}