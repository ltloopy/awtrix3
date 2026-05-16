#include "TimerManager.h"
#include "Globals.h"
#include "PeripheryManager.h"
#include "DisplayManager.h"
#include "MQTTManager.h"
#include "Overlays.h"
#include <Preferences.h>
#include <LittleFS.h>

static Preferences timerPrefs;

static const char *FALLBACK_END_RTTTL  = "timer:d=4,o=5,b=120:c,8p,c,8p,c";
static const char *FALLBACK_TICK_RTTTL = "tick:d=16,o=6,b=200:c";

static String loadRtttlFromFile(const char *path, const char *fallback)
{
    if (LittleFS.exists(path))
    {
        File f = LittleFS.open(path, "r");
        if (f)
        {
            String s;
            while (f.available()) s += (char)f.read();
            f.close();
            s.trim();
            if (s.length() > 0) return s;
        }
    }
    return String(fallback);
}

TimerManager_ &TimerManager_::getInstance()
{
    static TimerManager_ instance;
    return instance;
}

TimerManager_ &TimerManager = TimerManager_::getInstance();

void TimerManager_::setup()
{
    timerPrefs.begin("timer", false);
    durationSec    = timerPrefs.getUInt("DUR", 300);
    buzzerMode     = (BuzzerMode)timerPrefs.getUChar("BUZ", (uint8_t)BuzzerMode::End);
    finishedMode   = (FinishedMode)timerPrefs.getUChar("FIN", (uint8_t)FinishedMode::AutoClear);
    timerPrefs.end();

    if (durationSec < 1) durationSec = 1;
    if (TIMER_MAX_DURATION > 0 && durationSec > TIMER_MAX_DURATION) durationSec = TIMER_MAX_DURATION;
    remainingSec = durationSec;
    state = TimerState::Idle;
    lastTickedSecond = -1;
}

void TimerManager_::persist()
{
    timerPrefs.begin("timer", false);
    timerPrefs.putUInt("DUR", durationSec);
    timerPrefs.putUChar("BUZ", (uint8_t)buzzerMode);
    timerPrefs.putUChar("FIN", (uint8_t)finishedMode);
    timerPrefs.end();
}

uint32_t TimerManager_::computeCurrentRemaining() const
{
    if (state != TimerState::Running) return remainingSec;
    unsigned long elapsedMs = millis() - runStartMs;
    uint32_t elapsedSec = elapsedMs / 1000UL;
    if (elapsedSec >= runStartRemainingSec) return 0;
    return runStartRemainingSec - elapsedSec;
}

const char *TimerManager_::getStateString() const
{
    switch (state)
    {
        case TimerState::Idle:     return "idle";
        case TimerState::Running:  return "running";
        case TimerState::Paused:   return "paused";
        case TimerState::Finished: return "finished";
    }
    return "idle";
}

bool TimerManager_::isHidden() const
{
    return state == TimerState::Idle && TIMER_HIDE_WHEN_IDLE;
}

void TimerManager_::enterRunning()
{
    state = TimerState::Running;
    runStartMs = millis();
    runStartRemainingSec = remainingSec;
    lastTickedSecond = -1;
    lastPublishMs = 0;
    publishState();
    publishRemaining();
}

void TimerManager_::enterFinished()
{
    state = TimerState::Finished;
    remainingSec = 0;
    enteredFinishedMs = millis();
    lastRealertMs = enteredFinishedMs;
    publishState();
    publishRemaining();
    pushTimerNotification();
}

void TimerManager_::pushTimerNotification()
{
    Notification n;
    n.text       = "00:00";
    n.color      = TEXTCOLOR_888;
    n.channel    = "timer";
    n.wakeup     = true;
    n.center     = true;
    n.noScrolling = true;
    n.hold       = (finishedMode != FinishedMode::AutoClear);
    n.duration   = (finishedMode == FinishedMode::AutoClear) ? (long)TIMER_FINISHED_HOLD * 1000L : 0;
    n.blink      = (finishedMode != FinishedMode::AutoClear) ? 500 : 0;
    n.startime   = millis();
    if (SOUND_ACTIVE && buzzerMode != BuzzerMode::Off)
    {
        n.rtttl = loadRtttlFromFile("/MELODIES/timer_end.txt", FALLBACK_END_RTTTL);
    }
    notifications.push_back(n);
}

void TimerManager_::dismissTimerOverlay()
{
    bool hadTimerFront = !notifications.empty() && notifications.front().channel == "timer";
    if (hadTimerFront)
    {
        PeripheryManager.stopSound();
    }
    notifications.erase(
        std::remove_if(notifications.begin(), notifications.end(),
                       [](const Notification &n) { return n.channel == "timer"; }),
        notifications.end());
}

