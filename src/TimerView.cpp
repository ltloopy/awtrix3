#include "TimerView.h"

#include <stdio.h>

#include "TimerManager.h"

namespace
{
    // Display geometry owned by the view (app-local coordinates on the 32x8
    // panel). The painter (src/Apps.cpp) keeps only the font-/row-dependent bits
    // (text baseline, bottom row, config underline step).
    constexpr int16_t kScreenW   = 32;
    constexpr int16_t kTextX     = 8;   // text region starts right of the 8px icon
    constexpr int16_t kTextWidth = 24;
    constexpr int16_t kBarMaxLen = 23;
    constexpr int16_t kBarX0     = 9;

    constexpr unsigned long kBlinkMs = 500;
}

void TimerViewModel::formatTimerDisplay(uint32_t seconds, char *out, size_t outLen)
{
    if (seconds < 3600)
        snprintf(out, outLen, "%u:%02u", (unsigned)(seconds / 60), (unsigned)(seconds % 60));
    else if (seconds < 36000)
        snprintf(out, outLen, "%u:%02u", (unsigned)(seconds / 3600), (unsigned)((seconds % 3600) / 60));
    else
        snprintf(out, outLen, "%02u:%02u", (unsigned)(seconds / 3600), (unsigned)((seconds % 3600) / 60));
}

TimerView TimerViewModel::compute(unsigned long nowMs, bool iconEnabled)
{
    TimerView v = {};

    // Config screen: HH:MM:SS centered over the full panel, with a field underline.
    if (TimerManager.isInConfig())
    {
        v.screen = TimerView::Screen::Config;
        snprintf(v.text, sizeof(v.text), "%02u:%02u:%02u",
                 (unsigned)TimerManager.getConfigHH(),
                 (unsigned)TimerManager.getConfigMM(),
                 (unsigned)TimerManager.getConfigSS());
        v.showText       = true;
        v.textRegionX0   = 0;
        v.textRegionW    = kScreenW;
        v.showUnderline  = true;
        v.underlineField = TimerManager.getConfigField();
        return v;
    }

    const TimerState ts = TimerManager.getState();

    // Non-config screens draw the icon (unless disabled) and center their text in
    // the 24px area right of it. When the icon is hidden, text reflows to the full
    // 32px panel.
    v.showIcon = iconEnabled;
    if (iconEnabled) { v.textRegionX0 = kTextX; v.textRegionW = kTextWidth; } // 8 / 24
    else             { v.textRegionX0 = 0;      v.textRegionW = kScreenW;   } // full 32px

    // Finished screen: blinking "0:00", no bar.
    if (ts == TimerState::Finished)
    {
        v.screen = TimerView::Screen::Finished;
        snprintf(v.text, sizeof(v.text), "0:00");
        v.showText = ((nowMs / kBlinkMs) % 2 == 0);
        return v;
    }

    // Time screen: Idle shows the configured duration; Running/Paused show remaining.
    v.screen = TimerView::Screen::Time;
    const uint32_t duration  = TimerManager.getDuration();
    const uint32_t remaining = (ts == TimerState::Idle) ? duration : TimerManager.getRemaining();
    formatTimerDisplay(remaining, v.text, sizeof(v.text));
    v.showText = true;

    // The bar divides by the duration captured when the run began, not the live
    // configured duration, so editing the duration mid-run leaves the in-progress
    // bar untouched (it re-arms on the next start/reset). See API-17 in
    // TIMER_TEST_PLAN.md. Falls back to the configured duration if no snapshot.
    uint32_t barDuration = TimerManager.getRunDuration();
    if (barDuration == 0) barDuration = duration;
    if (ts != TimerState::Idle && barDuration > 0)
    {
        // The bar also reflows when the icon is hidden: it spans the full panel
        // instead of the 23px region right of the icon. Right edge stays anchored
        // at col 31 either way (barX0 + barMaxLen == kScreenW).
        const int16_t barX0     = iconEnabled ? kBarX0     : 0;          // 9  or 0
        const int16_t barMaxLen = iconEnabled ? kBarMaxLen : kScreenW;   // 23 or 32
        uint32_t len = ((uint32_t)barMaxLen * remaining) / barDuration;
        if (len > (uint32_t)barMaxLen)
            len = (uint32_t)barMaxLen;
        if (len > 0)
        {
            v.showBar   = true;
            v.barLen    = (uint8_t)len;
            v.barStartX = barX0 + (barMaxLen - (int16_t)len);  // right edge anchored at col 31
        }
    }

    return v;
}
