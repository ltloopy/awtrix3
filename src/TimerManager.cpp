#include "TimerManager.h"
#include "Globals.h"
#include "PeripheryManager.h"
#include "DisplayManager.h"
#include "MQTTManager.h"
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

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
    iconIdle       = timerPrefs.getString("ICON_IDLE",  "");
    iconRunning    = timerPrefs.getString("ICON_RUN",   "");
    iconPaused     = timerPrefs.getString("ICON_PAUSE", "");
    iconFinished   = timerPrefs.getString("ICON_FIN",   "");
    timerPrefs.end();

    if (TIMER_ICON_IDLE.length()     > 0) iconIdle     = TIMER_ICON_IDLE;
    if (TIMER_ICON_RUNNING.length()  > 0) iconRunning  = TIMER_ICON_RUNNING;
    if (TIMER_ICON_PAUSED.length()   > 0) iconPaused   = TIMER_ICON_PAUSED;
    if (TIMER_ICON_FINISHED.length() > 0) iconFinished = TIMER_ICON_FINISHED;

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
    timerPrefs.putString("ICON_IDLE",  iconIdle);
    timerPrefs.putString("ICON_RUN",   iconRunning);
    timerPrefs.putString("ICON_PAUSE", iconPaused);
    timerPrefs.putString("ICON_FIN",   iconFinished);
    timerPrefs.end();
}

String TimerManager_::capIconName(const String &name)
{
    if (name.length() > 32) return name.substring(0, 32);
    return name;
}

const String &TimerManager_::getIconForState(TimerState s) const
{
    switch (s)
    {
        case TimerState::Running:
            if (iconRunning.length()  > 0) return iconRunning;
            break;
        case TimerState::Paused:
            if (iconPaused.length()   > 0) return iconPaused;
            break;
        case TimerState::Finished:
            if (iconFinished.length() > 0) return iconFinished;
            break;
        case TimerState::Idle:
        default:
            break;
    }
    return iconIdle;
}

void TimerManager_::setIconIdle(const String &name, bool publish)
{
    String n = capIconName(name);
    if (iconIdle == n) return;
    iconIdle = n;
    persist();
    if (publish) publishIcons();
}

void TimerManager_::setIconRunning(const String &name, bool publish)
{
    String n = capIconName(name);
    if (iconRunning == n) return;
    iconRunning = n;
    persist();
    if (publish) publishIcons();
}

void TimerManager_::setIconPaused(const String &name, bool publish)
{
    String n = capIconName(name);
    if (iconPaused == n) return;
    iconPaused = n;
    persist();
    if (publish) publishIcons();
}

void TimerManager_::setIconFinished(const String &name, bool publish)
{
    String n = capIconName(name);
    if (iconFinished == n) return;
    iconFinished = n;
    persist();
    if (publish) publishIcons();
}

