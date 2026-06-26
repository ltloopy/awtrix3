#include "TimerSettings.h"

#include <stdlib.h>

#include "Globals.h"
#include "TimerManager.h"   // TimerManager_::isValidIconName (shared Name char-rule)
#include "MQTTManager.h"    // the (topic, payload) wire seam the publish hooks emit on
#include "TimerHa.h"        // TimerHaEntity slots for the hooks' canonical topics
#include <Preferences.h>

namespace
{
    // sync_targets accepts "" (off), "all", or a comma list of device-id tokens
    // ([A-Za-z0-9_-], 1..32 each). Moved here from TimerManager.cpp so the table is
    // the single home of the sync_targets validation. Same rule as before.
    bool isValidSyncTargets(const String &s)
    {
        String t = s; t.trim();
        if (t.length() == 0 || t == "all") return true;
        int start = 0;
        const int n = t.length();
        while (start <= n)
        {
            int comma = t.indexOf(',', start);
            if (comma < 0) comma = n;
            String tok = t.substring(start, comma); tok.trim();
            if (tok.length() == 0 || tok.length() > 32) return false;
            for (size_t i = 0; i < tok.length(); ++i)
            {
                char c = tok[i];
                bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                          (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
                if (!ok) return false;
            }
            if (comma == n) break;
            start = comma + 1;
        }
        return true;
    }

    // bar_color: a JSON number 0..0xFFFFFF, or an "#RRGGBB" / "RRGGBB" hex string.
    bool parseBarColor(JsonVariantConst v, TcValue &out)
    {
        if (v.is<long>() || v.is<float>())
        {
            uint32_t n = v.as<uint32_t>();
            if (n > 0xFFFFFFu) return false;
            out.num = n;
            return true;
        }
        if (v.is<const char *>() || v.is<String>())
        {
            String s = v.as<String>();
            s.trim();
            if (s.length() > 0 && s[0] == '#') s = s.substring(1);
            if (s.length() != 6) return false;   // exactly RRGGBB
            for (size_t i = 0; i < s.length(); ++i)
            {
                char c = s[i];
                bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                if (!ok) return false;
            }
            out.num = (uint32_t)strtoul(s.c_str(), nullptr, 16);
            return true;
        }
        return false;
    }

    // bar_color HA-attribute formatter (PRD #57 / issue #59): renders the stored
    // 0xRRGGBB int as the human string HA shows -- "default" when 0 (follow the
    // text color, ADR-0004), else uppercase "#RRGGBB" (matching DisplayManager's
    // "#%02X%02X%02X" spelling). Deliberately different from the raw-int form the
    // config snapshot emits via timerSettingEmitValue, which is why it needs a hook.
    void formatBarColor(const TimerSettingDesc &d, JsonDocument &doc)
    {
        uint32_t v = *static_cast<uint32_t *>(d.storage);
        if (v == 0)
        {
            doc[d.cmdKey] = "default";
            return;
        }
        char buf[8];
        snprintf(buf, sizeof(buf), "#%06X", (unsigned)(v & 0xFFFFFFu));
        doc[d.cmdKey] = buf;
    }

    // bar_bg_color HA-attribute formatter (ADR-0020): the background track's color.
    // Mirrors formatBarColor's structure but with the background's literal-off
    // semantics -- 0 means BLACK = no track (LEDs off), NOT the foreground's
    // "follow text color" sentinel -- so 0 renders as "none" (deliberately distinct
    // from bar_color's "default"); any other value renders uppercase "#RRGGBB".
    void formatBarBgColor(const TimerSettingDesc &d, JsonDocument &doc)
    {
        uint32_t v = *static_cast<uint32_t *>(d.storage);
        if (v == 0)
        {
            doc[d.cmdKey] = "none";
            return;
        }
        char buf[8];
        snprintf(buf, sizeof(buf), "#%06X", (unsigned)(v & 0xFFFFFFu));
        doc[d.cmdKey] = buf;
    }

    // max_duration carrier-native HA-attribute formatter (PRD #66 / issue #68):
    // renders the stored cap (raw seconds, a U32) as the trimmed H:MM:SS clock
    // string the Duration text entity's OWN state speaks, by reusing the exact
    // formatHMS the Duration state uses ("24:00:00", "1:00:00", "0:45"). This is
    // the first key whose attribute representation differs PER CARRIER: the state
    // sensor keeps the raw-seconds number (no formatter), while the Duration
    // carrier renders this clock string — each carrier in its native form, over
    // the same persisted storage, so the underlying value cannot drift.
    void formatMaxDurationHMS(const TimerSettingDesc &d, JsonDocument &doc)
    {
        uint32_t v = *static_cast<uint32_t *>(d.storage);
        doc[d.cmdKey] = TimerManager_::formatHMS(v);
    }

    // sync_targets: strict string type, then the comma-list rule above.
    bool parseSyncTargets(JsonVariantConst v, TcValue &out)
    {
        if (!(v.is<const char *>() || v.is<String>())) return false;
        String s = v.as<String>();
        if (!isValidSyncTargets(s)) return false;
        out.str = s;
        return true;
    }
}

// One row per persisted value-config Timer key. The two inSnapshot=false rows are
// the sync roles (local identity), excluded from the propagated config block.
//   cmdKey, devKey, nvsKey, type, check, lo, hi, dfltNum, dfltStr, bespoke, inSnapshot, storage
const TimerSettingDesc TIMER_SETTINGS_DESCS[] = {
    {"finished_hold",              "timer_finished_hold",              "TFHOLD",  TcType::U16,  TcCheck::UIntRange, 1,   300,    10,    nullptr,       nullptr,         true,  &TIMER_FINISHED_HOLD},
    {"realert_interval",           "timer_realert_interval",           "TRALERT", TcType::U16,  TcCheck::UIntRange, 5,   300,    15,    nullptr,       nullptr,         true,  &TIMER_REALERT_INTERVAL},
    {"countdown_seconds",          "timer_countdown_seconds",          "TCDOWN",  TcType::U16,  TcCheck::UIntRange, 0,   30,     3,     nullptr,       nullptr,         true,  &TIMER_COUNTDOWN_SECONDS},
    {"max_duration",               "timer_max_duration",               "TMAXD",   TcType::U32,  TcCheck::UIntRange, 1,   604800, 86400, nullptr,       nullptr,         true,  &TIMER_MAX_DURATION},
    {"remaining_publish_interval", "timer_remaining_publish_interval", "TPUBI",   TcType::U16,  TcCheck::UIntRange, 1,   60,     1,     nullptr,       nullptr,         true,  &TIMER_PUBLISH_INTERVAL},
    {"icon_enabled",               "timer_icon_enabled",               "TICONEN", TcType::Bool, TcCheck::Bool,     0,   0,      1,     nullptr,       nullptr,         true,  &TIMER_ICON_ENABLED},
    {"bar_enabled",                "timer_bar_enabled",                "TBAREN",  TcType::Bool, TcCheck::Bool,     0,   0,      1,     nullptr,       nullptr,         true,  &TIMER_BAR_ENABLED},
    {"bar_color",                  "timer_bar_color",                  "TBARC",   TcType::U32,  TcCheck::Bespoke,  0,   0,      0,     nullptr,       parseBarColor,   true,  &TIMER_BAR_COLOR},
    {"bar_bg_color",               "timer_bar_bg_color",               "TBARBC",  TcType::U32,  TcCheck::Bespoke,  0,   0,      0,     nullptr,       parseBarColor,   true,  &TIMER_BAR_BG_COLOR},
    {"melody_tick",                "timer_melody_tick",                "TMTICK",  TcType::Str,  TcCheck::Name,     0,   0,      0,     "timer_tick",  nullptr,         true,  &TIMER_MELODY_TICK},
    {"melody_end",                 "timer_melody_end",                 "TMEND",   TcType::Str,  TcCheck::Name,     0,   0,      0,     "timer_end",   nullptr,         true,  &TIMER_MELODY_END},
    {"sync_follow",                "timer_sync_follow",                "TSYNF",   TcType::Bool, TcCheck::Bool,     0,   0,      0,     nullptr,       nullptr,         false, &TIMER_SYNC_FOLLOW},
    {"sync_targets",               "timer_sync_targets",               "TSYNT",   TcType::Str,  TcCheck::Bespoke,  0,   0,      0,     "",            parseSyncTargets, false, &TIMER_SYNC_TARGETS},
};

const size_t TIMER_SETTINGS_DESC_COUNT = sizeof(TIMER_SETTINGS_DESCS) / sizeof(TIMER_SETTINGS_DESCS[0]);

// The header's compile-time extent (used to size the one-shot snapshot buffer) must
// match the actual table. If a row is added, bump TIMER_SETTINGS_DESC_CAP.
static_assert(sizeof(TIMER_SETTINGS_DESCS) / sizeof(TIMER_SETTINGS_DESCS[0]) == TIMER_SETTINGS_DESC_CAP,
              "TIMER_SETTINGS_DESC_CAP must equal the descriptor row count");

bool timerSettingParse(const TimerSettingDesc &d, JsonVariantConst v, TcValue &out)
{
    if (d.bespoke) return d.bespoke(v, out);

    switch (d.check)
    {
        case TcCheck::Bool:
            if (!v.is<bool>()) return false;
            out.b = v.as<bool>();
            return true;

        case TcCheck::UIntRange:
        {
            if (!(v.is<long>() || v.is<float>())) return false;   // number only (not bool/string/null)
            uint32_t n = v.as<uint32_t>();
            if (n < d.lo || n > d.hi) return false;
            out.num = n;
            return true;
        }

        case TcCheck::Name:
        {
            // Bare filename; same char-rule as icons. Empty resets to the default at
            // coerce time (matches legacy melody semantics). Mirrors the legacy path,
            // which coerced via as<String>() without a strict pre-type-check.
            String s = v.as<String>();
            if (!TimerManager_::isValidIconName(s)) return false;
            out.str = (s.length() == 0) ? String(d.dfltStr) : s;
            return true;
        }

        case TcCheck::Bespoke:
            return false;   // unreachable: bespoke rows carry d.bespoke, handled above
    }
    return false;
}

bool timerSettingStore(const TimerSettingDesc &d, const TcValue &v)
{
    switch (d.type)
    {
        case TcType::U16:
        {
            uint16_t *p = static_cast<uint16_t *>(d.storage);
            if (*p == (uint16_t)v.num) return false;
            *p = (uint16_t)v.num;
            return true;
        }
        case TcType::U32:
        {
            uint32_t *p = static_cast<uint32_t *>(d.storage);
            if (*p == v.num) return false;
            *p = v.num;
            return true;
        }
        case TcType::Bool:
        {
            bool *p = static_cast<bool *>(d.storage);
            if (*p == v.b) return false;
            *p = v.b;
            return true;
        }
        case TcType::Str:
        {
            String *p = static_cast<String *>(d.storage);
            if (*p == v.str) return false;
            *p = v.str;
            return true;
        }
    }
    return false;
}

void timerSettingsLoadNvs(Preferences &prefs)
{
    for (size_t i = 0; i < TIMER_SETTINGS_DESC_COUNT; ++i)
    {
        const TimerSettingDesc &d = TIMER_SETTINGS_DESCS[i];
        switch (d.type)
        {
            case TcType::U16:  *static_cast<uint16_t *>(d.storage) = (uint16_t)prefs.getUInt(d.nvsKey, d.dfltNum); break;
            case TcType::U32:  *static_cast<uint32_t *>(d.storage) = prefs.getUInt(d.nvsKey, d.dfltNum);           break;
            case TcType::Bool: *static_cast<bool *>    (d.storage) = prefs.getBool(d.nvsKey, d.dfltNum != 0);      break;
            case TcType::Str:  *static_cast<String *>  (d.storage) = prefs.getString(d.nvsKey, d.dfltStr);         break;
        }
    }
}

void timerSettingsSaveNvs(Preferences &prefs)
{
    for (size_t i = 0; i < TIMER_SETTINGS_DESC_COUNT; ++i)
    {
        const TimerSettingDesc &d = TIMER_SETTINGS_DESCS[i];
        switch (d.type)
        {
            case TcType::U16:  prefs.putUInt(d.nvsKey, *static_cast<uint16_t *>(d.storage)); break;
            case TcType::U32:  prefs.putUInt(d.nvsKey, *static_cast<uint32_t *>(d.storage)); break;
            case TcType::Bool: prefs.putBool(d.nvsKey, *static_cast<bool *>    (d.storage)); break;
            case TcType::Str:  prefs.putString(d.nvsKey, *static_cast<String *>(d.storage)); break;
        }
    }
}

void timerSettingsLoadDevJson(JsonObjectConst obj)
{
    for (size_t i = 0; i < TIMER_SETTINGS_DESC_COUNT; ++i)
    {
        const TimerSettingDesc &d = TIMER_SETTINGS_DESCS[i];
        if (!obj.containsKey(d.devKey)) continue;
        TcValue val;
        if (timerSettingParse(d, obj[d.devKey], val))   // per-key best-effort: skip if invalid
            timerSettingStore(d, val);
    }
}

void timerSettingEmitValue(const TimerSettingDesc &d, JsonDocument &doc)
{
    switch (d.type)
    {
        case TcType::U16:  doc[d.cmdKey] = *static_cast<uint16_t *>(d.storage); break;
        case TcType::U32:  doc[d.cmdKey] = *static_cast<uint32_t *>(d.storage); break;
        case TcType::Bool: doc[d.cmdKey] = *static_cast<bool *>    (d.storage); break;
        case TcType::Str:  doc[d.cmdKey] = *static_cast<String *>  (d.storage); break;
    }
}

void timerSettingsBuildSnapshot(JsonDocument &doc)
{
    for (size_t i = 0; i < TIMER_SETTINGS_DESC_COUNT; ++i)
    {
        const TimerSettingDesc &d = TIMER_SETTINGS_DESCS[i];
        if (!d.inSnapshot) continue;
        timerSettingEmitValue(d, doc);
    }
}

void timerSettingsCaptureSnapshot(TcValue out[])
{
    for (size_t i = 0; i < TIMER_SETTINGS_DESC_COUNT; ++i)
    {
        const TimerSettingDesc &d = TIMER_SETTINGS_DESCS[i];
        if (!d.inSnapshot) continue;   // sync_* (local identity) excluded by construction
        switch (d.type)
        {
            case TcType::U16:  out[i].num = *static_cast<uint16_t *>(d.storage); break;
            case TcType::U32:  out[i].num = *static_cast<uint32_t *>(d.storage); break;
            case TcType::Bool: out[i].b   = *static_cast<bool *>    (d.storage); break;
            case TcType::Str:  out[i].str = *static_cast<String *>  (d.storage); break;
        }
    }
}

void timerSettingsRestoreSnapshot(const TcValue in[])
{
    for (size_t i = 0; i < TIMER_SETTINGS_DESC_COUNT; ++i)
    {
        const TimerSettingDesc &d = TIMER_SETTINGS_DESCS[i];
        if (!d.inSnapshot) continue;
        timerSettingStore(d, in[i]);   // dispatch-by-type write; equality-skip return ignored
    }
}

const TimerSettingDesc *timerSettingByCmdKey(const char *cmdKey)
{
    for (size_t i = 0; i < TIMER_SETTINGS_DESC_COUNT; ++i)
        if (strcmp(TIMER_SETTINGS_DESCS[i].cmdKey, cmdKey) == 0)
            return &TIMER_SETTINGS_DESCS[i];
    return nullptr;
}

// Inline RTTTL classifier/validator (issue #102). Pure, no globals touched.
namespace
{
    // RTTTL tunes for a single timer alarm/tick are short; cap so a pathological
    // payload can't bloat the one-shot run state.
    constexpr size_t kInlineRtttlMaxLen = 256;

