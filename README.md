# Dionysus (Dio)

Firmware for **Dio** — an [M5Stack CoreS3](https://docs.m5stack.com/en/core/CoreS3) (ESP32-S3)
[Stack-Chan](https://github.com/stack-chan/stack-chan) desktop robot: the physical, animated
voice body for the **Ph3b3** assistant. Dio listens on a tap, streams your speech to the Ph3b3
server for transcription + reasoning, and speaks the reply back with a cosmic animated face and
voice-synced captions.

> Extracted 2026-07-12 from the `ph3b3` monorepo into its own home. The Ph3b3 server (STT,
> reasoning, TTS) lives separately; Dio talks to it over HTTPS on the LAN.

## Hardware
- **M5Stack CoreS3** (ESP32-S3, 320×240 display, built-in PDM mic + I2S speaker)
- Stack-Chan servo base (X/Y neck) via the M5StackChan BSP
- Optional: Si12T head-pat touch sensor (I2C `0x68`), SD card for karaoke tracks

## Repo layout
```
Ph3b3-Chan/          the Arduino sketch (open this folder in the IDE)
  Ph3b3-Chan.ino     entry point + app registration
  AppManager.h       cooperative app/state manager
  MenuApp.h / CrescentMenu.h   main menu
  TalkApp.h          voice pipeline: tap→record→/transcribe→/chat/stream→play
  KaraokeApp.h       SD-card karaoke
  GhostApp.h         "ghost hunting" mode
  NetworkApp.h       WiFi captive-portal config
  ph3b3_face.h       the animated face + speech bubble renderer
  secrets.example.h  copy to secrets.h (gitignored) and fill in
docs/                audit + hardware contracts
```

## Build & flash
Use **arduino-cli** (or the Arduino IDE). **Never PlatformIO** — `pio run` builds the wrong
~1.0 MB binary from an orphaned prototype and bricks the device. A correct build is **~1.39 MB**.

```bash
# one-time: copy and fill in your WiFi + Ph3b3-server creds
cp Ph3b3-Chan/secrets.example.h Ph3b3-Chan/secrets.h   # then edit it

# build
arduino-cli compile --fqbn m5stack:esp32:m5stack_cores3 Ph3b3-Chan

# flash (NVS-safe: app @ 0x10000 + boot_app0 @ 0xe000, preserves saved WiFi/creds)
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write-flash \
  0xe000  ~/.arduino15/packages/m5stack/hardware/esp32/<ver>/tools/partitions/boot_app0.bin \
  0x10000 Ph3b3-Chan/build/Ph3b3-Chan.ino.bin
```

**Verify the device before flashing.** Dio's USB serial is silent (USB-Serial-JTAG) and its
splash reads "Ph3b3", so it can be confused with the Iris combadge — confirm the MAC first:
```bash
esptool --port /dev/ttyACM0 read-mac     # Dio = 44:1b:f6:e5:56:60
```

## Landmines (don't trip)
- **`arduino-cli` only** — never `pio run` (wrong 1.0 MB binary).
- **Never `SPI.begin(36,35,37,-1)`** — GPIO35 is M5GFX's DC pin; it kills the display bus.
  `SD.begin(4, SPI, 20000000)` works without it.
- **VAD `VAD_SILENCE_MS = 700`** is demo-tuned — don't retune it casually.
- Speaker (I2S_NUM_1) and mic (I2S_NUM_0) share BCK/WS; call `M5.Speaker.end()` before
  `M5.Mic.begin()` or the mic records zeros.
- Serial is dead on this unit — the **screen is the instrument**.

## Server
Dio targets the Ph3b3 server at `https://<host>:7331` (mkcert TLS, `setInsecure()`), configured
via the WiFi portal / NVS. Voice endpoints: `POST /transcribe`, `POST /chat/stream` +
`GET /tts/chunk/{sid}/{n}` (chunked TTS so long replies stream instead of timing out).
