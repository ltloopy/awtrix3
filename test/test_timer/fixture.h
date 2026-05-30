#pragma once

#include <ArduinoFake.h>
#include <vector>

#include "Preferences.h"
#include "Overlays.h"
#include "MQTTManager.h"
#include "PeripheryManager.h"
#include "DisplayManager.h"
#include "Globals.h"
#include "../../src/TimerManager.h"

using namespace fakeit;

namespace fixture {

inline unsigned long &virtual_now() {
    static unsigned long t = 0;
    return t;
}

inline void advance(uint32_t ms) {
    virtual_now() += ms;
}

inline void install_millis_mock() {
    When(Method(ArduinoFake(), millis)).AlwaysDo([]() { return virtual_now(); });
}

inline void reset_all() {
    ArduinoFakeReset();
    install_millis_mock();
    virtual_now() = 0;

    MQTTManager.__test_reset();
    PeripheryManager.__test_reset();
    DisplayManager.__test_reset();
    Preferences::__test_reset();
    notifications.clear();
    saveSettings_calls = 0;

    CURRENT_APP       = "Time";
    SOUND_ACTIVE      = true;
    BLOCK_NAVIGATION  = false;
    GAME_ACTIVE       = false;
    MATRIX_OFF        = false;
    BRIGHTNESS        = 100;

    SHOW_TIMER              = true;
    TIMER_MAX_DURATION      = 86400;
    TIMER_STEP              = 1;
    TIMER_PUBLISH_INTERVAL  = 1;
    TIMER_FINISHED_HOLD     = 10;
    TIMER_REALERT_INTERVAL  = 15;
    TIMER_COUNTDOWN_SECONDS = 3;
    TIMER_CONFIG_TIMEOUT    = 30;
    TIMER_ICON_IDLE         = "";
    TIMER_ICON_RUNNING      = "";
    TIMER_ICON_PAUSED       = "";
    TIMER_ICON_FINISHED     = "";
    TIMER_MELODY_TICK       = "timer_tick";
    TIMER_MELODY_END        = "timer_end";
    TIMER_BAR_ENABLED       = true;
    TIMER_BAR_COLOR         = 0;
}

inline int count_publish(PublishCall::Kind k) {
    int n = 0;
    for (const auto &c : MQTTManager.recorded) if (c.kind == k) n++;
    return n;
}

inline const PublishCall *last_publish(PublishCall::Kind k) {
    for (auto it = MQTTManager.recorded.rbegin(); it != MQTTManager.recorded.rend(); ++it) {
        if (it->kind == k) return &*it;
    }
    return nullptr;
}

}  // namespace fixture
