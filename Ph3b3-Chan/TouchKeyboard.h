#pragma once
#include <M5Unified.h>
#include <M5StackChan.h>

// ── On-screen touch keyboard (blocking) ───────────────────────────────────────
// Draws a QWERTY over the whole screen and returns the typed string when the user
// taps "done", or "" if they tap "cancel". Used for device-native WiFi setup so no
// phone/captive-portal is needed. Blocks (its own touch loop) like _runPortal();
// M5StackChan.update() keeps touch/servos alive. mask=true shows '*' for passwords.
inline String tkPrompt(const char* title, bool mask) {
    auto& d = M5StackChan.Display();

    auto col = [&](uint8_t r, uint8_t g, uint8_t b) -> uint16_t {
#ifdef SC_FACE_BGR
        return d.color565(b, g, r);
#else
        return d.color565(r, g, b);
#endif
    };

    // NB: not named LOW/HIGH — those are Arduino GPIO macros (0x0/0x1).
    static const char* KB_LOW[4] = {"1234567890", "qwertyuiop", "asdfghjkl",  "zxcvbnm"};
    static const char* KB_UPP[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL",  "ZXCVBNM"};
    static const char* KB_SYM[4] = {"1234567890", "!@#$%^&*()", "-_=+/:;,.",  "?'\"()[]{}"};
    const int  ROWLEN[4] = {10, 10, 9, 7};
    const int  KW = 30, KH = 34, KB_Y = 52;

    String   text      = "";
    bool     shift     = false;
    int      page      = 0;      // 0 = letters, 1 = symbols
    uint32_t lastKeyMs = 0;      // password reveal: when the latest char was typed
    bool     reveal    = false;  // password reveal: show the latest char in the clear

    auto key = [&](int x, int y, int w, const char* lbl, bool accent) {
        d.fillRoundRect(x, y, w, KH - 3, 4, accent ? col(40, 22, 80) : col(22, 14, 46));
        d.drawRoundRect(x, y, w, KH - 3, 4, col(80, 50, 150));
        d.setTextDatum(middle_center);
        d.setTextSize(1);
        d.setTextColor(col(220, 205, 255), accent ? col(40, 22, 80) : col(22, 14, 46));
        d.drawString(lbl, x + w / 2, y + (KH - 3) / 2);
    };

    // Draw all keys OR hit-test a tap. draw=true renders; else returns an action for
    // (tx,ty): >=32 = that char, -2 back, -3 shift, -4 space, -5 done, -6 cancel,
    // -7 toggle page, -1 none. Geometry is shared so draw and hit never diverge.
    auto keys = [&](bool draw, int tx, int ty) -> int {
        const char** rows = page ? KB_SYM : (shift ? KB_UPP : KB_LOW);
        int hit = -1;
        for (int r = 0; r < 4; r++) {
            int n  = ROWLEN[r] + (r == 3 ? 1 : 0);   // row 3 gets a trailing backspace
            int x0 = (d.width() - n * KW) / 2;        // width-relative (was hardcoded 320)
            int y  = KB_Y + r * KH;
            for (int c = 0; c < n; c++) {
                int  kx     = x0 + c * KW;
                bool isBack = (r == 3 && c == ROWLEN[3]);
                if (draw) {
                    char s[2] = {isBack ? 0 : rows[r][c], 0};
                    key(kx, y, KW - 3, isBack ? "<x" : s, isBack);
                } else if (tx >= kx && tx < kx + KW && ty >= y && ty < y + KH) {
                    hit = isBack ? -2 : (int)rows[r][c];
                }
            }
        }
        // function row
        int fy = KB_Y + 4 * KH;
        struct { const char* l; int a; int w; } fb[5] = {
            {shift ? "SHIFT" : "shift", -3, 54},
            {page ? "abc" : "?123",     -7, 54},
            {"space",                   -4, 96},
            {"cancel",                  -6, 54},
            {"done",                    -5, 58},
        };
        int fx = (d.width() - (54 + 54 + 96 + 54 + 58)) / 2;   // center the function row
        if (fx < 0) fx = 0;
        for (auto& b : fb) {
            if (draw) key(fx, fy, b.w - 3, b.l, b.a == -5);
            else if (tx >= fx && tx < fx + b.w && ty >= fy && ty < fy + KH) hit = b.a;
            fx += b.w;
        }
        return hit;
    };

    auto render = [&]() {
        d.fillScreen(col(8, 6, 20));
        d.setTextDatum(top_left);
        d.setTextSize(1);
        d.setTextColor(col(160, 150, 200), col(8, 6, 20));
        d.drawString(title, 8, 5);
        d.drawRoundRect(6, 20, 308, 24, 4, col(90, 60, 170));
        String shown = text;
        if (mask) {                                  // mask all but the just-typed char
            shown = "";
            int n = (int)text.length();
            for (int i = 0; i < n; i++) shown += (reveal && i == n - 1) ? text[i] : '*';
        }
        d.setTextDatum(middle_left);
        d.setTextColor(col(235, 225, 255), col(8, 6, 20));
        d.drawString(shown.c_str(), 12, 32);
        keys(true, -1, -1);
    };

    // Draw the new prompt FIRST, then wait out any touch carried over from the
    // screen that handed off to us. We return on touch-DOWN of "done", so the
    // finger that ended the previous prompt is still on the glass when the next
    // one opens — and "done" is at the same coordinates on every keyboard, so
    // without this the password prompt would instantly "press" its own done and
    // return empty. Rendering before the wait keeps it from looking like a hang.
    render();
    {
        int16_t hx = 0, hy = 0;
        while (d.getTouch(&hx, &hy)) { M5StackChan.update(); delay(8); }
        delay(80);   // debounce the release
    }

    bool wasTouch = false, dirty = false;   // render() above already drew it
    for (;;) {
        M5StackChan.update();
        // Password reveal: keep the latest char visible ~1s, then re-mask it.
        if (mask && reveal && millis() - lastKeyMs >= 1000) { reveal = false; dirty = true; }
        if (dirty) { render(); dirty = false; }
        int16_t tx = 0, ty = 0;
        bool touching = d.getTouch(&tx, &ty);
        if (touching && !wasTouch) {
            wasTouch = true;
            int a = keys(false, tx, ty);
            if (a >= 32)        { text += (char)a; if (shift) shift = false;
                                  if (mask) { reveal = true; lastKeyMs = millis(); } dirty = true; }
            else if (a == -2)   { if (text.length()) text.remove(text.length() - 1);
                                  reveal = false; dirty = true; }
            else if (a == -3)   { shift = !shift; dirty = true; }
            else if (a == -4)   { text += ' ';
                                  if (mask) { reveal = true; lastKeyMs = millis(); } dirty = true; }
            else if (a == -7)   { page = page ? 0 : 1; dirty = true; }
            else if (a == -5)   { return text; }
            else if (a == -6)   { return String(); }
        } else if (!touching) {
            wasTouch = false;
        }
        delay(8);
    }
}
