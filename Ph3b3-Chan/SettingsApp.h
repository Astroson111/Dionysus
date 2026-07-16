#pragma once
#include "AppBase.h"
#include "AppManager.h"
#include "SettingsStore.h"
#include <M5Unified.h>
#include <M5StackChan.h>

extern bool       g_overlayOpen;   // crescent menu visible → it owns touch this frame
extern AppManager appMgr;

// Settings — a full-screen menu app (registered last, under the other modes). Rows:
//   WiFi Setup / Microphone / Volume / LED / Color.  Tap a row to cycle/act.
//   Right-to-LEFT swipe anywhere exits back to her face (Talk).
// Owns the screen (ownsScreen()=true) so loop() skips the face push — the panel is
// drawn once per change (no flicker). The crescent drawer still opens (swipe right
// from the top-left tab) to jump to another mode.
class SettingsApp : public AppBase {
public:
    void init() override {
        face.setState(Ph3b3Face::IDLE);
        _wasTouch = false; _ledPreviewEnd = 0; _dirty = true; _prevOverlay = false;
    }

    void update() override {
        face.setState(Ph3b3Face::IDLE);

        if (_ledPreviewEnd && millis() > _ledPreviewEnd) { _ledPreviewEnd = 0; _ledsOff(); }

        // Redraw once after the crescent drawer closes over us.
        if (_prevOverlay && !g_overlayOpen) _dirty = true;
        _prevOverlay = g_overlayOpen;

        if (g_overlayOpen) { _wasTouch = false; return; }   // drawer owns touch

        int16_t tx = 0, ty = 0;
        bool touching = M5StackChan.Display().getTouch(&tx, &ty);
        if (touching) {
            if (!_wasTouch) { _wasTouch = true; _downX = tx; _downY = ty; }
            _lastX = tx; _lastY = ty;                        // track for release classification
        } else if (_wasTouch) {
            _wasTouch = false;
            int dx = _lastX - _downX, dy = _lastY - _downY;
            if (dx < -SWIPE_EXIT && abs(dy) < 55) {          // right-to-left → leave Settings
                appMgr.switchTo(0);                          // back to her face (Talk)
                return;
            }
            if (abs(dx) < 14 && abs(dy) < 14) _onTap(_downX, _downY);  // tap → cycle/act
        }
    }

    void draw() override {
        if (g_overlayOpen) return;      // drawer draws on top; don't fight it
        if (_dirty) { _drawPanel(); _dirty = false; }
    }

    void exit() override { if (_ledPreviewEnd) { _ledPreviewEnd = 0; _ledsOff(); } }

    const char* name() const override { return "Settings"; }
    bool ownsScreen() const override { return true; }

private:
    bool     _wasTouch = false, _dirty = true, _prevOverlay = false;
    int      _downX = 0, _downY = 0, _lastX = 0, _lastY = 0;
    uint32_t _ledPreviewEnd = 0;

    static const int TITLE_Y = 14;   // "Settings"
    static const int HINT_Y  = 34;   // swipe hint
    static const int ROW_Y0  = 52;
    static const int ROW_H   = 34;
    static const int N_ROWS  = 5;    // 0 WiFi, 1 Mic, 2 Volume, 3 LED, 4 Color
    static const int SWIPE_EXIT = 70;

    // BGR-correct color565 (named _col; picolibc defines _C as a macro).
    static uint16_t _col(uint8_t r, uint8_t g, uint8_t b) {
#ifdef SC_FACE_BGR
        return M5StackChan.Display().color565(b, g, r);
#else
        return M5StackChan.Display().color565(r, g, b);
#endif
    }

    void _ledsOff() {
        for (int i = 0; i < 12; i++) M5StackChan.setRgbColor(i, 0, 0, 0);
        M5StackChan.refreshRgb();
    }
    void _ringPreview(uint8_t r, uint8_t g, uint8_t b) {
        for (int i = 0; i < 12; i++) M5StackChan.setRgbColor(i, r, g, b);
        M5StackChan.refreshRgb();
        _ledPreviewEnd = millis() + 1200;
    }
    void _brightnessPreview() {
        const uint8_t* c = SET_LEDC_RGB[gLedColorIdx]; float b = gLedBrightness;
        _ringPreview((uint8_t)(c[0] * b), (uint8_t)(c[1] * b), (uint8_t)(c[2] * b));
    }
    void _colorPreview() {
        const uint8_t* c = SET_LEDC_RGB[gLedColorIdx];
        float b = gLedBrightness > 0.6f ? gLedBrightness : 0.6f;
        _ringPreview((uint8_t)(c[0] * b), (uint8_t)(c[1] * b), (uint8_t)(c[2] * b));
    }

    void _onTap(int tx, int ty) {
        if (ty < ROW_Y0) return;
        int row = (ty - ROW_Y0) / ROW_H;
        if (row < 0 || row >= N_ROWS) return;
        switch (row) {
            case 0: launchWifiPortal();                     break;  // captive setup portal (Dio-Setup: broadcast + on-screen IP)
            case 1: settingsSetMic((gMicIdx + 1) % 3);        break;
            case 2: settingsSetVol((gVolIdx + 1) % 3);        break;
            case 3: settingsSetLed((gLedIdx + 1) % 3); _brightnessPreview(); break;
            case 4: settingsSetLedColor((gLedColorIdx + 1) % SET_LEDC_N); _colorPreview(); break;
        }
        _dirty = true;   // value changed → redraw the panel
    }

    void _drawRow(int row, const char* label, const String& value, bool action) {
        auto& d = M5StackChan.Display();
        int   y = ROW_Y0 + row * ROW_H, w = d.width();
        d.fillRoundRect(8, y + 3, w - 16, ROW_H - 6, 6, _col(18, 10, 44));
        d.drawRoundRect(8, y + 3, w - 16, ROW_H - 6, 6, _col(70, 40, 150));
        d.setTextDatum(middle_left);  d.setTextSize(1);
        d.setTextColor(_col(180, 160, 220), _col(18, 10, 44));
        d.drawString(label, 20, y + ROW_H / 2);
        d.setTextDatum(middle_right);
        d.setTextColor(action ? _col(150, 210, 255) : _col(210, 170, 255), _col(18, 10, 44));
        d.drawString(value.c_str(), w - 20, y + ROW_H / 2);
    }

    void _drawPanel() {
        auto& d = M5StackChan.Display();
        int   w = d.width(), h = d.height();
        d.fillScreen(_col(10, 7, 25));

        // crescent tab (top-left) — redraw since the face push is skipped for this app
        d.fillSmoothCircle(22, 22, 14, _col(200, 100, 255));
        d.fillSmoothCircle(30, 20, 14, TFT_BLACK);

        d.setTextDatum(top_center); d.setTextSize(2);
        d.setTextColor(_col(200, 130, 255), _col(10, 7, 25));
        d.drawString("Settings", w / 2, TITLE_Y);
        d.setTextSize(1);
        d.setTextColor(_col(120, 110, 160), _col(10, 7, 25));
        d.drawString("< swipe left to exit", w / 2, HINT_Y);

        _drawRow(0, "WiFi Setup", "phone",                   true);
        _drawRow(1, "Microphone", SET_MIC_NAMES[gMicIdx],    false);
        _drawRow(2, "Volume",     SET_VOL_NAMES[gVolIdx],    false);
        _drawRow(3, "LED",        SET_LED_NAMES[gLedIdx],    false);
        _drawRow(4, "Color",      SET_LEDC_NAMES[gLedColorIdx], false);
    }
};
