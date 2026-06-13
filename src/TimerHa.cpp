#include "TimerHa.h"

#include <stdio.h>

#include "TimerEnums.h"

// PROGMEM is a no-op for const data on the ESP32 (flash is memory-mapped and
// directly addressable), and isn't defined on the native host build. Define it
// away when absent so this table compiles in both environments.
#ifndef PROGMEM
#define PROGMEM
#endif

// Build a select's "`;`"-joined HA option list from a per-enum codec table's HA
// column, in enum-value order (the table's row index IS the enum value, pinned by
// its static_assert — see docs/adr/0010). One join site, no hand-written list to
// drift from the enum labels. The returned String has static lifetime below, so
// the descriptor's `options` pointer stays valid for the program's life.
static String joinHaOptions(const TimerEnumCodec *table, size_t count)
{
    String s;
    for (size_t i = 0; i < count; ++i)
    {
        if (i) s += ';';
        s += table[i].ha;
    }
    return s;
}

// HA entity strings for the Timer. Co-located with the descriptor table so the
// Timer's HA contract lives in one place (moved here from Dictionary.cpp).
static const char HAtimerDurID[] PROGMEM    = {"%s_timer_dur"};
static const char HAtimerDurIcon[] PROGMEM  = {"mdi:timer-cog-outline"};
static const char HAtimerDurName[] PROGMEM  = {"Timer duration"};

static const char HAtimerRemID[] PROGMEM    = {"%s_timer_rem"};
static const char HAtimerRemIcon[] PROGMEM  = {"mdi:timer-sand"};
static const char HAtimerRemName[] PROGMEM  = {"Timer remaining"};
static const char HAtimerRemUnit[] PROGMEM  = {"s"};
static const char HAtimerRemClass[] PROGMEM = {"duration"};

static const char HAtimerStateID[] PROGMEM   = {"%s_timer_state"};
static const char HAtimerStateIcon[] PROGMEM = {"mdi:state-machine"};
static const char HAtimerStateName[] PROGMEM = {"Timer state"};

static const char HAtimerBuzID[] PROGMEM      = {"%s_timer_buz"};
static const char HAtimerBuzIcon[] PROGMEM    = {"mdi:volume-high"};
static const char HAtimerBuzName[] PROGMEM    = {"Timer buzzer"};
static const String HAtimerBuzOptions = joinHaOptions(TIMER_BUZZER_CODEC, TIMER_BUZZER_CODEC_COUNT);

static const char HAtimerFinID[] PROGMEM      = {"%s_timer_fin"};
static const char HAtimerFinIcon[] PROGMEM    = {"mdi:bell-ring-outline"};
static const char HAtimerFinName[] PROGMEM    = {"Timer finished mode"};
static const String HAtimerFinOptions = joinHaOptions(TIMER_FINISHED_CODEC, TIMER_FINISHED_CODEC_COUNT);

static const char HAtimerStartID[] PROGMEM   = {"%s_timer_start"};
static const char HAtimerStartIcon[] PROGMEM = {"mdi:play"};
static const char HAtimerStartName[] PROGMEM = {"Timer start"};

static const char HAtimerPauseID[] PROGMEM   = {"%s_timer_pause"};
static const char HAtimerPauseIcon[] PROGMEM = {"mdi:pause"};
static const char HAtimerPauseName[] PROGMEM = {"Timer pause"};

static const char HAtimerResetID[] PROGMEM   = {"%s_timer_reset"};
static const char HAtimerResetIcon[] PROGMEM = {"mdi:restore"};
static const char HAtimerResetName[] PROGMEM = {"Timer reset"};

const TimerHaDescriptor TIMER_HA_DESCRIPTORS[TIMER_HA_DESCRIPTOR_COUNT] = {
    {TimerHaEntity::Duration, "text",   HAtimerDurID,   HAtimerDurIcon,   HAtimerDurName,   nullptr,            nullptr,         nullptr},
    {TimerHaEntity::Remaining,"sensor", HAtimerRemID,   HAtimerRemIcon,   HAtimerRemName,   nullptr,            HAtimerRemUnit,  HAtimerRemClass},
    {TimerHaEntity::State,    "sensor", HAtimerStateID, HAtimerStateIcon, HAtimerStateName, nullptr,            nullptr,         nullptr},
    {TimerHaEntity::Buzzer,   "select", HAtimerBuzID,   HAtimerBuzIcon,   HAtimerBuzName,   HAtimerBuzOptions.c_str(),  nullptr,         nullptr},
    {TimerHaEntity::Finished, "select", HAtimerFinID,   HAtimerFinIcon,   HAtimerFinName,   HAtimerFinOptions.c_str(),  nullptr,         nullptr},
    {TimerHaEntity::Start,    "button", HAtimerStartID, HAtimerStartIcon, HAtimerStartName, nullptr,            nullptr,         nullptr},
    {TimerHaEntity::Pause,    "button", HAtimerPauseID, HAtimerPauseIcon, HAtimerPauseName, nullptr,            nullptr,         nullptr},
    {TimerHaEntity::Reset,    "button", HAtimerResetID, HAtimerResetIcon, HAtimerResetName, nullptr,            nullptr,         nullptr},
};

void formatTimerHaEntityId(const TimerHaDescriptor &d, const char *macSuffix, char *out, size_t outLen)
{
    snprintf(out, outLen, d.idFormat, macSuffix);
}

void formatTimerHaDataTopic(const char *dataPrefix, const char *deviceUniqueId,
                            const char *entityId, char *out, size_t outLen)
{
    // "stat_t" is ArduinoHA's HAStateTopic suffix; see the contract in TimerHa.h.
    snprintf(out, outLen, "%s/%s/%s/stat_t", dataPrefix, deviceUniqueId, entityId);
}

void formatTimerHaAttrTopic(const char *dataPrefix, const char *deviceUniqueId,
                            const char *entityId, char *out, size_t outLen)
{
    // "json_attr_t" is ArduinoHA's HAJsonAttributesTopic suffix; see the contract
    // in TimerHa.h.
    snprintf(out, outLen, "%s/%s/%s/json_attr_t", dataPrefix, deviceUniqueId, entityId);
}
