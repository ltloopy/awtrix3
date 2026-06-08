#include "TimerManager.h"
#include "TimerSettings.h"
#include "Globals.h"
#include "PeripheryManager.h"
#include "DisplayManager.h"
#include "MQTTManager.h"
#include "MenuManager.h"
#include "ServerManager.h"
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

namespace {
    // Sized to hold the full config snapshot (~18 keys) plus the _sync envelope on
    // the propagation surface, with headroom for ArduinoJson's larger 64-bit slots
    // (host tests). The HTTP/MQTT control surfaces never approach it.
    constexpr uint16_t kTimerCmdJsonSize     = 2048;
    constexpr uint8_t  kIconNameMaxLen       = 32;
    constexpr uint32_t kConfigHHMax          = 99UL * 3600UL;
    constexpr unsigned long kBtnLongPressMs  = 500;
    constexpr unsigned long kBtnRepeatMs     = 250;

    const char *FALLBACK_END_RTTTL  = "timer:d=4,o=5,b=120:c,8p,c,8p,c";
    const char *FALLBACK_TICK_RTTTL = "tick:d=16,o=6,b=200:c";

    // The config-block keys that stay member-backed (B1 boundary, ADR-0007) rather
    // than living in TIMER_SETTINGS_DESCS: their values are owned by TimerManager's
    // publish-aware setters. This single list governs their snapshot/broadcast
    // membership so buildConfigSnapshot and the parseCommand broadcast trigger cannot
    // drift apart. (Value handling still lives in the setters; this is membership only.)
    const char *const kMemberConfigKeys[] = {
        "buzzer", "finished", "icon_idle", "icon_running", "icon_paused", "icon_finished"};
    constexpr size_t kMemberConfigKeyCount = sizeof(kMemberConfigKeys) / sizeof(kMemberConfigKeys[0]);

