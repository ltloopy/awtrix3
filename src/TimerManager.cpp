#include "TimerManager.h"
#include "Globals.h"
#include "PeripheryManager.h"
#include "DisplayManager.h"
#include "MQTTManager.h"
#include "MenuManager.h"
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

namespace {
    constexpr uint16_t kTimerCmdJsonSize     = 512;
    constexpr uint8_t  kIconNameMaxLen       = 32;
    constexpr uint32_t kConfigHHMax          = 99UL * 3600UL;
    constexpr unsigned long kBtnLongPressMs  = 500;
    constexpr unsigned long kBtnRepeatMs     = 250;

    const char *FALLBACK_END_RTTTL  = "timer:d=4,o=5,b=120:c,8p,c,8p,c";
    const char *FALLBACK_TICK_RTTTL = "tick:d=16,o=6,b=200:c";
}

static Preferences timerPrefs;

TimerManager_ &TimerManager_::getInstance()
{
    static TimerManager_ instance;
    return instance;
}

TimerManager_ &TimerManager = TimerManager_::getInstance();

void TimerManager_::loadMelodiesCached()
{
    const char *endName  = TIMER_MELODY_END.length()  > 0 ? TIMER_MELODY_END.c_str()  : "timer_end";
    const char *tickName = TIMER_MELODY_TICK.length() > 0 ? TIMER_MELODY_TICK.c_str() : "timer_tick";
    endRtttl  = PeripheryManager.resolveRtttl(endName,  FALLBACK_END_RTTTL);
    tickRtttl = PeripheryManager.resolveRtttl(tickName, FALLBACK_TICK_RTTTL);
}

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
    inConfig = false;   // a (re)boot is never mid-edit; complete the runtime reset

    loadMelodiesCached();
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

void TimerManager_::persistIfDirty()
{
    if (_suspendPersist)
    {
        _dirty = true;
        return;
    }
    persist();
    _dirty = false;
}

String TimerManager_::validateIconName(const String &name)
{
    if (name.length() == 0) return name;
    if (name.length() > kIconNameMaxLen) return String("");
    for (size_t i = 0; i < name.length(); ++i)
    {
        char c = name[i];
        bool ok = (c >= 'A' && c <= 'Z')
               || (c >= 'a' && c <= 'z')
               || (c >= '0' && c <= '9')
               || c == '_' || c == '-';
        if (!ok)
        {
            if (DEBUG_MODE) DEBUG_PRINTLN("timer: icon name rejected");
            return String("");
        }
    }
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
    String n = validateIconName(name);
    if (name.length() > 0 && n.length() == 0) return;
    if (iconIdle == n) return;
    iconIdle = n;
    persistIfDirty();
    if (publish) publishIcons();
}

void TimerManager_::setIconRunning(const String &name, bool publish)
{
    String n = validateIconName(name);
    if (name.length() > 0 && n.length() == 0) return;
    if (iconRunning == n) return;
    iconRunning = n;
    persistIfDirty();
    if (publish) publishIcons();
}

void TimerManager_::setIconPaused(const String &name, bool publish)
{
    String n = validateIconName(name);
    if (name.length() > 0 && n.length() == 0) return;
    if (iconPaused == n) return;
    iconPaused = n;
    persistIfDirty();
    if (publish) publishIcons();
}

