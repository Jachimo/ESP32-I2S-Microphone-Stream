# ESP32 I2S Microphone Stream - Testing Guide

This guide walks through the complete process of building, uploading, and testing the ESP32 I2S microphone streaming firmware.

## Prerequisites

- PlatformIO CLI installed (`pio` command available)
- ESP32 development board connected via USB
- INMP441 microphone wired to the ESP32 (see pin configuration in `include/config.h`)
- WiFi network credentials

## Step 1: Configure WiFi Credentials

### First Time Setup

Create the WiFi credentials file that won't be committed to git:

```bash
cat > src/secrets.cpp << 'EOF'
#include "secrets.h"

const char* ssid = "YourNetworkName";
const char* password = "YourNetworkPassword";
EOF
```

Replace `YourNetworkName` and `YourNetworkPassword` with your actual WiFi credentials.

**Verify the file was created:**
```bash
cat src/secrets.cpp
```

Expected output:
```cpp
#include "secrets.h"

const char* ssid = "YourNetworkName";
const char* password = "YourNetworkPassword";
```

## Step 2: Build the Firmware

Build the firmware without uploading:

```bash
pio run
```

**Expected output (abbreviated):**
```
Processing heltec_wifi_kit_32 (platform: espressif32 @ 6.6.0; ...)
...
Building in release mode
Compiling .pio/build/heltec_wifi_kit_32/src/main.cpp.o
...
Linking .pio/build/heltec_wifi_kit_32/firmware.elf
...
RAM:   [=         ]  13.4% (used 43836 bytes from 327680 bytes)
Flash: [======    ]  58.0% (used 760849 bytes from 1310720 bytes)
...
[SUCCESS] Took 15.23 seconds
```

**What this does:**
- Compiles all `.cpp` files in `src/` and `lib/`
- Links the firmware with ESP32 libraries
- Creates `firmware.elf` and `firmware.bin` in `.pio/build/heltec_wifi_kit_32/`
- Shows memory usage (RAM and Flash)

## Step 3: Upload to Device

### Upload Only

Upload the compiled firmware to the ESP32:

```bash
pio run --target upload
```

**Expected output (abbreviated):**
```
...
Configuring upload protocol...
CURRENT: upload_protocol = esptool
Looking for upload port...
Auto-detected: /dev/ttyUSB1
Uploading .pio/build/heltec_wifi_kit_32/firmware.bin
esptool.py v4.5.1
Serial port /dev/ttyUSB1
Connecting....
Chip is ESP32-D0WDQ6-V3 (revision v3.0)
...
Writing at 0x00010000... (3 %)
...
Writing at 0x000c95d6... (100 %)
Wrote 767520 bytes (497182 compressed) at 0x00010000 in 11.9 seconds
...
Hard resetting via RTS pin...
[SUCCESS] Took 25.17 seconds
```

**What this does:**
- Detects the USB serial port (e.g., `/dev/ttyUSB0` or `/dev/ttyUSB1`)
- Uploads firmware using esptool.py
- Resets the ESP32 to run the new firmware

### Upload + Monitor (Combined)

Most common workflow - upload and immediately start monitoring:

```bash
pio run --target upload && pio device monitor
```

**What this does:**
1. Builds the firmware (if needed)
2. Uploads to ESP32
3. Immediately connects serial monitor at 115200 baud
4. Shows live serial output from the device

**To exit monitor:** Press `Ctrl+C`

## Step 4: Monitor Serial Output

### Basic Monitoring

Connect to the serial port to see debug output:

```bash
pio device monitor
```

**Common options:**
- `--baud 115200` - Set baud rate (default is from platformio.ini)
- `--filter esp32_exception_decoder` - Decode crash stack traces
- `--port /dev/ttyUSB1` - Specify port manually

### Capturing Full Boot Sequence

The serial monitor often connects **after** the ESP32 has already booted, missing initial I2S setup messages.

**Method 1: Monitor then Reset**

1. Start monitoring:
   ```bash
   pio device monitor
   ```

2. Wait for connection message:
   ```
   --- Terminal on /dev/ttyUSB1 | 115200 8-N-1
   ```