    bool docTouchesMemberConfig(const JsonDocument &doc)
    {
        for (size_t i = 0; i < kMemberConfigKeyCount; ++i)
            if (doc.containsKey(kMemberConfigKeys[i])) return true;
        return false;
    }
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
    broadcastRunState(nullptr);   // duration is run-state; propagate the new length
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
    int next = (int)cur + (delta >= 0 ? 1 : -1);
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
    runDurationSec = durationSec;   // snapshot so a mid-run duration edit doesn't snap the progress bar
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
    else if (state == TimerState::Paused)
    {
        reset();   // editing duration while paused resets to Idle with the new duration
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

    // Table settings (TIMER_SETTINGS_DESCS): validate + coerce each present row into a
    // staging array. Nothing is written until every field below has validated too, so a
    // single bad field rejects the whole command (ADR-0001 atomic-reject). max_duration
    // is range-defining for duration: capture its staged value so a payload that raises
    // the ceiling and sets a duration within it in the same call is accepted atomically
    // (ADR-0001 addendum).
    TcValue  tableStaged[TIMER_SETTINGS_DESC_COUNT];
    bool     tablePresent[TIMER_SETTINGS_DESC_COUNT];
    uint32_t effectiveMaxDuration = TIMER_MAX_DURATION;
    for (size_t i = 0; i < TIMER_SETTINGS_DESC_COUNT; ++i)
    {
        const TimerSettingDesc &d = TIMER_SETTINGS_DESCS[i];
        tablePresent[i] = doc.containsKey(d.cmdKey);
        if (!tablePresent[i]) continue;
        if (!timerSettingParse(d, doc[d.cmdKey], tableStaged[i])) return TimerCmdResult::BadField;
        if (strcmp(d.cmdKey, "max_duration") == 0) effectiveMaxDuration = tableStaged[i].num;
    }

    // duration stays member-backed (B1, ADR-0007): validated here against the effective
    // ceiling staged above.
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

    // -- Command is known-good: apply. Table rows first, so TIMER_MAX_DURATION lands
    //    before setDuration() sees it (ADR-0001 addendum). The table writes globals
    //    only; they persist to the "awtrix" namespace via saveSettings() below. --
    bool tableChanged    = false;   // any table key written -> needs saveSettings()
    bool snapshotChanged = false;   // any config-block (inSnapshot) table key -> broadcastConfig()
    bool melodyChanged   = false;
    for (size_t i = 0; i < TIMER_SETTINGS_DESC_COUNT; ++i)
    {
        if (!tablePresent[i]) continue;
        const TimerSettingDesc &d = TIMER_SETTINGS_DESCS[i];
        timerSettingStore(d, tableStaged[i]);
        tableChanged = true;
        if (d.inSnapshot) snapshotChanged = true;
        if (strcmp(d.cmdKey, "melody_tick") == 0 || strcmp(d.cmdKey, "melody_end") == 0) melodyChanged = true;
    }

    // Member-backed applies via publish-aware setters; their "timer"-namespace NVS
    // writes are batched under _suspendPersist into a single persist().
    _suspendPersist = true;
    _dirty = false;
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

    if (tableChanged)  saveSettings();        // persist the "awtrix"-namespace table keys once
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

    // Propagation surface (one-hop): mirror this locally-accepted command to peers.
    // The broadcast* methods no-op when _remoteApply is set (inbound packet) or sync
    // is off. Run-state (action/duration) and config travel on separate packets; a
    // command touching both classes emits one of each. sync_follow/sync_targets are
    // local identity (inSnapshot=false) and intentionally trigger neither.
    if (!_remoteApply)
    {
        bool runStateChanged = haveAction || haveDuration;
        // configChanged = any config-block key edited. Snapshot membership IS the
        // broadcast trigger (one flag, ADR-0006): the table half via inSnapshot, the
        // member-backed half via the shared kMemberConfigKeys list.
        bool configChanged = snapshotChanged || docTouchesMemberConfig(doc);

        if (configChanged) broadcastConfig();
        if (runStateChanged)
        {
            if (haveAction)
            {
                String a = doc["action"].as<String>();
                a.toLowerCase();
                broadcastRunState(a.c_str());
            }
            else
            {
                broadcastRunState(nullptr);   // duration-only edit
            }
        }
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

// ---------------------------------------------------------------------------
// Propagation surface (device-to-device timer sync). See CONTEXT.md and
// docs/adr/0006-timer-multi-device-sync.md.
// ---------------------------------------------------------------------------

void TimerManager_::addSyncEnvelope(JsonObject &sync)
{
    sync["src"] = uniqueID;
    sync["seq"] = ++_syncSeq;
    String t = TIMER_SYNC_TARGETS; t.trim();
    if (t == "all") { sync["tgt"] = "all"; return; }
    JsonArray arr = sync.createNestedArray("tgt");
    int start = 0;
    const int n = t.length();
    while (start < n)
    {
        int comma = t.indexOf(',', start);
        if (comma < 0) comma = n;
        String id = t.substring(start, comma); id.trim();
        if (id.length() > 0) arr.add(id);
        start = comma + 1;
    }
}

void TimerManager_::addMemberConfigToSnapshot(JsonDocument &doc) const
{
    // The member-backed half of the config block (B1, ADR-0007). Membership is the
    // single shared kMemberConfigKeys list; values come from the live members here.
    for (size_t i = 0; i < kMemberConfigKeyCount; ++i)
    {
        const char *k = kMemberConfigKeys[i];
        if      (strcmp(k, "buzzer")        == 0) doc[k] = buzzerModeString();
        else if (strcmp(k, "finished")      == 0) doc[k] = finishedModeString();
        else if (strcmp(k, "icon_idle")     == 0) doc[k] = iconIdle;
        else if (strcmp(k, "icon_running")  == 0) doc[k] = iconRunning;
        else if (strcmp(k, "icon_paused")   == 0) doc[k] = iconPaused;
        else if (strcmp(k, "icon_finished") == 0) doc[k] = iconFinished;
    }
}

void TimerManager_::buildConfigSnapshot(JsonDocument &doc) const
{
    // Config block only — never action/duration (run-state) or sync_* (local identity,
    // inSnapshot=false). Two halves: the table (inSnapshot rows) and the member-backed
    // fields (B1, ADR-0007). Both source their membership from one definition, so the
    // snapshot can't drift from the parseCommand broadcast trigger.
    timerSettingsBuildSnapshot(doc);
    addMemberConfigToSnapshot(doc);
}

void TimerManager_::broadcastRunState(const char *action)
{
    if (_remoteApply) return;                       // one-hop: never re-emit an applied remote command
    if (TIMER_SYNC_TARGETS.length() == 0) return;   // sync off

    StaticJsonDocument<256> doc;
    JsonObject sync = doc.createNestedObject("_sync");
    addSyncEnvelope(sync);
    if (action) doc["action"] = action;
    // duration is run-state: it rides with a start (defines the countdown) and with a
    // bare duration edit (action == nullptr). pause/reset need only the action.
    bool withDuration = (action == nullptr) || (strcasecmp(action, "start") == 0);
    if (withDuration) doc["duration"] = durationSec;

    String out; serializeJson(doc, out);
    ServerManager.sendTimerSync(out);
}

void TimerManager_::broadcastConfig()
{
    if (_remoteApply) return;
    if (TIMER_SYNC_TARGETS.length() == 0) return;

    DynamicJsonDocument doc(kTimerCmdJsonSize);
    JsonObject sync = doc.createNestedObject("_sync");
    addSyncEnvelope(sync);
    buildConfigSnapshot(doc);

    String out; serializeJson(doc, out);
    ServerManager.sendTimerSync(out);
}

bool TimerManager_::syncTargetsMe(JsonVariantConst tgt) const
{
    if (tgt.is<const char *>())
    {
        String s = tgt.as<String>(); s.trim();
        return s == "all";
    }
    if (tgt.is<JsonArrayConst>())
    {
        for (JsonVariantConst v : tgt.as<JsonArrayConst>())
        {
            String id = v.as<String>(); id.trim();
            if (id == uniqueID) return true;
        }
    }
    return false;
}

bool TimerManager_::syncSeenRecently(const String &src, uint32_t seq, unsigned long nowMs)
{
    const unsigned long kTtlMs = 2000;
    for (uint8_t i = 0; i < kSyncSeenMax; ++i)
    {
        if (_syncSeen[i].atMs != 0 && (nowMs - _syncSeen[i].atMs) <= kTtlMs
            && _syncSeen[i].seq == seq && _syncSeen[i].src == src)
            return true;
    }
    _syncSeen[_syncSeenIdx].src  = src;
    _syncSeen[_syncSeenIdx].seq  = seq;
    _syncSeen[_syncSeenIdx].atMs = (nowMs == 0) ? 1 : nowMs;   // 0 doubles as "empty slot"
    _syncSeenIdx = (uint8_t)((_syncSeenIdx + 1) % kSyncSeenMax);
    return false;
}

void TimerManager_::applySyncCommand(const char *json)
{
    if (json == nullptr || json[0] == '\0') return;

    DynamicJsonDocument doc(kTimerCmdJsonSize);
    if (deserializeJson(doc, json)) return;

    JsonVariantConst sync = doc["_sync"];
    if (sync.isNull()) return;                       // not a sync packet

    String src = sync["src"].as<String>();
    if (src.length() == 0 || src == uniqueID) return; // malformed / own echo
    if (!TIMER_SYNC_FOLLOW) return;                   // consent gate
    if (!syncTargetsMe(sync["tgt"])) return;          // not addressed to this clock
    if (syncSeenRecently(src, sync["seq"].as<uint32_t>(), millis())) return; // redundant copy

    // Re-enter the local control surface. The send-path _remoteApply guard prevents
    // this from re-broadcasting (one-hop). parseCommand ignores the _sync envelope.
    _remoteApply = true;
    parseCommand(json);
    _remoteApply = false;
}
