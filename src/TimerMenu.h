#ifndef TimerMenu_h
#define TimerMenu_h

#include <Arduino.h>

// TIMER menu slot table: the data model behind the on-device TIMER global menu's
// seven slots (the third member of the Timer descriptor-table family, alongside
// TIMER_SETTINGS_DESCS and TIMER_HA_DESCRIPTORS). One row per slot; MenuManager
// walks the table for label / adjust and keeps only the drawing. This header is
// display-free (no DisplayManager) so the label/clamp/wrap logic is host-testable.
// See CONTEXT.md ("TIMER menu slot table") and docs/adr/0008.
//
// Two slot families, mirroring the B1 boundary (ADR-0007):
//   * table-backed slots (SteppedRange / BoolToggle) reuse their TIMER_SETTINGS_DESCS
//     row by cmdKey for storage + range -- they cannot drift from the settings table.
//   * the two enum slots (buzzer / finished) are member-backed: they carry bespoke
//     getEnum/setEnum hooks (like the settings table's `bespoke` fn pointers). Their
//     setEnum defers the NVS write to the menu commit (setBuzzerMode(m, persist=false)).

enum class TimerMenuKind : uint8_t { EnumCycle, SteppedRange, BoolToggle };

struct TimerMenuSlot
{
    TimerMenuKind      kind;
    const char        *cmdKey;       // SteppedRange/BoolToggle: -> TIMER_SETTINGS_DESCS (storage + lo/hi)
    const char        *prefix;       // SteppedRange/BoolToggle label prefix ("CLEAR ", "ICON ")
    uint16_t           step;         // SteppedRange step
    const char *const *labels;       // EnumCycle: labels indexed by current value
    uint8_t            labelCount;   // EnumCycle modulus
    uint8_t          (*getEnum)();   // EnumCycle only
    void             (*setEnum)(uint8_t);  // EnumCycle only (routes via TimerManager setter)
};

extern const TimerMenuSlot TIMER_MENU_SLOTS[];
extern const size_t        TIMER_MENU_SLOT_COUNT;

// The slot's display string, e.g. "CLEAR 10", "BZR END", "ICON ON". Returns "" for
// an out-of-range slot index.
String timerMenuLabel(uint8_t slot);

// Adjust the slot by one step in the given direction (dir > 0 = right/increment,
// dir <= 0 = left/decrement). Stepped ranges saturate at the descriptor's lo/hi;
// enums wrap; bools toggle (either direction). No-op for an out-of-range index.
void timerMenuAdjust(uint8_t slot, int dir);

#endif
