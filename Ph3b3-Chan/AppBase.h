#pragma once
#include "ph3b3_face.h"

// Forward-declared globals set by stackchan_rung4.ino
extern Ph3b3Face face;

class AppBase {
public:
    virtual ~AppBase() = default;
    virtual void init()   = 0;  // called once on switch-in
    virtual void update() = 0;  // called every loop; sets face state + app logic
    virtual void draw()   {}    // called after face.update(); draw overlays here
    virtual void exit()   {}    // called on switch-out
    virtual const char* name() const = 0;
    // Full-screen apps (own the whole display) return true → loop() skips face.update()
    // so the app's own draw() isn't overwritten every frame (no flicker).
    virtual bool ownsScreen() const { return false; }
};
