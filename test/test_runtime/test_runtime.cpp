// Unit tests for TimerRuntime (PRD #28, issues #178/#179) — the pure run-state
// engine that owns TimerManager::tick()'s decisions.
//
// The whole point of the extraction: step() is a total function of
// (state, nowMs, inputs) that RETURNS an ordered Effect[] + the run-state
// bookkeeping instead of touching hardware. These assert the exact effect
// sequence — no TimerManager singleton, no PeripheryManager / DisplayManager,
// no globals. The [env:native_runtime] link (TimerRuntime.cpp + nothing else)
// is the architectural claim.

#include <unity.h>

#include "../../src/TimerRuntime.h"

using TimerRuntime::Command;
using TimerRuntime::Effect;
using TimerRuntime::EffectKind;
using TimerRuntime::Inputs;
using TimerRuntime::Result;
using TimerRuntime::State;
using TimerRuntime::Tone;
using TimerRuntime::Transition;

void setUp(void) {}
void tearDown(void) {}

// --- Running -> Finished transition (issue #178) -------------------------------

// A Running timer that has just reached zero, with every gate open: navigation
// free, matrix off (so brightness is restored) and the end tone armed. The full
// effect set, in enterFinished's order.
void test_R1_finished_all_gates_open(void) {
    Inputs in;
    in.newRemaining = 0;
    in.navigationFree = true;
    in.matrixOff = true;
    in.brightness = 42;
    in.playEndTone = true;

    Result r = TimerRuntime::step(State{TimerState::Running, 1}, 1000, in);

    TEST_ASSERT_EQUAL(Transition::ToFinished, r.transition);
    TEST_ASSERT_EQUAL_UINT(5, r.effects.size());
    TEST_ASSERT_EQUAL(EffectKind::PublishState, r.effects[0].kind);
    TEST_ASSERT_EQUAL(EffectKind::PublishRemaining, r.effects[1].kind);
    TEST_ASSERT_EQUAL(EffectKind::SwitchToTimerApp, r.effects[2].kind);
    TEST_ASSERT_EQUAL(EffectKind::SetBrightness, r.effects[3].kind);
    TEST_ASSERT_EQUAL_UINT(42, r.effects[3].brightness);
    TEST_ASSERT_EQUAL(EffectKind::PlayTone, r.effects[4].kind);
    TEST_ASSERT_EQUAL(Tone::End, r.effects[4].tone);
}

// Every gate closed: navigation blocked (in a game/menu), matrix on, tone off.
// The two publishes are unconditional; nothing else fires.
void test_R2_finished_all_gates_closed(void) {
    Inputs in;
    in.newRemaining = 0;
    in.navigationFree = false;
    in.matrixOff = false;
    in.playEndTone = false;

    Result r = TimerRuntime::step(State{TimerState::Running, 1}, 1000, in);

    TEST_ASSERT_EQUAL(Transition::ToFinished, r.transition);
    TEST_ASSERT_EQUAL_UINT(2, r.effects.size());
    TEST_ASSERT_EQUAL(EffectKind::PublishState, r.effects[0].kind);
    TEST_ASSERT_EQUAL(EffectKind::PublishRemaining, r.effects[1].kind);
}

// A Running timer whose newly-computed remaining is non-zero is not the Finished
// transition. (Beep/publish are covered by their own tests below.)
void test_R3_running_nonzero_no_transition(void) {
    Inputs in;
    in.newRemaining = 5;

    Result r = TimerRuntime::step(State{TimerState::Running, 6}, 1000, in);

    TEST_ASSERT_EQUAL(Transition::None, r.transition);
}

