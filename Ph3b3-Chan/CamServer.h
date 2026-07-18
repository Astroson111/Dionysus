#pragma once
// ── CamServer — Stack-Chan camera → Ph3b3 server (Phase 2) ────────────────────
// Owns Dio's CoreS3 GC0308 camera and a small HTTP control server on :8080:
//   POST /snapshot        pause face → capture → POST JPEG to server → resume
//   POST /monitor/start   ghost-hunt: capture on on-device motion, POST frames
//   POST /monitor/stop    stop monitor mode
//   GET  /cam/status      {"camera":bool,"monitor":bool}
// Verified spikes: the camera works on the approved pinout AND shares M5's
// internal I2C bus (sccb_i2c_port=1, no In_I2C.release) — servos survive.
// The face is never touched directly: a capture sets g_faceOwnedByCamera so
// loop() skips face.update(); it is ALWAYS cleared, even on failure.
#include <Arduino.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "esp_camera.h"
#include "img_converters.h"

// Set by a capture so the render loop pauses the face. Wrap, never touch render.
extern volatile bool g_faceOwnedByCamera;
// Server target (defined in the .ino).
extern String gSrvHost; extern int gSrvPort; extern String gSrvUser; extern String gSrvPass;

class CamServer {
public:
    void begin(uint16_t port = 8080) {
        camera_config_t c = {};
        c.ledc_channel = LEDC_CHANNEL_0;
        c.ledc_timer   = LEDC_TIMER_0;
        c.pin_d0 = 39; c.pin_d1 = 40; c.pin_d2 = 41; c.pin_d3 = 42;
        c.pin_d4 = 15; c.pin_d5 = 16; c.pin_d6 = 48; c.pin_d7 = 47;
        c.pin_xclk  = -1; c.pin_pclk = 45; c.pin_vsync = 46; c.pin_href = 38;
        c.pin_sccb_sda = -1; c.pin_sccb_scl = -1; c.sccb_i2c_port = 1;  // share M5 I2C
        c.pin_pwdn = -1; c.pin_reset = -1;
        c.xclk_freq_hz = 20000000;
        c.pixel_format = PIXFORMAT_RGB565;   // raw → frame2jpg for POST + motion diff
        c.frame_size   = FRAMESIZE_QVGA;     // 320x240; plenty for LLaVA
        c.jpeg_quality = 12;
        c.fb_count     = 2;
        c.fb_location  = CAMERA_FB_IN_PSRAM;
        c.grab_mode    = CAMERA_GRAB_LATEST;
        _camOk = (esp_camera_init(&c) == ESP_OK);
        Serial.printf("[cam] esp_camera_init %s\n", _camOk ? "OK" : "FAIL");

        _http = new WebServer(port);
        _http->on("/snapshot", HTTP_POST, [this] {
            if (!_camOk) { _http->send(503, "text/plain", "no camera"); return; }
            bool ok = _captureAndPost();
            _http->send(ok ? 200 : 502, "text/plain", ok ? "captured" : "capture/post failed");
        });
        _http->on("/monitor/start", HTTP_POST, [this] {
            _monitor = _camOk; _haveRef = false;
            _http->send(200, "text/plain", _monitor ? "monitoring" : "no camera");
        });
        _http->on("/monitor/stop", HTTP_POST, [this] {
            _monitor = false; _http->send(200, "text/plain", "stopped");
        });
        _http->on("/cam/status", HTTP_GET, [this] {
            String j = String("{\"camera\":") + (_camOk ? "true" : "false") +
                       ",\"monitor\":" + (_monitor ? "true" : "false") + "}";
            _http->send(200, "application/json", j);
        });
        _http->begin();
        Serial.printf("[cam] control server on :%u\n", port);
    }

    // Call every loop() iteration.
    void handle() {
        if (_http) _http->handleClient();
        if (_monitor && _camOk && millis() - _lastMotionMs > 700) {
            _lastMotionMs = millis();
            _motionTick();
        }
    }

    bool cameraOk() const { return _camOk; }

