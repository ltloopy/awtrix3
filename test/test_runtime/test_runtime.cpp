// Unit tests for TimerRuntime (PRD #28, issue #178) — the pure run-state engine
// extracted from TimerManager's enterFinished decision.
//
// The whole point of the extraction: step() is a total function of
// (state, nowMs, inputs) that RETURNS an ordered Effect[] instead of touching
// hardware. These assert the exact effect sequence for the Running->Finished
// transition directly — no TimerManager singleton, no PeripheryManager /
// DisplayManager, no globals. The [env:native_runtime] link (which pulls in
// TimerRuntime.cpp and nothing else) is the architectural claim.

#include <unity.h>

#include "../../src/TimerRuntime.h"

using TimerRuntime::Effect;
using TimerRuntime::EffectKind;
using TimerRuntime::Inputs;
using TimerRuntime::State;
using TimerRuntime::Tone;

void setUp(void) {}
void tearDown(void) {}

// A Running timer that has just reached zero, with every gate open:
// navigation free, matrix off (so brightness is restored) and the end tone
// armed. The full effect set, in enterFinished's order.
void test_R1_finished_all_gates_open(void) {
    Inputs in;
    in.navigationFree = true;
    in.matrixOff = true;
    in.brightness = 42;
    in.playEndTone = true;

    std::vector<Effect> fx =
        TimerRuntime::step(State{TimerState::Running, 0}, 1000, in);

    TEST_ASSERT_EQUAL_UINT(5, fx.size());
    TEST_ASSERT_EQUAL(EffectKind::PublishState, fx[0].kind);
    TEST_ASSERT_EQUAL(EffectKind::PublishRemaining, fx[1].kind);
    TEST_ASSERT_EQUAL(EffectKind::SwitchToTimerApp, fx[2].kind);
    TEST_ASSERT_EQUAL(EffectKind::SetBrightness, fx[3].kind);
    TEST_ASSERT_EQUAL_UINT(42, fx[3].brightness);
    TEST_ASSERT_EQUAL(EffectKind::PlayTone, fx[4].kind);
    TEST_ASSERT_EQUAL(Tone::End, fx[4].tone);
}

// Every gate closed: navigation blocked (in a game/menu), matrix on, tone off.
// The two publishes are unconditional; nothing else fires.
void test_R2_finished_all_gates_closed(void) {
    Inputs in;
    in.navigationFree = false;
    in.matrixOff = false;
    in.playEndTone = false;

    std::vector<Effect> fx =
        TimerRuntime::step(State{TimerState::Running, 0}, 1000, in);

    TEST_ASSERT_EQUAL_UINT(2, fx.size());
    TEST_ASSERT_EQUAL(EffectKind::PublishState, fx[0].kind);
    TEST_ASSERT_EQUAL(EffectKind::PublishRemaining, fx[1].kind);
}

// A Running timer that has NOT reached zero is not the Finished transition —
// no effects. (The other slices will fill in the Running tick effects later.)
void test_R3_running_nonzero_no_effects(void) {
    Inputs in;
    in.navigationFree = true;
    in.matrixOff = true;
    in.playEndTone = true;

    std::vector<Effect> fx =
        TimerRuntime::step(State{TimerState::Running, 5}, 1000, in);

    TEST_ASSERT_EQUAL_UINT(0, fx.size());
}

// Zero remaining in a non-Running phase is not a transition this slice owns —
// only Running->Finished routes through step() today.
void test_R4_nonrunning_zero_no_effects(void) {
    Inputs in;
    in.navigationFree = true;
    in.matrixOff = true;
    in.playEndTone = true;

    TEST_ASSERT_EQUAL_UINT(0, TimerRuntime::step(State{TimerState::Idle, 0}, 1000, in).size());
    TEST_ASSERT_EQUAL_UINT(0, TimerRuntime::step(State{TimerState::Paused, 0}, 1000, in).size());
    TEST_ASSERT_EQUAL_UINT(0, TimerRuntime::step(State{TimerState::Finished, 0}, 1000, in).size());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_R1_finished_all_gates_open);
    RUN_TEST(test_R2_finished_all_gates_closed);
    RUN_TEST(test_R3_running_nonzero_no_effects);
    RUN_TEST(test_R4_nonrunning_zero_no_effects);
    return UNITY_END();
}