// newRemaining == 0 in Idle/Paused is not a transition the engine owns.
void test_R4_nonrunning_zero_no_effects(void) {
    Inputs in;
    in.newRemaining = 0;

    Result idle = TimerRuntime::step(State{TimerState::Idle, 0}, 1000, in);
    TEST_ASSERT_EQUAL(Transition::None, idle.transition);
    TEST_ASSERT_EQUAL_UINT(0, idle.effects.size());

    Result paused = TimerRuntime::step(State{TimerState::Paused, 0}, 1000, in);
    TEST_ASSERT_EQUAL(Transition::None, paused.transition);
    TEST_ASSERT_EQUAL_UINT(0, paused.effects.size());
}

// --- Countdown-beep gap-coalescing (US4, issue #179) ---------------------------

// A tick that fell several seconds behind: remaining jumped 5 -> 1 in one tick,
// crossing seconds 3, 2, 1 with a 3-second countdown window. The buzzer plays one
// tone at a time, so a single PlayTone(Tick) must cover the gap — NOT one per
// crossed second.
void test_B1_beep_gap_single_tone(void) {
    Inputs in;
    in.newRemaining = 1;
    in.countdownArmed = true;
    in.isPlaying = false;
    in.countdownSeconds = 3;

    Result r = TimerRuntime::step(State{TimerState::Running, 5}, 1000, in);

    TEST_ASSERT_EQUAL(Transition::None, r.transition);
    TEST_ASSERT_EQUAL_UINT(1, r.effects.size());
    TEST_ASSERT_EQUAL(EffectKind::PlayTone, r.effects[0].kind);
    TEST_ASSERT_EQUAL(Tone::Tick, r.effects[0].tone);
}

// --- Auto-clear (US5, issue #179) ----------------------------------------------

// Finished + AutoClear returns to Idle exactly at the hold boundary: with a 2s
// hold entered at t=1000ms, nothing fires at t=2999ms (1999ms elapsed) and the
// return-to-Idle fires at t=3000ms (2000ms elapsed). Matrix on -> no brightness.
void test_C1_autoclear_hold_boundary(void) {
    Inputs in;
    in.finishedMode = FinishedMode::AutoClear;
    in.finishedHold = 2;
    in.matrixOff = false;
    State st{TimerState::Finished, 0};
    st.enteredFinishedMs = 1000;

    Result before = TimerRuntime::step(st, 2999, in);
    TEST_ASSERT_EQUAL(Transition::None, before.transition);
    TEST_ASSERT_EQUAL_UINT(0, before.effects.size());

    Result at = TimerRuntime::step(st, 3000, in);
    TEST_ASSERT_EQUAL(Transition::ToIdle, at.transition);
    TEST_ASSERT_EQUAL_UINT(3, at.effects.size());
    TEST_ASSERT_EQUAL(EffectKind::StopSound, at.effects[0].kind);
    TEST_ASSERT_EQUAL(EffectKind::PublishState, at.effects[1].kind);
    TEST_ASSERT_EQUAL(EffectKind::PublishRemaining, at.effects[2].kind);
}

// Auto-clear with the matrix dark restores brightness to 0 as its last effect.
void test_C2_autoclear_matrix_off_dims(void) {
    Inputs in;
    in.finishedMode = FinishedMode::AutoClear;
    in.finishedHold = 0;
    in.matrixOff = true;
    State st{TimerState::Finished, 0};
    st.enteredFinishedMs = 0;

    Result r = TimerRuntime::step(st, 0, in);
    TEST_ASSERT_EQUAL(Transition::ToIdle, r.transition);
    TEST_ASSERT_EQUAL_UINT(4, r.effects.size());
    TEST_ASSERT_EQUAL(EffectKind::SetBrightness, r.effects[3].kind);
    TEST_ASSERT_EQUAL_UINT(0, r.effects[3].brightness);
}

// --- Re-alert (US5, issue #179) ------------------------------------------------

