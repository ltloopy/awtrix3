#pragma once

#include <ArduinoFake.h>
#include <vector>

#include "Preferences.h"
#include "Overlays.h"
#include "MQTTManager.h"
#include "PeripheryManager.h"
#include "DisplayManager.h"
#include "ServerManager.h"
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
    ServerManager.__test_reset();
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
    TIMER_PUBLISH_INTERVAL  = 1;
    TIMER_FINISHED_HOLD     = 10;
    TIMER_REALERT_INTERVAL  = 15;
    TIMER_COUNTDOWN_SECONDS = 3;
    TIMER_ICON_IDLE         = "";
    TIMER_ICON_RUNNING      = "";
    TIMER_ICON_PAUSED       = "";
    TIMER_ICON_FINISHED     = "";
    TIMER_MELODY_TICK       = "timer_tick";
    TIMER_MELODY_END        = "timer_end";
    TIMER_BAR_ENABLED       = true;
    TIMER_ICON_ENABLED      = true;
    TIMER_BAR_COLOR         = 0;
    TIMER_SYNC_FOLLOW       = false;
    TIMER_SYNC_TARGETS      = "";
    uniqueID                = "awtrix_self";
    AP_MODE                 = false;
}

// Parse the last broadcast packet's _sync envelope + a probe key. Returns false
// if nothing was sent. Helpers for the propagation-surface tests.
inline int sync_packet_count() { return (int)ServerManager.sent.size(); }

inline String last_sync_payload() {
    return ServerManager.sent.empty() ? String() : ServerManager.sent.back();
}

// The canonical state topic the broker would see, hand-spelled (NOT computed
// via the TimerHa builders) so tests assert against an independent spelling of
// the wire contract: {TEST_WIRE_DATA_PREFIX}/{TEST_WIRE_DEVICE_ID}/
// {TEST_WIRE_MAC_SUFFIX}_timer_state/stat_t.
inline const char *TIMER_STATE_TOPIC = "awtrix_self/a1b2c3d4e5f6/d4e5f6_timer_state/stat_t";

// Same hand-spelled wire contract for the remaining-seconds sensor:
// {TEST_WIRE_DATA_PREFIX}/{TEST_WIRE_DEVICE_ID}/{TEST_WIRE_MAC_SUFFIX}_timer_rem/stat_t.
inline const char *TIMER_REMAINING_TOPIC = "awtrix_self/a1b2c3d4e5f6/d4e5f6_timer_rem/stat_t";

// And for the two enum selects (issue #33), ids per TimerHa.cpp's
// HAtimerBuzID "%s_timer_buz" / HAtimerFinID "%s_timer_fin".
inline const char *TIMER_BUZZER_TOPIC   = "awtrix_self/a1b2c3d4e5f6/d4e5f6_timer_buz/stat_t";
inline const char *TIMER_FINISHED_TOPIC = "awtrix_self/a1b2c3d4e5f6/d4e5f6_timer_fin/stat_t";

// The finished-mode select's JSON-attributes topic (issue #51 / PRD #17):
// same id as TIMER_FINISHED_TOPIC but the json_attr_t suffix HASelect advertises.
inline const char *TIMER_FINISHED_ATTR_TOPIC = "awtrix_self/a1b2c3d4e5f6/d4e5f6_timer_fin/json_attr_t";

// The buzzer-select's JSON-attributes topic (PRD #57 / issue #58): the json_attr_t
// sibling of TIMER_BUZZER_TOPIC, spelled independently of the TimerHa builders.
inline const char *TIMER_BUZZER_ATTR_TOPIC = "awtrix_self/a1b2c3d4e5f6/d4e5f6_timer_buz/json_attr_t";

// The state + remaining sensors' JSON-attributes topics (PRD #57 / issue #59):
// the json_attr_t siblings of TIMER_STATE_TOPIC / TIMER_REMAINING_TOPIC, now that
// HASensor carries the opt-in too. Spelled independently of the TimerHa builders.
inline const char *TIMER_STATE_ATTR_TOPIC     = "awtrix_self/a1b2c3d4e5f6/d4e5f6_timer_state/json_attr_t";
inline const char *TIMER_REMAINING_ATTR_TOPIC = "awtrix_self/a1b2c3d4e5f6/d4e5f6_timer_rem/json_attr_t";

// Duration text entity (issue #34), id per TimerHa.cpp's HAtimerDurID "%s_timer_dur".
inline const char *TIMER_DURATION_TOPIC = "awtrix_self/a1b2c3d4e5f6/d4e5f6_timer_dur/stat_t";

// The Duration text entity's JSON-attributes topic (PRD #66 / issue #68): the
// json_attr_t sibling of TIMER_DURATION_TOPIC, now that HAText carries the opt-in
// too (issue #67). Spelled independently of the TimerHa builders. Carries the
// carrier-native max_duration clock string ("24:00:00").
inline const char *TIMER_DURATION_ATTR_TOPIC = "awtrix_self/a1b2c3d4e5f6/d4e5f6_timer_dur/json_attr_t";

// The aggregate icons JSON rides a plain prefix topic, not an HA entity data
// topic: {MQTT_PREFIX}/timer/icons (MQTT_PREFIX defaults to uniqueID on device).
inline const char *TIMER_ICONS_TOPIC = "awtrix_self/timer/icons";

// Topic-keyed queries over the wire seam's (topic, payload) recordings.
inline int count_publish(const String &topic) {
    int n = 0;
    for (const auto &c : MQTTManager.recorded)
        if (c.topic == topic) n++;
    return n;
}

inline const PublishCall *last_publish(const String &topic) {
    for (auto it = MQTTManager.recorded.rbegin(); it != MQTTManager.recorded.rend(); ++it) {
        if (it->topic == topic) return &*it;
    }
    return nullptr;
}

}  // namespace fixture
