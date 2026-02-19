# INMP441 I2S Microphone Configuration Guide

## Overview

The INMP441 is a **mono MEMS microphone** that uses the **I2S digital audio interface**. Configuring it correctly on the ESP32 requires understanding the relationship between mono audio output and the stereo I2S protocol.

## Key Concepts

### I2S Protocol is Always Stereo

The I2S (Inter-IC Sound) protocol **always transmits two time slots per frame** - one for left channel, one for right channel. This is true even when using mono microphones like the INMP441.

**Per-frame bit transmission:**
```
Sample Rate: 16 kHz
Bits per Sample: 32 bits
Channels: 2 (left + right time slots)
Bit Clock = 16000 Hz × 32 bits × 2 = 1,024,000 Hz
```

### INMP441 Channel Selection

The INMP441 outputs audio on **either the left or right channel**, determined by its L/R pin:
- **L/R pin LOW (or GND)**: Outputs on LEFT channel (when WS=LOW)
- **L/R pin HIGH (or VDD)**: Outputs on RIGHT channel (when WS=HIGH)

In our configuration, the INMP441 L/R pin is typically tied HIGH, so it outputs on the **right channel**.

## ESP32 I2S Configuration

### Channel Format vs. Clock Configuration

There are two separate but related I2S settings:

#### 1. Channel Format (in `i2s_config_t`)
```cpp
.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT
```

This tells the ESP32 **which channel(s) to read and store in memory**:
- `I2S_CHANNEL_FMT_ONLY_LEFT`: Read only left channel, discard right
- `I2S_CHANNEL_FMT_ONLY_RIGHT`: Read only right channel, discard left  
- `I2S_CHANNEL_FMT_RIGHT_LEFT`: Read both channels

**Result:** Each call to `i2s_read()` returns ONE sample per frame (just the right channel data).

#### 2. Clock Configuration (in `i2s_set_clk()`)
```cpp
i2s_set_clk(I2S_PORT, sample_rate, bits_per_sample, I2S_CHANNEL_STEREO)
```

This tells the ESP32 **how to calculate the bit clock rate**:
- `I2S_CHANNEL_MONO`: Bit clock = sample_rate × bits_per_sample × 1
- `I2S_CHANNEL_STEREO`: Bit clock = sample_rate × bits_per_sample × 2

**Critical:** Even though we're only *reading* one channel, the I2S bus still transmits two time slots per frame, so we must use `I2S_CHANNEL_STEREO` for correct clock timing.

## Correct Configuration for INMP441

```cpp
// In i2s_config_t
.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
.sample_rate = 16000,
.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,  // Read only right channel
.communication_format = I2S_COMM_FORMAT_STAND_I2S,

// Later, in i2s_set_clk()
i2s_set_clk(I2S_PORT, 16000, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_STEREO);
//                                                       ^^^^^^^^^^^^^^^^^^
//                                                       STEREO for correct bit clock!
```

## Common Mistake: Using I2S_CHANNEL_MONO

### Symptoms of Incorrect Configuration

If you mistakenly use `I2S_CHANNEL_MONO` in `i2s_set_clk()`:

```cpp
// WRONG - causes slow sample rate
i2s_set_clk(I2S_PORT, 16000, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_MONO);
```

**Observed behavior:**
- **Expected:** 16,000 samples/sec
- **Actual:** ~256 samples/sec (62.5× slower)
- **Streamer throughput:** 0 B/s (buffer never fills)
- **Audio stream:** Only WAV header sent, no audio data

### Why This Happens

With MONO clock setting, the ESP32 calculates:
```
Bit Clock = 16000 Hz × 32 bits × 1 = 512,000 Hz
```

But the INMP441 expects:
```
Bit Clock = 16000 Hz × 32 bits × 2 = 1,024,000 Hz
```

The clock is half the required rate, so the effective sample rate becomes 16000 ÷ 2 = 8000 Hz. Combined with the driver only capturing one channel from the two-slot frames, you end up with ~256 samples/sec.

## Verification

After correct configuration, you should observe:

1. **Self-test shows alternating pattern:**
   ```
   Self-test samples: FFF93DC0 00000000 FFF8AC00 00000000 FFF94800 00000000
                      ^right    ^left    ^right    ^left    ^right    ^left
   ```
   (Left channel is zero, right channel has microphone data)

2. **I2S stats show correct sample rate:**
   ```
   I2S_STATS: samples/sec=16000
   ```

3. **Streaming throughput is non-zero:**
   ```
   Streamer throughput: 16000 B/s
   ```

## Summary

**The INMP441 is mono, but the I2S protocol is stereo.**

- Use `I2S_CHANNEL_FMT_ONLY_RIGHT` to read only the microphone channel
- Use `I2S_CHANNEL_STEREO` in clock config to match the two-slot I2S frame timing
- This combination gives you mono audio at the correct sample rate

## References

- [INMP441 Datasheet](https://invensense.tdk.com/wp-content/uploads/2015/02/INMP441.pdf)
- [ESP32 I2S Driver Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2s.html)
- I2S Bus Specification (NXP/Philips)
