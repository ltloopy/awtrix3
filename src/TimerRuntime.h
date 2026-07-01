#ifndef TimerRuntime_h
#define TimerRuntime_h

#include <cstdint>
#include <vector>

#include "TimerEnums.h"   // TimerState (the run-state phase the engine keys on)

// Pure timer run-state engine (PRD #28, issue #178). step() is a total function
// of (state, nowMs, inputs) that RETURNS an ordered list of effects instead of
// performing them; the TimerManager adapter executes each returned effect in
// order against PeripheryManager / DisplayManager / the wire seam.
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

    // The full effect vocabulary the later slices will reuse. Only a subset is
    // emitted by the Running->Finished transition ported here (issue #178).
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

    // The environment gates enterFinished() reads, resolved to plain bools so the
    // engine needs none of the globals/singletons that own them:
    //   navigationFree = !GAME_ACTIVE && !BLOCK_NAVIGATION && !MenuManager.inMenu
    //   matrixOff      = MATRIX_OFF (restore brightness when the panel is dark)
    //   playEndTone    = SOUND_ACTIVE && buzzerMode != Off && endRtttl set
    struct Inputs
    {
        bool    navigationFree = false;
        bool    matrixOff      = false;
        uint8_t brightness     = 0;
        bool    playEndTone    = false;
    };

    struct State
    {
        TimerState phase;
        uint32_t   remainingSec;
    };

    // Returns the ordered effects for the transition `state` (with `inputs`)
    // represents. Running-with-zero-remaining is the Finished transition.
    std::vector<Effect> step(const State &state, unsigned long nowMs, const Inputs &in);
}

#endif
