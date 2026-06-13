#include "TimerMenu.h"

#include "TimerManager.h"
#include "TimerSettings.h"

namespace
{
    // Enum hooks. The setters defer the NVS write (persist=false): the TIMER menu
    // applies + publishes live during scroll and persists on the long-press
    // commit, where MenuManager opens the PersistBatch guard (ADR-0008 as amended
    // by PRD #29 / #45). Non-capturing lambdas decay to the row's function pointers.
    uint8_t getBuzzer()        { return (uint8_t)TimerManager.getBuzzerMode(); }
    void    setBuzzer(uint8_t v) { TimerManager.setBuzzerMode((BuzzerMode)v, /*persist=*/false); }
    uint8_t getFinished()      { return (uint8_t)TimerManager.getFinishedMode(); }
    void    setFinished(uint8_t v) { TimerManager.setFinishedMode((FinishedMode)v, /*persist=*/false); }
}

// idx order is the on-screen slot order (selectButton cycles through it).
//   kind, cmdKey, prefix, step, codec, labelCount, getEnum, setEnum
// The two EnumCycle slots read their labels from the per-enum codec table's menu
// column (ADR-0010) -- no private label copy to drift from it.
const TimerMenuSlot TIMER_MENU_SLOTS[] = {
    {TimerMenuKind::EnumCycle,    nullptr,             nullptr,  0, TIMER_BUZZER_CODEC,   (uint8_t)BuzzerMode::COUNT,   getBuzzer,   setBuzzer},
    {TimerMenuKind::SteppedRange, "countdown_seconds", "CDOWN ", 1, nullptr,              0, nullptr,     nullptr},
    {TimerMenuKind::EnumCycle,    nullptr,             nullptr,  0, TIMER_FINISHED_CODEC, (uint8_t)FinishedMode::COUNT, getFinished, setFinished},
    {TimerMenuKind::SteppedRange, "finished_hold",     "CLEAR ", 5, nullptr,              0, nullptr,     nullptr},
    {TimerMenuKind::SteppedRange, "realert_interval",  "ALERT ", 5, nullptr,              0, nullptr,     nullptr},
    {TimerMenuKind::BoolToggle,   "icon_enabled",      "ICON ",  0, nullptr,              0, nullptr,     nullptr},
    {TimerMenuKind::BoolToggle,   "bar_enabled",       "BAR ",   0, nullptr,              0, nullptr,     nullptr},
};

const size_t TIMER_MENU_SLOT_COUNT = sizeof(TIMER_MENU_SLOTS) / sizeof(TIMER_MENU_SLOTS[0]);

String timerMenuLabel(uint8_t slot)
{
    if (slot >= TIMER_MENU_SLOT_COUNT) return String();
    const TimerMenuSlot &s = TIMER_MENU_SLOTS[slot];
    switch (s.kind)
    {
        case TimerMenuKind::EnumCycle:
            return String(s.codec[s.getEnum()].menu);
        case TimerMenuKind::SteppedRange:
        {
            const TimerSettingDesc *d = timerSettingByCmdKey(s.cmdKey);
            uint16_t v = d ? *static_cast<uint16_t *>(d->storage) : 0;
            return String(s.prefix) + String(v);
        }
        case TimerMenuKind::BoolToggle:
        {
            const TimerSettingDesc *d = timerSettingByCmdKey(s.cmdKey);
            bool on = d && *static_cast<bool *>(d->storage);
            return String(s.prefix) + (on ? "ON" : "OFF");
        }
    }
    return String();
}

void timerMenuAdjust(uint8_t slot, int dir)
{
    if (slot >= TIMER_MENU_SLOT_COUNT) return;
    const TimerMenuSlot &s = TIMER_MENU_SLOTS[slot];
    switch (s.kind)
    {
        case TimerMenuKind::EnumCycle:
        {
            uint8_t cur  = s.getEnum();
            uint8_t next = (dir > 0)
                ? (uint8_t)((cur + 1) % s.labelCount)
                : (uint8_t)((cur + s.labelCount - 1) % s.labelCount);
            s.setEnum(next);
            break;
        }
        case TimerMenuKind::SteppedRange:
        {
            const TimerSettingDesc *d = timerSettingByCmdKey(s.cmdKey);
            if (!d) break;
            uint16_t &v = *static_cast<uint16_t *>(d->storage);
            if (dir > 0) v = (v + s.step <= d->hi) ? (uint16_t)(v + s.step) : (uint16_t)d->hi;
            else         v = (v >= d->lo + s.step) ? (uint16_t)(v - s.step) : (uint16_t)d->lo;
            break;
        }
        case TimerMenuKind::BoolToggle:
        {
            const TimerSettingDesc *d = timerSettingByCmdKey(s.cmdKey);
            if (!d) break;
            bool &b = *static_cast<bool *>(d->storage);
            b = !b;
            break;
        }
    }
}
