#ifndef TimerSettings_h
#define TimerSettings_h

#include <Arduino.h>
#include <ArduinoJson.h>

// Persisted Timer settings: the single descriptor table that drives validation,
// apply, NVS persistence, dev.json overrides and the propagated config snapshot
// for the value-config Timer keys. Modelled on TimerHa.h ("one table, no drift").
//
// This table is a SUPERSET, not "the config block": the `inSnapshot` column is the
// codified boundary CONTEXT.md describes in prose --
//   * config block            = the inSnapshot == true rows (the propagated snapshot)
//   * sync roles / local id    = the inSnapshot == false rows (sync_follow/sync_targets,
//                                deliberately excluded from the snapshot per ADR-0006)
//
// Out of scope (the B1 boundary, see docs/adr/0007): duration/buzzer/finished and the
// four icon_<state> keys stay on TimerManager's publish-aware setters. They are config
// block too, but their snapshot/broadcast membership is governed by the shared
// member-config list in TimerManager.cpp, not by this table.

enum class TcType  : uint8_t { U16, U32, Bool, Str };
enum class TcCheck : uint8_t { Bool, UIntRange, Name, Bespoke };

// A validated + coerced value, staged before any global is written so parseCommand
// keeps its atomic-reject contract (validate everything, then apply).
struct TcValue
{
    uint32_t num = 0;
    bool     b   = false;
    String   str;
};

struct TimerSettingDesc
{
    const char *cmdKey;     // POST/MQTT + snapshot key, e.g. "finished_hold"
    const char *devKey;     // dev.json key,            e.g. "timer_finished_hold"
    const char *nvsKey;     // NVS "awtrix" key,        e.g. "TFHOLD"
    TcType      type;
    TcCheck     check;
    uint32_t    lo, hi;     // UIntRange bounds (ranges live HERE, once)
    uint32_t    dfltNum;    // NVS default for U16/U32/Bool
    const char *dfltStr;    // NVS default for Str; also the Name empty-substitution value
    // Bespoke validators (bar_color / sync_targets only); nullptr for declarative rows.
    bool      (*bespoke)(JsonVariantConst, TcValue &out);
    bool        inSnapshot; // member of the propagated config block iff true
    void       *storage;    // typed by `type`: uint16_t* / uint32_t* / bool* / String*
};

extern const TimerSettingDesc TIMER_SETTINGS_DESCS[];
extern const size_t           TIMER_SETTINGS_DESC_COUNT;

// Validate + coerce one field into `out`. Never writes a global (pure parse). Strict
// JSON types match the legacy parseCommand: numbers reject bool/string/null; bools
// require a real JSON bool. Returns false on any violation.
bool timerSettingParse(const TimerSettingDesc &d, JsonVariantConst v, TcValue &out);

// Write a previously-parsed value to the descriptor's storage (dispatch on type).
void timerSettingStore(const TimerSettingDesc &d, const TcValue &v);

// NVS round-trip for the whole table (caller brackets begin()/end()).
void timerSettingsLoadNvs(class Preferences &prefs);
void timerSettingsSaveNvs(class Preferences &prefs);

// dev.json overrides: per-key best-effort (validate each present devKey, store iff
// valid, skip-and-continue otherwise -- NOT atomic; dev.json is a boot override layer).
void timerSettingsLoadDevJson(JsonObjectConst obj);

// Emit the inSnapshot rows into `doc` (the propagated config block, table half).
void timerSettingsBuildSnapshot(JsonDocument &doc);

// Lookup by command key (used by MenuManager to reuse a row's range bounds).
const TimerSettingDesc *timerSettingByCmdKey(const char *cmdKey);

// ---------------------------------------------------------------------------
// The config block's SECOND table: the member-backed half (B1, ADR-0007/0009).
//
// One row per member-backed config key (buzzer / finished / the four icon_<state>).
// Unlike TIMER_SETTINGS_DESCS' DECLARATIVE rows, these carry function-pointer hooks
// -- exactly like TIMER_MENU_SLOTS' enum slots -- because their values are owned by
// TimerManager's publish-aware setters (equality-skip, _suspendPersist batching, MQTT
// publish), not by a typed storage pointer. This is NOT ADR-0007's rejected "B2 / fold
// into the declarative table"; it is a separate hook table, the fourth member of the
// descriptor-table family (TIMER_SETTINGS_DESCS, TIMER_HA_DESCRIPTORS, TIMER_MENU_SLOTS).
//
// Together the two tables ARE the config block. `duration` is member-backed too but is
// deliberately EXCLUDED: it is run-state, not config (CONTEXT.md), and its validation
// depends on the cross-field effectiveMaxDuration staged from the table half.
struct TimerMemberConfigDesc
{
    const char *cmdKey;                                  // POST/MQTT + snapshot key
    bool (*validate)(JsonVariantConst, TcValue &out);    // pure: validate + coerce, no mutation
    void (*apply)(const TcValue &);                      // routes via TimerManager's deep setter
    void (*emit)(JsonDocument &doc);                     // writes the live value into the snapshot
};

extern const TimerMemberConfigDesc TIMER_MEMBER_CONFIG_DESCS[];
extern const size_t                TIMER_MEMBER_CONFIG_DESC_COUNT;

// Emit each member-config key's live value into `doc` (the B1 half of the config snapshot).
void timerMemberConfigBuildSnapshot(JsonDocument &doc);

// True iff `doc` carries any member-config key -- the broadcast trigger for the B1 half
// (snapshot membership IS the broadcast trigger, ADR-0006).
bool timerDocTouchesMemberConfig(const JsonDocument &doc);

#endif
