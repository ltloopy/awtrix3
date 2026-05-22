// See tests/stubs/Globals.h for the include-guard substitution mechanism.
#ifndef PeripheryManager_h
#define PeripheryManager_h

#include <Arduino.h>
#include <vector>

class EasyButtonStub {
public:
    bool pressed = false;
    unsigned long pressedForMs = 0;
    bool isPressed() const { return pressed; }
    bool pressedFor(unsigned long ms) const { return pressed && pressedForMs >= ms; }
};

class PeripheryManager_ {
public:
    bool isPlaying() const { return playing_; }
    void stopSound() { stop_calls++; playing_ = false; }
    const char *playRTTTLString(String rtttl) {
        play_calls.push_back(rtttl);
        playing_ = true;
        return nullptr;
    }
    String resolveRtttl(const String &, const char *fallback = nullptr) {
        return fallback ? String(fallback) : String();
    }

    EasyButtonStub *buttonL = nullptr;
    EasyButtonStub *buttonR = nullptr;

    std::vector<String> play_calls;
    int stop_calls = 0;
    void __test_set_playing(bool v) { playing_ = v; }
    void __test_reset() { play_calls.clear(); stop_calls = 0; playing_ = false; }

private:
    bool playing_ = false;
};

using EasyButton = EasyButtonStub;

extern PeripheryManager_ PeripheryManager;

#endif
