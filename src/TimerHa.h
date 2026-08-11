#ifndef TimerHa_h
#define TimerHa_h

#include <stddef.h>
#include <stdint.h>

// Timer HA Presence: the single source of truth for the Home Assistant entities
// that project the Timer over the MQTT discovery layer. This header is pure data
// (no ArduinoHA, no Globals) so it compiles on the host and the contract below can
// be unit-tested. The ArduinoHA `new HAX + setters` apply and the discovery
// teardown both read TIMER_HA_DESCRIPTORS in MQTTManager — one table, no drift.
// See CONTEXT.md ("Timer HA Presence") and docs/timer.md.

enum class TimerHaEntity : uint8_t
{
    Duration = 0,
    Remaining,
    State,
    Buzzer,
    Finished,
    Start,
    Pause,
    Reset,
    COUNT
};

struct TimerHaDescriptor
{
    TimerHaEntity slot;
    const char *component;    // HA discovery component: "text" | "sensor" | "select" | "button"
    const char *idFormat;     // unique-id format, e.g. "%s_timer_dur" ("%s" is the MAC suffix)
    const char *icon;
    const char *name;
    const char *options;      // select only ("`;`"-joined), else nullptr
    const char *unit;         // sensor only, else nullptr
    const char *deviceClass;  // sensor only, else nullptr
};

// One descriptor per TimerHaEntity slot, in slot order (TIMER_HA_DESCRIPTORS[(size_t)slot]).
constexpr size_t TIMER_HA_DESCRIPTOR_COUNT = static_cast<size_t>(TimerHaEntity::COUNT);
extern const TimerHaDescriptor TIMER_HA_DESCRIPTORS[TIMER_HA_DESCRIPTOR_COUNT];

// Convenience accessor: the descriptor for a given slot.
inline const TimerHaDescriptor &timerHaDescriptor(TimerHaEntity slot)
{
    return TIMER_HA_DESCRIPTORS[static_cast<size_t>(slot)];
}

#endif
