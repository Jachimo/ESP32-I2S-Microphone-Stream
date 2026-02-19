#include "i2s_manager.h"
#include "config.h"
#include <Arduino.h>
#include <driver/i2s.h>

void I2SSetup(void) {
    Serial.println("I2SSetup: Starting I2S configuration...");
    
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,  // Read BOTH channels
        .communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_I2S,  // Changed from STAND_I2S
        .intr_alloc_flags = 0,
        .dma_buf_count = AUDIO_DMA_BUF_COUNT,
        .dma_buf_len = AUDIO_DMA_BUF_LEN,
        .use_apll = true,  // Use APLL for more accurate clock generation
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = MIC_SCK,
        .ws_io_num  = MIC_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = MIC_SDOUT
    };

    Serial.println("I2SSetup: Installing I2S driver...");
    esp_err_t res = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    Serial.printf("I2SSetup: i2s_driver_install() -> %d\n", res);
    if (res != ESP_OK) {
        Serial.printf("i2s_driver_install failed: %d\n", res);
        return;
    }
    
    Serial.println("I2SSetup: Setting I2S pins...");
    res = i2s_set_pin(I2S_PORT, &pin_config);
    Serial.printf("I2SSetup: i2s_set_pin() -> %d\n", res);
    if (res != ESP_OK) {
        Serial.printf("i2s_set_pin failed: %d\n", res);
        return;
    }
    
    // NOTE: Removed i2s_set_clk() call - let driver auto-configure from i2s_config
    Serial.println("I2SSetup: Clock auto-configured from i2s_config");
    
    Serial.println("I2SSetup: Zeroing DMA buffer...");
    i2s_zero_dma_buffer(I2S_PORT);
    Serial.println("I2SSetup: Complete!");
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
        Serial.print("Self-test samples (L/R alternating): ");
        for (int i = 0; i < dump; ++i) {
            uint32_t v = (uint32_t)test_buf[i];
            Serial.printf("%08X ", v);
        }
        Serial.println();
    } else {
        Serial.println("Self-test: no data or read failed");
    }
}