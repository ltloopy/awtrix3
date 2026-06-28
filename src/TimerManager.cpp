#include "TimerManager.h"
#include "TimerSettings.h"
#include "TimerHa.h"
#include "SyncEnvelope.h"     // wire envelope + pure inbound receive gate (ADR-0023)
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

// One-shot override (PRD #99 / issue #100). captureSnapshot records the SAVED config
// before a save:false command applies on top of it; restoreSnapshot writes it back
// when the timer returns to Idle. The table half (Family A inSnapshot rows) is captured
// generically over TIMER_SETTINGS_DESCS; the member-backed half (Family B) plus the
// run-state duration and the resolved melody RAM are TimerManager's own members, so they
// are captured/restored directly. Restore writes members directly (no setter), so it
// triggers no persist/publish side effects — carriers reported saved values throughout.
void TimerManager_::captureSnapshot()
{
    timerSettingsCaptureSnapshot(_snapTable);
    _snapBuzzer       = buzzerMode;
    _snapFinished     = finishedMode;
    _snapIconIdle     = iconIdle;
    _snapIconRunning  = iconRunning;
    _snapIconPaused   = iconPaused;
    _snapIconFinished = iconFinished;
    _snapDuration     = durationSec;
    _snapEndRtttl     = endRtttl;
    _snapTickRtttl    = tickRtttl;
}

void TimerManager_::restoreSnapshot()
{
    timerSettingsRestoreSnapshot(_snapTable);
    buzzerMode    = _snapBuzzer;
    finishedMode  = _snapFinished;
    iconIdle      = _snapIconIdle;
    iconRunning   = _snapIconRunning;
    iconPaused    = _snapIconPaused;
    iconFinished  = _snapIconFinished;
    durationSec   = _snapDuration;
    endRtttl      = _snapEndRtttl;
    tickRtttl     = _snapTickRtttl;
}

// The single revert seam shared by reset() and the tick auto-clear transition: if a
// one-shot override is active, restore the saved config and clear the flag. A no-op
// otherwise, so the normal return-to-Idle paths are unaffected.
void TimerManager_::returnToIdle()
{
    if (!_overrideActive) return;
    restoreSnapshot();
    _overrideActive = false;
}

// Honest observation carriers (issue #101): swap the SAVED config block into live
// storage for the duration of a carrier projection, then restore the effective
// (one-shot) values. Only the config-block state the projections read is swapped —
// the table inSnapshot rows (sync_* excluded by construction) and the member-backed
// half (buzzer/finished/icons). Run-state (duration) and the resolved melody RAM are
// not touched. No-op when no override is active.
TimerManager_::SavedConfigScope::SavedConfigScope(const TimerManager_ &t)
    : tm(const_cast<TimerManager_ &>(t)), active(t._overrideActive)
{
    if (!active) return;
    // Stash the effective (one-shot) config block, then present the saved config.
    timerSettingsCaptureSnapshot(effTable);
    effBuzzer       = tm.buzzerMode;
    effFinished     = tm.finishedMode;
    effIconIdle     = tm.iconIdle;
    effIconRunning  = tm.iconRunning;
    effIconPaused   = tm.iconPaused;
    effIconFinished = tm.iconFinished;

    timerSettingsRestoreSnapshot(tm._snapTable);
    tm.buzzerMode   = tm._snapBuzzer;
    tm.finishedMode = tm._snapFinished;
    tm.iconIdle     = tm._snapIconIdle;
    tm.iconRunning  = tm._snapIconRunning;
    tm.iconPaused   = tm._snapIconPaused;
    tm.iconFinished = tm._snapIconFinished;
}