3. **Press the RESET button on the ESP32** (or send Ctrl+T then R)

4. You'll now see the complete boot sequence from the beginning

**Method 2: Use screen or minicom**

Alternative serial monitors that you can attach before powering the board:

```bash
# Using screen
screen /dev/ttyUSB1 115200

# Using minicom
minicom -D /dev/ttyUSB1 -b 115200
```

Then power cycle or reset the ESP32.

### Expected Serial Output

**Complete successful boot sequence:**

```
rst:0x1 (POWERON_RESET),boot:0x16 (SPI_FAST_FLASH_BOOT)
configsip: 0, SPIWP:0xee
clk_drv:0x00,q_drv:0x00,d_drv:0x00,cs0_drv:0x00,hd_drv:0x00,wp_drv:0x00
mode:DIO, clock div:2
load:0x3fff0030,len:1184
load:0x40078000,len:13232
load:0x40080400,len:3028
entry 0x400805e4
I2SSetup: Starting I2S configuration...
I2SSetup: Installing I2S driver...
I2SSetup: i2s_driver_install() -> 0
I2SSetup: Setting I2S pins...
I2SSetup: i2s_set_pin() -> 0
I2SSetup: Setting I2S clock to STEREO mode for I2S protocol (I2S_CHANNEL_STEREO=2)...
i2s_set_clk(sample_rate=16000, bits=32, channels=STEREO [2]) -> 0
I2SSetup: Zeroing DMA buffer...
I2SSetup: Complete!
I2S self-test read bytes: 256
Self-test samples (left only): FFF93DC0 00000000 FFF8AC00 00000000 FFF94800 00000000 FFF8E800 00000000

=== WiFi connect sequence ===
Attempting to connect to SSID: 'YourNetworkName'
Device MAC: 78:21:84:99:8C:10
Performing initial WiFi scan...
  0: YourNetworkName (-56 dBm) secure
  1: OtherNetwork (-72 dBm) secure
...
Connect attempt 1/30: calling WiFi.begin()
  WiFi status: WL_DISCONNECTED (0x06), elapsed 0s
  WiFi status: WL_IDLE_STATUS (0x00), elapsed 1s
  WiFi status: WL_IDLE_STATUS (0x00), elapsed 2s
  WiFi status: WL_CONNECTED (0x03), elapsed 3s
Connected to WiFi
IP: 192.168.2.57
RSSI: -62 dBm
esp_wifi_set_ps(WIFI_PS_NONE) -> 0
esp_wifi_set_protocol(STA, 11B|11G|11N) -> 0
AP info: SSID=YourNetworkName, primary_chan=6, rssi=-62, authmode=3
Stream server listening on port 8080
```

**Key things to verify:**

1. ✅ **I2S initialization successful:** All `-> 0` (ESP_OK)
2. ✅ **STEREO clock mode:** `channels=STEREO [2]`
3. ✅ **Self-test shows alternating pattern:** non-zero, zero, non-zero, zero
   - Non-zero values = right channel (microphone data)
   - Zero values = left channel (unused)
4. ✅ **WiFi connected:** Shows IP address (e.g., `192.168.2.57`)
5. ✅ **Server listening:** `Stream server listening on port 8080`

## Step 5: Test Audio Streaming

Once the ESP32 is connected to WiFi and shows an IP address, you can test the audio stream.

### Find the Device IP

Look for this line in the serial output:
```
IP: 192.168.2.57
```

Or check your router's DHCP leases, or use network scanning:
```bash
# Option 1: nmap scan (if available)
nmap -sn 192.168.1.0/24 | grep -B 2 "Espressif"

# Option 2: Check from router admin interface
```

### Test Stream with curl

Download 10 seconds of audio:

```bash
timeout 10 curl http://192.168.2.57:8080/ --output test.wav
```

**Expected curl output:**
```
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100  160k    0  160k    0     0  16000      0 --:--:--  0:00:10 --:--:-- 16000
```

