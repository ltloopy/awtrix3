// Stub uses the same include guard as src/Globals.h. When force-included via
// native_prelude.h, this header runs first and sets GLOBALS_H; the subsequent
// `#include "Globals.h"` from src/TimerManager.cpp then resolves to
// src/Globals.h, finds the guard defined, and skips the production body
// (which drags in FastLED/effects and can't compile on the host).
#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>

extern String CURRENT_APP;
extern bool   SOUND_ACTIVE;
extern bool   BLOCK_NAVIGATION;
extern bool   GAME_ACTIVE;
extern bool   MATRIX_OFF;
extern uint8_t BRIGHTNESS;

extern bool     SHOW_TIMER;
extern bool     SHOW_TIMER_HA_PREV;
extern uint32_t TIMER_MAX_DURATION;
extern uint32_t TIMER_STEP;
extern uint16_t TIMER_PUBLISH_INTERVAL;
extern uint16_t TIMER_FINISHED_HOLD;
extern uint16_t TIMER_REALERT_INTERVAL;
extern uint16_t TIMER_COUNTDOWN_SECONDS;
extern uint16_t TIMER_CONFIG_TIMEOUT;
extern String   TIMER_ICON_IDLE;
extern String   TIMER_ICON_RUNNING;
extern String   TIMER_ICON_PAUSED;
extern String   TIMER_ICON_FINISHED;

extern uint32_t TEXTCOLOR_888;

#endif
