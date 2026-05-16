// See tests/stubs/Globals.h for the include-guard substitution mechanism.
#ifndef DisplayManager_h
#define DisplayManager_h

#include <Arduino.h>
#include <vector>

#include "Overlays.h"

class DisplayManager_ {
public:
    void drainDeferredNotifications() { drain_calls++; }
    void nextApp() { next_app_calls++; }
    void switchToApp(const char *json) {
        switch_to_calls.push_back(String(json ? json : ""));
    }

    int drain_calls = 0;
    int next_app_calls = 0;
    std::vector<String> switch_to_calls;

    void __test_reset() {
        drain_calls = 0;
        next_app_calls = 0;
        switch_to_calls.clear();
    }
};

extern DisplayManager_ DisplayManager;

#endif