**Expected serial monitor output during streaming:**
```
Streamer: client connected from 192.168.1.101
Streamer: request first line: GET / HTTP/1.1
Streamer: response headers & WAV header sent, entering stream loop
Streamer throughput: 16032 B/s
I2S_STATS: samples/sec=16000, last_bytes_read=512
Streamer throughput: 15968 B/s
I2S_STATS: samples/sec=16128, last_bytes_read=512
Streamer throughput: 16000 B/s
I2S_STATS: samples/sec=15872, last_bytes_read=512
...
Streamer: client disconnected
```

**Key indicators of success:**
- ✅ `I2S_STATS: samples/sec` is around **16000** (±5%)
- ✅ `Streamer throughput` is **>0 B/s** and around **16000 B/s**
- ✅ WAV file is **not empty** (should be ~160KB for 10 seconds)

### Verify WAV File

Check file size and format:

```bash
ls -lh test.wav
file test.wav
```

**Expected output:**
```
-rw-rw-r-- 1 user user 160K Feb 16 04:05 test.wav
test.wav: RIFF (little-endian) data, WAVE audio, ITU G.711 mu-law, mono 16000 Hz
```

**What to look for:**
- ✅ File size is reasonable (16000 bytes/sec, so ~160KB for 10 seconds)
- ✅ Format is "WAVE audio, ITU G.711 mu-law, mono 16000 Hz"

View WAV header in hex:

```bash
hexdump -C test.wav | head -3
```

**Expected output:**
```
00000000  52 49 46 46 ff ff ff ff  57 41 56 45 66 6d 74 20  |RIFF....WAVEfmt |
00000010  10 00 00 00 07 00 01 00  80 3e 00 00 80 3e 00 00  |.........>...>..|
00000020  01 00 08 00 64 61 74 61  ff ff ff ff XX XX XX XX  |....data....XXXX|
```

- `52 49 46 46` = "RIFF"
- `57 41 56 45` = "WAVE"
- `07 00` = Audio format 7 (μ-law)
- `01 00` = 1 channel (mono)
- `80 3e 00 00` = 16000 Hz sample rate (little-endian)
- After byte 44: actual audio data (non-zero values)

### Play the Audio

**Using VLC:**
```bash
vlc test.wav
```

**Using aplay (Linux):**
```bash
aplay test.wav
```

**Using ffplay:**
```bash
ffplay test.wav
```

**Using SoX (convert to PCM WAV first):**
```bash
sox -t wav test.wav -t wav test_pcm.wav
aplay test_pcm.wav
```

### Stream Directly to Media Player

Instead of saving to file, pipe directly to a player:

```bash
# Stream to ffplay (will play until you Ctrl+C)
curl http://192.168.2.57:8080/ --no-buffer | ffplay -f wav -

# Stream to VLC
curl http://192.168.2.57:8080/ --no-buffer | vlc -
```

### Test in Web Browser

Simply open the URL in a browser:
```
http://192.168.2.57:8080/
```

Most browsers will either:
- Start playing the audio stream
- Prompt to download the stream
- Open in the browser's audio player

## Step 6: Longer Testing Cycles

### Continuous Streaming Test

Stream for 60 seconds and save:

```bash
timeout 60 curl http://192.168.2.57:8080/ --output test_60s.wav
```

Verify file size should be approximately:
```
16000 samples/sec × 1 byte/sample × 60 seconds + 44 bytes (header) = ~960KB
```

Check it:
```bash
ls -lh test_60s.wav
```

### Multiple Client Test

The server can handle one client at a time. Test connection handling:

**Terminal 1:**
```bash
curl http://192.168.2.57:8080/ --output client1.wav
```

**Terminal 2 (while Terminal 1 is running):**
```bash
curl http://192.168.2.57:8080/ --output client2.wav
```

The second client will wait until the first disconnects, then get served.

### Stability Test

Leave streaming overnight:

```bash
# Stream for 1 hour
timeout 3600 curl http://192.168.2.57:8080/ --output test_1hour.wav

# Expected file size: ~57.6 MB
```

Monitor serial output for any errors or restarts.

## Troubleshooting

### Issue: Empty WAV File (0 bytes)

**Symptoms:**
```bash
ls -lh test.wav
-rw-rw-r-- 1 user user 0 Feb 16 04:05 test.wav
```

