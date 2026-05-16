#ifndef TimerManager_h
#define TimerManager_h

#include <Arduino.h>

enum class TimerState : uint8_t { Idle = 0, Running = 1, Paused = 2, Finished = 3 };
enum class BuzzerMode : uint8_t { Off = 0, End = 1, Countdown = 2 };
enum class FinishedMode : uint8_t { AutoClear = 0, Hold = 1, ReAlert = 2 };

class TimerManager_
{
private:
    TimerManager_() = default;

    TimerState state = TimerState::Idle;
    BuzzerMode buzzerMode = BuzzerMode::End;
    FinishedMode finishedMode = FinishedMode::AutoClear;
    uint32_t durationSec = 300;
    uint32_t remainingSec = 300;

    unsigned long runStartMs = 0;
    uint32_t runStartRemainingSec = 0;
    unsigned long enteredFinishedMs = 0;
    unsigned long lastRealertMs = 0;
    unsigned long lastPublishMs = 0;
    int32_t lastTickedSecond = -1;

    uint32_t computeCurrentRemaining() const;
    void enterRunning();
    void enterFinished();
    void publishState();
    void publishRemaining();
    void publishDuration();
    void publishBuzzerMode();
    void publishFinishedMode();
    void pushTimerNotification();
    void dismissTimerOverlay();
    void persist();

public:
    static TimerManager_ &getInstance();
    void setup();
    void tick();

    void start();
    void pause();
    void reset();

    void setDuration(uint32_t seconds);
    void setBuzzerMode(BuzzerMode m);
    void setFinishedMode(FinishedMode m);

    void parseCommand(const char *json);

    TimerState   getState()        const { return state; }
    uint32_t     getRemaining()    const { return remainingSec; }
    uint32_t     getDuration()     const { return durationSec; }
    BuzzerMode   getBuzzerMode()   const { return buzzerMode; }
    FinishedMode getFinishedMode() const { return finishedMode; }

    const char *getStateString() const;
    bool isHidden() const;
};

extern TimerManager_ &TimerManager;

#endif
