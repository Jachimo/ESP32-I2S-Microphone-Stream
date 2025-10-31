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
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,         // 4
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
    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
    i2s_zero_dma_buffer(I2S_PORT);
}


// Function to send the WAV header to the client
void sendWavHeader() {
    // 32-bit PCM, mono, 44100 Hz
    byte header[44] = {
        0x52, 0x49, 0x46, 0x46, // RIFF
        0xFF, 0xFF, 0xFF, 0xFF, // File size, placeholder to be replaced
        0x57, 0x41, 0x56, 0x45, // WAVE
        0x66, 0x6D, 0x74, 0x20, // fmt 
        0x10, 0x00, 0x00, 0x00, // Subchunk1Size (16 for PCM)
        0x01, 0x00,             // AudioFormat (PCM = 1)
        0x01, 0x00,             // NumChannels (Mono = 1)
        0x44, 0xAC, 0x00, 0x00, // SampleRate (44100 Hz)
        0x10, 0xB1, 0x02, 0x00, // ByteRate (44100 * 1 * 32/8 = 176400)
        0x04, 0x00,             // BlockAlign (1 * 32/8 = 4)
        0x20, 0x00,             // BitsPerSample (32)
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
            // Log bytes read and a small sample dump for debugging
            Serial.printf("I2S read bytes: %u\n", (unsigned)bytes_read);
            int samples = bytes_read / sizeof(int32_t);
            int dump = samples < 8 ? samples : 8;
            Serial.print("Sample hex: ");
            for (int i = 0; i < dump; ++i) {
                Serial.printf("%08X ", (uint32_t)i2s_read_buff[i]);
            }
            Serial.println();
 
            // send raw bytes to client socket
            WiFiClient client = server.client();
            if (client && client.connected()) {
                size_t written = 0;
                while (written < bytes_read) {
                    written += client.write(((const uint8_t*)i2s_read_buff) + written, bytes_read - written);
                }
            }
         }
 
         if (!server.client().connected()) {
             Serial.println("Client Disconnected");
             break;
         }
     }
 }

void setup() {
    Serial.begin(115200);
    I2SSetup();

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.println("Connecting to WiFi...");
    }
    Serial.println("Connected to WiFi");

    server.on("/stream", HTTP_GET, handleAudioStream);

    server.begin();
}

void loop() {
    server.handleClient();
}