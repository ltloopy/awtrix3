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

    String iconIdle;
    String iconRunning;
    String iconPaused;
    String iconFinished;

    String endRtttl;
    String tickRtttl;

    unsigned long runStartMs = 0;
    uint32_t runStartRemainingSec = 0;
    unsigned long enteredFinishedMs = 0;
    unsigned long lastRealertMs = 0;
    unsigned long lastPublishMs = 0;

    bool          inConfig            = false;
    uint8_t       configField         = 0;
    uint8_t       configHH = 0, configMM = 0, configSS = 0;
    unsigned long configLastInputMs   = 0;
    unsigned long configRepeatLeftMs  = 0;
    unsigned long configRepeatRightMs = 0;

    bool _suspendPersist = false;
    bool _dirty          = false;

    uint32_t computeCurrentRemaining() const;
    void enterRunning();
    void enterFinished();
    void publishState();
    void publishRemaining();
    void publishDuration();
    void publishBuzzerMode();
    void publishFinishedMode();
    void persist();
    void persistIfDirty();
    void loadMelodiesCached();

    static String validateIconName(const String &name);

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

    void setIconIdle    (const String &name, bool publish = true);
    void setIconRunning (const String &name, bool publish = true);
    void setIconPaused  (const String &name, bool publish = true);
    void setIconFinished(const String &name, bool publish = true);

    void publishIcons();

    void parseCommand(const char *json);

    void onShowTimerChange(bool prev, bool now);

    void enterConfigMode();
    void exitConfigMode();
    void configCycleField();
    void configAdjust(int delta);
    bool    isInConfig()      const { return inConfig; }
    uint8_t getConfigField()  const { return configField; }
    uint8_t getConfigHH()     const { return configHH; }
    uint8_t getConfigMM()     const { return configMM; }
    uint8_t getConfigSS()     const { return configSS; }

    TimerState   getState()        const { return state; }
    uint32_t     getRemaining()    const { return remainingSec; }
    uint32_t     getDuration()     const { return durationSec; }
    BuzzerMode   getBuzzerMode()   const { return buzzerMode; }
    FinishedMode getFinishedMode() const { return finishedMode; }

    const String &getIconIdle()     const { return iconIdle; }
    const String &getIconRunning()  const { return iconRunning; }
    const String &getIconPaused()   const { return iconPaused; }
    const String &getIconFinished() const { return iconFinished; }
    const String &getIconForState(TimerState s) const;

    const char *getStateString() const;
};

extern TimerManager_ &TimerManager;

#endif
