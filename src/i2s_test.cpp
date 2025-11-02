#include <Arduino.h>
#include <driver/i2s.h>

// Pin mapping for Heltec WiFi Kit 32 + INMP441
#define I2S_WS   25  // LRCL
#define I2S_SD   32  // DOUT
#define I2S_SCK  33  // BCLK

#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE 16000
#define DMA_BUF_LEN 128

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("ESP32 I2S INMP441 Test");

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 3,
        .dma_buf_len = DMA_BUF_LEN,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = -1,      // Not used
        .data_in_num = I2S_SD
    };

    esp_err_t err;
    err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    Serial.printf("i2s_driver_install: %d\n", err);
    err = i2s_set_pin(I2S_PORT, &pin_config);
    Serial.printf("i2s_set_pin: %d\n", err);
    err = i2s_set_clk(I2S_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_MONO);
    Serial.printf("i2s_set_clk: %d\n", err);
}

void loop() {
    static int32_t i2s_read_buff[DMA_BUF_LEN];
    size_t bytes_read = 0;
    esp_err_t r = i2s_read(I2S_PORT, (void*)i2s_read_buff, sizeof(i2s_read_buff), &bytes_read, 1000 / portTICK_PERIOD_MS);

    if (r == ESP_OK && bytes_read > 0) {
        int samples = bytes_read / sizeof(int32_t);
        Serial.printf("Read %d samples: ", samples);
        for (int i = 0; i < min(samples, 8); ++i) {
            Serial.printf("%ld ", i2s_read_buff[i]);
        }
        Serial.println();
    } else {
        Serial.printf("i2s_read error: %d bytes_read: %u\n", r, (unsigned)bytes_read);
    }
    delay(500);
}