#pragma once
#include <Arduino.h>

// ── Device settings store ─────────────────────────────────────────────────────
// Three-preset knobs (Volume / Microphone / LED) persisted to NVS namespace "sc"
// as preset INDICES (0/1/2), read + applied at boot. Runtime state lives in the
// g* globals below (defined in Ph3b3-Chan.ino); the apply points read those:
//   gSpeakerVolume    → M5.Speaker.setVolume() in the playback path (TalkApp)
//   gMicMagnification → mcfg.magnification in the capture path (TalkApp)
//   gLedBrightness    → scales the status LED cue (_listenLeds in the .ino)
//
// WiFi Config is not a preset — it launches the existing captive portal
// (launchWifiPortal → _runPortal, which reboots on save).

// Preset tables (index → applied value). static const = per-TU copy, tiny.
static const int   SET_VOL_LEVELS[3] = {102, 178, 255};    // Low 40% / Med 70% / High 100% of 255
static const int   SET_MIC_LEVELS[3] = {4, 6, 8};          // Low / Med(=pre-change) / High magnification
static const float SET_LED_LEVELS[3] = {0.0f, 0.35f, 1.0f};// Off / Dim / Full brightness scale
static const char* SET_VOL_NAMES[3]  = {"Low", "Medium", "High"};
static const char* SET_MIC_NAMES[3]  = {"Low", "Medium", "High"};
static const char* SET_LED_NAMES[3]  = {"Off", "Dim", "Full"};

// Defaults (first boot / missing key): Volume Medium, Mic Medium, LED Full (= pre-change).
static const int   SET_VOL_DEFAULT = 1;
static const int   SET_MIC_DEFAULT = 1;
static const int   SET_LED_DEFAULT = 2;

// Runtime state — DEFINED in Ph3b3-Chan.ino.
extern int   gVolIdx, gMicIdx, gLedIdx;
extern int   gSpeakerVolume;      // = SET_VOL_LEVELS[gVolIdx]
extern int   gMicMagnification;   // = SET_MIC_LEVELS[gMicIdx]
extern float gLedBrightness;      // = SET_LED_LEVELS[gLedIdx]

void settingsLoad();              // read indices from NVS "sc" + apply (call once at boot)
void settingsSetVol(int idx);     // set + persist + apply
void settingsSetMic(int idx);
void settingsSetLed(int idx);

void launchWifiPortal();          // enter the captive portal (defined in .ino; reboots on save)
