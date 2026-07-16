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
// WiFi Config is not a preset — it launches the on-screen WiFi keyboard
// (launchWifiKeyboard: SSID + key on the touchscreen, connect-first).

// Preset tables (index → applied value). static const = per-TU copy, tiny.
static const int   SET_VOL_LEVELS[3] = {102, 178, 255};    // Low 40% / Med 70% / High 100% of 255
static const int   SET_MIC_LEVELS[3] = {4, 6, 8};          // Low / Med(=pre-change) / High magnification
static const float SET_LED_LEVELS[3] = {0.0f, 0.35f, 1.0f};// Off / Dim / Full brightness scale
static const char* SET_VOL_NAMES[3]  = {"Low", "Medium", "High"};
static const char* SET_MIC_NAMES[3]  = {"Low", "Medium", "High"};
static const char* SET_LED_NAMES[3]  = {"Off", "Dim", "Full"};

// LED color palette (applied to the status ring; scaled by brightness above).
// Default Purple keeps the listening-cue color contract; user may override.
static const int     SET_LEDC_N = 9;
static const char*   SET_LEDC_NAMES[SET_LEDC_N] =
    {"Purple", "Pink", "Red", "Orange", "Yellow", "Green", "Cyan", "Blue", "White"};
static const uint8_t SET_LEDC_RGB[SET_LEDC_N][3] = {
    {140,  20, 255},  // Purple (listening default)
    {255,  40, 150},  // Pink
    {255,  30,  30},  // Red
    {255, 110,  10},  // Orange
    {230, 200,  20},  // Yellow
    { 30, 220,  60},  // Green
    { 20, 210, 210},  // Cyan
    { 40,  80, 255},  // Blue
    {220, 220, 230},  // White
};

// Defaults (first boot / missing key): Volume Medium, Mic Medium, LED Full (= pre-change), color Purple.
static const int   SET_VOL_DEFAULT  = 1;
static const int   SET_MIC_DEFAULT  = 1;
static const int   SET_LED_DEFAULT  = 2;
static const int   SET_LEDC_DEFAULT = 0;

// Runtime state — DEFINED in Ph3b3-Chan.ino.
extern int   gVolIdx, gMicIdx, gLedIdx, gLedColorIdx;
extern int   gSpeakerVolume;      // = SET_VOL_LEVELS[gVolIdx]
extern int   gMicMagnification;   // = SET_MIC_LEVELS[gMicIdx]
extern float gLedBrightness;      // = SET_LED_LEVELS[gLedIdx]

void settingsLoad();              // read indices from NVS "sc" + apply (call once at boot)
void settingsSetVol(int idx);     // set + persist + apply
void settingsSetMic(int idx);
void settingsSetLed(int idx);
void settingsSetLedColor(int idx);

void launchWifiKeyboard();        // on-screen touch-keyboard WiFi entry (defined in .ino; connect-first)
