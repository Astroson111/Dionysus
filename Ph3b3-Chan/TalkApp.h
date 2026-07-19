#pragma once
#include "AppBase.h"
#include "SettingsStore.h"
#include <M5StackChan.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Defined once in stackchan_rung4.ino
extern const char ISRG_ROOT_X1[];
extern bool g_overlayOpen;
extern bool g_crescentTapped;

// Atomic server-target record (defined in Ph3b3-Chan.ino, seeded from secrets.h).
extern String gSrvHost;
extern int    gSrvPort;
extern String gSrvUser;
extern String gSrvPass;

// millis() of the last head-pat (Si12T), stamped by _cueLeds() in the .ino.
// A head-pat is an activity signal that keeps her awake (feeds the dead-air exit).
extern volatile uint32_t gLastPetMs;

// Native photo loop: set while the camera holds the LCD (see ph3b3_face.h), and
// the .ino bridge that captures a frame, shows it, and POSTs it to Nyx. Returns
// true if the frame was posted (then g_faceOwnedByCamera stays set until cleared).
extern volatile bool g_faceOwnedByCamera;
extern bool camVisionCapture();

// ── TalkApp — full Ph3b3 voice round-trip ────────────────────────────────────
//
// Flow:  tap screen → LISTENING (record) → release or 5s max → THINKING
//        → POST /transcribe → show heard text → POST /chat → stream audio
//        → SPEAKING (play) → back to IDLE
//
// WiFi must be connected (managed by main sketch). If offline, shows notice.
//
// Sequential: mic records THEN speaker plays — no BCK/WS simultaneity issue.
// Pattern: M5.Mic.begin() → record → M5.Mic.end() → M5.Speaker.begin() → play.

class TalkApp : public AppBase {
public:
    void init() override {
        M5StackChan.Display().fillScreen(TFT_BLACK);
        face.begin();  // full-screen face — no panel
        face.setStatusVisible(false);
        face.setCrescentTabVisible(true);
        _phase      = PH_IDLE;
        _pttBuf     = nullptr;
        _pttSamples = 0;
        _wasTouch   = false;
        _bargeIn         = false;
        _heardText       = "";
        _replyText       = "";
        _sessionId       = "sc-" + String(millis(), HEX);  // stable for this conversation
        _lastTalkMs      = millis();
        _inConversation  = false;
        _convLastValidMs = 0;
        _exitAfterTurn   = false;
        _awaitFloor      = 0.0f;
        _awaitAccum      = 0.0f;
        _awaitSamples    = 0;
        _awaitOnsetMs    = 0;
        _awaitCalibEnd   = 0;
        // One-time TLS setup — cert pointer stays valid (ISRG_ROOT_X1 is in flash).
        // setTimeout() takes SECONDS on WiFiClientSecure — 90s covers LLM+TTS latency.
        // setReuse(true) tells HTTPClient to preserve the socket after http.end()
        // when the server responds with Connection: keep-alive.
        _tls.setInsecure();          // LAN mkcert self-signed cert (Iris lesson)
        _tls.setTimeout(90);
        _http.setReuse(true);
    }

    // Audio arbitration for the pet purr: the mic and speaker share the I2S bus, so
    // a purr may only touch the speaker when the mic is OFF. That's exactly PH_IDLE
    // (resting, waiting for a tap) — never AWAITING/RECORDING (mic live) or playback.
    bool micIdle() const { return _phase == PH_IDLE; }

    // Soft one-shot purr on a pet. Guarded: never over TTS/any playback. Caller must
    // also confirm micIdle() so we don't grab the I2S bus from an active mic.
    void purr() {
        if (M5.Speaker.isPlaying(0)) return;          // never stomp TTS or any audio
        if (!_purrReady) { _genPurr(); _purrReady = true; }
        if (!M5.Speaker.isEnabled()) M5.Speaker.begin();
        M5.Speaker.setVolume(gSpeakerVolume);         // match the system volume (was unset → silent)
        M5.Speaker.playRaw(_purrBuf, PURR_SAMPLES, PURR_RATE, false, 1, 0);   // non-blocking
    }

    void update() override {
        // ── Overlay gate — CrescentMenu handles all touches while panel is open ─
        if (g_overlayOpen) {
            int16_t _tx, _ty;
            _wasTouch = M5StackChan.Display().getTouch(&_tx, &_ty);
            return;
        }

        // Drain crescentTap here even if gated below — flag must not linger
        bool crescentTap = g_crescentTapped;
        g_crescentTapped = false;
        if (crescentTap) Serial.printf("[Talk] crescentTap seen — wifi=%d phase=%d\n",
                                       (int)(WiFi.status() == WL_CONNECTED), (int)_phase);

        // ── WiFi gate ──────────────────────────────────────────────────────────
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[Talk] WiFi gate fired — crescentTap lost");
            face.setState(Ph3b3Face::CONNECTING);
            return;
        }

