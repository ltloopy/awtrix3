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
    {"app_config_timeout",         "timer_app_config_timeout",         "TCFGT",   TcType::U16,  TcCheck::UIntRange, 5,   300,    30,    nullptr,       nullptr,         true,  &TIMER_CONFIG_TIMEOUT},
    {"icon_enabled",               "timer_icon_enabled",               "TICONEN", TcType::Bool, TcCheck::Bool,     0,   0,      1,     nullptr,       nullptr,         true,  &TIMER_ICON_ENABLED},
    {"bar_enabled",                "timer_bar_enabled",                "TBAREN",  TcType::Bool, TcCheck::Bool,     0,   0,      1,     nullptr,       nullptr,         true,  &TIMER_BAR_ENABLED},
    {"bar_color",                  "timer_bar_color",                  "TBARC",   TcType::U32,  TcCheck::Bespoke,  0,   0,      0,     nullptr,       parseBarColor,   true,  &TIMER_BAR_COLOR},
    {"melody_tick",                "timer_melody_tick",                "TMTICK",  TcType::Str,  TcCheck::Name,     0,   0,      0,     "timer_tick",  nullptr,         true,  &TIMER_MELODY_TICK},
    {"melody_end",                 "timer_melody_end",                 "TMEND",   TcType::Str,  TcCheck::Name,     0,   0,      0,     "timer_end",   nullptr,         true,  &TIMER_MELODY_END},
    {"sync_follow",                "timer_sync_follow",                "TSYNF",   TcType::Bool, TcCheck::Bool,     0,   0,      0,     nullptr,       nullptr,         false, &TIMER_SYNC_FOLLOW},
    {"sync_targets",               "timer_sync_targets",               "TSYNT",   TcType::Str,  TcCheck::Bespoke,  0,   0,      0,     "",            parseSyncTargets, false, &TIMER_SYNC_TARGETS},
};

const size_t TIMER_SETTINGS_DESC_COUNT = sizeof(TIMER_SETTINGS_DESCS) / sizeof(TIMER_SETTINGS_DESCS[0]);

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

const TimerSettingDesc *timerSettingByCmdKey(const char *cmdKey)
{
    for (size_t i = 0; i < TIMER_SETTINGS_DESC_COUNT; ++i)
        if (strcmp(TIMER_SETTINGS_DESCS[i].cmdKey, cmdKey) == 0)
            return &TIMER_SETTINGS_DESCS[i];
    return nullptr;
}

// ---------------------------------------------------------------------------
// HA attribute-group projection (PRD #57 / issue #58). This slice lights up the
// two carriers that already support JSON attributes (the buzzer + finished
// HASelects), so no ArduinoHA change is needed yet. realert_interval is listed
// first on the finished carrier so the folded payload is a superset of the
// legacy bespoke {"realert_interval":N}. All formatters are nullptr this slice
// (bar_color's "#RRGGBB" formatter arrives with the state-sensor carrier).
//   carrier, cmdKey, format
const TimerAttrGroupDesc TIMER_ATTR_GROUP_DESCS[] = {
    {TimerHaEntity::Finished, "realert_interval",  nullptr},
    {TimerHaEntity::Finished, "finished_hold",     nullptr},
    {TimerHaEntity::Buzzer,   "countdown_seconds", nullptr},
    {TimerHaEntity::Buzzer,   "melody_tick",       nullptr},
    {TimerHaEntity::Buzzer,   "melody_end",        nullptr},
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