    // ── Native photo loop (PUSH-primary) ──────────────────────────────────────
    // Called from the Talk flow on a vision intent routed to Dio's own camera:
    // capture ONE frame, draw it to the LCD immediately, then POST it to Nyx.
    // Ordering is deliberate for the worst-case current spike (camera + WiFi TX +
    // LCD): capture and draw FIRST, transmit AFTER — never capture while the radio
    // is transmitting. On success g_faceOwnedByCamera stays SET so the photo holds
    // on-screen through the spoken description; the Talk flow clears it when the
    // reply finishes. On any failure it is cleared so the error face can show.
    bool captureDisplayPush() {
        if (!_camOk) return false;
        g_faceOwnedByCamera = true;               // pause the face before we paint
        bool posted = false;
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
            if (fb->format == PIXFORMAT_RGB565) { // QVGA RGB565 == the 320x240 LCD
                M5.Display.startWrite();
                M5.Display.setSwapBytes(true);    // esp_camera RGB565 is byte-swapped vs M5GFX
                M5.Display.pushImage(0, 0, fb->width, fb->height, (uint16_t*)fb->buf);
                M5.Display.setSwapBytes(false);
                M5.Display.endWrite();
            }
            uint8_t* jpg = nullptr; size_t jlen = 0;
            if (frame2jpg(fb, 80, &jpg, &jlen)) { posted = _postJpeg(jpg, jlen); free(jpg); }
            esp_camera_fb_return(fb);
        }
        if (!posted) g_faceOwnedByCamera = false; // failed → release, let the error show
        Serial.printf("[cam] captureDisplayPush posted=%d\n", (int)posted);
        return posted;
    }

private:
    WebServer* _http    = nullptr;
    bool       _camOk   = false;
    bool       _monitor = false;
    uint32_t   _lastMotionMs = 0;

    static const int GW = 20, GH = 15;   // coarse luma grid for motion
    static const int MOTION_THRESH = 900;
    uint8_t _ref[GW * GH];
    bool    _haveRef = false;

    // pause face → grab a frame → JPEG → POST → resume. Face ALWAYS resumes.
    bool _captureAndPost() {
        g_faceOwnedByCamera = true;
        bool ok = false;
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
            uint8_t* jpg = nullptr; size_t jlen = 0;
            if (frame2jpg(fb, 80, &jpg, &jlen)) { ok = _postJpeg(jpg, jlen); free(jpg); }
            esp_camera_fb_return(fb);
        }
        g_faceOwnedByCamera = false;
        return ok;
    }

    bool _postJpeg(const uint8_t* buf, size_t len) {
        if (gSrvHost.length() == 0 || gSrvPort == 0) return false;
        WiFiClientSecure tls; tls.setInsecure(); tls.setTimeout(8000);
        HTTPClient http;
        if (!http.begin(tls, gSrvHost.c_str(), gSrvPort, "/vision/frame", true)) return false;
        http.setAuthorization(gSrvUser.c_str(), gSrvPass.c_str());
        http.addHeader("X-Ph3b3-Device", "stackchan");
        http.addHeader("Content-Type", "image/jpeg");
        http.setTimeout(8000);
        int code = http.POST((uint8_t*)buf, len);
        http.end();
        return code == 200;
    }

    // On-device motion: coarse luma grid diff vs the previous frame. On motion,
    // JPEG + POST. Runs only while _monitor (investigation) is on — no timer spam.
    void _motionTick() {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) return;
        if (fb->format != PIXFORMAT_RGB565) { esp_camera_fb_return(fb); return; }
        const uint16_t* px = (const uint16_t*)fb->buf;
        int w = fb->width, h = fb->height;
        uint8_t grid[GW * GH];
        for (int gy = 0; gy < GH; gy++)
            for (int gx = 0; gx < GW; gx++) {
                uint16_t p = px[(gy * h / GH) * w + (gx * w / GW)];
                uint8_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
                grid[gy * GW + gx] = (uint8_t)((r * 2 + g + b * 2));
            }
        int score = 0;
        if (_haveRef) for (int i = 0; i < GW * GH; i++) score += abs((int)grid[i] - (int)_ref[i]);
        memcpy(_ref, grid, GW * GH); _haveRef = true;
        if (score > MOTION_THRESH) {
            g_faceOwnedByCamera = true;
            uint8_t* jpg = nullptr; size_t jlen = 0;
            if (frame2jpg(fb, 80, &jpg, &jlen)) { _postJpeg(jpg, jlen); free(jpg); }
            g_faceOwnedByCamera = false;
            Serial.printf("[cam] motion score=%d → posted\n", score);
        }
        esp_camera_fb_return(fb);
    }
};
