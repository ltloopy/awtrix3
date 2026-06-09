#ifndef TimerConfigEditor_h
#define TimerConfigEditor_h

#include <Arduino.h>

// The Timer-app's on-device duration editor (HH/MM/SS wheels), extracted from
// TimerManager as a display-free pure value object. It owns only the config-mode
// working state — the field cursor and the three edit buffers — plus the cap-aware
// adjust math. It has no DisplayManager dependency and never reads millis() or the
// buttons: the 30 s no-input timeout and the hold-to-repeat live in
// TimerManager::tick() (tick-loop bookkeeping). Duration flows in via enter() and
// back out via exit(); TimerManager commits the result through setDuration().
// See docs/adr/0011-timer-config-editor-extraction.md.
class TimerConfigEditor
{
public:
    // Begin editing: decompose durationSec into HH/MM/SS, start on the HH field,
    // become active. HH is clamped to 99 so the field stays two-digit-editable
    // (the caller is responsible for any run-state clamp before handing it in).
    void enter(uint32_t durationSec);

    // Finish editing: become inactive and return the edited duration in seconds.
    // The caller commits it (TimerManager::setDuration).
    uint32_t exit();

    // Advance the highlighted field: HH -> MM -> SS -> HH.
    void cycleField();

    // Inc/dec the current field by one with wrapping. The wrap ceiling is the
    // per-field hard max (99 for HH, 59 for MM/SS) further capped by the headroom
    // left under the TIMER_MAX_DURATION global given the other two fields.
    void adjust(int delta);

    bool    isActive() const { return active_; }
    uint8_t field()    const { return field_; }
    uint8_t hh()       const { return hh_; }
    uint8_t mm()       const { return mm_; }
    uint8_t ss()       const { return ss_; }

private:
    bool    active_ = false;
    uint8_t field_  = 0;   // 0 = HH, 1 = MM, 2 = SS
    uint8_t hh_     = 0;
    uint8_t mm_     = 0;
    uint8_t ss_     = 0;
};

#endif
