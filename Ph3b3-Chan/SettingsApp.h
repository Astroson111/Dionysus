#pragma once
#include "AppBase.h"
#include "SettingsStore.h"
#include <M5Unified.h>
#include <M5StackChan.h>

extern bool g_overlayOpen;   // crescent menu visible → it owns touch this frame

// Settings — a menu app (registered last, under the other modes). Four rows:
//   WiFi Setup   → launches the captive portal (reboots on save)
//   Microphone   → Low / Medium / High   (tap cycles)
//   Volume       → Low / Medium / High   (tap cycles)
//   LED          → Off / Dim / Full       (tap cycles, previews the ring)
//
// Tap-to-cycle matches the tile idiom (tap = act). The crescent tab stays visible
// above the panel (panel starts below y=PANEL_Y), so a right-swipe on the crescent
// still opens the mode menu to leave Settings — no back button needed.
class SettingsApp : public AppBase {
public:
    void init() override {
        face.setState(Ph3b3Face::IDLE);
        _wasTouch      = false;
        _ledPreviewEnd = 0;
    }

    void update() override {
        face.setState(Ph3b3Face::IDLE);

        // LED preview timeout → clear the ring
        if (_ledPreviewEnd && millis() > _ledPreviewEnd) {
            _ledPreviewEnd = 0;
            _ledsOff();
        }

        if (g_overlayOpen) { _wasTouch = false; return; }  // menu drawer owns touch

        int16_t tx = 0, ty = 0;
        bool touching = M5StackChan.Display().getTouch(&tx, &ty);
        if (touching && !_wasTouch) {          // act on touch-DOWN edge only
            _wasTouch = true;
            _onTap(tx, ty);
        } else if (!touching) {
            _wasTouch = false;
        }
    }

    void draw() override { _drawPanel(); }

    void exit() override {
        if (_ledPreviewEnd) { _ledPreviewEnd = 0; _ledsOff(); }
    }

    const char* name() const override { return "Settings"; }

private:
    bool     _wasTouch      = false;
    uint32_t _ledPreviewEnd = 0;

    // Layout — panel below the crescent tab zone so the tab stays tappable.
    static const int PANEL_Y = 56;
    static const int TITLE_Y = 70;
    static const int ROW_Y0  = 84;
    static const int ROW_H   = 30;
    static const int N_ROWS  = 5;   // 0 WiFi, 1 Mic, 2 Volume, 3 LED, 4 Color

    // BGR-correct color565 (mirrors Ph3b3Face::C / CrescentMenu::_col). Named _col:
    // picolibc defines _C as a macro, which would corrupt a _C(...) helper name.
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

    // Light the ring for ~1.2s as visual feedback (r,g,b already scaled).
    void _ringPreview(uint8_t r, uint8_t g, uint8_t b) {
        for (int i = 0; i < 12; i++) M5StackChan.setRgbColor(i, r, g, b);
        M5StackChan.refreshRgb();
        _ledPreviewEnd = millis() + 1200;
    }
    // Brightness change → show the current color at the new brightness.
    void _brightnessPreview() {
        const uint8_t* c = SET_LEDC_RGB[gLedColorIdx];
        float b = gLedBrightness;
        _ringPreview((uint8_t)(c[0] * b), (uint8_t)(c[1] * b), (uint8_t)(c[2] * b));
    }
    // Color change → show the new color at a visible level (picker-usable even at Dim/Off).
    void _colorPreview() {
        const uint8_t* c = SET_LEDC_RGB[gLedColorIdx];
        float b = gLedBrightness > 0.6f ? gLedBrightness : 0.6f;
        _ringPreview((uint8_t)(c[0] * b), (uint8_t)(c[1] * b), (uint8_t)(c[2] * b));
    }

    void _onTap(int tx, int ty) {
        if (ty < ROW_Y0) return;                    // title / crescent strip
        int row = (ty - ROW_Y0) / ROW_H;
        if (row < 0 || row >= N_ROWS) return;
        switch (row) {
            case 0: launchWifiPortal();                       break;  // never returns (reboots)
            case 1: settingsSetMic((gMicIdx + 1) % 3);        break;
            case 2: settingsSetVol((gVolIdx + 1) % 3);        break;
            case 3: settingsSetLed((gLedIdx + 1) % 3); _brightnessPreview(); break;
            case 4: settingsSetLedColor((gLedColorIdx + 1) % SET_LEDC_N); _colorPreview(); break;
        }
    }

    void _drawRow(int row, const char* label, const String& value, bool action) {
        auto& d  = M5StackChan.Display();
        int   y  = ROW_Y0 + row * ROW_H;
        int   w  = d.width();
        d.fillRoundRect(8, y + 3, w - 16, ROW_H - 6, 6, _col(18, 10, 44));
        d.drawRoundRect(8, y + 3, w - 16, ROW_H - 6, 6, _col(70, 40, 150));
        // label (left)
        d.setTextDatum(middle_left);
        d.setTextSize(1);
        d.setTextColor(_col(180, 160, 220), _col(18, 10, 44));
        d.drawString(label, 20, y + ROW_H / 2);
        // value (right) — accent for action rows / current preset
        d.setTextDatum(middle_right);
        d.setTextColor(action ? _col(150, 210, 255) : _col(210, 170, 255), _col(18, 10, 44));
        d.drawString(value.c_str(), w - 20, y + ROW_H / 2);
    }

    void _drawPanel() {
        auto& d = M5StackChan.Display();
        int   w = d.width(), h = d.height();
        // panel background (leaves the crescent tab strip above PANEL_Y visible)
        d.fillRect(0, PANEL_Y, w, h - PANEL_Y, _col(10, 7, 25));
        // title
        d.setTextDatum(middle_center);
        d.setTextSize(2);
        d.setTextColor(_col(200, 130, 255), _col(10, 7, 25));
        d.drawString("Settings", w / 2, TITLE_Y);

        _drawRow(0, "WiFi Setup", "open", true);
        _drawRow(1, "Microphone", SET_MIC_NAMES[gMicIdx], false);
        _drawRow(2, "Volume",     SET_VOL_NAMES[gVolIdx], false);
        _drawRow(3, "LED",        SET_LED_NAMES[gLedIdx], false);
        _drawRow(4, "Color",      SET_LEDC_NAMES[gLedColorIdx], false);
    }
};
