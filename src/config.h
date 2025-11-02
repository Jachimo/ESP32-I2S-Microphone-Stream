#ifndef CONFIG_H
#define CONFIG_H

#include "secrets.h" // must define const char* ssid; const char* password;

#define MIC_SCK   26
#define MIC_WS    25
#define MIC_SDOUT 32
#define I2S_PORT  I2S_NUM_0

#define AUDIO_SAMPLE_RATE 16000  // changing to 16000 from 8000 to see if that works...
#define AUDIO_BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_32BIT
#define AUDIO_DMA_BUF_COUNT 3
#define AUDIO_DMA_BUF_LEN 128

#endif // CONFIG_H