void TimerManager_::publishIcons()
{
    MQTTManager.publishTimerIcons(iconIdle, iconRunning, iconPaused, iconFinished);
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

void TimerManager_::enterConfigMode()
{
    if (state != TimerState::Idle) return;
    uint32_t d = durationSec;
    uint32_t h = d / 3600;
    if (h > 99) h = 99;
    configHH = (uint8_t)h;
    configMM = (uint8_t)((d % 3600) / 60);
    configSS = (uint8_t)(d % 60);
    configField = 0;
    configLastInputMs = millis();
    configRepeatLeftMs = 0;
    configRepeatRightMs = 0;
    inConfig = true;
}

void TimerManager_::exitConfigMode()
{
    if (!inConfig) return;
    uint32_t total = (uint32_t)configHH * 3600UL + (uint32_t)configMM * 60UL + (uint32_t)configSS;
    inConfig = false;
    setDuration(total);
    DisplayManager.drainDeferredNotifications();
}

void TimerManager_::configCycleField()
{
    if (!inConfig) return;
    configField = (configField + 1) % 3;
    configLastInputMs = millis();
}

void TimerManager_::configAdjust(int delta)
{
    if (!inConfig) return;
    uint8_t maxVal = (configField == 0) ? 99 : 59;
    uint8_t cur = (configField == 0) ? configHH : (configField == 1 ? configMM : configSS);
    int next = (int)cur + delta;
    if (next < 0) next = maxVal;
    else if (next > maxVal) next = 0;
    if      (configField == 0) configHH = (uint8_t)next;
    else if (configField == 1) configMM = (uint8_t)next;
    else                       configSS = (uint8_t)next;
    configLastInputMs = millis();
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

    if (!GAME_ACTIVE && !BLOCK_NAVIGATION)
    {
        String j = "{\"name\":\"Timer\"}";
        DisplayManager.switchToApp(j.c_str());
    }
    if (MATRIX_OFF)
    {
        DisplayManager.setBrightness(BRIGHTNESS);
    }
    if (SOUND_ACTIVE && buzzerMode != BuzzerMode::Off)
    {
        String t = loadRtttlFromFile("/MELODIES/timer_end.txt", FALLBACK_END_RTTTL);
        if (t.length() > 0) PeripheryManager.playRTTTLString(t);
    }
}

void TimerManager_::start()
{
    if (state == TimerState::Running) return;
    if (state == TimerState::Finished)
    {
        PeripheryManager.stopSound();
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
    PeripheryManager.stopSound();
    state = TimerState::Idle;
    remainingSec = durationSec;
    lastTickedSecond = -1;
    publishState();
    publishRemaining();
    if (MATRIX_OFF)
    {
        DisplayManager.setBrightness(0);
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

    if (inConfig)
    {
        if (now - configLastInputMs >= (unsigned long)TIMER_CONFIG_TIMEOUT * 1000UL)
        {
            exitConfigMode();
            return;
        }

        EasyButton *bL = PeripheryManager.buttonL;
        EasyButton *bR = PeripheryManager.buttonR;
        if (bL && bL->isPressed() && bL->pressedFor(500))
        {
            if (configRepeatLeftMs == 0 || (now - configRepeatLeftMs) >= 250)
            {
                configAdjust(-1);
                configRepeatLeftMs = now;
            }
        }
        else
        {
            configRepeatLeftMs = 0;
        }
        if (bR && bR->isPressed() && bR->pressedFor(500))
        {
            if (configRepeatRightMs == 0 || (now - configRepeatRightMs) >= 250)
            {
                configAdjust(+1);
                configRepeatRightMs = now;
            }
        }
        else
        {
            configRepeatRightMs = 0;
        }
        return;
    }

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
            PeripheryManager.stopSound();
            state = TimerState::Idle;
            remainingSec = durationSec;
            publishState();
            publishRemaining();
            if (MATRIX_OFF)
            {
                DisplayManager.setBrightness(0);
            }
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
    }
}

void TimerManager_::parseCommand(const char *json)
{
    if (!SHOW_TIMER) return;
    if (json == nullptr || json[0] == '\0') return;

    if (inConfig) exitConfigMode();

    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, json) != DeserializationError::Ok) return;

    if (doc.containsKey("duration"))
    {
        setDuration(doc["duration"].as<uint32_t>());
    }
    if (doc.containsKey("icon_idle"))     setIconIdle    (doc["icon_idle"].as<String>());
    if (doc.containsKey("icon_running"))  setIconRunning (doc["icon_running"].as<String>());
    if (doc.containsKey("icon_paused"))   setIconPaused  (doc["icon_paused"].as<String>());
    if (doc.containsKey("icon_finished")) setIconFinished(doc["icon_finished"].as<String>());
    if (doc.containsKey("buzzer"))
    {
        String b = doc["buzzer"].as<String>();
        b.toLowerCase();
        if      (b == "off")       setBuzzerMode(BuzzerMode::Off);
        else if (b == "end")       setBuzzerMode(BuzzerMode::End);
        else if (b == "countdown") setBuzzerMode(BuzzerMode::Countdown);
    }
    if (doc.containsKey("finished"))
    {
        String f = doc["finished"].as<String>();
        f.toLowerCase();
        if      (f == "auto-clear" || f == "autoclear") setFinishedMode(FinishedMode::AutoClear);
        else if (f == "hold")                            setFinishedMode(FinishedMode::Hold);
        else if (f == "re-alert"   || f == "realert")    setFinishedMode(FinishedMode::ReAlert);
    }
    if (doc.containsKey("action"))
    {
        String a = doc["action"].as<String>();
        a.toLowerCase();
        if (a == "start")
        {
            bool fromIdle = (state == TimerState::Idle);
            start();
            if (fromIdle && !GAME_ACTIVE && !BLOCK_NAVIGATION)
            {
                String j = "{\"name\":\"Timer\"}";
                DisplayManager.switchToApp(j.c_str());
            }
        }
        else if (a == "pause") pause();
        else if (a == "reset") reset();
    }
}

void TimerManager_::onShowTimerChange(bool prev, bool now)
{
    if (prev && !now) reset();
}

void TimerManager_::publishState()        { MQTTManager.publishTimerState(getStateString()); }
void TimerManager_::publishRemaining()    { MQTTManager.publishTimerRemaining(remainingSec); }
void TimerManager_::publishDuration()     { MQTTManager.publishTimerDuration(durationSec); }
void TimerManager_::publishBuzzerMode()   { MQTTManager.publishTimerBuzzer((uint8_t)buzzerMode); }
void TimerManager_::publishFinishedMode() { MQTTManager.publishTimerFinished((uint8_t)finishedMode); }
