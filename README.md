# Dionysus (Dio)

Firmware for **Dio** — an [M5Stack CoreS3](https://docs.m5stack.com/en/core/CoreS3) (ESP32-S3)
[Stack-Chan](https://github.com/stack-chan/stack-chan) desktop robot: the physical, animated
voice body for the **Ph3b3** assistant. Dio listens on a tap, streams your speech to the Ph3b3
server for transcription + reasoning, and speaks the reply back with a cosmic animated face and
voice-synced captions.

> Extracted 2026-07-12 from the `ph3b3` monorepo into its own home. The Ph3b3 server (STT,
> reasoning, TTS) lives separately; Dio talks to it over HTTPS on the LAN.

**Ecosystem:** [Ph3b3](https://github.com/astroson111/ph3b3) — the local server (STT · reasoning ·
TTS) · [Iris](https://github.com/astroson111/iris) — the wearable combadge, Ph3b3's other body ·
Dio (this repo). All local, all clients of the same server.

## Hardware
- **M5Stack CoreS3** (ESP32-S3, 320×240 display, built-in PDM mic + I2S speaker)
- Stack-Chan servo base (X/Y neck) via the M5StackChan BSP
- Optional: Si12T head-pat touch sensor (I2C `0x68`), SD card for karaoke tracks

## Repo layout
```
Ph3b3-Chan/          the Arduino sketch (open this folder in the IDE)
  Ph3b3-Chan.ino     entry point, app registration, WiFi, boot homing, LED cue
  AppManager.h       cooperative app/state manager
  CrescentMenu.h     the mode drawer (swipe the top-left crescent to open)
  TalkApp.h          voice pipeline: tap→record→/transcribe→/chat/stream→play
  KaraokeApp.h       SD-card karaoke
  NetworkApp.h       "Network" scan-intent stub (NOT WiFi config — see Settings)
  GhostApp.h         "ghost hunting" mode (unregistered by default)
  SettingsApp.h      Settings screen (WiFi / Mic / Volume / LED / Color)
  SettingsStore.h    settings presets + NVS persistence (namespace "sc")
  TouchKeyboard.h    on-screen touch QWERTY (device-native WiFi entry)
  ph3b3_face.h       the animated face + speech bubble renderer
  secrets.example.h  copy to secrets.h (gitignored) and fill in
docs/                audit + hardware contracts
```

## Controls & Settings
- **Mode drawer** — tap/swipe the **crescent tab** (top-left corner); swipe *right* to slide
  it open, then tap a mode: **Talk · Network · Karaoke · Settings**.
- **Head-pat** — pet the Si12T sensor on her head and the LED ring glows **pink**, brighter the
  harder you pet (respects the LED brightness setting below). A pat also counts as activity, so
  petting her keeps her awake instead of dropping to dormant mid-cuddle.
- **Settings** (last item in the drawer) — tap-to-cycle rows; a **right-to-left swipe** anywhere
  exits back to her face. All choices persist to NVS (namespace `"sc"`) and reload at boot:
  | Row | Options | Effect |
  |-----|---------|--------|
  | **WiFi Setup** | on-screen keyboard | Type SSID + password on the touchscreen, save + reboot to join. No phone needed. (The phone captive portal `Dio-Setup` still auto-appears as a fallback on repeated connect failure.) |
  | **Microphone** | Low / **Medium** / High | Capture gain 4 / 6 / 8. Medium is the tuned default. |
  | **Volume** | Low / **Medium** / High | Speaker output 40 / 70 / 100%. |
  | **LED** | Off / Dim / **Full** | Brightness of the status/pet ring. Off = dark (status logic still runs). |
  | **Color** | Purple … White (9) | Color of the *listening* ring. Default Purple; pet stays pink. |

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
- **The tilt (vertical) servo needs battery power to home** — USB-only is current-limited and
  can't lift the head against gravity (pan homes fine, tilt droops). This is power, not code;
  don't "fix" it by firming up `gentleHome()` (more torque draws more current → brown-out).
- Row/array names must not be `LOW`/`HIGH` — those are Arduino GPIO macros.

## Voice & session
- **Long stories play whole.** Replies stream as chunked TTS (a manifest chunk, then lazy
  `GET /tts/chunk/{sid}/{n}`), played first-chunk-while-fetching-next. There is **no wall-clock
  cap** on the reply: an **inter-chunk watchdog** resets on every byte from the server and tears down
  only a genuinely *stalled* stream, so a five-minute recitation holds to the end while a hung
  connection still bails fast. (Replaces the old fixed 60 s per-chunk read deadline that could
  truncate a long reply when the server was slow to synthesize.)
- **Completion = audio drained, not stream closed.** She keeps talking until her local playback
  buffer empties, even if the network stream closed while she was still mid-sentence.
- **Activity-based session exit.** A chunk arriving, audio playing, detected speech, or a
  head-pat all reset the idle clock; she drops to dormant only after true dead air (~10 s), never
  on a fixed timer. Tap her face (or pet her) to keep her awake / wake her back up.

## Server
Dio targets the Ph3b3 server at `https://<host>:7331` (mkcert TLS, `setInsecure()`); WiFi + host
are set via **Settings → WiFi Setup** (on-screen keyboard) or the `Dio-Setup` captive portal, and
stored atomically in NVS. Voice endpoints: `POST /transcribe`, `POST /chat/stream` +
`GET /tts/chunk/{sid}/{n}` (chunked TTS so long replies stream instead of timing out).