TimerManager_::SavedConfigScope::~SavedConfigScope()
{
    if (!active) return;
    timerSettingsRestoreSnapshot(effTable);
    tm.buzzerMode   = effBuzzer;
    tm.finishedMode = effFinished;
    tm.iconIdle     = effIconIdle;
    tm.iconRunning  = effIconRunning;
    tm.iconPaused   = effIconPaused;
    tm.iconFinished = effIconFinished;
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
    _suspendPersist = false;
    _dirty = false;        // just loaded from NVS: RAM matches it, nothing pending

    // A (re)boot knows no peers and has emitted no beacon yet — the peer registry is
    // pure RAM/LAN-derived state, repopulated by inbound beacons (#111 / ADR-0019).
    _registry.setOwnId(uniqueID);
    _registry.clear();
    _lastPresenceMs   = 0;
    _presenceEverSent = false;

    // A (re)boot has applied no sync command yet; the dedup set is pure RAM
    // (ADR-0022), repopulated by inbound commands.
    _seen.clear();

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
    // Bumped from the old 512-byte fixed buffer to the shared command-size
    // constant the snapshot/broadcast paths use, to hold the added config mirror
    // (PRD #73: ~20 keys incl. two melodies, a CSV sync_targets, four icon names).
    DynamicJsonDocument doc(kTimerCmdJsonSize);
    uint32_t remaining = computeCurrentRemaining();
    doc["state"]         = getStateString();
    doc["enabled"]       = (bool)SHOW_TIMER;
    doc["remaining"]     = remaining;
    doc["remaining_str"] = formatHMS(remaining);
    doc["duration"]      = durationSec;
    doc["duration_str"]  = formatHMS(durationSec);
    doc["buzzer"]        = buzzerModeString();
    doc["finished"]      = finishedModeString();

    // Persisted-config mirror (PRD #73): the complete two-table dump under a
    // nested `config` object, so an HTTP-only client reads back everything it can
    // POST. Built in a temp doc by the pure table-walking projection, then
    // deep-copied in -- duration stays top-level only (run-state, not config).
    DynamicJsonDocument cfg(kTimerCmdJsonSize);
    {
        // During a one-shot override the `config` mirror reports the SAVED config,
        // while the top-level run-state above stays effective (issue #101 / ADR-0015).
        SavedConfigScope saved(*this);
        timerBuildFullConfig(cfg);
    }
    doc["config"] = cfg.as<JsonObject>();

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

// Forwarder: the parse logic now lives in the descriptor-table family as the free
// function timerParseHMS (#142), so the command validator links the table and not
// this singleton. Callers reach it unchanged through this static.
bool TimerManager_::parseHMS(const String &in, uint32_t &outSeconds)
{
    return timerParseHMS(in, outSeconds);
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

// Forwarder: the predicate now lives in the descriptor-table family as the free
// function timerIsValidAction (#142). Callers reach it unchanged through this static.
bool TimerManager_::isValidAction(const String &s)
{
    return timerIsValidAction(s);
}

// Legacy Timer-app config-mode forwarders. The TIMER menu's DURATION leaf replaced
// this surface (#87), so these have no callers; they remain only so TimerView/
// Apps.cpp/PeripheryManager keep compiling. The value logic and hold-to-repeat live
// in TimerConfigEditor (the editor has no auto-apply timeout anymore, #88). Only the
// run-state mutation (the enter-time 99h clamp, the exit-time setDuration/drain/
// broadcast) stays here. See docs/adr/0011 (extraction) and docs/adr/0016.
void TimerManager_::enterConfigMode()
{
    if (state != TimerState::Idle) return;
    if (durationSec > kConfigHHMax) durationSec = kConfigHHMax;   // keep HH two-digit-editable (run-state)
    configEditor.enter(durationSec);
}

void TimerManager_::exitConfigMode()
{
    if (!configEditor.isActive()) return;
    setDuration(configEditor.exit());   // commit the edited duration through the unchanged path
    DisplayManager.drainDeferredNotifications();
    // A bare duration edit propagates nothing (#126): duration rides only with a start.
}

void TimerManager_::configCycleField()
{
    if (!configEditor.isActive()) return;
    configEditor.cycleField();
}

void TimerManager_::configAdjust(int delta)
{
    if (!configEditor.isActive()) return;
    configEditor.adjust(delta);
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
    returnToIdle();                 // one-shot: restore saved config before deriving remaining (issue #100)
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
    else _dirty = true;   // deferred edit: pending until the next PersistBatch commit
    publishBuzzerMode();
}

void TimerManager_::setFinishedMode(FinishedMode m, bool persist)
{
    if (finishedMode == m) return;
    finishedMode = m;
    if (persist) persistIfDirty();
    else _dirty = true;   // deferred edit: pending until the next PersistBatch commit
    publishFinishedMode();
}

void TimerManager_::tick()
{
    unsigned long now = millis();

    if (configEditor.isActive())
    {
        // Legacy Timer-app config mode is no longer entered (the TIMER menu's
        // DURATION leaf replaced it, #87); this only drives the editor's hold-to-
        // repeat if it is ever active. There is no auto-apply timeout (#88).
        EasyButton *bL = PeripheryManager.buttonL;
        EasyButton *bR = PeripheryManager.buttonR;
        TimerConfigEditor::ButtonState buttons{ bL && bL->isPressed(), bR && bR->isPressed() };
        configEditor.tick(now, buttons);
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
            returnToIdle();             // one-shot: restore saved config (shared seam, issue #100)
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
    // Inline RTTTL melodies (PRD #99 / issue #102): melody_end/melody_tick accept
    // EITHER a bare file-name token (the normal table path below) OR an inline tune
    // (detected by content). An inline tune is validated as RTTTL here, staged into
    // RAM, and the table row is excluded from the bare-name store — the saved melody
    // name global is never touched. An inline tune is always one-shot (it has no
    // persistable file form), so its presence forces the command one-shot.
    String inlineEnd, inlineTick;
    bool   haveInlineEnd = false, haveInlineTick = false;

    TcValue  tableStaged[TIMER_SETTINGS_DESC_COUNT];
    bool     tablePresent[TIMER_SETTINGS_DESC_COUNT];
    uint32_t effectiveMaxDuration = TIMER_MAX_DURATION;
    for (size_t i = 0; i < TIMER_SETTINGS_DESC_COUNT; ++i)
    {
        const TimerSettingDesc &d = TIMER_SETTINGS_DESCS[i];
        tablePresent[i] = doc.containsKey(d.cmdKey);
        if (!tablePresent[i]) continue;

        bool isMelodyKey = (strcmp(d.cmdKey, "melody_end") == 0 || strcmp(d.cmdKey, "melody_tick") == 0);
        if (isMelodyKey)
        {
            String mv = doc[d.cmdKey].as<String>();
            if (timerMelodyIsInline(mv))
            {
                if (!timerMelodyValidateInline(mv)) return TimerCmdResult::BadField;
                if (strcmp(d.cmdKey, "melody_end") == 0) { inlineEnd = mv;  haveInlineEnd = true; }
                else                                     { inlineTick = mv; haveInlineTick = true; }
                tablePresent[i] = false;   // excluded from the bare-name parse/store/snapshot
                continue;
            }
        }

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

    // One-shot flag (PRD #99 / issue #100): a payload-level boolean, default true.
    // A non-boolean rejects the whole command (atomic-reject, ADR-0001). save:false
    // makes this command one-shot — its config applies to the current run only and
    // reverts when the timer next returns to Idle (see the override block below).
    bool saveFlag = true;
    if (doc.containsKey("save"))
    {
        JsonVariantConst sv = doc["save"];
        if (!sv.is<bool>()) return TimerCmdResult::BadField;
        saveFlag = sv.as<bool>();
    }
    // An inline melody (issue #102) is always one-shot, so its presence forces the
    // whole command one-shot regardless of `save` — it has no persistable file form.
    // Receiver-forced one-shot (issue #108 / ADR-0018): a remote-applied command is
    // ALWAYS one-shot regardless of the leader's `save` flag, so a follower mirrors the
    // synced run but never persists it and reverts to its own saved config/duration on
    // return to Idle. Reuses the ADR-0017 override core via the existing _remoteApply guard.
    bool oneShot = !saveFlag || haveInlineEnd || haveInlineTick || _remoteApply;

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

    // -- Command is known-good: apply, inside ONE PersistBatch window; its scope
    //    exit commits the whole config (both NVS namespaces, each at most once).
    //    Table rows first, so TIMER_MAX_DURATION lands before setDuration() sees
    //    it (ADR-0001 addendum). Then duration (run-state, B1; applied before any
    //    member-config side effects), then the member-backed applies via their
    //    publish-aware setters. --
    // One-shot override (issue #100): before applying, snapshot the saved config so
    // returnToIdle() can restore it. Only the first save:false in a run captures
    // (latest-command-wins, single snapshot); a later save:false applies on top of
    // the same baseline.
    if (oneShot && !_overrideActive)
    {
        captureSnapshot();
        _overrideActive = true;
    }

    bool snapshotChanged = false;   // any config-block (inSnapshot) table key -> broadcastConfig()
    bool melodyChanged   = false;
    {
        PersistBatch batch(*this);
        if (oneShot) batch.setTransient();   // one-shot: scope exit writes nothing to flash
        for (size_t i = 0; i < TIMER_SETTINGS_DESC_COUNT; ++i)
        {
            if (!tablePresent[i]) continue;
            const TimerSettingDesc &d = TIMER_SETTINGS_DESCS[i];
            if (timerSettingStore(d, tableStaged[i])) batch.markTableDirty();
            if (d.inSnapshot) snapshotChanged = true;
            if (strcmp(d.cmdKey, "melody_tick") == 0 || strcmp(d.cmdKey, "melody_end") == 0) melodyChanged = true;
        }
        if (haveDuration) setDuration(durSecs);   // pre-validated in range
        for (size_t i = 0; i < TIMER_MEMBER_CONFIG_DESC_COUNT; ++i)
            if (memberPresent[i]) TIMER_MEMBER_CONFIG_DESCS[i].apply(memberStaged[i]);
    }

    if (melodyChanged) loadMelodiesCached();
    // Inline melodies (issue #102): use the tune directly as the resolved RAM, after
    // any bare-name re-resolve above so it wins. The saved name globals are untouched,
    // so the config mirror keeps showing the saved name; the snapshot captured the
    // saved-resolved RAM, so returnToIdle() reverts these on return to Idle.
    if (haveInlineEnd)  endRtttl  = inlineEnd;
    if (haveInlineTick) tickRtttl = inlineTick;

    // configInCommand: this payload carries a config-block key (table inSnapshot half
    // OR the member-backed half). Drives both the rebaseline trigger and the config
    // broadcast. sync_* (inSnapshot=false) and action/duration (run-state) are excluded.
    bool configInCommand = snapshotChanged || timerDocTouchesMemberConfig(doc);

    // Rebaseline (issue #100, story 21): a normal (save:true) config command arriving
    // during an active one-shot run commits the live config — including prior one-shot
    // values — as the new saved baseline and ends the override, so a later revert
    // leaves the promoted truth in place. A pure action/duration command does NOT
    // rebaseline (it carries no config to commit), so the override survives to revert.
    if (!oneShot && _overrideActive && configInCommand)
    {
        persist();        // member half: full live state -> "timer" NVS
        saveSettings();   // table half: full live state -> "awtrix" NVS
        _overrideActive = false;
    }

    // Settings projected as read-only HA attributes (PRD #57) are not wire rows
    // with their own setters, so republish each affected carrier's bag here when
    // any of its mapped keys was in the command. Table-driven, so a multi-carrier
    // key republishes every carrier; deduped so a carrier publishes at most once.
    // Fires on the remote-apply path too (not _remoteApply-gated), keeping each
    // synced peer's HA attributes consistent with no extra code (generalizes #52).
    // Suppressed under a one-shot command so the retained HA attribute bags keep
    // reporting the saved config (carriers stay honest, issue #101 builds on this).
    if (!oneShot)
    {
        bool attrCarrierDirty[(size_t)TimerHaEntity::COUNT] = {false};
        for (size_t i = 0; i < TIMER_ATTR_GROUP_DESC_COUNT; ++i)
        {
            const TimerAttrGroupDesc &g = TIMER_ATTR_GROUP_DESCS[i];
            if (doc.containsKey(g.cmdKey)) attrCarrierDirty[(size_t)g.carrier] = true;
        }
        for (size_t c = 0; c < (size_t)TimerHaEntity::COUNT; ++c)
            if (attrCarrierDirty[c]) publishAttributeGroup((TimerHaEntity)c);
    }

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
    // is off. Run-scoped config mirror (ADR-0018, superseding ADR-0006's config
    // propagation): a config EDIT propagates nothing — config travels only bundled
    // with a `start` (the combined packet broadcastRunState emits, carrying the
    // leader's effective config snapshot). pause/reset propagate run-state only. A
    // bare duration edit propagates NOTHING (#126): `duration` rides only with a
    // `start`, so a leader's edit no longer moves a follower's displayed time — a
    // follower adopts the leader's duration on the next start and reverts on Idle.
    // sync_follow/sync_targets are local identity (inSnapshot=false) and never propagate.
    if (!_remoteApply && haveAction)
    {
        String a = doc["action"].as<String>();
        a.toLowerCase();
        broadcastRunState(a.c_str());
    }

    return TimerCmdResult::Ok;
}

// HA control adapter (issue #109): each HA timer callback re-enters the control
// surface through parseCommand, exactly as the propagation surface does, rather
// than poking a deep setter. The minimal JSON each entity builds is the SAME shape
// the {prefix}/timer MQTT topic accepts, so HA edits inherit atomic-reject
// validation, run-state propagation, and the per-enum codec spellings for free —
// nothing about duration parsing or enum encoding is duplicated in the HA layer.
TimerCmdResult TimerManager_::timerHaApply(TimerHaEntity entity, const String &rawValue)
{
    StaticJsonDocument<128> doc;
    switch (entity)
    {
    case TimerHaEntity::Buzzer:
    {
        // The select callback hands us the chosen option index; map it through the
        // per-enum codec (ADR-0010) so the emitted wire string is the one
        // parseCommand accepts — the two cannot drift.
        long idx = rawValue.toInt();
        if (idx < 0 || (size_t)idx >= TIMER_BUZZER_CODEC_COUNT) return TimerCmdResult::BadField;
        doc["buzzer"] = TIMER_BUZZER_CODEC[idx].wire;
        break;
    }
    case TimerHaEntity::Finished:
    {
        long idx = rawValue.toInt();
        if (idx < 0 || (size_t)idx >= TIMER_FINISHED_CODEC_COUNT) return TimerCmdResult::BadField;
        doc["finished"] = TIMER_FINISHED_CODEC[idx].wire;
        break;
    }
    case TimerHaEntity::Duration:
        // The raw HH:MM:SS text rides straight into parseCommand, which owns the
        // parse/validate (parseHMS + range, reject-not-clamp). On a non-Ok result
        // the caller echoes the canonical live value back (snap-back).
        doc["duration"] = rawValue;
        break;
    case TimerHaEntity::Start:  doc["action"] = "start"; break;
    case TimerHaEntity::Pause:  doc["action"] = "pause"; break;
    case TimerHaEntity::Reset:  doc["action"] = "reset"; break;
    case TimerHaEntity::SyncFollow:
        // The switch callback hands us the new bool ("1"/"0"); emit the strict
        // bool parseCommand's sync_follow validator (TcCheck::Bool) accepts. Local
        // identity (inSnapshot=false), so it persists but never propagates.
        doc["sync_follow"] = (rawValue.toInt() != 0);
        break;
    case TimerHaEntity::SyncTargets:
        // The dynamic select (issue #112) hands us the RESOLVED sync_targets value,
        // not an index: "" (Off), "all" (All), or a discovered peer id. MQTTManager
        // maps the chosen option index through the CURRENT id list before calling, so
        // the option set tracks the registry. The bespoke sync_targets validator
        // (parseSyncTargets) rejects a malformed id (atomic-reject parity). Local
        // identity (inSnapshot=false): persists but never propagates.
        doc["sync_targets"] = rawValue;
        break;
    default:
        return TimerCmdResult::BadField;   // not a control entity
    }

    String json;
    serializeJson(doc, json);
    return parseCommand(json.c_str());
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

// A carrier's read-only JSON attribute object (PRD #57 / issue #58): the bag is
// built table-driven by timerBuildAttributeGroup, serialized, and ridden onto the
// carrier's json_attr_t topic by the wire seam — the same path every other Timer
// value takes. HA reads it because createTimerHAEntities opted the carrier select
// into json attributes (setJsonAttributes), so the discovery config advertises
// this topic. Retained means HA repopulates after a restart for free. A carrier
// with no mapped rows yields an empty bag and publishes nothing.
void TimerManager_::publishAttributeGroup(TimerHaEntity carrier)
{
    // 512: the state sensor's bag is the largest (eight config-view keys incl.
    // two strings), which overflows 256 on a 64-bit host (issue #59).
    DynamicJsonDocument doc(512);
    {
        // A republish (e.g. on reconnect) during a one-shot override serializes the
        // SAVED config, so HA attribute bags never show transient one-off values
        // (issue #101 / ADR-0014).
        SavedConfigScope saved(*this);
        timerBuildAttributeGroup(carrier, doc);
    }
    if (doc.as<JsonObjectConst>().size() == 0) return;
    String payload;
    serializeJson(doc, payload);
    MQTTManager.publishTimerWire(MQTTManager.timerWireAttrTopic(carrier).c_str(), payload.c_str());
}

// Every distinct carrier's attribute object, each published once. Carriers are
// deduped by first appearance in the attribute-group table (a carrier owns
// several rows), mirroring publishAllWire's hook dedupe.
void TimerManager_::publishAllAttributeGroups()
{
    for (size_t i = 0; i < TIMER_ATTR_GROUP_DESC_COUNT; ++i)
    {
        TimerHaEntity carrier = TIMER_ATTR_GROUP_DESCS[i].carrier;
        bool seen = false;
        for (size_t j = 0; j < i && !seen; ++j)
            seen = (TIMER_ATTR_GROUP_DESCS[j].carrier == carrier);
        if (!seen) publishAttributeGroup(carrier);
    }
}

// Teardown mirror of publishAllAttributeGroups: empty the retained json_attr_t
// topic of every distinct carrier so disabling the Timer (discovery teardown,
// issue #60) leaves no orphaned attribute object on the broker. Same carrier
// dedupe and same wire seam — an empty retained payload is the MQTT clear.
// publishTimerWire's creation-sentinel gate makes this no-op when no entity ever
// existed (nothing was advertised, so nothing to clear).
void TimerManager_::clearAllAttributeGroups()
{
    for (size_t i = 0; i < TIMER_ATTR_GROUP_DESC_COUNT; ++i)
    {
        TimerHaEntity carrier = TIMER_ATTR_GROUP_DESCS[i].carrier;
        bool seen = false;
        for (size_t j = 0; j < i && !seen; ++j)
            seen = (TIMER_ATTR_GROUP_DESCS[j].carrier == carrier);
        if (!seen)
            MQTTManager.publishTimerWire(MQTTManager.timerWireAttrTopic(carrier).c_str(), "");
    }
}

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
    // Thin forwarder: this class owns the monotonic _syncSeq counter (injected as
    // seq); the envelope shape + target-CSV parsing live in SyncEnvelope (ADR-0023).
    SyncEnvelope::build(sync, uniqueID, ++_syncSeq, TIMER_SYNC_TARGETS);
}

void TimerManager_::buildConfigSnapshot(JsonDocument &doc) const
{
    // Config block only — never action/duration (run-state) or sync_* (local identity,
    // inSnapshot=false). Two tables, one config block: TIMER_SETTINGS_DESCS' inSnapshot
    // rows and TIMER_MEMBER_CONFIG_DESCS (the member-backed half, B1, ADR-0007/0009).
    // Each table also feeds the parseCommand broadcast trigger, so the snapshot can't
    // drift from what fires a broadcast.
    // Under a one-shot override the propagated snapshot reports the SAVED config, so a
    // follower never receives transient one-off values it has no notion of reverting
    // (issue #101 / ADR-0006). broadcastConfig is itself suppressed during an override
    // (issue #100), so this is also a defensive guarantee for any other caller.
    SavedConfigScope saved(*this);
    timerSettingsBuildSnapshot(doc);
    timerMemberConfigBuildSnapshot(doc);
}

void TimerManager_::broadcastRunState(const char *action)
{
    if (_remoteApply) return;                       // one-hop: never re-emit an applied remote command
    if (TIMER_SYNC_TARGETS.length() == 0) return;   // sync off

    // Only an action ever reaches here — start / pause / reset (#126). A `start` is the
    // SOLE duration-bearing packet: it carries the leader's effective `duration` plus the
    // leader's EFFECTIVE config snapshot, bundled into one combined packet — the
    // run-scoped config mirror (ADR-0018). Config no longer travels on a config edit, and
    // a bare duration edit propagates nothing; both ride one combined packet with the
    // start so a follower mirrors the leader for that run. The combined packet needs the
    // full kTimerCmdJsonSize buffer (config snapshot + envelope); pause/reset stay
    // run-state-only and fit a small static buffer.
    bool isStart = (strcasecmp(action, "start") == 0);

    if (isStart)
    {
        DynamicJsonDocument doc(kTimerCmdJsonSize);
        JsonObject sync = doc.createNestedObject("_sync");
        addSyncEnvelope(sync);
        doc["action"] = action;
        doc["duration"] = durationSec;   // duration rides only with a start (defines the countdown)
        // EFFECTIVE config (no SavedConfigScope): a leader's own one-shot run mirrors
        // to followers, so the snapshot reports what is actually running. sync_* are
        // inSnapshot=false and excluded by construction; inline melodies never travel
        // (the saved bare name globals back the snapshot, ADR-0017).
        timerSettingsBuildSnapshot(doc);
        timerMemberConfigBuildSnapshot(doc);

        String out; serializeJson(doc, out);
        ServerManager.sendTimerSync(out);
        return;
    }

    // pause / reset: run-state only, no duration, no config.
    StaticJsonDocument<256> doc;
    JsonObject sync = doc.createNestedObject("_sync");
    addSyncEnvelope(sync);
    doc["action"] = action;

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

void TimerManager_::applySyncCommand(const char *json)
{
    if (json == nullptr || json[0] == '\0') return;

    DynamicJsonDocument doc(kTimerCmdJsonSize);
    if (deserializeJson(doc, json)) return;

    // The echo/presence/follow/target decision is a PURE function (SyncEnvelope,
    // ADR-0023): this shell only deserializes, then acts on the Decision. Dedup is
    // deliberately NOT in classify — SyncSeenCache::seen() is stateful (test-and-
    // record), so it stays here as the single guard before re-entry.
    SyncEnvelope::Decision d =
        SyncEnvelope::classify(doc.as<JsonVariantConst>(), {uniqueID, TIMER_SYNC_FOLLOW});

    switch (d.kind)
    {
    case SyncEnvelope::Decision::HarvestPresence:
        // Presence harvest (#111 / ADR-0019): record the sender ungated, apply no
        // timer state. classify already bypassed the follow/target gate.
        _registry.record(d.src, millis());
        break;

    case SyncEnvelope::Decision::Apply:
        if (_seen.seen(d.src, d.seq, millis())) return;   // redundant copy of a burst
        // Re-enter the local control surface. The send-path _remoteApply guard
        // prevents re-broadcasting (one-hop); parseCommand ignores the _sync envelope.
        _remoteApply = true;
        parseCommand(json);
        _remoteApply = false;
        break;

    case SyncEnvelope::Decision::Ignore:
        break;
    }
}

// ---------------------------------------------------------------------------
// Peer presence beacon (#111 / ADR-0019). The peer SET lives in PeerRegistry
// (extracted per ADR-0021); this class keeps only the beacon cadence + UDP send.
// ---------------------------------------------------------------------------

void TimerManager_::broadcastPresence()
{
    // A small unconditional beacon: {_sync:{src,seq}, presence:true}. No tgt — it is
    // informational, harvested ungated by every receiver. Independent of sync targets
    // so even a clock that commands nobody is still discoverable. 256 matches the
    // pause/reset run-state beacon buffer (the src/seq envelope routes through
    // SyncEnvelope::build, which wants a touch more pool headroom on the 64-bit host).
    StaticJsonDocument<256> doc;
    JsonObject sync = doc.createNestedObject("_sync");
    SyncEnvelope::build(sync, uniqueID, ++_syncSeq);   // bare {src,seq}: no tgt (informational)
    doc["presence"] = true;

    String out; serializeJson(doc, out);
    ServerManager.sendTimerSync(out);   // also AP-gated at the transport (defense in depth)
}

void TimerManager_::tickPresence(unsigned long nowMs)
{
    _registry.prune(nowMs);

    if (AP_MODE) return;   // no beacon in AP mode (a standalone clock with no real LAN)

    if (!_presenceEverSent || (nowMs - _lastPresenceMs) >= kPresenceIntervalMs)
    {
        broadcastPresence();
        _lastPresenceMs   = nowMs;
        _presenceEverSent = true;
    }
}
