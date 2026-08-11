// See tests/stubs/Globals.h for the include-guard substitution mechanism.
// Minimal Notification subset — only the fields TimerManager.cpp touches.
#ifndef Overlays_H
#define Overlays_H

#include <Arduino.h>
#include <vector>

#include "LittleFS.h"

struct Notification {
    bool center = false;
    String text;
    uint32_t color = 0;
    bool noScrolling = true;
    long duration = 0;
    int blink = 0;
    unsigned long startime = 0;
    bool hold = false;
    bool wakeup = false;
    String rtttl;
    String channel;
    File icon;
};

extern std::vector<Notification> notifications;

#endif
