#include "TimerManager.h"
#include "TimerSettings.h"
#include "TimerHa.h"
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

    const char *FALLBACK_END_RTTTL  = "timer:d=4,o=5,b=120:c,8p,c,8p,c";
    const char *FALLBACK_TICK_RTTTL = "tick:d=16,o=6,b=200:c";

    // The config-block keys that stay member-backed (B1 boundary, ADR-0007/0009) now
    // live in TIMER_MEMBER_CONFIG_DESCS (TimerSettings.cpp), the config block's second
    // table. Validation, apply, snapshot-emit and the broadcast trigger all loop that
    // one table, so they cannot drift apart.
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
    configEditor.exit();   // a (re)boot is never mid-edit; discard any editor state (return unused)

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
    // The four icon rows declare ONE shared aggregate publish hook (issue #34),
    // so dispatching any icon key reaches the same declaration.
    timerMemberConfigPublish("icon_idle");
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

// Canonical wire spellings are a single row read from the per-enum codec table
// (src/TimerEnums.cpp) -- the enum value is the row index. See docs/adr/0010.
const char *TimerManager_::buzzerModeString() const
{
    return buzzerCodec(buzzerMode).wire;
}

const char *TimerManager_::finishedModeString() const
{
    return finishedCodec(finishedMode).wire;
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

// String->enum is a case-insensitive scan of the codec table (canonical wire
// spelling or any alias). The matched row index is the enum value. ADR-0010.
bool TimerManager_::parseBuzzerMode(const String &s, BuzzerMode &out)
{
    uint8_t idx;
    if (!timerEnumParse(TIMER_BUZZER_CODEC, TIMER_BUZZER_CODEC_COUNT, s, idx)) return false;
    out = (BuzzerMode)idx;
    return true;
}

bool TimerManager_::parseFinishedMode(const String &s, FinishedMode &out)
{
    uint8_t idx;
    if (!timerEnumParse(TIMER_FINISHED_CODEC, TIMER_FINISHED_CODEC_COUNT, s, idx)) return false;
    out = (FinishedMode)idx;
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

// The config-mode value logic AND timing (hold-to-repeat + 30 s auto-apply) live in
// TimerConfigEditor; these methods stay as thin forwarders so TimerView/Apps.cpp/
// PeripheryManager are unchanged. Only the run-state mutation (the enter-time 99h
// clamp, the exit-time setDuration/drain/broadcast) stays here; each forwarder
// noteInput()s the editor so any input resets its idle timer. See docs/adr/0011
// (editor extraction) and docs/adr/0012 (timing moved into editor.tick()).
void TimerManager_::enterConfigMode()
{
    if (state != TimerState::Idle) return;
    if (durationSec > kConfigHHMax) durationSec = kConfigHHMax;   // keep HH two-digit-editable (run-state)
    configEditor.enter(durationSec);
    configEditor.noteInput(millis());   // seed the editor's no-input idle clock
}

void TimerManager_::exitConfigMode()
{
    if (!configEditor.isActive()) return;
    setDuration(configEditor.exit());   // commit the edited duration through the unchanged path
    DisplayManager.drainDeferredNotifications();
    broadcastRunState(nullptr);         // duration is run-state; propagate the new length
}

void TimerManager_::configCycleField()
{
    if (!configEditor.isActive()) return;
    configEditor.cycleField();
    configEditor.noteInput(millis());   // any input resets the editor's auto-apply timeout
}

void TimerManager_::configAdjust(int delta)
{
    if (!configEditor.isActive()) return;
    configEditor.adjust(delta);
    configEditor.noteInput(millis());   // any input resets the editor's auto-apply timeout
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

void TimerManager_::setBuzzerMode(BuzzerMode m, bool persist)
{
    if (buzzerMode == m) return;
    buzzerMode = m;
    if (persist) persistIfDirty();
    publishBuzzerMode();
}

void TimerManager_::setFinishedMode(FinishedMode m, bool persist)
{
    if (finishedMode == m) return;
    finishedMode = m;
    if (persist) persistIfDirty();
    publishFinishedMode();
}

void TimerManager_::persistConfig()
{
    persist();
}

void TimerManager_::tick()
{
    unsigned long now = millis();

    if (configEditor.isActive())
    {
        // Config-mode timing (hold-to-repeat + 30 s auto-apply) lives in the editor,
        // which reads injected button state and the current time. We only feed it the
        // raw button presses and commit on its TimedOut signal. See docs/adr/0012.
        EasyButton *bL = PeripheryManager.buttonL;
        EasyButton *bR = PeripheryManager.buttonR;
        TimerConfigEditor::ButtonState buttons{ bL && bL->isPressed(), bR && bR->isPressed() };
        if (configEditor.tick(now, buttons) == TimerConfigEditor::TickOutcome::TimedOut)
        {
            exitConfigMode();   // commit the edited duration through setDuration + drain + broadcast
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

    // Member-backed config half (B1): validate + coerce each present row into a staging
    // array via its hook. Pure -- no global written until the apply pass below, so a bad
    // member field rejects the whole command atomically (ADR-0001), same as the table half.
    TcValue memberStaged[TIMER_MEMBER_CONFIG_DESC_COUNT];
    bool    memberPresent[TIMER_MEMBER_CONFIG_DESC_COUNT];
    for (size_t i = 0; i < TIMER_MEMBER_CONFIG_DESC_COUNT; ++i)
    {
        const TimerMemberConfigDesc &d = TIMER_MEMBER_CONFIG_DESCS[i];
        memberPresent[i] = doc.containsKey(d.cmdKey);
        if (!memberPresent[i]) continue;
        if (!d.validate(doc[d.cmdKey], memberStaged[i])) return TimerCmdResult::BadField;
    }

    bool haveAction = doc.containsKey("action");
    if (haveAction && !isValidAction(doc["action"].as<String>())) return TimerCmdResult::BadField;

    // -- Command is known-good: only now disturb device state. --
    if (configEditor.isActive())
    {
        // An accepted inbound command discards an in-progress on-device edit and
        // drains any notifications deferred during config. A rejected command (above)
        // leaves the edit untouched. exit() deactivates; the return is intentionally
        // discarded (the edit is aborted, not committed via setDuration).
        configEditor.exit();
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

    // Member-backed applies via publish-aware setters (routed through the member table's
    // apply hooks); their "timer"-namespace NVS writes are batched by the PersistBatch
    // guard into one flush on scope exit. duration stays its own call (run-state, B1;
    // applied first so it lands before any member-config side effects).
    {
        PersistBatch batch(*this);
        if (haveDuration) setDuration(durSecs);   // pre-validated in range
        for (size_t i = 0; i < TIMER_MEMBER_CONFIG_DESC_COUNT; ++i)
            if (memberPresent[i]) TIMER_MEMBER_CONFIG_DESCS[i].apply(memberStaged[i]);
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
        // member-backed half via the TIMER_MEMBER_CONFIG_DESCS table.
        bool configChanged = snapshotChanged || timerDocTouchesMemberConfig(doc);

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

// State is run-state, not a member-config row, so it goes through the wire
// seam directly: the exact (topic, payload) the broker receives, byte-identical
// to the retired HASensor::setValue path (issue #31 / PRD #28).
void TimerManager_::publishState()
{
    MQTTManager.publishTimerWire(MQTTManager.timerWireTopic(TimerHaEntity::State).c_str(),
                                 getStateString());
}
// Remaining is run-state too (issue #32): straight through the seam, payload a
// plain decimal string — byte-identical to the retired HASensorNumber
// (PrecisionP0) setValue path.
void TimerManager_::publishRemaining()
{
    MQTTManager.publishTimerWire(MQTTManager.timerWireTopic(TimerHaEntity::Remaining).c_str(),
                                 String(remainingSec).c_str());
}
// Duration is run-state too (issue #34): straight through the seam, payload the
// trimmed-HMS clock string — byte-identical to the retired HAText::setState path.
void TimerManager_::publishDuration()
{
    MQTTManager.publishTimerWire(MQTTManager.timerWireTopic(TimerHaEntity::Duration).c_str(),
                                 formatHMS(durationSec).c_str());
}
// The enum keys are member-config rows (issue #33): dispatch through the row's
// declared publish hook so validate/apply/emit/publish stay co-located and the
// table is the single definition of how each key goes out on the wire.
void TimerManager_::publishBuzzerMode()   { timerMemberConfigPublish("buzzer"); }
void TimerManager_::publishFinishedMode() { timerMemberConfigPublish("finished"); }

// Full wire refresh (issue #41, closing PRD #28). The run-state trio is a fixed
// set (state/remaining/duration are run-state, not table rows); the config half
// is DERIVED from TIMER_MEMBER_CONFIG_DESCS, so a row added with a publish hook
// is republished on connect / discovery-enable without touching this function.
// Hooks are deduped by pointer — the four icon rows share one aggregate hook,
// whose JSON must hit the wire exactly once. Order preserved from the retired
// hand-listed blocks: duration, remaining, state, then table order.
void TimerManager_::publishAllWire()
{
    publishDuration();
    publishRemaining();
    publishState();
    for (size_t i = 0; i < TIMER_MEMBER_CONFIG_DESC_COUNT; ++i)
    {
        void (*hook)() = TIMER_MEMBER_CONFIG_DESCS[i].publish;
        if (!hook) continue;
        bool fired = false;
        for (size_t j = 0; j < i && !fired; ++j)
            fired = (TIMER_MEMBER_CONFIG_DESCS[j].publish == hook);
        if (!fired) hook();
    }
}

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

void TimerManager_::buildConfigSnapshot(JsonDocument &doc) const
{
    // Config block only — never action/duration (run-state) or sync_* (local identity,
    // inSnapshot=false). Two tables, one config block: TIMER_SETTINGS_DESCS' inSnapshot
    // rows and TIMER_MEMBER_CONFIG_DESCS (the member-backed half, B1, ADR-0007/0009).
    // Each table also feeds the parseCommand broadcast trigger, so the snapshot can't
    // drift from what fires a broadcast.
    timerSettingsBuildSnapshot(doc);
    timerMemberConfigBuildSnapshot(doc);
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
