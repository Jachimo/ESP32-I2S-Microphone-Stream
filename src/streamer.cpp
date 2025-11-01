#include "streamer.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <driver/i2s.h>

// mu-law encoder for G.711 μ-law
static inline uint8_t linear_to_mulaw(int16_t pcm_val) {
    const uint16_t BIAS = 0x84; // 132
    uint16_t mask;
    uint16_t seg;
    uint8_t uval;
    int16_t pcm = pcm_val;
    if (pcm < 0) { pcm = -pcm; mask = 0x7F; } else { mask = 0xFF; }
    uint16_t pcm_u = (uint16_t)pcm + BIAS;
    if (pcm_u > 0x7FFF) pcm_u = 0x7FFF;
    // find segment (0..7)
    seg = 0;
    uint16_t tmp = pcm_u >> 7;
    while (tmp) { seg++; tmp >>= 1; }
    if (seg >= 8) {
        uval = (uint8_t)(0x7F ^ mask);
    } else {
        uval = (uint8_t)((seg << 4) | ((pcm_u >> (seg + 3)) & 0x0F));
        uval ^= mask;
    }
    return uval;
}

static WiFiServer s_streamServer(8080);

static void streamingTask(void *pvParameters) {
    (void)pvParameters;
    const int i2s_read_len = AUDIO_DMA_BUF_LEN; // match DMA buffer
    int32_t i2s_read_buff[i2s_read_len];

    // WAV header: 8 kHz, 8-bit μ-law, mono. WAVE format tag 7 = μ-law
    byte streamingWavHeader[44] = {
        'R','I','F','F', 0xFF,0xFF,0xFF,0xFF,
        'W','A','V','E',
        'f','m','t',' ',
        0x10,0x00,0x00,0x00,      // Subchunk1Size (16)
        0x07,0x00,                // AudioFormat (7 = μ-law)
        0x01,0x00,                // NumChannels (1)
        0x40,0x1F,0x00,0x00,      // SampleRate = 8000 (0x00001F40)
        0x40,0x1F,0x00,0x00,      // ByteRate = 8000 * 1 * 1 = 8000 (we'll set below)
        0x01,0x00,                // BlockAlign = 1
        0x08,0x00,                // BitsPerSample = 8
        'd','a','t','a',
        0xFF,0xFF,0xFF,0xFF
    };
    // Correct ByteRate and little-endian sample-rate/byterate:
    // sample rate 8000 -> 0x00001F40 -> bytes already set above; fix ByteRate
    streamingWavHeader[28] = 0x40; streamingWavHeader[29] = 0x1F; streamingWavHeader[30] = 0x00; streamingWavHeader[31] = 0x00;
    // ByteRate = 8000 -> bytes 32..35
    streamingWavHeader[32] = 0x40; streamingWavHeader[33] = 0x1F; streamingWavHeader[34] = 0x00; streamingWavHeader[35] = 0x00;

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
        size_t total_sent_since_ts = 0;
        unsigned long ts = millis();
        while (client && client.connected()) {
            size_t bytes_read = 0;
            esp_err_t r = i2s_read(I2S_PORT, i2s_read_buff, i2s_read_len * sizeof(int32_t), &bytes_read, 200 / portTICK_PERIOD_MS);
            if (r != ESP_OK || bytes_read == 0) {
                vTaskDelay(5 / portTICK_PERIOD_MS);
                continue;
            }
            int samples = bytes_read / sizeof(int32_t); // mono-left configuration -> one 32-bit per sample
            if (samples <= 0) continue;

            // convert up to CHUNK_SAMPLES samples to μ-law bytes
            const int CHUNK_SAMPLES = 256;
            int to_convert = samples < CHUNK_SAMPLES ? samples : CHUNK_SAMPLES;
            uint8_t mulaw_buf[CHUNK_SAMPLES];
            for (int i = 0; i < to_convert; ++i) {
                int32_t s32 = i2s_read_buff[i];
                int16_t s16 = (int16_t)(s32 >> 8); // 24-bit left-justified -> 16-bit
                mulaw_buf[i] = linear_to_mulaw(s16);
            }
            size_t bytes_to_send = (size_t)to_convert; // 1 byte per sample (μ-law)

            // write larger chunks reliably
            const uint8_t* src = mulaw_buf;
            size_t written = 0;
            unsigned long write_deadline = millis() + 1000;
            while (written < bytes_to_send && client && client.connected()) {
                size_t chunk = bytes_to_send - written;
                if (chunk > 1024) chunk = 1024;
                int w = client.write(src + written, chunk);
                if (w > 0) {
                    written += (size_t)w;
                    total_sent_since_ts += (size_t)w;
                } else {
                    vTaskDelay(1 / portTICK_PERIOD_MS);
                    if (millis() > write_deadline) break;
                }
            }

            // throughput debug: print bytes/sec every second
            if (millis() - ts >= 1000) {
                Serial.printf("Streamer throughput: %u B/s\n", (unsigned)total_sent_since_ts);
                total_sent_since_ts = 0;
                ts = millis();
            }

            if (written == 0) {
                static int stall_count = 0;
                if (++stall_count > 10) { Serial.println("Streamer: write stalled, closing client"); break; }
            } else {
                // reset stall_count on success
                // (declare stall_count above if needed)
            }

            // yield to WiFi/lwip
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