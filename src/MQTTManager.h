#ifndef MQTTManager_h
#define MQTTManager_h

#include <Arduino.h>
#include <map>

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

    void publishTimerDuration(uint32_t seconds);
    void publishTimerRemaining(uint32_t seconds);
    void publishTimerState(const char *stateStr);
    void publishTimerBuzzer(uint8_t index);
    void publishTimerFinished(uint8_t index);
    void publishTimerIcons(const String &idle, const String &running, const String &paused, const String &finished);
    void removeTimerHAEntities();
};

void reconcileTimerHAState();

extern MQTTManager_ &MQTTManager;

#endif