    // True iff the comma-separated control section carries at least one RTTTL default
    // token -- a token whose key is exactly d/o/b (duration/octave/beat). Checks the
    // token PREFIX (not a substring), so "foo=4" is not mistaken for an "o=" default.
    bool controlHasDefaultToken(const String &control)
    {
        int start = 0;
        const int n = control.length();
        while (start <= n)
        {
            int comma = control.indexOf(',', start);
            if (comma < 0) comma = n;
            String tok = control.substring(start, comma);
            tok.trim();
            tok.toLowerCase();
            if (tok.startsWith("d=") || tok.startsWith("o=") || tok.startsWith("b=")) return true;
            if (comma == n) break;
            start = comma + 1;
        }
        return false;
    }
}

bool timerMelodyIsInline(const String &s)
{
    // A bare melody file-name token is [A-Za-z0-9_-]* and never contains a colon;
    // an inline RTTTL tune always carries ':' separators. Content is the only signal.
    return s.indexOf(':') >= 0;
}

bool timerMelodyValidateInline(const String &s)
{
    if (s.length() == 0 || s.length() > kInlineRtttlMaxLen) return false;

    // RTTTL is name:control:notes -- exactly two colons (name may be empty).
    int c1 = s.indexOf(':');
    if (c1 < 0) return false;
    int c2 = s.indexOf(':', c1 + 1);
    if (c2 < 0) return false;
    if (s.indexOf(':', c2 + 1) >= 0) return false;   // a third colon is malformed

    String control = s.substring(c1 + 1, c2);
    String notes   = s.substring(c2 + 1);
    if (control.length() == 0 || notes.length() == 0) return false;

    // The control section must carry at least one RTTTL default token (d=/o=/b=).
    if (!controlHasDefaultToken(control)) return false;

    return true;
}

// ---------------------------------------------------------------------------
// HA attribute-group projection (PRD #57 / issues #58, #59). The buzzer + finished
// HASelects were lit up first (#58); #59 extended the JSON-attributes opt-in to
// HASensor, lighting up the remaining + state sensors. realert_interval is listed
// first on the finished carrier so the folded payload is a superset of the legacy
// bespoke {"realert_interval":N}. remaining_publish_interval rides BOTH the
// remaining sensor (its own cadence) and the state sensor (a complete config view)
// -- one settings row, two carrier rows. bar_color carries the per-row formatter
// so it renders as "default"/"#RRGGBB" rather than its raw-int snapshot form.
// max_duration (PRD #66 / issue #68) is the first MULTI-CARRIER key whose
// representation differs PER CARRIER: it rides the Duration text entity in
// carrier-native clock form (formatMaxDurationHMS -> "24:00:00") AND the state
// sensor in raw seconds (no formatter -> 86400) -- one settings row, two carrier
// rows, two representations over the same persisted storage.
//   carrier, cmdKey, format
const TimerAttrGroupDesc TIMER_ATTR_GROUP_DESCS[] = {
    {TimerHaEntity::Duration,  "max_duration",               formatMaxDurationHMS},
    {TimerHaEntity::Finished,  "realert_interval",           nullptr},
    {TimerHaEntity::Finished,  "finished_hold",              nullptr},
    {TimerHaEntity::Buzzer,    "countdown_seconds",          nullptr},
    {TimerHaEntity::Buzzer,    "melody_tick",                nullptr},
    {TimerHaEntity::Buzzer,    "melody_end",                 nullptr},
    {TimerHaEntity::Remaining, "remaining_publish_interval", nullptr},
    {TimerHaEntity::State,     "max_duration",               nullptr},
    {TimerHaEntity::State,     "remaining_publish_interval", nullptr},
    {TimerHaEntity::State,     "icon_enabled",               nullptr},
    {TimerHaEntity::State,     "bar_enabled",                nullptr},
    {TimerHaEntity::State,     "bar_color",                  formatBarColor},
    {TimerHaEntity::State,     "bar_bg_color",               formatBarBgColor},
    {TimerHaEntity::State,     "sync_follow",                nullptr},
    {TimerHaEntity::State,     "sync_targets",               nullptr},
};

const size_t TIMER_ATTR_GROUP_DESC_COUNT =
    sizeof(TIMER_ATTR_GROUP_DESCS) / sizeof(TIMER_ATTR_GROUP_DESCS[0]);

void timerBuildAttributeGroup(TimerHaEntity carrier, JsonDocument &doc)
{
    for (size_t i = 0; i < TIMER_ATTR_GROUP_DESC_COUNT; ++i)
    {
        const TimerAttrGroupDesc &g = TIMER_ATTR_GROUP_DESCS[i];
        if (g.carrier != carrier) continue;
        const TimerSettingDesc *d = timerSettingByCmdKey(g.cmdKey);
        if (!d) continue;   // table self-consistency is pinned by test T14
        if (g.format) g.format(*d, doc);
        else          timerSettingEmitValue(*d, doc);
    }
}

// ===========================================================================
// Member-backed config half (B1) -- the second table of the config block.
// Each hook routes through TimerManager's existing public setters/getters; the
// enum rows stage the enum cast in TcValue::num, the icon rows stage the name in
// TcValue::str (validate is pure -- no global is touched until apply).
// ===========================================================================
namespace
{
    // -- buzzer --
    bool memValidateBuzzer(JsonVariantConst v, TcValue &out)
    {
        BuzzerMode m;
        if (!TimerManager_::parseBuzzerMode(v.as<String>(), m)) return false;
        out.num = (uint32_t)m;
        return true;
    }
    void memApplyBuzzer(const TcValue &v) { TimerManager.setBuzzerMode((BuzzerMode)v.num); }
    void memEmitBuzzer (JsonDocument &doc) { doc["buzzer"] = TimerManager.buzzerModeString(); }
    // Publish hook (issue #33): the live value onto the wire seam, payload the
    // per-enum codec's canonical `wire` string -- never the numeric index, and
    // deliberately not the HASelect `ha` label the retired setState path sent.
    void memPublishBuzzer()
    {
        MQTTManager.publishTimerWire(MQTTManager.timerWireTopic(TimerHaEntity::Buzzer).c_str(),
                                     TimerManager.buzzerModeString());
    }