        // ── Touch detection ────────────────────────────────────────────────────
        int16_t tx = 0, ty = 0;
        bool touching = M5StackChan.Display().getTouch(&tx, &ty);
        bool tapped = crescentTap || ((touching && !_wasTouch) && !(tx < 60 && ty < 60));
        _wasTouch = touching;
        if (tapped) Serial.printf("[Talk] tapped — crescentTap=%d bodyTap=%d phase=%d\n",
                                  (int)crescentTap,
                                  (int)((touching && !crescentTap) && !(tx < 60 && ty < 60)),
                                  (int)_phase);
        // ── State machine ──────────────────────────────────────────────────────
        switch (_phase) {

        case PH_IDLE:
            face.setState(Ph3b3Face::IDLE);
            if (tapped) {
                _lastTalkMs      = millis();
                _inConversation  = true;
                _convLastValidMs = millis();
                _exitAfterTurn   = false;
                _startRecording();   // tap → immediate LISTENING, not ambient wait
            }
            break;

        case PH_AWAITING: {
            face.setState(Ph3b3Face::IDLE);
            if (millis() - _lastTalkMs > SESSION_IDLE_RESET_MS) {
                _sessionId  = "sc-" + String(millis(), HEX);
                _lastTalkMs = millis();
            }

            // Record a small monitoring chunk
            static int16_t monBuf[MONITOR_CHUNK];
            M5.Mic.record(monBuf, MONITOR_CHUNK, PTT_RATE);

            // High-passed level (same meter the recording loop uses) so ambient
            // calibration and endpointing are on the SAME footing — fan rumble is
            // out of both, and _awaitFloor is directly reusable as the recording floor.
            float rms = _hpRms(monBuf, MONITOR_CHUNK);

            uint32_t now = millis();

            // Calibrate noise floor during initial window
            if (now < _awaitCalibEnd) {
                _awaitAccum += rms * rms;
                _awaitSamples++;
            } else if (_awaitFloor == 0.0f && _awaitSamples > 0) {
                _awaitFloor = max(VAD_FLOOR_MIN, sqrtf(_awaitAccum / _awaitSamples));
                Serial.printf("[await] floor=%.4f\n", _awaitFloor);
            }

            // Activity resets the dead-air session timer. Speech (onset) is one
            // signal; a head-pat within the last ~1.5s is another — petting her
            // keeps her awake instead of dropping to dormant mid-cuddle. (TTS
            // playback and chunk arrival can't fire here: playback is blocking, so
            // the dead-air clock is already refreshed post-reply.)
            if (_awaitFloor > 0.0f && rms > _awaitFloor * ONSET_THRESH) _deadAirStartMs = now;
            if (gLastPetMs && now - gLastPetMs < 1500) _deadAirStartMs = now;

            // Dead air: DEAD_AIR_MS of true silence in chat mode (no TTS runs in
            // AWAITING; speech resets above) → end chat mode and go dormant.
            if (_awaitFloor > 0.0f && millis() - _deadAirStartMs >= DEAD_AIR_MS) {
                Serial.println("[chat] dead air — ending chat mode (dormant)");
                _exitChatMode();
                break;
            }

            // Speech onset: loud enough for long enough → start recording
            if (_awaitFloor > 0.0f && rms > _awaitFloor * ONSET_THRESH) {
                if (_awaitOnsetMs == 0) _awaitOnsetMs = now;
                if (now - _awaitOnsetMs >= ONSET_MIN_MS) {
                    Serial.printf("[await] onset rms=%.4f floor=%.4f\n", rms, _awaitFloor);
                    _awaitOnsetMs    = 0;
                    _lastTalkMs      = now;
                    _inConversation  = true;
                    _convLastValidMs = now;
                    _exitAfterTurn   = false;
                    M5.Mic.end();
                    _startRecording();
                }
            } else {
                _awaitOnsetMs = 0;
            }

            // Tap override — immediate recording without waiting for onset
            if (tapped) {
                _awaitOnsetMs    = 0;
                _lastTalkMs      = now;
                _inConversation  = true;
                _convLastValidMs = now;
                _exitAfterTurn   = false;
                M5.Mic.end();
                _startRecording();
            }
            break;
        }

        case PH_RECORDING: {
            // TIGHT capture loop. Reading ONE chunk per cooperative loop() iteration let
            // ~50ms of other loop work (face blit + servo + delay) run between reads, so
            // the mic DMA outran the reader and OVERFLOWED — ~60% of samples dropped →
            // audio came out time-compressed ("too fast") and choppy → Whisper hallucinated
            // regardless of gain. Drain the mic here WITHOUT yielding (no face blit) until
            // VAD closes / full / a deliberate mid-turn tap. Face shows LISTENING once.
            // Instant-clear any prior reply bubble: the animated collapse can't run inside
            // the render-blocking tight loop below, so it would freeze stale on-screen.
            face.clearBubbleNow();
            face.setState(Ph3b3Face::LISTENING);
            face.update();
            while (_phase == PH_RECORDING && _pttBuf) {
                int chunk = min(512, PTT_MAX - _pttSamples);
                if (chunk <= 0) { _stopRecordingAndDispatch(); break; }
                M5.Mic.record(&_pttBuf[_pttSamples], chunk, PTT_RATE);
                _pttSamples += chunk;
                uint32_t elapsed = millis() - _recStartMs;

                // Energy-based endpointing was scrapped (2026-07-19): a fan/HEPA
                // basement keeps the level above any silence floor — even high-passed —
                // so the VAD never fired and captures rode to the cap anyway. Fixed cap
                // instead: predictable, no premature mid-word cuts. End early with a tap.
                // (The 9s convIdle cut was a separate, already-removed bug — do NOT test
                // conversation-idle here; the user is actively talking during a recording.)
                bool full = (elapsed >= REC_CAP_MS || _pttSamples >= PTT_MAX);

                // Deliberate mid-turn tap → exit the conversation (entry bounce rejected
                // by REC_MIN_MS).
                int16_t _tx, _ty;
                if (M5StackChan.Display().getTouch(&_tx, &_ty) && _inConversation && elapsed >= REC_MIN_MS) {
                    M5.Mic.end();
                    if (_pttBuf) { heap_caps_free(_pttBuf); _pttBuf = nullptr; }
                    _pttSamples = 0;
                    _endConversation();
                    break;
                }
                if (full) {
                    _stopRecordingAndDispatch();
                    break;
                }
            }
            break;
        }

        case PH_DONE:
            // Auto-return to always-listening after conversation ends
            _heardText = "";
            _replyText = "";
            _startAwaiting();
            break;

        case PH_ERROR:
            face.setState(Ph3b3Face::ERROR);
            if (tapped) {
                _startAwaiting();
            }
            break;
        }
    }

    void draw() override {}

    void exit() override {
        if (_phase == PH_RECORDING || _phase == PH_AWAITING) {
            M5.Mic.end();
        }
        if (_pttBuf) { heap_caps_free(_pttBuf); _pttBuf = nullptr; }
        M5.Speaker.stop(0);
        M5.Speaker.end();
        face.begin();  // restore full-screen face
        _phase          = PH_IDLE;
        _sessionId      = "";  // will be regenerated on next init()
        _inConversation = false;
        _exitAfterTurn  = false;
        // Drop the persistent TLS socket when leaving TalkApp — it will be
        // stale by the time the user returns anyway.
        // Cap the TLS close-notify wait to 500ms: with setReuse(true) the socket
        // may be live (keep-alive), and the default 30s timeout would freeze the
        // display for up to 30 seconds while the TCP FIN/ACK completes.
        _http.end();
        _tls.setTimeout(500);
        _tls.stop();
    }

    const char* name() const override { return "Talk / Ph3b3"; }