// Finished + ReAlert re-sounds the end tone at its interval and NOT before: with a
// 5s interval last sounded at t=1000ms, nothing at t=5999ms; at t=6000ms it emits
// PlayTone(End) and reports realertFired so the adapter advances the anchor.
void test_D1_realert_interval_boundary(void) {
    Inputs in;
    in.finishedMode = FinishedMode::ReAlert;
    in.realertInterval = 5;
    in.realertArmed = true;
    in.realertToneSet = true;
    in.isPlaying = false;
    State st{TimerState::Finished, 0};
    st.lastRealertMs = 1000;

    Result before = TimerRuntime::step(st, 5999, in);
    TEST_ASSERT_FALSE(before.realertFired);
    TEST_ASSERT_EQUAL_UINT(0, before.effects.size());

    Result at = TimerRuntime::step(st, 6000, in);
    TEST_ASSERT_TRUE(at.realertFired);
    TEST_ASSERT_EQUAL_UINT(1, at.effects.size());
    TEST_ASSERT_EQUAL(EffectKind::PlayTone, at.effects[0].kind);
    TEST_ASSERT_EQUAL(Tone::End, at.effects[0].tone);
}

// The buzzer plays one tone at a time: if a tone is already playing when the
// interval elapses, no new tone is queued — but the anchor still advances.
void test_D2_realert_skips_tone_while_playing(void) {
    Inputs in;
    in.finishedMode = FinishedMode::ReAlert;
    in.realertInterval = 5;
    in.realertArmed = true;
    in.realertToneSet = true;
    in.isPlaying = true;
    State st{TimerState::Finished, 0};
    st.lastRealertMs = 1000;

    Result r = TimerRuntime::step(st, 6000, in);
    TEST_ASSERT_TRUE(r.realertFired);
    TEST_ASSERT_EQUAL_UINT(0, r.effects.size());
}

// --- Input-driven lifecycle transitions (issue #180) ---------------------------
//
// The same public verbs TimerManager exposes, decided here: start/pause/reset/
// setDuration. Each asserts the returned Transition + the ordered publish/stop/
// brightness effects, exactly as the tick tests do — no singleton, no hardware.

// start from Idle: arm the run. enterRunning's two publishes, ToRunning (the
// adapter loads remaining = durationSec because the prior phase is not Paused).
void test_S1_start_from_idle(void) {
    Inputs in;
    in.command = Command::Start;
    in.durationSec = 300;

    Result r = TimerRuntime::step(State{TimerState::Idle, 300}, 1000, in);

    TEST_ASSERT_EQUAL(Transition::ToRunning, r.transition);
    TEST_ASSERT_EQUAL_UINT(2, r.effects.size());
    TEST_ASSERT_EQUAL(EffectKind::PublishState, r.effects[0].kind);
    TEST_ASSERT_EQUAL(EffectKind::PublishRemaining, r.effects[1].kind);
}

// start from Finished stops the end tone FIRST, then arms the run.
void test_S2_start_from_finished_stops_sound(void) {
    Inputs in;
    in.command = Command::Start;
    in.durationSec = 300;

    Result r = TimerRuntime::step(State{TimerState::Finished, 0}, 1000, in);

    TEST_ASSERT_EQUAL(Transition::ToRunning, r.transition);
    TEST_ASSERT_EQUAL_UINT(3, r.effects.size());
    TEST_ASSERT_EQUAL(EffectKind::StopSound, r.effects[0].kind);
    TEST_ASSERT_EQUAL(EffectKind::PublishState, r.effects[1].kind);
    TEST_ASSERT_EQUAL(EffectKind::PublishRemaining, r.effects[2].kind);
}

// start from Paused resumes: ToRunning with no StopSound (the adapter keeps the
// frozen remaining because the prior phase IS Paused).
void test_S3_start_from_paused_resumes(void) {
    Inputs in;
    in.command = Command::Start;
    in.durationSec = 300;

    Result r = TimerRuntime::step(State{TimerState::Paused, 240}, 1000, in);

    TEST_ASSERT_EQUAL(Transition::ToRunning, r.transition);
    TEST_ASSERT_EQUAL_UINT(2, r.effects.size());
    TEST_ASSERT_EQUAL(EffectKind::PublishState, r.effects[0].kind);
    TEST_ASSERT_EQUAL(EffectKind::PublishRemaining, r.effects[1].kind);
}