void TimerManager_::start()
{
    if (state == TimerState::Running) return;
    if (state == TimerState::Finished)
    {
        dismissTimerOverlay();
        remainingSec = durationSec;
    }
    else if (state == TimerState::Idle)
    {
        remainingSec = durationSec;
    }
    enterRunning();
}

void TimerManager_::pause()
{
    if (state == TimerState::Running)
    {
        remainingSec = computeCurrentRemaining();
        state = TimerState::Paused;
        publishState();
        publishRemaining();
    }
    else if (state == TimerState::Paused)
    {
        enterRunning();
    }
}

void TimerManager_::reset()
{
    bool wasCurrentTimerApp = (CURRENT_APP == "Timer");
    if (state == TimerState::Finished)
    {
        dismissTimerOverlay();
    }
    PeripheryManager.stopSound();
    state = TimerState::Idle;
    remainingSec = durationSec;
    lastTickedSecond = -1;
    publishState();
    publishRemaining();
    if (wasCurrentTimerApp && TIMER_HIDE_WHEN_IDLE)
    {
        DisplayManager.nextApp();
    }
}

void TimerManager_::setDuration(uint32_t seconds)
{
    if (seconds < 1) seconds = 1;
    if (TIMER_MAX_DURATION > 0 && seconds > TIMER_MAX_DURATION) seconds = TIMER_MAX_DURATION;
    if (durationSec == seconds) return;
    durationSec = seconds;
    if (state == TimerState::Idle)
    {
        remainingSec = durationSec;
        publishRemaining();
    }
    persist();
    publishDuration();
}

void TimerManager_::setBuzzerMode(BuzzerMode m)
{
    if (buzzerMode == m) return;
    buzzerMode = m;
    persist();
    publishBuzzerMode();
}

void TimerManager_::setFinishedMode(FinishedMode m)
{
    if (finishedMode == m) return;
    finishedMode = m;
    persist();
    publishFinishedMode();
}

void TimerManager_::tick()
{
    unsigned long now = millis();

    if (state == TimerState::Running)
    {
        uint32_t newRemaining = computeCurrentRemaining();
        if (newRemaining != remainingSec)
        {
            remainingSec = newRemaining;
            lastTickedSecond = (int32_t)newRemaining;

            if (SOUND_ACTIVE && buzzerMode == BuzzerMode::Countdown
                && newRemaining > 0 && newRemaining <= TIMER_COUNTDOWN_SECONDS)
            {
                String t = loadRtttlFromFile("/MELODIES/timer_tick.txt", FALLBACK_TICK_RTTTL);
                if (t.length() > 0) PeripheryManager.playRTTTLString(t);
            }

            if (TIMER_PUBLISH_INTERVAL > 0 && (now - lastPublishMs >= (unsigned long)TIMER_PUBLISH_INTERVAL * 1000UL))
            {
                publishRemaining();
                lastPublishMs = now;
            }
        }

        if (newRemaining == 0)
        {
            enterFinished();
        }
    }
    else if (state == TimerState::Finished)
    {
        if (finishedMode == FinishedMode::AutoClear
            && (now - enteredFinishedMs >= (unsigned long)TIMER_FINISHED_HOLD * 1000UL))
        {
            dismissTimerOverlay();
            state = TimerState::Idle;
            remainingSec = durationSec;
            publishState();
            publishRemaining();
            return;
        }

        if (finishedMode == FinishedMode::ReAlert
            && SOUND_ACTIVE && buzzerMode != BuzzerMode::Off
            && (now - lastRealertMs >= (unsigned long)TIMER_REALERT_INTERVAL * 1000UL))
        {
            if (!PeripheryManager.isPlaying())
            {
                String t = loadRtttlFromFile("/MELODIES/timer_end.txt", FALLBACK_END_RTTTL);
                if (t.length() > 0) PeripheryManager.playRTTTLString(t);
            }
            lastRealertMs = now;
        }

        bool hasTimerNotification = false;
        for (const auto &n : notifications)
        {
            if (n.channel == "timer") { hasTimerNotification = true; break; }
        }
        if (!hasTimerNotification)
        {
            PeripheryManager.stopSound();
            state = TimerState::Idle;
            remainingSec = durationSec;
            publishState();
            publishRemaining();
        }
    }
}

void TimerManager_::publishState()        { MQTTManager.publishTimerState(getStateString()); }
void TimerManager_::publishRemaining()    { MQTTManager.publishTimerRemaining(remainingSec); }
void TimerManager_::publishDuration()     { MQTTManager.publishTimerDuration(durationSec); }
void TimerManager_::publishBuzzerMode()   { MQTTManager.publishTimerBuzzer((uint8_t)buzzerMode); }
void TimerManager_::publishFinishedMode() { MQTTManager.publishTimerFinished((uint8_t)finishedMode); }