void TimerManager_::setIconFinished(const String &name, bool publish)
{
    String n = validateIconName(name);
    if (name.length() > 0 && n.length() == 0) return;
    if (iconFinished == n) return;
    iconFinished = n;
    persistIfDirty();
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

const char *TimerManager_::buzzerModeString() const
{
    switch (buzzerMode)
    {
        case BuzzerMode::Off:       return "off";
        case BuzzerMode::End:       return "end";
        case BuzzerMode::Countdown: return "countdown";
    }
    return "end";
}

const char *TimerManager_::finishedModeString() const
{
    switch (finishedMode)
    {
        case FinishedMode::AutoClear: return "auto-clear";
        case FinishedMode::Hold:      return "hold";
        case FinishedMode::ReAlert:   return "re-alert";
    }
    return "auto-clear";
}

String TimerManager_::getStateJson() const
{
    StaticJsonDocument<512> doc;
    uint32_t remaining = computeCurrentRemaining();
    doc["state"]         = getStateString();
    doc["enabled"]       = (bool)SHOW_TIMER;
    doc["remaining"]     = remaining;
    doc["remaining_str"] = formatHMS(remaining);
    doc["duration"]      = durationSec;
    doc["duration_str"]  = formatHMS(durationSec);
    doc["buzzer"]        = buzzerModeString();
    doc["finished"]      = finishedModeString();

    String out;
    serializeJson(doc, out);
    return out;
}

void TimerManager_::secondsToHMS(uint32_t sec, uint32_t &h, uint32_t &m, uint32_t &s)
{
    h = sec / 3600;
    m = (sec % 3600) / 60;
    s = sec % 60;
}

uint32_t TimerManager_::hmsToSeconds(uint32_t h, uint32_t m, uint32_t s)
{
    return h * 3600UL + m * 60UL + s;
}

String TimerManager_::formatHMS(uint32_t seconds)
{
    uint32_t h, m, s;
    secondsToHMS(seconds, h, m, s);
    char buf[16];
    // Trimmed clock string: drop the hours group when zero; the most-significant
    // shown field is unpadded, lower fields are zero-padded to two digits.
    if (h > 0) snprintf(buf, sizeof(buf), "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
    else       snprintf(buf, sizeof(buf), "%u:%02u",                 (unsigned)m, (unsigned)s);
    return String(buf);
}

bool TimerManager_::parseHMS(const String &in, uint32_t &outSeconds)
{
    String s = in;
    s.trim();
    if (s.length() == 0) return false;

    // Split on ':' into up to three numeric fields. Colon count decides units:
    // two colons = HH:MM:SS, one = MM:SS, none = bare seconds. Each field must be
    // a non-empty run of digits. Fields are summed without a 0-59 cap (carry).
    uint32_t fields[3] = {0, 0, 0};
    int count = 0;
    int start = 0;
    for (int i = 0; i <= s.length(); i++)
    {
        if (i == s.length() || s[i] == ':')
        {
            if (count >= 3) return false;          // more than two colons
            int len = i - start;
            if (len == 0) return false;            // empty field (e.g. "5:", ":30")
            uint32_t v = 0;
            for (int j = start; j < i; j++)
            {
                char c = s[j];
                if (c < '0' || c > '9') return false;  // non-numeric
                v = v * 10 + (uint32_t)(c - '0');
            }
            fields[count++] = v;
            start = i + 1;
        }
    }

    uint32_t h = 0, m = 0, sec = 0;
    if (count == 3)      { h = fields[0]; m = fields[1]; sec = fields[2]; }
    else if (count == 2) {                m = fields[0]; sec = fields[1]; }
    else                 {                                sec = fields[0]; }

    outSeconds = hmsToSeconds(h, m, sec);
    return true;
}

bool TimerManager_::isValidDuration(uint32_t seconds)
{
    if (seconds < 1) return false;
    if (TIMER_MAX_DURATION > 0 && seconds > TIMER_MAX_DURATION) return false;
    return true;
}

bool TimerManager_::parseBuzzerMode(const String &s, BuzzerMode &out)
{
    String b = s; b.toLowerCase();
    if      (b == "off")       out = BuzzerMode::Off;
    else if (b == "end")       out = BuzzerMode::End;
    else if (b == "countdown") out = BuzzerMode::Countdown;
    else return false;
    return true;
}

bool TimerManager_::parseFinishedMode(const String &s, FinishedMode &out)
{
    String f = s; f.toLowerCase();
    if      (f == "auto-clear" || f == "autoclear") out = FinishedMode::AutoClear;
    else if (f == "hold")                           out = FinishedMode::Hold;
    else if (f == "re-alert"   || f == "realert")   out = FinishedMode::ReAlert;
    else return false;
    return true;
}

bool TimerManager_::isValidIconName(const String &name)
{
    // validateIconName returns the name unchanged when acceptable (empty = clear),
    // or "" when rejected. So a name is valid iff it survives unchanged.
    return validateIconName(name) == name;
}

bool TimerManager_::isValidAction(const String &s)
{
    String a = s; a.toLowerCase();
    return a == "start" || a == "pause" || a == "reset";
}

void TimerManager_::enterConfigMode()
{
    if (state != TimerState::Idle) return;
    if (durationSec > kConfigHHMax) durationSec = kConfigHHMax;
    uint32_t h, m, s;
    secondsToHMS(durationSec, h, m, s);
    if (h > 99) h = 99;
    configHH = (uint8_t)h;
    configMM = (uint8_t)m;
    configSS = (uint8_t)s;
    configField = 0;
    configLastInputMs = millis();
    configRepeatLeftMs = 0;
    configRepeatRightMs = 0;
    inConfig = true;
}

void TimerManager_::exitConfigMode()
{
    if (!inConfig) return;
    uint32_t total = hmsToSeconds(configHH, configMM, configSS);
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

    const uint8_t  hardMax = (configField == 0) ? 99 : 59;
    const uint32_t perUnit = (configField == 0) ? 3600UL : (configField == 1) ? 60UL : 1UL;
    const uint32_t otherSec = (configField == 0)
        ? (uint32_t)configMM * 60UL + (uint32_t)configSS
        : (configField == 1)
            ? (uint32_t)configHH * 3600UL + (uint32_t)configSS
            : (uint32_t)configHH * 3600UL + (uint32_t)configMM * 60UL;

    uint8_t maxVal = hardMax;
    if (TIMER_MAX_DURATION > 0)
    {
        uint32_t headroom = (TIMER_MAX_DURATION > otherSec) ? (TIMER_MAX_DURATION - otherSec) : 0;
        uint32_t room     = headroom / perUnit;
        if (room < maxVal) maxVal = (uint8_t)room;
    }

    uint8_t cur = (configField == 0) ? configHH : (configField == 1 ? configMM : configSS);
    if (cur > maxVal) cur = maxVal;
    const int step = (TIMER_STEP > 0 && TIMER_STEP <= 99) ? (int)TIMER_STEP : 1;
    int next = (int)cur + (delta >= 0 ? step : -step);
    if (next < 0) next = maxVal;
    else if (next > (int)maxVal) next = 0;
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
    publishState();
    publishRemaining();
    lastPublishMs = millis();
}

void TimerManager_::enterFinished()
{
    state = TimerState::Finished;
    remainingSec = 0;
    enteredFinishedMs = millis();
    lastRealertMs = enteredFinishedMs;
    publishState();
    publishRemaining();

    if (!GAME_ACTIVE && !BLOCK_NAVIGATION && !MenuManager.inMenu)
    {
        String j = "{\"name\":\"Timer\",\"fast\":true}";
        DisplayManager.switchToApp(j.c_str());
    }
    if (MATRIX_OFF)
    {
        DisplayManager.setBrightness(BRIGHTNESS);
    }
    if (SOUND_ACTIVE && buzzerMode != BuzzerMode::Off)
    {
        if (endRtttl.length() > 0) PeripheryManager.playRTTTLString(endRtttl);
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
    persistIfDirty();
    publishDuration();
}

void TimerManager_::setBuzzerMode(BuzzerMode m)
{
    if (buzzerMode == m) return;
    buzzerMode = m;
    persistIfDirty();
    publishBuzzerMode();
}

void TimerManager_::setFinishedMode(FinishedMode m)
{
    if (finishedMode == m) return;
    finishedMode = m;
    persistIfDirty();
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
        if (bL && bL->isPressed() && bL->pressedFor(kBtnLongPressMs))
        {
            if (configRepeatLeftMs == 0 || (now - configRepeatLeftMs) >= kBtnRepeatMs)
            {
                configAdjust(-1);
                configRepeatLeftMs = now;
            }
        }
        else
        {
            configRepeatLeftMs = 0;
        }
        if (bR && bR->isPressed() && bR->pressedFor(kBtnLongPressMs))
        {
            if (configRepeatRightMs == 0 || (now - configRepeatRightMs) >= kBtnRepeatMs)
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
            uint32_t prevRemaining = remainingSec;
            remainingSec = newRemaining;

            if (SOUND_ACTIVE && buzzerMode == BuzzerMode::Countdown && tickRtttl.length() > 0)
            {
                // Beep if any second in [1, TIMER_COUNTDOWN_SECONDS] was crossed this tick.
                // The buzzer plays one tone at a time, so a single beep covers the gap when
                // tick() falls behind (rather than queuing N back-to-back plays).
                uint32_t lo = newRemaining > 0 ? newRemaining : 1;
                uint32_t hi = prevRemaining > 0 ? prevRemaining - 1 : 0;
                if (hi > TIMER_COUNTDOWN_SECONDS) hi = TIMER_COUNTDOWN_SECONDS;
                if (hi >= lo && !PeripheryManager.isPlaying())
                {
                    PeripheryManager.playRTTTLString(tickRtttl);
                }
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
                if (endRtttl.length() > 0) PeripheryManager.playRTTTLString(endRtttl);
            }
            lastRealertMs = now;
        }
    }
}

TimerCmdResult TimerManager_::parseCommand(const char *json)
{
    if (!SHOW_TIMER) return TimerCmdResult::Disabled;
    if (json == nullptr || json[0] == '\0') return TimerCmdResult::BadJson;

    DynamicJsonDocument doc(kTimerCmdJsonSize);
    auto err = deserializeJson(doc, json);
    if (err)
    {
        if (DEBUG_MODE && err == DeserializationError::NoMemory)
            DEBUG_PRINTLN("timer: parseCommand NoMemory");
        return TimerCmdResult::BadJson;
    }

    // -- Validation pass: mutate nothing; reject the whole command on the first
    //    invalid field. Out-of-range is rejected here, not clamped (parity). --

    // max_duration is range-defining for duration; validate first so a payload
    // that raises the ceiling and sets a duration within the new ceiling in the
    // same call is accepted atomically (ADR-0001 addendum).
    uint32_t maxDuration = TIMER_MAX_DURATION;
    bool haveMaxDuration = doc.containsKey("max_duration");
    if (haveMaxDuration)
    {
        JsonVariant v = doc["max_duration"];
        if (!(v.is<long>() || v.is<float>())) return TimerCmdResult::BadField;
        uint32_t n = v.as<uint32_t>();
        if (n < 1 || n > 604800) return TimerCmdResult::BadField;
        maxDuration = n;
    }
    const uint32_t effectiveMaxDuration = haveMaxDuration ? maxDuration : TIMER_MAX_DURATION;

    uint32_t durSecs = 0;
    bool haveDuration = doc.containsKey("duration");
    if (haveDuration)
    {
        JsonVariant dv = doc["duration"];
        if (dv.is<const char *>())
        {
            if (!parseHMS(dv.as<String>(), durSecs)) return TimerCmdResult::BadField;
        }
        else if (dv.is<long>() || dv.is<float>())   // any JSON number (int or float); not bool/object/null
        {
            durSecs = dv.as<uint32_t>();
        }
        else
        {
            return TimerCmdResult::BadField;
        }
        if (durSecs < 1 || (effectiveMaxDuration > 0 && durSecs > effectiveMaxDuration)) return TimerCmdResult::BadField;
    }

    BuzzerMode   buzzer   = buzzerMode;
    FinishedMode finished = finishedMode;
    bool haveBuzzer   = doc.containsKey("buzzer");
    bool haveFinished = doc.containsKey("finished");
    if (haveBuzzer   && !parseBuzzerMode  (doc["buzzer"].as<String>(),   buzzer))   return TimerCmdResult::BadField;
    if (haveFinished && !parseFinishedMode(doc["finished"].as<String>(), finished)) return TimerCmdResult::BadField;

    if (doc.containsKey("icon_idle")     && !isValidIconName(doc["icon_idle"].as<String>()))     return TimerCmdResult::BadField;
    if (doc.containsKey("icon_running")  && !isValidIconName(doc["icon_running"].as<String>()))  return TimerCmdResult::BadField;
    if (doc.containsKey("icon_paused")   && !isValidIconName(doc["icon_paused"].as<String>()))   return TimerCmdResult::BadField;
    if (doc.containsKey("icon_finished") && !isValidIconName(doc["icon_finished"].as<String>())) return TimerCmdResult::BadField;

    uint16_t finishedHold     = TIMER_FINISHED_HOLD;
    uint16_t realertInterval  = TIMER_REALERT_INTERVAL;
    uint16_t countdownSeconds = TIMER_COUNTDOWN_SECONDS;
    bool haveFinishedHold     = doc.containsKey("finished_hold");
    bool haveRealertInterval  = doc.containsKey("realert_interval");
    bool haveCountdownSeconds = doc.containsKey("countdown_seconds");
    if (haveFinishedHold)
    {
        JsonVariant v = doc["finished_hold"];
        if (!(v.is<long>() || v.is<float>())) return TimerCmdResult::BadField;
        uint32_t n = v.as<uint32_t>();
        if (n < 1 || n > 300) return TimerCmdResult::BadField;
        finishedHold = (uint16_t)n;
    }
    if (haveRealertInterval)
    {
        JsonVariant v = doc["realert_interval"];
        if (!(v.is<long>() || v.is<float>())) return TimerCmdResult::BadField;
        uint32_t n = v.as<uint32_t>();
        if (n < 5 || n > 300) return TimerCmdResult::BadField;
        realertInterval = (uint16_t)n;
    }
    if (haveCountdownSeconds)
    {
        JsonVariant v = doc["countdown_seconds"];
        if (!(v.is<long>() || v.is<float>())) return TimerCmdResult::BadField;
        uint32_t n = v.as<uint32_t>();
        if (n > 30) return TimerCmdResult::BadField;
        countdownSeconds = (uint16_t)n;
    }

    // Behavior parameters (ADR-0004): four distinct categories, atomic-reject validation.
    // (max_duration is hoisted above to gate duration's effective ceiling — ADR-0001 addendum.)
    uint32_t buttonStep        = TIMER_STEP;
    uint16_t publishInterval   = TIMER_PUBLISH_INTERVAL;
    uint16_t appConfigTimeout  = TIMER_CONFIG_TIMEOUT;
    bool haveButtonStep        = doc.containsKey("button_step");
    bool havePublishInterval   = doc.containsKey("remaining_publish_interval");
    bool haveAppConfigTimeout  = doc.containsKey("app_config_timeout");
    if (haveButtonStep)
    {
        JsonVariant v = doc["button_step"];
        if (!(v.is<long>() || v.is<float>())) return TimerCmdResult::BadField;
        uint32_t n = v.as<uint32_t>();
        if (n < 1 || n > 99) return TimerCmdResult::BadField;
        buttonStep = n;
    }
    if (havePublishInterval)
    {
        JsonVariant v = doc["remaining_publish_interval"];
        if (!(v.is<long>() || v.is<float>())) return TimerCmdResult::BadField;
        uint32_t n = v.as<uint32_t>();
        if (n < 1 || n > 60) return TimerCmdResult::BadField;
        publishInterval = (uint16_t)n;
    }
    if (haveAppConfigTimeout)
    {
        JsonVariant v = doc["app_config_timeout"];
        if (!(v.is<long>() || v.is<float>())) return TimerCmdResult::BadField;
        uint32_t n = v.as<uint32_t>();
        if (n < 5 || n > 300) return TimerCmdResult::BadField;
        appConfigTimeout = (uint16_t)n;
    }

    // Melody filenames (bare names, same validation as icons — alphanumeric + _ + -).
    // Empty resets to canonical defaults at apply time. (ADR-0004 §melody empty semantics.)
    if (doc.containsKey("melody_tick") && !isValidIconName(doc["melody_tick"].as<String>())) return TimerCmdResult::BadField;
    if (doc.containsKey("melody_end")  && !isValidIconName(doc["melody_end"].as<String>()))  return TimerCmdResult::BadField;

    // bar_enabled: strict bool. bar_color: number or "#RRGGBB" hex string (0..0xFFFFFF).
    bool haveBarEnabled = doc.containsKey("bar_enabled");
    bool haveBarColor   = doc.containsKey("bar_color");
    bool barEnabled     = TIMER_BAR_ENABLED;
    uint32_t barColor   = TIMER_BAR_COLOR;
    if (haveBarEnabled)
    {
        JsonVariant v = doc["bar_enabled"];
        if (!v.is<bool>()) return TimerCmdResult::BadField;
        barEnabled = v.as<bool>();
    }
    if (haveBarColor)
    {
        JsonVariant v = doc["bar_color"];
        if (v.is<long>() || v.is<float>())
        {
            uint32_t n = v.as<uint32_t>();
            if (n > 0xFFFFFFu) return TimerCmdResult::BadField;
            barColor = n;
        }
        else if (v.is<const char*>() || v.is<String>())
        {
            String s = v.as<String>();
            s.trim();
            if (s.length() > 0 && s[0] == '#') s = s.substring(1);
            if (s.length() != 6) return TimerCmdResult::BadField;  // exactly RRGGBB
            for (size_t i = 0; i < s.length(); ++i)
            {
                char c = s[i];
                bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                if (!ok) return TimerCmdResult::BadField;
            }
            barColor = (uint32_t)strtoul(s.c_str(), nullptr, 16);
        }
        else
        {
            return TimerCmdResult::BadField;
        }
    }

    bool haveAction = doc.containsKey("action");
    if (haveAction && !isValidAction(doc["action"].as<String>())) return TimerCmdResult::BadField;

    // -- Command is known-good: only now disturb device state. --
    if (inConfig)
    {
        // An accepted inbound command discards an in-progress on-device edit and
        // drains any notifications deferred during config. A rejected command (above)
        // leaves the edit untouched.
        inConfig = false;
        DisplayManager.drainDeferredNotifications();
        if (DEBUG_MODE) DEBUG_PRINTLN("timer: config aborted by inbound command");
    }

    _suspendPersist = true;
    _dirty = false;

    // TIMER_MAX_DURATION must land before setDuration() so its internal backstop
    // clamp sees the in-payload ceiling, not the pre-payload one (ADR-0001 addendum).
    if (haveMaxDuration)            TIMER_MAX_DURATION = maxDuration;

    if (haveDuration)               setDuration(durSecs);   // pre-validated in range
    if (doc.containsKey("icon_idle"))     setIconIdle    (doc["icon_idle"].as<String>());
    if (doc.containsKey("icon_running"))  setIconRunning (doc["icon_running"].as<String>());
    if (doc.containsKey("icon_paused"))   setIconPaused  (doc["icon_paused"].as<String>());
    if (doc.containsKey("icon_finished")) setIconFinished(doc["icon_finished"].as<String>());
    if (haveBuzzer)                 setBuzzerMode(buzzer);
    if (haveFinished)               setFinishedMode(finished);

    _suspendPersist = false;
    if (_dirty)
    {
        _dirty = false;
        persist();
    }

    bool persistedKeyChanged = false;
    if (haveFinishedHold)     { TIMER_FINISHED_HOLD     = finishedHold;     persistedKeyChanged = true; }
    if (haveRealertInterval)  { TIMER_REALERT_INTERVAL  = realertInterval;  persistedKeyChanged = true; }
    if (haveCountdownSeconds) { TIMER_COUNTDOWN_SECONDS = countdownSeconds; persistedKeyChanged = true; }
    if (haveMaxDuration)      { /* TIMER_MAX_DURATION already assigned above */ persistedKeyChanged = true; }
    if (haveButtonStep)       { TIMER_STEP              = buttonStep;       persistedKeyChanged = true; }
    if (havePublishInterval)  { TIMER_PUBLISH_INTERVAL  = publishInterval;  persistedKeyChanged = true; }
    if (haveAppConfigTimeout) { TIMER_CONFIG_TIMEOUT    = appConfigTimeout; persistedKeyChanged = true; }
    if (haveBarEnabled)       { TIMER_BAR_ENABLED       = barEnabled;       persistedKeyChanged = true; }
    if (haveBarColor)         { TIMER_BAR_COLOR         = barColor;         persistedKeyChanged = true; }

    bool melodyChanged = false;
    if (doc.containsKey("melody_tick"))
    {
        String s = doc["melody_tick"].as<String>();
        TIMER_MELODY_TICK = (s.length() == 0) ? String("timer_tick") : s;
        persistedKeyChanged = true;
        melodyChanged = true;
    }
    if (doc.containsKey("melody_end"))
    {
        String s = doc["melody_end"].as<String>();
        TIMER_MELODY_END = (s.length() == 0) ? String("timer_end") : s;
        persistedKeyChanged = true;
        melodyChanged = true;
    }
    if (persistedKeyChanged) saveSettings();
    if (melodyChanged) loadMelodiesCached();

    if (haveAction)
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

    return TimerCmdResult::Ok;
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
