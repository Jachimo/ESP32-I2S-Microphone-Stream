#include "i2s_manager.h"
#include "config.h"
#include <Arduino.h>
#include <driver/i2s.h>

void I2SSetup(void) {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = 0,
        .dma_buf_count = AUDIO_DMA_BUF_COUNT,
        .dma_buf_len = AUDIO_DMA_BUF_LEN,
    };

    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = MIC_SCK,
        .ws_io_num  = MIC_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = MIC_SDOUT
    };

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
    res = i2s_set_clk(I2S_PORT, i2s_config.sample_rate, i2s_config.bits_per_sample, I2S_CHANNEL_MONO);
    Serial.printf("i2s_set_clk(sample_rate=%u, bits=%u, channels=%s) -> %d\n",
        i2s_config.sample_rate,
        (unsigned)i2s_config.bits_per_sample,
        (I2S_CHANNEL_MONO == I2S_CHANNEL_MONO) ? "MONO" : "STEREO",
        (int)res);
    if (res != ESP_OK) {
        Serial.printf("i2s_set_clk failed: %d\n", res);
    }
    i2s_zero_dma_buffer(I2S_PORT);
}

void I2SSelfTest(void) {
    const int test_len = 64;
    int32_t test_buf[test_len];
    size_t bytes_read = 0;
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_err_t r = i2s_read(I2S_PORT, test_buf, sizeof(test_buf), &bytes_read, 100 / portTICK_PERIOD_MS);
    Serial.printf("I2S self-test read bytes: %u\n", (unsigned)bytes_read);
    if (r == ESP_OK && bytes_read > 0) {
        int samples = bytes_read / sizeof(int32_t);
        int dump = samples < 8 ? samples : 8;
        Serial.print("Self-test samples (left only): ");
        for (int i = 0; i < dump; ++i) {
            uint32_t v = (uint32_t)test_buf[i];
            Serial.printf("%08X ", v);
        }
        Serial.println();
    } else {
        Serial.println("Self-test: no data or read failed");
    }
}