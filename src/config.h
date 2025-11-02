#ifndef CONFIG_H
#define CONFIG_H

#include "secrets.h" // must define const char* ssid; const char* password;

#define MIC_SCK   33
#define MIC_WS    25
#define MIC_SDOUT 32
#define I2S_PORT  I2S_NUM_0

// Use 16 kHz sample rate
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_32BIT
#define AUDIO_DMA_BUF_COUNT 3
#define AUDIO_DMA_BUF_LEN 128

#endif // CONFIG_H