// start while already Running is a no-op — no transition, no effects.
void test_S4_start_while_running_noop(void) {
    Inputs in;
    in.command = Command::Start;
    in.durationSec = 300;

    Result r = TimerRuntime::step(State{TimerState::Running, 120}, 1000, in);

    TEST_ASSERT_EQUAL(Transition::None, r.transition);
    TEST_ASSERT_EQUAL_UINT(0, r.effects.size());
}

// pause from Running freezes to Paused, republishing the frozen run-state.
void test_P1_pause_from_running(void) {
    Inputs in;
    in.command = Command::Pause;
    in.newRemaining = 174;

    Result r = TimerRuntime::step(State{TimerState::Running, 175}, 1000, in);

    TEST_ASSERT_EQUAL(Transition::ToPaused, r.transition);
    TEST_ASSERT_EQUAL_UINT(2, r.effects.size());
    TEST_ASSERT_EQUAL(EffectKind::PublishState, r.effects[0].kind);
    TEST_ASSERT_EQUAL(EffectKind::PublishRemaining, r.effects[1].kind);
}

// pause while Paused resumes the run (ToRunning), same publishes as a start.
void test_P2_pause_while_paused_resumes(void) {
    Inputs in;
    in.command = Command::Pause;

    Result r = TimerRuntime::step(State{TimerState::Paused, 174}, 1000, in);

    TEST_ASSERT_EQUAL(Transition::ToRunning, r.transition);
    TEST_ASSERT_EQUAL_UINT(2, r.effects.size());
    TEST_ASSERT_EQUAL(EffectKind::PublishState, r.effects[0].kind);
    TEST_ASSERT_EQUAL(EffectKind::PublishRemaining, r.effects[1].kind);
}

// pause from Idle or Finished is a no-op.
void test_P3_pause_from_idle_or_finished_noop(void) {
    Inputs in;
    in.command = Command::Pause;

    Result idle = TimerRuntime::step(State{TimerState::Idle, 300}, 1000, in);
    TEST_ASSERT_EQUAL(Transition::None, idle.transition);
    TEST_ASSERT_EQUAL_UINT(0, idle.effects.size());

    Result fin = TimerRuntime::step(State{TimerState::Finished, 0}, 1000, in);
    TEST_ASSERT_EQUAL(Transition::None, fin.transition);
    TEST_ASSERT_EQUAL_UINT(0, fin.effects.size());
}

// reset from any phase returns to Idle: stop sound, republish, and (matrix dark)
// drop brightness to 0. Asserted from Running with the matrix off.
void test_X1_reset_stops_and_dims(void) {
    Inputs in;
    in.command = Command::Reset;
    in.durationSec = 300;
    in.matrixOff = true;

    Result r = TimerRuntime::step(State{TimerState::Running, 120}, 1000, in);

    TEST_ASSERT_EQUAL(Transition::ToIdle, r.transition);
    TEST_ASSERT_EQUAL_UINT(4, r.effects.size());
    TEST_ASSERT_EQUAL(EffectKind::StopSound, r.effects[0].kind);
    TEST_ASSERT_EQUAL(EffectKind::PublishState, r.effects[1].kind);
    TEST_ASSERT_EQUAL(EffectKind::PublishRemaining, r.effects[2].kind);
    TEST_ASSERT_EQUAL(EffectKind::SetBrightness, r.effects[3].kind);
    TEST_ASSERT_EQUAL_UINT(0, r.effects[3].brightness);
}

// reset with the matrix on omits the brightness effect.
void test_X2_reset_matrix_on_no_brightness(void) {
    Inputs in;
    in.command = Command::Reset;
    in.durationSec = 300;
    in.matrixOff = false;

    Result r = TimerRuntime::step(State{TimerState::Finished, 0}, 1000, in);

    TEST_ASSERT_EQUAL(Transition::ToIdle, r.transition);
    TEST_ASSERT_EQUAL_UINT(3, r.effects.size());
    TEST_ASSERT_EQUAL(EffectKind::StopSound, r.effects[0].kind);
}

