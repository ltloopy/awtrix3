// See tests/stubs/Globals.h for the include-guard substitution mechanism.
// Stub for the propagation-surface transport: records each sendTimerSync payload
// so tests can assert what TimerManager broadcast (and how many packets).
#ifndef ServerManager_h
#define ServerManager_h

#include <Arduino.h>
#include <vector>

class ServerManager_ {
public:
    void sendTimerSync(const String &payload) { sent.push_back(payload); }

    std::vector<String> sent;

    void __test_reset() { sent.clear(); }
};

extern ServerManager_ ServerManager;

#endif
