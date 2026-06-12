// See tests/stubs/Globals.h for the include-guard substitution mechanism.
#ifndef MQTTManager_h
#define MQTTManager_h

#include <Arduino.h>
#include <vector>

// Relative path, not "TimerHa.h": native_prelude.h force-includes this stub
// into every compilation unit, including library builds (ArduinoFake) whose
// include path has tests/stubs but not src/.
#include "../../src/TimerHa.h"

// Fixture identity for the wire seam's canonical topics. dataPrefix mirrors the
// device default MQTT_PREFIX = String(uniqueID); deviceUniqueId is the 12-hex
// full MAC ArduinoHA derives via device.setUniqueId(mac, 6); macSuffix is the
// last-3-bytes form MQTTManager::setup() feeds formatTimerHaEntityId.
#define TEST_WIRE_DATA_PREFIX   "awtrix_self"
#define TEST_WIRE_DEVICE_ID     "a1b2c3d4e5f6"
#define TEST_WIRE_MAC_SUFFIX    "d4e5f6"

// Every Timer publish flows through the wire seam (PRD #28, issues #31–#34),
// so a recorded publish IS a (topic, payload) pair — the exact bytes the
// broker would see. There is no other shape to record.
struct PublishCall {
    String topic;      // full data topic as the broker would see it
    String payload;    // exact payload string
};

class MQTTManager_ {
public:
    // The (topic, payload) wire seam (issue #31): same signature the device
    // implements with a retained mqtt.publish; here it records the pair.
    void publishTimerWire(const char *topic, const char *payload) {
        recorded.push_back({String(topic), String(payload)});
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

    // Mirror of the device's MQTT_PREFIX + "/timer/icons" (MQTT_PREFIX defaults
    // to String(uniqueID) = the fixture data prefix).
    String timerIconsTopic() { return String(TEST_WIRE_DATA_PREFIX "/timer/icons"); }

    std::vector<PublishCall> recorded;

    void __test_reset() { recorded.clear(); }
};

extern MQTTManager_ MQTTManager;

#endif