// setDuration while Idle reloads remaining to the new duration and republishes it,
// staying Idle (IdleReload — no phase change).
void test_X3_setduration_idle_reloads_remaining(void) {
    Inputs in;
    in.command = Command::SetDuration;
    in.durationSec = 600;

    Result r = TimerRuntime::step(State{TimerState::Idle, 300}, 1000, in);

    TEST_ASSERT_EQUAL(Transition::IdleReload, r.transition);
    TEST_ASSERT_EQUAL_UINT(1, r.effects.size());
    TEST_ASSERT_EQUAL(EffectKind::PublishRemaining, r.effects[0].kind);
}

// US6: editing the duration while Paused resets cleanly to Idle with the new
// duration — the reset bundle + ToIdle, identical to an explicit reset.
void test_X4_setduration_paused_resets_to_idle(void) {
    Inputs in;
    in.command = Command::SetDuration;
    in.durationSec = 600;
    in.matrixOff = false;

    Result r = TimerRuntime::step(State{TimerState::Paused, 240}, 1000, in);

    TEST_ASSERT_EQUAL(Transition::ToIdle, r.transition);
    TEST_ASSERT_EQUAL_UINT(3, r.effects.size());
    TEST_ASSERT_EQUAL(EffectKind::StopSound, r.effects[0].kind);
    TEST_ASSERT_EQUAL(EffectKind::PublishState, r.effects[1].kind);
    TEST_ASSERT_EQUAL(EffectKind::PublishRemaining, r.effects[2].kind);
}

// setDuration while Running or Finished changes no run-state: no transition, no
// run-state effects (the adapter still persists + republishes the duration).
void test_X5_setduration_running_or_finished_no_transition(void) {
    Inputs in;
    in.command = Command::SetDuration;
    in.durationSec = 600;

    Result run = TimerRuntime::step(State{TimerState::Running, 120}, 1000, in);
    TEST_ASSERT_EQUAL(Transition::None, run.transition);
    TEST_ASSERT_EQUAL_UINT(0, run.effects.size());

    Result fin = TimerRuntime::step(State{TimerState::Finished, 0}, 1000, in);
    TEST_ASSERT_EQUAL(Transition::None, fin.transition);
    TEST_ASSERT_EQUAL_UINT(0, fin.effects.size());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_R1_finished_all_gates_open);
    RUN_TEST(test_R2_finished_all_gates_closed);
    RUN_TEST(test_R3_running_nonzero_no_transition);
    RUN_TEST(test_R4_nonrunning_zero_no_effects);
    RUN_TEST(test_B1_beep_gap_single_tone);
    RUN_TEST(test_C1_autoclear_hold_boundary);
    RUN_TEST(test_C2_autoclear_matrix_off_dims);
    RUN_TEST(test_D1_realert_interval_boundary);
    RUN_TEST(test_D2_realert_skips_tone_while_playing);
    RUN_TEST(test_S1_start_from_idle);
    RUN_TEST(test_S2_start_from_finished_stops_sound);
    RUN_TEST(test_S3_start_from_paused_resumes);
    RUN_TEST(test_S4_start_while_running_noop);
    RUN_TEST(test_P1_pause_from_running);
    RUN_TEST(test_P2_pause_while_paused_resumes);
    RUN_TEST(test_P3_pause_from_idle_or_finished_noop);
    RUN_TEST(test_X1_reset_stops_and_dims);
    RUN_TEST(test_X2_reset_matrix_on_no_brightness);
    RUN_TEST(test_X3_setduration_idle_reloads_remaining);
    RUN_TEST(test_X4_setduration_paused_resets_to_idle);
    RUN_TEST(test_X5_setduration_running_or_finished_no_transition);
    return UNITY_END();
}
