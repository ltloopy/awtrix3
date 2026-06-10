// See tests/stubs/Globals.h for the include-guard substitution mechanism.
#ifndef MQTTManager_h
#define MQTTManager_h

#include <Arduino.h>
#include <vector>

#include "TimerHa.h"

// Fixture identity for the wire seam's canonical topics. dataPrefix mirrors the
// device default MQTT_PREFIX = String(uniqueID); deviceUniqueId is the 12-hex
// full MAC ArduinoHA derives via device.setUniqueId(mac, 6); macSuffix is the
// last-3-bytes form MQTTManager::setup() feeds formatTimerHaEntityId.
#define TEST_WIRE_DATA_PREFIX   "awtrix_self"
#define TEST_WIRE_DEVICE_ID     "a1b2c3d4e5f6"
#define TEST_WIRE_MAC_SUFFIX    "d4e5f6"

struct PublishCall {
    enum Kind { Wire, Remaining, Duration, Buzzer, Finished, Icons };
    Kind kind;
    String  topic;     // Wire only: full data topic as the broker would see it
    String  payload;   // Wire only: exact payload string
    uint32_t value;
    String  icon_idle;
    String  icon_running;
    String  icon_paused;
    String  icon_finished;
};

class MQTTManager_ {
public:
    // The (topic, payload) wire seam (issue #31): same signature the device
    // implements with a retained mqtt.publish; here it records the pair.
    void publishTimerWire(const char *topic, const char *payload) {
        recorded.push_back({PublishCall::Wire, String(topic), String(payload), 0, "", "", "", ""});
    }

    // Canonical full data topic for a Timer HA entity slot, built through the
    // REAL formatTimerHaEntityId + formatTimerHaDataTopic (TimerHa.cpp compiles
    // in the native env), so the construction itself is under test.
    String timerWireTopic(TimerHaEntity slot) {
        char id[48];
        formatTimerHaEntityId(timerHaDescriptor(slot), TEST_WIRE_MAC_SUFFIX, id, sizeof(id));
        char topic[160];
        formatTimerHaDataTopic(TEST_WIRE_DATA_PREFIX, TEST_WIRE_DEVICE_ID, id, topic, sizeof(topic));
        return String(topic);
    }

    void publishTimerRemaining(uint32_t seconds){ recorded.push_back({PublishCall::Remaining, "", "", seconds, "", "", "", ""}); }
    void publishTimerDuration(uint32_t seconds) { recorded.push_back({PublishCall::Duration,  "", "", seconds, "", "", "", ""}); }
    void publishTimerBuzzer(uint8_t index)      { recorded.push_back({PublishCall::Buzzer,    "", "", index, "", "", "", ""}); }
    void publishTimerFinished(uint8_t index)    { recorded.push_back({PublishCall::Finished,  "", "", index, "", "", "", ""}); }
    void publishTimerIcons(const String &idle, const String &running, const String &paused, const String &finished) {
        recorded.push_back({PublishCall::Icons, "", "", 0, idle, running, paused, finished});
    }

    std::vector<PublishCall> recorded;

    void __test_reset() { recorded.clear(); }
};

extern MQTTManager_ MQTTManager;

#endif