    // -- finished --
    bool memValidateFinished(JsonVariantConst v, TcValue &out)
    {
        FinishedMode m;
        if (!TimerManager_::parseFinishedMode(v.as<String>(), m)) return false;
        out.num = (uint32_t)m;
        return true;
    }
    void memApplyFinished(const TcValue &v) { TimerManager.setFinishedMode((FinishedMode)v.num); }
    void memEmitFinished (JsonDocument &doc) { doc["finished"] = TimerManager.finishedModeString(); }
    // Publish hook: see memPublishBuzzer.
    void memPublishFinished()
    {
        MQTTManager.publishTimerWire(MQTTManager.timerWireTopic(TimerHaEntity::Finished).c_str(),
                                     TimerManager.finishedModeString());
    }

    // -- icon_<state> (shared validate; per-state apply/emit) --
    bool memValidateIcon(JsonVariantConst v, TcValue &out)
    {
        String s = v.as<String>();
        if (!TimerManager_::isValidIconName(s)) return false;
        out.str = s;
        return true;
    }
    void memApplyIconIdle    (const TcValue &v) { TimerManager.setIconIdle    (v.str); }
    void memApplyIconRunning (const TcValue &v) { TimerManager.setIconRunning (v.str); }
    void memApplyIconPaused  (const TcValue &v) { TimerManager.setIconPaused  (v.str); }
    void memApplyIconFinished(const TcValue &v) { TimerManager.setIconFinished(v.str); }
    void memEmitIconIdle    (JsonDocument &doc) { doc["icon_idle"]     = TimerManager.getIconIdle(); }
    void memEmitIconRunning (JsonDocument &doc) { doc["icon_running"]  = TimerManager.getIconRunning(); }
    void memEmitIconPaused  (JsonDocument &doc) { doc["icon_paused"]   = TimerManager.getIconPaused(); }
    void memEmitIconFinished(JsonDocument &doc) { doc["icon_finished"] = TimerManager.getIconFinished(); }
    // Publish hook, shared by all four icon rows (issue #34): the icon keys have
    // ONE wire artifact — the aggregate four-state JSON on the plain
    // {MQTT_PREFIX}/timer/icons topic (not an HA entity data topic), payload
    // byte-identical to the retired MQTTManager::publishTimerIcons composer.
    void memPublishIcons()
    {
        DynamicJsonDocument doc(256);
        doc["idle"]     = TimerManager.getIconIdle();
        doc["running"]  = TimerManager.getIconRunning();
        doc["paused"]   = TimerManager.getIconPaused();
        doc["finished"] = TimerManager.getIconFinished();
        String payload;
        serializeJson(doc, payload);
        MQTTManager.publishTimerWire(MQTTManager.timerIconsTopic().c_str(), payload.c_str());
    }
}

const TimerMemberConfigDesc TIMER_MEMBER_CONFIG_DESCS[] = {
    {"buzzer",        memValidateBuzzer,   memApplyBuzzer,        memEmitBuzzer,        memPublishBuzzer},
    {"finished",      memValidateFinished, memApplyFinished,      memEmitFinished,      memPublishFinished},
    {"icon_idle",     memValidateIcon,     memApplyIconIdle,      memEmitIconIdle,      memPublishIcons},
    {"icon_running",  memValidateIcon,     memApplyIconRunning,   memEmitIconRunning,   memPublishIcons},
    {"icon_paused",   memValidateIcon,     memApplyIconPaused,    memEmitIconPaused,    memPublishIcons},
    {"icon_finished", memValidateIcon,     memApplyIconFinished,  memEmitIconFinished,  memPublishIcons},
};

const size_t TIMER_MEMBER_CONFIG_DESC_COUNT =
    sizeof(TIMER_MEMBER_CONFIG_DESCS) / sizeof(TIMER_MEMBER_CONFIG_DESCS[0]);

void timerMemberConfigBuildSnapshot(JsonDocument &doc)
{
    for (size_t i = 0; i < TIMER_MEMBER_CONFIG_DESC_COUNT; ++i)
        TIMER_MEMBER_CONFIG_DESCS[i].emit(doc);
}

bool timerDocTouchesMemberConfig(const JsonDocument &doc)
{
    for (size_t i = 0; i < TIMER_MEMBER_CONFIG_DESC_COUNT; ++i)
        if (doc.containsKey(TIMER_MEMBER_CONFIG_DESCS[i].cmdKey)) return true;
    return false;
}

void timerMemberConfigPublish(const char *cmdKey)
{
    for (size_t i = 0; i < TIMER_MEMBER_CONFIG_DESC_COUNT; ++i)
    {
        const TimerMemberConfigDesc &d = TIMER_MEMBER_CONFIG_DESCS[i];
        if (strcmp(d.cmdKey, cmdKey) != 0) continue;
        if (d.publish) d.publish();
        return;
    }
}

// ===========================================================================
// HTTP GET /api/timer config mirror (PRD #73). The complete persisted-config
// projection: both tables, no snapshot filter, raw values. Lives here beside the
// descriptor tables (and, from #75, the file-local carrier-native formatters it
// will reuse) rather than in the manager. See the header for the full contract.
// ===========================================================================
void timerBuildFullConfig(JsonDocument &doc)
{
    // Table half: EVERY settings row, ignoring inSnapshot, so the sync-role keys
    // (sync_follow/sync_targets) are part of the read mirror even though they are
    // never propagated. Raw value per row via the shared single-row emitter, with
    // the two deliberate carrier-native overrides (PRD #73 D2) reusing the
    // file-local formatters beside them -- diverging from the raw propagation
    // snapshot by design, yet never able to disagree in value (same storage).
    for (size_t i = 0; i < TIMER_SETTINGS_DESC_COUNT; ++i)
    {
        const TimerSettingDesc &d = TIMER_SETTINGS_DESCS[i];
        if (strcmp(d.cmdKey, "bar_color") == 0)
        {
            formatBarColor(d, doc);          // "default" / uppercase "#RRGGBB", not the raw int
        }
        else if (strcmp(d.cmdKey, "bar_bg_color") == 0)
        {
            formatBarBgColor(d, doc);        // "none" / uppercase "#RRGGBB", not the raw int
        }
        else if (strcmp(d.cmdKey, "max_duration") == 0)
        {
            timerSettingEmitValue(d, doc);   // raw seconds kept (e.g. 86400)
            // ...plus a trimmed clock-string sibling, this endpoint's raw+_str
            // duration precedent, reusing the exact formatHMS the duration fields use.
            doc["max_duration_str"] = TimerManager_::formatHMS(*static_cast<uint32_t *>(d.storage));
        }
        else
        {
            timerSettingEmitValue(d, doc);
        }
    }

    // Member-backed half: buzzer/finished + the four icon_* live values.
    timerMemberConfigBuildSnapshot(doc);
}
