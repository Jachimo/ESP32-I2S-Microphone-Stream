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
    // Layout (little-endian):
    // 0..3   "RIFF"
    // 4..7   ChunkSize (unknown -> 0xFFFFFFFF)
    // 8..11  "WAVE"
    // 12..15 "fmt "
    // 16..19 Subchunk1Size = 16
    // 20..21 AudioFormat = 7 (mu-law)
    // 22..23 NumChannels = 1
    // 24..27 SampleRate = 8000
    // 28..31 ByteRate = SampleRate * NumChannels * BytesPerSample = 8000
    // 32..33 BlockAlign = 1
    // 34..35 BitsPerSample = 8
    // 36..39 "data"
    // 40..43 Subchunk2Size = 0xFFFFFFFF (unknown/streaming)
    uint8_t streamingWavHeader[44] = {
        'R','I','F','F',
        0xFF,0xFF,0xFF,0xFF,   // ChunkSize (unknown)
        'W','A','V','E',
        'f','m','t',' ',
        0x10,0x00,0x00,0x00,   // Subchunk1Size = 16
        0x07,0x00,             // AudioFormat = 7 (mu-law)
        0x01,0x00,             // NumChannels = 1
        0x40,0x1F,0x00,0x00,   // SampleRate = 8000 (0x00001F40)
        0x40,0x1F,0x00,0x00,   // ByteRate = 8000 (same as sample rate for 1B/sample)
        0x01,0x00,             // BlockAlign = 1
        0x08,0x00,             // BitsPerSample = 8
        'd','a','t','a',
        0xFF,0xFF,0xFF,0xFF    // Subchunk2Size (unknown)
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

        // Send response headers + correct WAV header
        client.print("HTTP/1.1 200 OK\r\n");
        client.print("Content-Type: audio/wav\r\n");
        client.print("Cache-Control: no-cache\r\n");
        client.print("Connection: keep-alive\r\n");
        client.print("\r\n");
        client.write(streamingWavHeader, sizeof(streamingWavHeader));
        client.flush();
        Serial.println("Streamer: response headers & WAV header sent, entering stream loop");

        // Stream loop: read, convert to μ-law and send small, regular packets (~20ms)
        size_t total_sent_since_ts = 0;
        unsigned long ts = millis();

        

        // Send fixed-size chunks ~20ms of audio 
        const int SAMPLES_PER_CHUNK = 160; // 20 ms @ 8 kHz = 160 bytes (μ-law)
        uint8_t sendbuf[SAMPLES_PER_CHUNK];

        while (client && client.connected()) {
            // Read exactly SAMPLES_PER_CHUNK samples (each sample is 1 32-bit slot from I2S)
            size_t bytes_needed = SAMPLES_PER_CHUNK * sizeof(int32_t);
            size_t bytes_read = 0;
            esp_err_t r = i2s_read(I2S_PORT, i2s_read_buff, bytes_needed, &bytes_read, (SAMPLES_PER_CHUNK / 1000 + 40) / portTICK_PERIOD_MS);
            if (r != ESP_OK || bytes_read == 0) {
                // no data available now; give WiFi/lwip time and retry quickly
                vTaskDelay(2 / portTICK_PERIOD_MS);
                continue;
            }

            int samples = bytes_read / sizeof(int32_t);
            if (samples <= 0) continue;

            // convert the samples to μ-law bytes
            for (int i = 0; i < samples; ++i) {
                int32_t s32 = i2s_read_buff[i];
                int16_t s16 = (int16_t)(s32 >> 8);
                sendbuf[i] = linear_to_mulaw(s16);
            }

            // send the chunk in one write (or retry briefly on EAGAIN)
            size_t written = 0;
            unsigned long write_deadline = millis() + 200;
            while (written < (size_t)samples && client && client.connected()) {
                int w = client.write(sendbuf + written, samples - written);
                if (w > 0) {
                    written += (size_t)w;
                } else {
                    vTaskDelay(1 / portTICK_PERIOD_MS);
                    if (millis() > write_deadline) break;
                }
            }

            total_sent_since_ts += written;
            // throughput debug every 1s
            if (millis() - ts >= 1000) {
                Serial.printf("Streamer throughput: %u B/s\n", (unsigned)total_sent_since_ts);
                total_sent_since_ts = 0;
                ts = millis();
            }

            // small yield to let lwIP and WiFi tasks run, avoid long sleeps
            taskYIELD();
        }

        if (client) client.stop();
        Serial.println("Streamer: client disconnected");
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void start_streamer(void) {
    xTaskCreatePinnedToCore(streamingTask, "streamingTask", 8192, NULL, 1, NULL, 1);
}