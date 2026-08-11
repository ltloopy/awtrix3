// See tests/stubs/Globals.h for the include-guard substitution mechanism.
#ifndef MQTTManager_h
#define MQTTManager_h

#include <Arduino.h>
#include <vector>

struct PublishCall {
    enum Kind { State, Remaining, Duration, Buzzer, Finished, Icons };
    Kind kind;
    String  state_str;
    uint32_t value;
    String  icon_idle;
    String  icon_running;
    String  icon_paused;
    String  icon_finished;
};

class MQTTManager_ {
public:
    void publishTimerState(const char *s)       { recorded.push_back({PublishCall::State,     String(s), 0, "", "", "", ""}); }
    void publishTimerRemaining(uint32_t seconds){ recorded.push_back({PublishCall::Remaining, String(),  seconds, "", "", "", ""}); }
    void publishTimerDuration(uint32_t seconds) { recorded.push_back({PublishCall::Duration,  String(),  seconds, "", "", "", ""}); }
    void publishTimerBuzzer(uint8_t index)      { recorded.push_back({PublishCall::Buzzer,    String(),  index, "", "", "", ""}); }
    void publishTimerFinished(uint8_t index)    { recorded.push_back({PublishCall::Finished,  String(),  index, "", "", "", ""}); }
    void publishTimerIcons(const String &idle, const String &running, const String &paused, const String &finished) {
        recorded.push_back({PublishCall::Icons, String(), 0, idle, running, paused, finished});
    }

    std::vector<PublishCall> recorded;

    void __test_reset() { recorded.clear(); }
};

extern MQTTManager_ MQTTManager;

#endif