private:
    // ── Constants ─────────────────────────────────────────────────────────────
    // Server target (host/port/user/pass) now lives in the runtime NVS record
    // gSrv* (Ph3b3-Chan.ino) — no longer compile-time statics here.
    static constexpr uint32_t SESSION_IDLE_RESET_MS = 60000;  // 60s idle resets conversation
    static constexpr uint32_t CONV_IDLE_MS          = 9000;   // 9s no valid speech → end loop
    static constexpr uint32_t DEAD_AIR_MS           = 10000;  // 10s true silence in chat mode → exit to dormant (NVS candidate, like spk_vol)
    static constexpr uint32_t PLAYBACK_WATCHDOG_MS  = 5000;   // no audio progress this long during playback → stream hung, tear down
    static constexpr uint32_t PEEK_SILENCE_MS       = 15000;  // no bytes this long while reading a chunk's head → stream stalled (inter-byte watchdog, NOT a total cap)
    static constexpr uint32_t FACE_TICK_MS          = 100;   // playback face redraw cadence (~10fps) so the bubble grows/scrolls live; raise (125/166/200) if audio sputters
    static constexpr uint16_t DBG_STATE_PORT        = 7332;  // [DBG-STATE] UDP state telemetry port on Nyx (serial dead → journal-readable)
    // Mic gain is now a runtime Settings preset: gMicMagnification (SettingsStore.h),
    // Low 4 / Medium 6 (= this old default) / High 8. Narrow clean window: 16 & 8 clip
    // (peak 32752), 4 is faint (rms ~500 — but still clears the server's rms<1200 silence
    // gate at ~1730). Calibrate via /transcribe [DBG-MIC]: want rms ~2000, peak < ~30000.
    static constexpr int      JUNK_MIN_LEN          = 2;      // transcript chars below this → noise
    static constexpr int      PTT_RATE      = 16000;
    static constexpr int      PTT_MAX       = PTT_RATE * 12;  // 12s hard cap = 384 KB PSRAM (fixed-cap capture)
    static constexpr int      CHUNK_SAMP    = 2048;           // ~93 ms @ 22050 Hz; larger = fewer gaps
    // Pet purr — a short soft warble synthesised once into _purrBuf, played on a pet
    // only when micIdle() (PH_IDLE) and the speaker is free.
    static constexpr int      PURR_RATE     = 16000;
    static constexpr int      PURR_SAMPLES  = 4480;           // ~280 ms
    // Capture is FIXED-CAP: energy endpointing was scrapped (2026-07-19) — a fan/HEPA
    // basement kept the level above any silence floor so the VAD never fired and
    // captures rode to the cap anyway. A recording ends only at REC_CAP_MS or an
    // early tap. The always-listening ONSET (wake) still uses a high-passed ambient
    // floor, so HP_ALPHA / VAD_FLOOR_MIN / VAD_CALIBRATE_MS remain for that path.
    static constexpr uint32_t REC_CAP_MS       = 12000;  // hard 12s capture cap; tap to end sooner
    static constexpr uint32_t REC_MIN_MS       = 1000;   // ignore a screen tap in the first 1s (entry-bounce reject)
    static constexpr uint32_t VAD_CALIBRATE_MS = 200;    // AWAITING ambient-floor window (onset/wake only)
    static constexpr float    VAD_FLOOR_MIN    = 0.0010f;// min high-passed ambient floor for onset detection
    static constexpr float    HP_ALPHA         = 0.90f;  // single-pole HP ~275Hz for the onset level meter (fan rumble out)
    // Always-listening onset detection
    static constexpr int      MONITOR_CHUNK   = 256;     // samples per onset-check frame (~16 ms @ 16 kHz)
    static constexpr uint32_t ONSET_MIN_MS    = 150;     // ms of loud signal before auto-trigger
    static constexpr float    ONSET_THRESH    = 6.0f;    // × noise floor → speech onset

    // ── State ─────────────────────────────────────────────────────────────────
    enum Phase { PH_IDLE, PH_AWAITING, PH_RECORDING, PH_DONE, PH_ERROR };
    Phase    _phase      = PH_IDLE;
    int16_t* _pttBuf     = nullptr;
    int      _pttSamples = 0;
    uint32_t _recStartMs = 0;
    bool     _wasTouch   = false;
    bool     _bargeIn    = false;  // set when tap interrupts playback → restart listening
    String   _heardText;
    String   _replyText;
    String   _sessionId;          // generated on init(), stable for a conversation, reset on exit/idle
    uint32_t _lastTalkMs      = 0;  // tracks idle time for session reset
    // Continuous conversation
    bool     _inConversation  = false;
    uint32_t _convLastValidMs = 0;   // millis() of last successful /chat; idle clock measures from here
    bool     _exitAfterTurn   = false; // set when exit word detected — end after current SPEAK
    bool     _visionTurn      = false; // Nyx flagged this utterance for Dio's own camera (native photo loop)
    // Always-listening state
    float    _awaitFloor    = 0.0f;
    float    _awaitAccum    = 0.0f;
    int      _awaitSamples  = 0;
    uint32_t _awaitOnsetMs  = 0;      // millis() when onset window opened; 0 = no onset
    uint32_t _awaitCalibEnd = 0;      // millis() when noise-floor calibration ends
    uint32_t _deadAirStartMs = 0;     // millis() dead-air (silence) window began in AWAITING; reset on speech

    // Persistent TLS connection — cert set once; reused for /transcribe then /chat
    // within the same turn so only one TLS handshake is needed per turn.
    // HTTPClient::setReuse(true) keeps the socket alive across http.end() calls
    // as long as the server responds with Connection: keep-alive.
    WiFiClientSecure _tls;
    HTTPClient       _http;
    WiFiUDP          _dbgUdp;   // [DBG-STATE] fire-and-forget state telemetry to Nyx
    int16_t          _purrBuf[PURR_SAMPLES];   // synthesised pet purr (lazy-filled)
    bool             _purrReady = false;
    bool             _dbgUdpReady = false;   // WiFiUDP must be begin()'d before it will send

    // ── Base64 helpers ────────────────────────────────────────────────────────
    static int _b64val(char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    }
    static const char* _b64enc() {
        return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    }

    static void _buildWavHdr(uint8_t* h, int samples, int rate) {
        int d = samples * 2, f = 36 + d, br = rate * 2;
        memcpy(h, "RIFF", 4);
        h[4]=f; h[5]=f>>8; h[6]=f>>16; h[7]=f>>24;
        memcpy(h+8, "WAVE", 4); memcpy(h+12, "fmt ", 4);
        h[16]=16; h[17]=0; h[18]=0; h[19]=0;
        h[20]=1; h[21]=0; h[22]=1; h[23]=0;
        h[24]=rate; h[25]=rate>>8; h[26]=rate>>16; h[27]=rate>>24;
        h[28]=br; h[29]=br>>8; h[30]=br>>16; h[31]=br>>24;
        h[32]=2; h[33]=0; h[34]=16; h[35]=0;
        memcpy(h+36, "data", 4);
        h[40]=d; h[41]=d>>8; h[42]=d>>16; h[43]=d>>24;
    }

    // ── Always-listening ──────────────────────────────────────────────────────
    void _startAwaiting() {
        M5.Speaker.stop(0);
        M5.Speaker.end();
        delay(30);
        auto mcfg = M5.Mic.config();
        mcfg.sample_rate   = PTT_RATE;
        mcfg.magnification = gMicMagnification;
        M5.Mic.config(mcfg);
        M5.Mic.begin();
        _awaitFloor    = 0.0f;
        _awaitAccum    = 0.0f;
        _awaitSamples  = 0;
        _awaitOnsetMs  = 0;
        _awaitCalibEnd = millis() + VAD_CALIBRATE_MS;
        _deadAirStartMs = millis();   // dead-air clock starts when we begin listening
        // Snapshot live touch state so a still-held tap (e.g. exit tap from PH_RECORDING)
        // is not re-detected as a new entry tap on the very next frame.
        { int16_t _tx, _ty; _wasTouch = M5StackChan.Display().getTouch(&_tx, &_ty); }
        face.setState(Ph3b3Face::IDLE);
        face.clearBubble();
        _phase = PH_AWAITING;
        _dbgState("armed");
        Serial.println("[await] armed — waiting for speech");
    }

    // ── Recording ─────────────────────────────────────────────────────────────
    void _startRecording() {
        // Allocate in PSRAM — 5s × 16kHz × 2 bytes = 160 KB
        _pttBuf = (int16_t*)heap_caps_malloc(PTT_MAX * sizeof(int16_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!_pttBuf) {
            Serial.println("[talk] OOM: failed to alloc PTT buffer");
            face.setState(Ph3b3Face::ERROR);
            _phase = PH_ERROR;
            return;
        }
        _pttSamples     = 0;
        _recStartMs     = millis();

        // Ensure speaker I2S is fully released before mic claims the shared BCK/WS bus.
        // _doChatAndPlay() ends it after normal drain, but _startRecording() can also
        // be called from PH_IDLE (first turn) where the speaker may not have been used.
        M5.Speaker.stop(0);
        M5.Speaker.end();
        delay(30);  // let shared BCK/WS lines settle; prevents switching noise from polluting VAD calibration
        Serial.println("[i2s] mic.begin() — speaker torn down");

        auto mcfg = M5.Mic.config();
        mcfg.sample_rate   = PTT_RATE;
        mcfg.magnification = gMicMagnification;
        M5.Mic.config(mcfg);
        M5.Mic.begin();

        face.setState(Ph3b3Face::LISTENING);
        face.clearBubble();
        _phase = PH_RECORDING;
        _dbgState("capturing");
    }

    void _stopRecordingAndDispatch() {
        _dbgState("endpointed");
        M5.Mic.end();
        M5.Speaker.end();
        M5.Speaker.begin();
        M5.Speaker.setVolume(gSpeakerVolume);   // Settings → Volume preset

        if (!_pttBuf || _pttSamples < PTT_RATE / 4) {
            if (_pttBuf) { heap_caps_free(_pttBuf); _pttBuf = nullptr; }
            if (!(millis() - _convLastValidMs < CONV_IDLE_MS)) _inConversation = false;
            _startAwaiting();  // re-arm voice detection
            return;
        }

        face.setState(Ph3b3Face::THINKING);
        face.update();

        // Clear _pttBuf BEFORE dispatch so the CC re-arm path inside _dispatch()
        // can safely call _startRecording() and assign a fresh buffer to _pttBuf.
        // If we free _pttBuf AFTER dispatch, we'd free the new turn-2 buffer instead.
        int16_t* audio      = _pttBuf;
        int      numSamples = _pttSamples;
        _pttBuf     = nullptr;
        _pttSamples = 0;

        _dispatch(audio, numSamples);
        heap_caps_free(audio);
    }

    // ── Main dispatch: encode → /transcribe → /chat → play ───────────────────
    void _dispatch(int16_t* audio, int numSamples) {
        Serial.printf("[D1] dispatch samples=%d sess=%s inConv=%d validAge=%lums\n",
                      numSamples, _sessionId.c_str(), (int)_inConversation,
                      _convLastValidMs ? millis() - _convLastValidMs : 0);

        // Idle check: only abort pre-dispatch if we're not in a conversation loop.
        // In the conversation loop, the re-arm calls _startRecording() immediately —
        // recording can run up to VAD_MAX_MS (12 s), which exceeds CONV_IDLE_MS.
        // Unlimited loop exit is via tap or farewell phrase only.
        if (!_inConversation && _convLastValidMs > 0 &&
            millis() - _convLastValidMs > CONV_IDLE_MS) {
            Serial.println("[D1] idle timeout at dispatch entry — ending conv");
            _endConversation();
            return;
        }

        // ── Step 1: build {"audio":"<base64 WAV>"} in PSRAM ──────────────────
        uint8_t wavHdr[44];
        _buildWavHdr(wavHdr, numSamples, PTT_RATE);
        int wavBytes = 44 + numSamples * 2;
        int b64Len   = ((wavBytes + 2) / 3) * 4;
        int jLen     = 10 + b64Len + 2;   // {"audio":"..."}
        char* jbuf   = (char*)heap_caps_malloc(jLen + 1,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!jbuf) {
            Serial.printf("[D2] OOM jbuf jLen=%d\n", jLen);
            face.setState(Ph3b3Face::ERROR);
            _phase = PH_ERROR;
            return;
        }
        Serial.printf("[D2] jbuf OK %d bytes\n", jLen);

        // Inline base64 encoder — feeds WAV header then PCM
        memcpy(jbuf, "{\"audio\":\"", 10);
        int pos = 10;
        const char* enc = _b64enc();
        uint8_t tri[3]; int triPos = 0;

        auto flushTri = [&](int valid) {
            while (triPos < 3) tri[triPos++] = 0;
            jbuf[pos++] = enc[(tri[0]>>2)&0x3F];
            jbuf[pos++] = enc[((tri[0]&3)<<4)|((tri[1]>>4)&0xF)];
            jbuf[pos++] = valid < 2 ? '=' : enc[((tri[1]&0xF)<<2)|((tri[2]>>6)&0x3)];
            jbuf[pos++] = valid < 3 ? '=' : enc[tri[2]&0x3F];
            triPos = 0;
        };
        auto feedB = [&](uint8_t b) {
            tri[triPos++] = b;
            if (triPos == 3) flushTri(3);
        };

        for (int i = 0; i < 44; i++) feedB(wavHdr[i]);
        for (int i = 0; i < numSamples; i++) {
            feedB((uint8_t)(audio[i] & 0xFF));
            feedB((uint8_t)((audio[i] >> 8) & 0xFF));
        }
        if (triPos > 0) flushTri(triPos);
        jbuf[pos++] = '"'; jbuf[pos++] = '}'; jbuf[pos] = '\0';

        // ── Step 2: POST /transcribe ──────────────────────────────────────────
        // Use the persistent _tls/_http pair.  _tls may already be connected
        // (between-turn reuse); if not, HTTPClient reconnects automatically.

        _http.begin(_tls, gSrvHost.c_str(), gSrvPort, "/transcribe", true);
        _http.setAuthorization(gSrvUser.c_str(), gSrvPass.c_str());
        _http.addHeader("Content-Type", "application/json");
        _http.addHeader("X-Ph3b3-Device", "stackchan");
        _http.setConnectTimeout(20000);
        _http.setTimeout(30000);

        int code = _http.POST((uint8_t*)jbuf, pos);
        heap_caps_free(jbuf); jbuf = nullptr;

        _heardText = "";
        _visionTurn = false;
        if (code == HTTP_CODE_OK) {
            String body = _http.getString();
            JsonDocument doc;
            deserializeJson(doc, body);
            const char* t = doc["text"];
            if (t && *t) _heardText = String(t);
            // Nyx routes vision intent server-side; "camera":"dio" means this
            // utterance goes to HER own camera → run the native photo loop.
            const char* cam = doc["camera"];
            _visionTurn = (cam && strcmp(cam, "dio") == 0);
        }
        _http.end();  // keeps _tls alive if server returned Connection: keep-alive
        Serial.printf("[talk] transcribe=%d heard='%s'\n", code, _heardText.c_str());

        if (_heardText.length() == 0) {
            String err = (code > 0) ? "http " + String(code) :
                         (code == 0) ? "timeout" : "err " + String(code);
            if (_inConversation && millis() - _convLastValidMs < CONV_IDLE_MS) {
                Serial.printf("[talk] transcribe error in conv: %s — re-await\n", err.c_str());
                _startAwaiting(); return;
            }
            face.setState(Ph3b3Face::ERROR);
            _inConversation = false;
            _phase = PH_ERROR;
            return;
        }

        // Gaze intent — on-device keyword match, no server contract change
        if (_isGazeIntent(_heardText)) face.setGaze(0.0f, 0.0f);

        // Junk filter: short transcripts (noise artifacts) don't trigger /chat
        if (_isJunkTranscript(_heardText)) {
            Serial.printf("[talk] junk transcript '%s' — discarded\n", _heardText.c_str());
            if (_inConversation && millis() - _convLastValidMs >= CONV_IDLE_MS) {
                _inConversation = false;
            }
            _startAwaiting(); return;  // always re-await; idle clock unchanged
        }

        // Exit phrase detection — "goodbye" + Phoebe-name variant required; bare "goodbye" stays in loop
        _exitAfterTurn = _isFarewellToName(_heardText);

        if (_visionTurn) {
            // Native photo loop: capture on HER camera, show it on the LCD, POST to
            // Nyx — all BEFORE /chat, so look() consumes this fresh push instead of
            // pulling :8080. Capture+draw run before any TX (brownout-safe). On
            // success the photo is held on-screen through the spoken description; on
            // failure Nyx speaks the honest "camera offline" miss (no frame posted).
            bool captured = camVisionCapture();
            if (!captured) {
                face.setState(Ph3b3Face::ERROR);
                face.update();
                Serial.println("[talk] vision capture FAILED — server speaks the miss");
            }
        } else {
            face.setState(Ph3b3Face::THINKING);
            face.update();
        }
        // Thinking tell — direct servo command: loop() is blocked here, can't use applyBodyLanguage.
        M5StackChan.Motion.moveX(160, 200);   // 16° off-axis contemplative look
        M5StackChan.Motion.moveY(390, 180);   // 39° — soft upward gaze

        // ── Step 3: POST /chat ────────────────────────────────────────────────
        Serial.printf("[D3] /chat heard='%s'\n", _heardText.c_str());
        _replyText = _doChatAndPlay(_heardText);
        bool ok = !_replyText.startsWith("ERR") && !_replyText.startsWith("HTTP") &&
                  !_replyText.startsWith("(no");
        Serial.printf("[D4] /chat reply='%.60s' ok=%d\n", _replyText.c_str(), (int)ok);

        // Native photo loop teardown: the spoken description has finished, so
        // release the held photo and hand the LCD back to the face. Runs on every
        // exit path below (barge-in / exit word / normal). No-op if capture failed.
        if (_visionTurn) {
            _visionTurn = false;
            if (g_faceOwnedByCamera) {
                g_faceOwnedByCamera = false;
                face.clearBubbleNow();
                face.setState(Ph3b3Face::IDLE);
                face.update();
            }
        }

        // Barge-in: tap during SPEAK → reset idle and go straight to LISTEN
        if (_bargeIn) {
            _bargeIn = false;
            _exitAfterTurn = false;
            if (ok) _convLastValidMs = millis();
            face.releaseGaze();
            _startRecording();
            return;
        }

        // Update idle clock on a good turn (only if not exit-word — exit closes the session)
        if (ok && !_exitAfterTurn) _convLastValidMs = millis();

        // Exit word: give the sign-off, then drop to ready
        if (_exitAfterTurn) {
            _exitAfterTurn = false;
            _endConversation();
            return;
        }

        // Continuous loop: SPEAK done → re-arm directly to LISTENING (no ambient wait).
        // Exit conditions are tap (handled in PH_RECORDING) and farewell phrase only.
        if (ok && _inConversation) {
            _convLastValidMs = millis();
            face.releaseGaze();   // release any gaze lock so LISTENING settle fires in loop()
            Serial.println("[D5] CC re-arm → LISTEN");
            _startRecording();
            return;
        }

        Serial.printf("[D5] conv end ok=%d inConv=%d\n", (int)ok, (int)_inConversation);
        face.setState(ok ? Ph3b3Face::IDLE : Ph3b3Face::ERROR);
        _phase = ok ? PH_DONE : PH_ERROR;
    }

    // ── Conversation helpers ──────────────────────────────────────────────────
    bool _isJunkTranscript(const String& text) {
        String t = text; t.trim();
        return t.length() < (uint32_t)JUNK_MIN_LEN;
    }

    bool _isGazeIntent(const String& text) {
        String t = text; t.toLowerCase();
        return t.indexOf("look at me")  >= 0
            || t.indexOf("face me")     >= 0
            || t.indexOf("eyes up")     >= 0
            || t.indexOf("look here")   >= 0;
    }

    bool _isPhoebeName(const String& t) {
        // t must already be lowercased; covers common Whisper transcription variants
        return t.indexOf("phoebe")  >= 0
            || t.indexOf("pheobe")  >= 0
            || t.indexOf("phoeby")  >= 0
            || t.indexOf("feeby")   >= 0
            || t.indexOf("phoebee") >= 0
            || t.indexOf("phoebi")  >= 0
            || t.indexOf("feebs")   >= 0
            || t.indexOf("foebe")   >= 0
            || t.indexOf("pheeby")  >= 0
            || t.indexOf("feebe")   >= 0;
    }

    bool _isFarewellToName(const String& text) {
        String t = text; t.toLowerCase(); t.trim();
        bool hasGoodbye = t.indexOf("goodbye") >= 0 || t.indexOf("good bye") >= 0;
        return hasGoodbye && _isPhoebeName(t);
    }

    // High-passed RMS for LEVEL detection ONLY (single-pole HP, HP_ALPHA ≈ 275 Hz).
    // Fan/HEPA rumble is low-frequency; removing it makes the meter track speech, not
    // noise — the main fix for endpointing in a noisy room. The audio that ships to
    // Whisper is always the raw buffer; this filtered copy never leaves this function.
    // Stateless (xPrev seeded from buf[0], y=0) so there's no cross-chunk startup step.
    // Synthesise the pet chirp: a gentle pitch rise (~420→560 Hz — well within the
    // tiny CoreS3 speaker's range, unlike a bass purr) with a ~32 Hz tremolo giving
    // it a trill/purr quality, under a short attack/release envelope. Phase is
    // integrated so the glide has no discontinuities. Filled once, then reused.
    void _genPurr() {
        const float dur = (float)PURR_SAMPLES / PURR_RATE;
        float phase = 0.0f;
        for (int i = 0; i < PURR_SAMPLES; i++) {
            float t    = (float)i / PURR_RATE;
            float env  = min(t / 0.03f, (dur - t) / 0.08f);   // 30ms attack, 80ms release
            if (env < 0.0f) env = 0.0f;
            if (env > 1.0f) env = 1.0f;
            float freq = 420.0f + 140.0f * (t / dur);         // gentle upward chirp
            phase += 2.0f * M_PI * freq / PURR_RATE;
            float trem = 0.6f + 0.4f * sinf(2.0f * M_PI * 32.0f * t);   // purr/trill roll
            _purrBuf[i] = (int16_t)(sinf(phase) * trem * env * 15000.0f);
        }
    }

    static float _hpRms(const int16_t* buf, int n) {
        if (n < 2) return 0.0f;
        float xPrev = buf[0] / 32768.0f, y = 0.0f, acc = 0.0f;
        for (int i = 1; i < n; i++) {
            float x = buf[i] / 32768.0f;
            y = HP_ALPHA * (y + x - xPrev);
            xPrev = x;
            acc += y * y;
        }
        return sqrtf(acc / (n - 1));
    }

    // [DBG-STATE] fire-and-forget UDP state ping to Nyx — readable in the ph3b3
    // journal (grep DIO_STATE). Cheap: no handshake, non-blocking, drops silently.
    void _dbgState(const char* s) {
        if (WiFi.status() != WL_CONNECTED || gSrvHost.length() == 0) return;
        // WiFiUDP must be begin()'d once before it will send — without it
        // beginPacket() silently returns 0 and the packet never leaves the device
        // (why DIO_STATE telemetry never reached Nyx despite the listener being up).
        if (!_dbgUdpReady) _dbgUdpReady = (_dbgUdp.begin(DBG_STATE_PORT + 1) == 1);
        if (!_dbgUdpReady) return;
        _dbgUdp.beginPacket(gSrvHost.c_str(), DBG_STATE_PORT);
        _dbgUdp.printf("%lu %s", (unsigned long)millis(), s);
        _dbgUdp.endPacket();
    }

    // Dead-air exit: leave always-listening and go dormant (tap to resume).
    // Distinct from _endConversation(), which re-arms AWAITING.
    void _exitChatMode() {
        if (_phase == PH_AWAITING || _phase == PH_RECORDING) M5.Mic.end();
        face.releaseGaze();
        face.clearBubble();
        face.setState(Ph3b3Face::IDLE);
        _inConversation  = false;
        _exitAfterTurn   = false;
        _convLastValidMs = 0;
        _phase           = PH_IDLE;
        _dbgState("idle");
    }

    void _endConversation() {
        face.releaseGaze();
        _inConversation  = false;
        _exitAfterTurn   = false;
        _convLastValidMs = 0;
        _startAwaiting();
    }

    // ── /chat/stream + chunked TTS playback ───────────────────────────────────
    // Long replies (a 1000-word story ≈ 5 min ≈ 12 MB of audio) cannot arrive as
    // one monolithic /chat blob — the ESP32 has ~300 KB RAM and the single long
    // TLS transfer times out. Consume /chat/stream instead: a small manifest
    // {response, stream_id, chunk_count, audio(chunk0)} then lazy
    // GET /tts/chunk/{sid}/{n}. Each chunk is a short independent request the
    // server synthesises on demand (~1.3 s, with read-ahead); Dio plays + discards.
    String _doChatAndPlay(const String& message) {
        if (!_http.begin(_tls, gSrvHost.c_str(), gSrvPort, "/chat/stream", true))
            return String("ERR: begin failed");
        _http.setConnectTimeout(90000);
        _http.setTimeout(120000);
        _http.setAuthorization(gSrvUser.c_str(), gSrvPass.c_str());
        _http.addHeader("Content-Type", "application/json");
        _http.addHeader("X-Ph3b3-Device", "stackchan");

        JsonDocument body;
        body["message"]    = message;
        body["session_id"] = _sessionId;
        String payload;
        serializeJson(body, payload);

        face.update();
        _dbgState("sent /chat");
        int code = _http.POST(payload);
        Serial.printf("[D3b] /chat/stream POST code=%d\n", code);
        if (code != HTTP_CODE_OK) { _http.end(); return "HTTP " + String(code); }
        face.update();

        // ── Shared decode/playback state (persists across chunks → gapless queue) ─
        static const int PEEK_MAX = 6144;
        static char      peek[PEEK_MAX + 1];
        static int16_t   pcmBuf[2][CHUNK_SAMP];
        int   fillIdx       = 0;
        int   chunkPos      = 0;
        int   wavHdrSkipped = 0;
        uint8_t halfLo      = 0;
        bool  halfReady     = false;
        char  b4[4]; int b4pos = 0;
        bool  keepGoing     = true;   // false at end of ONE chunk's audio field
        bool  abort         = false;  // barge-in / watchdog → stop the whole reply
        uint32_t speakSweepMs = 0;

        auto flushChunk = [&]() {
            if (chunkPos == 0) return;
            float rms = 0.0f;
            for (int i = 0; i < chunkPos; i++) {
                float s = pcmBuf[fillIdx][i] / 32768.0f;
                rms += s * s;
            }
            face.setSpeakingLevel(min(1.0f, sqrtf(rms / chunkPos) * 5.0f));
            while (M5.Speaker.isPlaying(0) >= 2) delay(1);
            if (abort) return;   // barge-in/watchdog only; gating on !keepGoing here
                                 // would drop every chunk's tail samples.
            M5.Speaker.playRaw(pcmBuf[fillIdx], chunkPos, 22050, false, 1, 0);
            fillIdx ^= 1;
            chunkPos = 0;
        };

        auto pushByte = [&](uint8_t b) {
            if (wavHdrSkipped++ < 44) return;     // skip this chunk's 44-byte WAV header
            if (!halfReady) { halfLo = b; halfReady = true; return; }
            if (chunkPos == 0) {
                while (M5.Speaker.isPlaying(0) >= 2) delay(1);
            }
            pcmBuf[fillIdx][chunkPos++] = (int16_t)((b << 8) | halfLo);
            halfReady = false;
            if (chunkPos == CHUNK_SAMP) flushChunk();
        };

        auto feedCh = [&](char ch) {
            if (!keepGoing) return;
            if (ch == '"') { keepGoing = false; return; }
            int v = _b64val(ch);
            if (v < 0) return;
            b4[b4pos++] = ch;
            if (b4pos == 4) {
                int v0=_b64val(b4[0]), v1=_b64val(b4[1]),
                    v2=_b64val(b4[2]), v3=_b64val(b4[3]);
                if (v0 >= 0 && v1 >= 0) {
                    pushByte((uint8_t)((v0<<2)|(v1>>4)));
                    if (v2 >= 0 && b4[2] != '=') {
                        pushByte((uint8_t)((v1<<4)|(v2>>2)));
                        if (v3 >= 0 && b4[3] != '=') pushByte((uint8_t)((v2<<6)|v3));
                    }
                }
                b4pos = 0;
            }
        };

        // Read the head of the current HTTP response into peek[], stopping once the
        // "audio":" field start is buffered (or the whole short body is in). Returns len.
        auto fillPeek = [&]() -> int {
            auto* raw = _http.getStreamPtr();
            raw->setTimeout(30000);
            int bodyTotal = _http.getSize();
            int pl = 0;
            // Inter-byte watchdog, NOT a wall-clock cap: reset on every byte, give
            // up only after PEEK_SILENCE_MS of dead air. A slow-but-progressing head
            // read (Nyx busy synthesising, e.g. GPU-locked) is never truncated; a
            // hung/killed stream is caught. Mirrors the Iris fix.
            uint32_t lastRxMs = millis();
            while (pl < PEEK_MAX) {
                int avail = raw->available();
                if (avail > 0) {
                    int n = min(avail, PEEK_MAX - pl);
                    pl += raw->readBytes(peek + pl, n);
                    peek[pl] = '\0';
                    lastRxMs = millis();
                    if (pl > 20 && strstr(peek, "\"audio\":\"")) break;
                    if (bodyTotal > 0 && pl >= bodyTotal) break;
                } else if (!raw->connected()) {
                    break;
                } else if (bodyTotal > 0 && pl >= bodyTotal) {
                    break;
                } else if (millis() - lastRxMs >= PEEK_SILENCE_MS) {
                    Serial.printf("[peek] silence %ums — stream stalled, giving up\n",
                                  (unsigned)PEEK_SILENCE_MS);
                    break;
                } else {
                    face.update();
                    delay(10);
                }
            }
            peek[pl] = '\0';
            return pl;
        };

        // Decode + play ONE base64 WAV "audio" field: peek[audioStart..peekLen) is the
        // already-read head, then drain the live stream to the field's closing quote /
        // barge-in / watchdog. Resets per-chunk decode state (each chunk is its own WAV).
        auto playField = [&](int peekLen, int audioStart) {
            wavHdrSkipped = 0; halfReady = false; b4pos = 0; keepGoing = true;
            auto* raw = _http.getStreamPtr();
            for (int i = audioStart; i < peekLen && keepGoing; i++) feedCh(peek[i]);

            uint32_t lastFaceMs     = 0;
            uint32_t lastProgressMs = millis();
            while (keepGoing) {
                bool gotData = false;
                while (keepGoing && raw->available() > 0) {
                    feedCh((char)raw->read());
                    gotData = true;
                }
                if (gotData || M5.Speaker.isPlaying(0) > 0) {
                    lastProgressMs = millis();
                } else if (millis() - lastProgressMs >= PLAYBACK_WATCHDOG_MS) {
                    Serial.printf("[i2s] playback watchdog: no progress %ums — stream hung, tearing down\n",
                                  (unsigned)PLAYBACK_WATCHDOG_MS);
                    M5.Speaker.stop(0); M5.Speaker.end();
                    keepGoing = false; abort = true;
                    break;
                }
                M5.update();
                int16_t tx2, ty2;
                if (M5StackChan.Display().getTouch(&tx2, &ty2)) {
                    M5.Speaker.stop(0); M5.Speaker.end();
                    face.setSpeakingLevel(0.0f);
                    _bargeIn = true; keepGoing = false; abort = true;
                    break;
                }
                if (millis() - lastFaceMs >= FACE_TICK_MS) {
                    lastFaceMs = millis();
                    face.update();
                    if (speakSweepMs > 0 && millis() >= speakSweepMs) {
                        M5StackChan.Motion.moveX(random(-200, 201), 210);
                        M5StackChan.Motion.moveY(450 + random(-25, 26), 190);
                        speakSweepMs = millis() + 900 + random(800);
                    }
                }
                delay(1);
            }
            flushChunk();   // queue this chunk's final partial buffer (unless aborted)
        };

        // Drain trailing body bytes (the small chunk_index/last tail) so the
        // keep-alive TLS socket is clean before the next request reuses it.
        auto drainTail = [&]() {
            auto* raw = _http.getStreamPtr();
            uint32_t idle = millis();
            while (millis() - idle < 60) {
                if (raw->available()) { raw->read(); idle = millis(); }
                else delay(2);
            }
        };

        // ── Manifest: stream_id + chunk_count + chunk 0's text & audio ───────────
        // "text"/"stream_id"/"chunk_count" precede "audio"/"response" in the JSON, so
        // they're always inside the 6 KB peek head regardless of reply length. The full
        // reply lives server-side (echo-guard); Dio captions per chunk instead.
        int peekLen = fillPeek();
        JsonDocument filter;
        filter["stream_id"]   = true;
        filter["chunk_count"] = true;
        filter["text"]        = true;
        JsonDocument jdoc;
        deserializeJson(jdoc, peek, peekLen, DeserializationOption::Filter(filter));
        const char* sid = jdoc["stream_id"];
        String streamId = (sid && *sid) ? String(sid) : "";
        int chunkCount  = jdoc["chunk_count"] | 0;
        const char* t0  = jdoc["text"];
        String chunk0Text   = (t0 && *t0) ? String(t0) : "";
        String responseText = chunk0Text;   // return/log value (first slice of the reply)

        char* foundTag = strstr(peek, "\"audio\":\"");
        int audioStart = foundTag ? (int)(foundTag - peek) + 9 : -1;   // 9 = strlen("\"audio\":\"")

        if (chunkCount > 0 && audioStart >= 0) {
            // Speaking pose + mood beat once; caption chunk 0, then play it.
            _applyMoodReaction(chunk0Text);
            face.setState(Ph3b3Face::SPEAKING);
            _dbgState("playing");
            M5StackChan.Motion.moveX(0, 280);
            M5StackChan.Motion.moveY(450, 280);
            speakSweepMs = millis() + 800;
            face.setBubble(chunk0Text);
            face.update();
            playField(peekLen, audioStart);          // chunk 0 (carried in the manifest)
            drainTail();
            _http.end();

            // ── Chunks 1..N-1: lazy GET, play each ──────────────────────────────
            for (int n = 1; n < chunkCount && !abort; n++) {
                String path = "/tts/chunk/" + streamId + "/" + String(n);
                if (!_http.begin(_tls, gSrvHost.c_str(), gSrvPort, path.c_str(), true)) break;
                _http.setConnectTimeout(30000);
                _http.setTimeout(60000);
                _http.setAuthorization(gSrvUser.c_str(), gSrvPass.c_str());
                _http.addHeader("X-Ph3b3-Device", "stackchan");
                int gc = _http.GET();
                if (gc != HTTP_CODE_OK) {
                    Serial.printf("[chunk %d] GET code=%d — stopping\n", n, gc);
                    _http.end(); break;
                }
                int cpl = fillPeek();
                JsonDocument cfilter; cfilter["text"] = true;
                JsonDocument cjdoc;
                deserializeJson(cjdoc, peek, cpl, DeserializationOption::Filter(cfilter));
                const char* ctx = cjdoc["text"];
                char* ct = strstr(peek, "\"audio\":\"");
                int cAudio = ct ? (int)(ct - peek) + 9 : -1;
                if (cAudio < 0) { Serial.printf("[chunk %d] no audio field\n", n); _http.end(); break; }
                if (ctx && *ctx) face.setBubbleText(String(ctx));   // caption this chunk, synced to its audio
                playField(cpl, cAudio);
                drainTail();
                _http.end();
            }

            // Post-drain: let the last queued audio finish (barge-in still honoured).
            while (M5.Speaker.isPlaying(0)) {
                M5.update();
                int16_t tx2, ty2;
                if (M5StackChan.Display().getTouch(&tx2, &ty2)) {
                    M5.Speaker.stop(0); M5.Speaker.end();
                    face.setSpeakingLevel(0.0f);
                    _bargeIn = true;
                    break;
                }
                face.update();
                delay(50);
            }
            if (!_bargeIn) {
                Serial.println("[i2s] speaker.end() — normal drain complete");
                M5.Speaker.end();
                delay(15);
            }
        } else {
            _http.end();
        }

        return responseText.length() > 0 ? responseText : String("(no response)");
    }

    // ── Mood reaction — expression beat before TTS plays ─────────────────────
    void _applyMoodReaction(const String& text) {
        String t = text; t.toLowerCase();
        Ph3b3Face::State mood = Ph3b3Face::SPEAKING;

        if (t.indexOf("obviously") >= 0 || t.indexOf("clearly") >= 0 ||
            t.indexOf("actually")  >= 0 || t.indexOf("sarcas")  >= 0 ||
            t.indexOf("really")    >= 0) {
            mood = Ph3b3Face::THINKING;   // sly squint before the quip

        } else if (t.indexOf("sorry")   >= 0 || t.indexOf("unfortunately") >= 0 ||
                   t.indexOf("can't")   >= 0 || t.indexOf("cannot")        >= 0 ||
                   t.indexOf("unable")  >= 0) {
            mood = Ph3b3Face::ERROR;      // blue "bad news" look

        } else if (t.indexOf("fascinating") >= 0 || t.indexOf("interesting") >= 0 ||
                   t.indexOf("love")        >= 0 || t.indexOf("brilliant")   >= 0 ||
                   t.indexOf("excellent")   >= 0) {
            mood = Ph3b3Face::LISTENING;  // wide-eyed excitement
        }

        if (mood != Ph3b3Face::SPEAKING) {
            face.setState(mood);
            for (int i = 0; i < 5; i++) { face.update(); delay(60); }
        }
    }

};