**Possible causes:**
1. ESP32 not connected to network
2. Wrong IP address
3. Firewall blocking port 8080

**Debug steps:**
```bash
# Verify ESP32 is on network
ping 192.168.2.57

# Verify port is open
nc -zv 192.168.2.57 8080

# Try with verbose curl
curl -v http://192.168.2.57:8080/ --output test.wav --max-time 5
```

### Issue: Only WAV Header (44 bytes)

**Symptoms:**
```bash
ls -lh test.wav
-rw-rw-r-- 1 user user 44 Feb 16 04:05 test.wav
```

Serial output shows:
```
I2S_STATS: samples/sec=256, last_bytes_read=512
Streamer throughput: 0 B/s
```

**Cause:** I2S clock misconfiguration (MONO instead of STEREO)

**Fix:** Verify `i2s_set_clk()` uses `I2S_CHANNEL_STEREO` in `src/i2s_manager.cpp`

See [INMP441.md](./INMP441.md) for detailed explanation.

### Issue: No Serial Output

**Possible causes:**
1. Wrong baud rate
2. Wrong serial port
3. USB cable issue (some cables are power-only)

**Debug steps:**
```bash
# List available serial ports
ls -l /dev/ttyUSB* /dev/ttyACM*

# Try with explicit port and baud
pio device monitor --port /dev/ttyUSB0 --baud 115200

# Check dmesg for USB connection
dmesg | tail -20
```

### Issue: WiFi Won't Connect

Serial output shows:
```
WiFi status: WL_IDLE_STATUS (0x00), elapsed 1s
WiFi status: WL_IDLE_STATUS (0x00), elapsed 2s
...
WiFi status: WL_IDLE_STATUS (0x00), elapsed 30s
```

**Possible causes:**
1. Wrong SSID/password in `src/secrets.cpp`
2. Network out of range (check RSSI in scan results)
3. 5GHz network (ESP32 only supports 2.4GHz)

**Fix:**
1. Verify credentials:
   ```bash
   cat src/secrets.cpp
   ```
2. Check the WiFi scan results in serial output for your network
3. Ensure you're using a 2.4GHz WiFi network

## Quick Reference

### Complete Build and Test Cycle

```bash
# 1. Configure WiFi (first time only)
cat > src/secrets.cpp << 'EOF'
#include "secrets.h"
const char* ssid = "YourNetwork";
const char* password = "YourPassword";
EOF

# 2. Build, upload, and monitor
pio run --target upload && pio device monitor

# 3. Note the IP address from serial output (e.g., 192.168.2.57)

# 4. In another terminal: test the stream
timeout 10 curl http://192.168.2.57:8080/ --output test.wav

# 5. Verify file
ls -lh test.wav
file test.wav

# 6. Play audio
vlc test.wav
```

### Essential Commands

| Command | Purpose |
|---------|---------|
| `pio run` | Build firmware only |
| `pio run --target upload` | Upload firmware |
| `pio device monitor` | Monitor serial output |
| `pio run --target upload && pio device monitor` | Upload and monitor |
| `curl http://IP:8080/ --output file.wav` | Download audio stream |
| `timeout 10 curl http://IP:8080/ --output file.wav` | Download 10 seconds |
| `file test.wav` | Check WAV file format |
| `hexdump -C test.wav \| head` | View WAV header |
| `vlc test.wav` | Play audio file |

### Important Files

| File | Purpose |
|------|---------|
| `src/secrets.cpp` | WiFi credentials (gitignored) |
| `include/secrets.h` | Header for WiFi credentials |
| `include/config.h` | I2S pins and audio settings |
| `src/i2s_manager.cpp` | I2S initialization |
| `src/streamer.cpp` | HTTP server and audio streaming |
| `platformio.ini` | Build configuration |

## Additional Resources

- [INMP441.md](./INMP441.md) - Detailed I2S configuration explanation
- [README.md](../README.md) - Project overview
- [PlatformIO Documentation](https://docs.platformio.org/)
- [ESP32 Arduino Core](https://github.com/espressif/arduino-esp32)
