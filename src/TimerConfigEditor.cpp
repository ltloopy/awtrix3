#include "TimerConfigEditor.h"

#include "Globals.h"        // TIMER_MAX_DURATION (cap math)
#include "TimerManager.h"   // secondsToHMS / hmsToSeconds (static math helpers, header-only)

void TimerConfigEditor::enter(uint32_t durationSec)
{
    uint32_t h, m, s;
    TimerManager_::secondsToHMS(durationSec, h, m, s);
    if (h > 99) h = 99;
    hh_    = (uint8_t)h;
    mm_    = (uint8_t)m;
    ss_    = (uint8_t)s;
    field_ = 0;
    active_ = true;
}

uint32_t TimerConfigEditor::exit()
{
    active_ = false;
    return TimerManager_::hmsToSeconds(hh_, mm_, ss_);
}

void TimerConfigEditor::cycleField()
{
    if (!active_) return;
    field_ = (field_ + 1) % 3;
}

void TimerConfigEditor::adjust(int delta)
{
    if (!active_) return;

    const uint8_t  hardMax = (field_ == 0) ? 99 : 59;
    const uint32_t perUnit = (field_ == 0) ? 3600UL : (field_ == 1) ? 60UL : 1UL;
    const uint32_t otherSec = (field_ == 0)
        ? (uint32_t)mm_ * 60UL + (uint32_t)ss_
        : (field_ == 1)
            ? (uint32_t)hh_ * 3600UL + (uint32_t)ss_
            : (uint32_t)hh_ * 3600UL + (uint32_t)mm_ * 60UL;

    uint8_t maxVal = hardMax;
    if (TIMER_MAX_DURATION > 0)
    {
        uint32_t headroom = (TIMER_MAX_DURATION > otherSec) ? (TIMER_MAX_DURATION - otherSec) : 0;
        uint32_t room     = headroom / perUnit;
        if (room < maxVal) maxVal = (uint8_t)room;
    }

    uint8_t cur = (field_ == 0) ? hh_ : (field_ == 1 ? mm_ : ss_);
    if (cur > maxVal) cur = maxVal;
    int next = (int)cur + (delta >= 0 ? 1 : -1);
    if (next < 0) next = maxVal;
    else if (next > (int)maxVal) next = 0;
    if      (field_ == 0) hh_ = (uint8_t)next;
    else if (field_ == 1) mm_ = (uint8_t)next;
    else                  ss_ = (uint8_t)next;
}
