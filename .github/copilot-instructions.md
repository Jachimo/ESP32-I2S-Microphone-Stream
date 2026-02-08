# ESP32 I2S Microphone Stream - Copilot Instructions

## Build & Upload

```bash
# Build the firmware
pio run

# Upload to device (monitor at 115200 baud)
pio run --target upload
pio device monitor

# Or combine upload + monitor
pio run --target upload && pio device monitor
```

## Project Architecture

This is an ESP32 Arduino/PlatformIO project that streams INMP441 I2S microphone audio to HTTP clients as μ-law WAV.

### Core Components

1. **I2S Manager** (`i2s_manager.cpp`) - Configures ESP32 I2S peripheral for 16kHz, 32-bit samples, mono right channel from INMP441 microphone
2. **WiFi Manager** (`wifi_manager.cpp`) - Handles WiFi connection with extensive retry logic, verbose status logging, and power-save disabling for low-latency streaming
3. **Streamer** (`streamer.cpp`) - FreeRTOS task running WiFiServer on port 8080, reads I2S samples, converts 32→16-bit PCM, encodes to G.711 μ-law, streams as WAV over HTTP

### Data Flow

```
INMP441 mic → I2S peripheral (32-bit samples) → DMA buffers (128 samples × 3) 
  → i2s_read() → shift right 8 bits to 16-bit → μ-law encode → HTTP stream
```

- Sample rate: 16kHz (configurable in `config.h`)
- I2S reads 32-bit samples but only upper 24 bits contain data
- Converted to 16-bit, then compressed to 8-bit μ-law for streaming

## Key Conventions

### Hardware Pin Mapping (config.h)
- `MIC_SCK` (26): Serial clock
- `MIC_WS` (25): Word select
- `MIC_SDOUT` (32): Serial data out
- Board: Heltec WiFi Kit 32

### WiFi Credentials
Define `ssid` and `password` in `include/secrets.h` (header only) and implement as `extern const char*` in a `.cpp` file (gitignored). See `include/secrets.h` for pattern.

### I2S Configuration
- **DMA buffer sizing** (`AUDIO_DMA_BUF_LEN`): Must match read chunk size in streamer to avoid underruns
- **Channel format**: `I2S_CHANNEL_FMT_ONLY_RIGHT` - INMP441 outputs on right channel when WS=HIGH
- Always call `i2s_zero_dma_buffer()` after setup to clear noise

### Streaming Architecture
- **FreeRTOS task**: Streamer runs on core 1 with 8KB stack
- **Blocking reads**: Uses `i2s_read()` with calculated timeouts based on sample rate
- **Flow control**: Checks `client.availableForWrite()` before sending to prevent TCP backpressure
- Client connects to `http://<ESP32-IP>:8080/` - server auto-responds with infinite WAV stream

### Debug Output
- Serial monitor shows I2S stats (samples/sec), WiFi connection attempts with status codes, and streaming throughput (B/s)
- `I2SSelfTest()` dumps first 8 samples on startup for validation

## Common Modifications

### Change Sample Rate
Update `AUDIO_SAMPLE_RATE` in `config.h` and matching bytes in `streamingWavHeader` (bytes 24-31: sample rate and byte rate must match).

### Adjust DMA Buffers
Increase `AUDIO_DMA_BUF_COUNT` or `AUDIO_DMA_BUF_LEN` if experiencing audio dropouts. Ensure `SAMPLES_PER_CHUNK` in streamer doesn't exceed buffer length.

### Different Microphone
For non-INMP441 mics, may need to change `I2S_CHANNEL_FMT_ONLY_RIGHT` to `ONLY_LEFT` or `RIGHT_LEFT` depending on mic wiring and WS behavior.
