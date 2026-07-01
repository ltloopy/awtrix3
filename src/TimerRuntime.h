#ifndef TimerRuntime_h
#define TimerRuntime_h

#include <cstdint>
#include <vector>

#include "TimerEnums.h"   // TimerState / FinishedMode (the run-state the engine keys on)

// Pure timer run-state engine (PRD #28, issues #178/#179). step() is a total
// function of (state, nowMs, inputs) that RETURNS the ordered effects plus the
// run-state bookkeeping the adapter must apply, instead of performing anything;
// the TimerManager adapter executes each returned effect in order against
// PeripheryManager / DisplayManager / the wire seam and applies the bookkeeping.
//
// No hardware headers: the engine names PeripheryManager/DisplayManager nowhere,
// so it is host-compilable on its own. The [env:native_runtime] link — which
// pulls in TimerRuntime.cpp and links no TimerManager singleton — is the
// architectural claim, exactly as native_sync/native_validate are for theirs.
namespace TimerRuntime
{
    // Which pre-loaded tune the adapter should play. The engine stays free of the
    // RTTTL strings themselves: End -> endRtttl, Tick -> tickRtttl at the adapter.
    enum class Tone : uint8_t { End, Tick };

    // The full effect vocabulary. The Running tick emits PlayTone(Tick)/publish;
    // the Finished transition emits the switch/brightness/end-tone bundle;
    // auto-clear emits StopSound + the return-to-Idle publish/brightness effects.
    enum class EffectKind : uint8_t
    {
        PlayTone,
        StopSound,
        SwitchToTimerApp,
        SetBrightness,
        PublishState,
        PublishRemaining,
    };

    // Flat tagged effect. `tone` is meaningful only for PlayTone; `brightness`
    // only for SetBrightness. A flat struct (not a union/variant) keeps the type
    // trivially copyable and gnu++11-friendly for the device build.
    struct Effect
    {
        EffectKind kind;
        Tone       tone       = Tone::End;   // valid iff kind == PlayTone
        uint8_t    brightness = 0;           // valid iff kind == SetBrightness
    };

    // The run-state bookkeeping the adapter applies AFTER executing the effects.
    // The engine decides the transition; the adapter owns the state mutation (and
    // the override-store restore behind ToIdle) so ADR-0024's store stays in the
    // manager (PRD #28 US7).
    enum class Transition : uint8_t
    {
        None,        // no phase change
        ToFinished,  // Running reached zero
        ToIdle,      // Finished auto-cleared after the hold
    };

    // Environment gates resolved to plain bools/values so the engine needs none of
    // the globals/singletons that own them:
    //   navigationFree  = !GAME_ACTIVE && !BLOCK_NAVIGATION && !MenuManager.inMenu
    //   matrixOff       = MATRIX_OFF (restore brightness when the panel is dark)
    //   playEndTone     = SOUND_ACTIVE && buzzer != Off && endRtttl set
    //   countdownArmed  = SOUND_ACTIVE && buzzer == Countdown && tickRtttl set
    //   realertArmed    = SOUND_ACTIVE && buzzer != Off
    //   realertToneSet  = endRtttl set
    struct Inputs
    {
        // Running tick.
        uint32_t newRemaining     = 0;       // computeCurrentRemaining()
        bool     countdownArmed   = false;
        bool     isPlaying        = false;   // PeripheryManager.isPlaying()
        uint16_t countdownSeconds = 0;       // TIMER_COUNTDOWN_SECONDS
        uint16_t publishInterval  = 0;       // TIMER_PUBLISH_INTERVAL (sec; 0 disables)

        // Running->Finished transition.
        bool    navigationFree = false;
        bool    matrixOff      = false;
        uint8_t brightness     = 0;
        bool    playEndTone    = false;

        // Finished branch.
        FinishedMode finishedMode    = FinishedMode::AutoClear;
        uint16_t     finishedHold    = 0;    // TIMER_FINISHED_HOLD (sec)
        uint16_t     realertInterval = 0;    // TIMER_REALERT_INTERVAL (sec)
        bool         realertArmed    = false;
        bool         realertToneSet  = false;
    };

    struct State
    {
        TimerState    phase;
        uint32_t      remainingSec;          // the previous remaining (before this tick)
        unsigned long lastPublishMs     = 0; // Running publish throttle anchor
        unsigned long enteredFinishedMs = 0; // auto-clear hold anchor
        unsigned long lastRealertMs     = 0; // re-alert interval anchor
    };

    struct Result
    {
        std::vector<Effect> effects;
        Transition          transition   = Transition::None;
        bool                publishFired = false;  // Running: adapter sets lastPublishMs = nowMs
        bool                realertFired = false;  // Finished: adapter sets lastRealertMs = nowMs
    };

    // Returns the ordered effects + bookkeeping for the tick that `state` (with
    // `inputs` at `nowMs`) represents.
    Result step(const State &state, unsigned long nowMs, const Inputs &in);
}

#endif
