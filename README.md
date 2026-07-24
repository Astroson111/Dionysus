# Dionysus (Dio)

Firmware for **Dio** — an [M5Stack CoreS3](https://docs.m5stack.com/en/core/CoreS3) (ESP32-S3)
[Stack-Chan](https://github.com/stack-chan/stack-chan) desktop robot: the physical, animated
voice body for the **Ph3b3** assistant. Dio listens on a tap, streams your speech to the Ph3b3
server for transcription + reasoning, and speaks the reply back with a cosmic animated face and
voice-synced captions.

> **New here? Start with [QUICKSTART.md](QUICKSTART.md)** — box to talking in about 20 minutes.
> This README is the full reference.

> Extracted 2026-07-12 from the `ph3b3` monorepo into its own home. The Ph3b3 server (STT,
> reasoning, TTS) lives separately; Dio talks to it over HTTPS on the LAN.

**Ecosystem:** [Ph3b3](https://github.com/astroson111/ph3b3) — the local server (STT · reasoning ·
TTS) · [Iris](https://github.com/astroson111/iris) — the wearable combadge, Ph3b3's other body ·
[Control Panel](https://github.com/astroson111/ph3b3/tree/main/static) — the local web PWA (served
by Ph3b3) · Dio (this repo). All local, all clients of the same server.

## Hardware
- **M5Stack CoreS3** (ESP32-S3, 320×240 display, built-in PDM mic + I2S speaker)
- Stack-Chan servo base (X/Y neck) via the M5StackChan BSP
- Optional: Si12T head-pat touch sensor (I2C `0x68`), SD card for karaoke tracks

## Repo layout
```
QUICKSTART.md        first-time setup: build, flash, first boot, first words
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
  | **WiFi** | 3 saved networks | Shows the network she's *actually* on. Opens a slot list: **tap** a row to type that slot's SSID + password on the on-screen keyboard, **hold** a row ~1s to clear it. No phone needed. The footer opens the `Dio-Setup` phone portal, which is also where the Ph3b3 host / port / device key live. |
  | **Microphone** | Low / **Medium** / High | Capture gain 4 / 6 / 8. Medium is the tuned default. |
  | **Volume** | Low / **Medium** / High | Speaker output 40 / 70 / 100%. |
  | **LED** | Off / Dim / **Full** | Brightness of the status/pet ring. Off = dark (status logic still runs). |
  | **Color** | Purple … White (9) | Color of the *listening* ring. Default Purple; pet stays pink. |

  On boot — and again after any drop — she walks the filled WiFi slots **top to bottom**, ~8s
  each, and keeps the first that answers. Empty slots are skipped, so slot 3 can be the only one
  filled. Put the everyday network in slot 1 and the away-from-home one at the bottom, where it's
  only reached when the ones above it aren't there. The boot splash names the network she landed
  on (or lists the ones that didn't answer).

## Karaoke — bring your own music
**No music ships with this repo — supply your own tracks** (and respect the copyright of
whatever you load; that's on you, not this project). Dio's `KaraokeApp` plays **WAV** files
from a `/karaoke/` folder on a **FAT32** SD card, browsed on-screen (top⅓ = prev · center =
play · bottom⅓ = next).

Per-file requirements:
- **PCM signed 16-bit, mono or stereo** (44.1 kHz works well), with a **canonical 44-byte
  WAV header** — stray metadata chunks make the on-device parser mis-read the stream.
- The filename (minus `.wav`) is the on-screen title; tracks sort alphabetically — lead with
  `01 `, `02 `… to order them.
- Optional `<name>.lrc` (LRC timestamps) beside a track → synced on-screen lyrics.

Convert any source file with ffmpeg — these flags strip metadata/art and force a clean header:
```
ffmpeg -i "your song.mp3" -map 0:a:0 -c:a pcm_s16le -ar 44100 -ac 2 \
       -map_metadata -1 -fflags +bitexact -flags:a +bitexact "/karaoke/01 Your Song.wav"
```
The card must be **FAT32** — the ESP32 SD library cannot mount exFAT (a big card formatted
on Windows is usually exFAT; reformat it FAT32).

## Build & flash
Use **arduino-cli** (or the Arduino IDE). **Never PlatformIO** — `pio run` builds the wrong
~1.0 MB binary from an orphaned prototype and bricks the device. A correct build is **~1.49 MB**
(it was ~1.39 MB before the three-slot WiFi work; the ~1.0 MB wrong-target signature is what
matters).

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
Dio targets the Ph3b3 server at `https://<host>:7331` (mkcert TLS, `setInsecure()`); WiFi is set
via **Settings → WiFi** (on-screen keyboard, three saved networks) and host/port/device key via
the `Dio-Setup` captive portal, stored atomically in NVS. Voice endpoints: `POST /transcribe`, `POST /chat/stream` +
`GET /tts/chunk/{sid}/{n}` (chunked TTS so long replies stream instead of timing out).
