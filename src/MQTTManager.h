#ifndef MQTTManager_h
#define MQTTManager_h

#include <Arduino.h>
#include <map>

#include "TimerHa.h"

class MQTTManager_
{
private:
    MQTTManager_() = default;

public:
    static MQTTManager_ &getInstance();

    void setup();
    void tick();
    void rawPublish(const char *prefix, const char *topic, const char *payload);
    void publish(const char *topic, const char *payload);
    void setCurrentApp(String);
    void sendStats();
    void sendButton(byte, bool);
    void setIndicatorState(uint8_t indicator, bool state, uint32_t color);
    void beginPublish(const char *topic, unsigned int plength, boolean retained);
    void writePayload(const char *data, const uint16_t length);
    void endPublish();
    bool subscribe(const char* topic);
    bool isConnected();
    String getValueForTopic(const String &topic);

    // The Timer wire seam (issue #31 / PRD #28): the single chokepoint through
    // which Timer MQTT output flows as (topic, payload) strings. On device it
    // reaches the broker (retained, like the HA setValue path it replaces); the
    // host-test stub records the pair. timerWireTopic() sources an entity's
    // canonical data topic — byte-identical to what ArduinoHA emits.
    void publishTimerWire(const char *topic, const char *payload);
    String timerWireTopic(TimerHaEntity slot);

    void publishTimerDuration(uint32_t seconds);
    void publishTimerRemaining(uint32_t seconds);
    void publishTimerBuzzer(uint8_t index);
    void publishTimerFinished(uint8_t index);
    void publishTimerIcons(const String &idle, const String &running, const String &paused, const String &finished);
    void createTimerHAEntities();
    void enableTimerHADiscovery();
    void removeTimerHAEntities();
};

void reconcileTimerHAState();

extern MQTTManager_ &MQTTManager;

#endif