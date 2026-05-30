// Starter unit tests U1–U8 for TimerManager. Proves every layer of the
// native test scaffolding: ArduinoFake millis mocking, recording-mock
// ordering (MQTT), stateful fakes (notifications, PeripheryManager),
// Preferences round-trip, and the AutoClear lifecycle end-to-end.

#include <unity.h>
#include <ArduinoFake.h>
#include <string.h>

#include "fixture.h"
#include "../../src/TimerHa.h"
#include "../../src/TimerView.h"

void setUp(void) {
    fixture::reset_all();
    // setup() loads from Preferences (fresh map after reset → defaults apply:
    // durationSec=300, buzzerMode=End, finishedMode=AutoClear) and sets
    // state=Idle. This effectively returns the singleton to a clean state.
    TimerManager.setup();
    // setup() doesn't publish, but clear recordings to make per-test
    // assertions count only post-setup activity.
    MQTTManager.__test_reset();
    PeripheryManager.__test_reset();
}

void tearDown(void) {}

// ============================================================================
// U1 — setDuration clamping
// Proves: Globals + Preferences fake wired correctly; publishDuration recorded.
// ============================================================================
void test_U1_setDuration_clamps_low_and_high(void) {
    TimerManager.setDuration(0);
    TEST_ASSERT_EQUAL_UINT32(1, TimerManager.getDuration());
    const PublishCall *low = fixture::last_publish(PublishCall::Duration);
    TEST_ASSERT_NOT_NULL(low);
    TEST_ASSERT_EQUAL_UINT32(1, low->value);

    TimerManager.setDuration(99999);
    TEST_ASSERT_EQUAL_UINT32(TIMER_MAX_DURATION, TimerManager.getDuration());
    const PublishCall *high = fixture::last_publish(PublishCall::Duration);
    TEST_ASSERT_NOT_NULL(high);
    TEST_ASSERT_EQUAL_UINT32(TIMER_MAX_DURATION, high->value);

    // Preferences round-trip: simulate reboot by calling setup() again.
    TimerManager.setup();
    TEST_ASSERT_EQUAL_UINT32(TIMER_MAX_DURATION, TimerManager.getDuration());
}

// ============================================================================
// U2 — parseCommand null/empty/garbage safety
// Proves: null-safety, parser resilience; subsequent valid command works.
// ============================================================================
void test_U2_parseCommand_null_empty_garbage_are_noops(void) {
    TimerManager.parseCommand(nullptr);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));

    TimerManager.parseCommand("");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));

    TimerManager.parseCommand("{garbage");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));

    // No MQTT publish should have occurred for any of the bad inputs.
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(PublishCall::State));

    // A subsequent valid command works.
    TimerManager.parseCommand("{\"action\":\"start\"}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running),
                      static_cast<int>(TimerManager.getState()));
}

// ============================================================================
// U3 — start() from Idle publishes state-then-remaining in that exact order
// Proves: recording-mock ordering captures the wire contract HA depends on.
// ============================================================================
void test_U3_start_from_idle_publishes_state_then_remaining(void) {
    TimerManager.start();
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running),
                      static_cast<int>(TimerManager.getState()));

    // The contract: publishState("running") MUST precede publishRemaining(...).
    // Find the index of the first State and first Remaining publish post-start.
    int stateIdx = -1, remIdx = -1;
    for (size_t i = 0; i < MQTTManager.recorded.size(); ++i) {
        const auto &c = MQTTManager.recorded[i];
        if (stateIdx < 0 && c.kind == PublishCall::State    && c.state_str == "running") stateIdx = (int)i;
        if (remIdx   < 0 && c.kind == PublishCall::Remaining && c.value == 300)          remIdx   = (int)i;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, stateIdx);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, remIdx);
    TEST_ASSERT_LESS_THAN_INT(remIdx, stateIdx);  // state must come first
}

// ============================================================================
// U4 — tick across zero enters Finished, plays end melody once, no overlay
// Proves: enterFinished() plays sound inline and no overlay notification is
// pushed (the rendering now lives in TimerApp itself).
// ============================================================================
void test_U4_tick_crosses_zero_enters_finished(void) {
    TimerManager.setDuration(5);
    TimerManager.start();
    TEST_ASSERT_EQUAL_UINT32(5, TimerManager.getRemaining());

    fixture::advance(5000);
    TimerManager.tick();

    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Finished),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_UINT32(0, TimerManager.getRemaining());

    // No overlay notification pushed — rendering happens inside TimerApp now.
    TEST_ASSERT_EQUAL_size_t(0, notifications.size());

    // End melody plays exactly once on entering Finished.
    TEST_ASSERT_EQUAL_size_t(1, PeripheryManager.play_calls.size());
}

// ============================================================================
// U5 — AutoClear path returns to Idle after TIMER_FINISHED_HOLD
// Proves: AutoClear lifecycle without the notification path.
// ============================================================================
void test_U5_autoclear_returns_to_idle_after_hold(void) {
    // Arrange: a freshly-fired finished AutoClear timer.
    TimerManager.setDuration(2);
    TimerManager.start();
    fixture::advance(2000);
    TimerManager.tick();
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Finished),
                      static_cast<int>(TimerManager.getState()));

    // Act: advance the AutoClear hold window and tick.
    fixture::advance(static_cast<uint32_t>(TIMER_FINISHED_HOLD) * 1000U + 100U);
    TimerManager.tick();

    // Assert: state returned to Idle; last state publish == "idle".
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));
    const PublishCall *last = fixture::last_publish(PublishCall::State);
    TEST_ASSERT_NOT_NULL(last);
    TEST_ASSERT_EQUAL_STRING("idle", last->state_str.c_str());
}

// ============================================================================
// U6 — parseCommand is a no-op when SHOW_TIMER is false
// Proves: the disable flag short-circuits HTTP + MQTT command surfaces.
// ============================================================================
void test_U6_parseCommand_noop_when_disabled(void) {
    SHOW_TIMER = false;

    // Disabled returns TimerCmdResult::Disabled (HTTP maps it to 409) and applies nothing.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Disabled),
                      static_cast<int>(TimerManager.parseCommand("{\"action\":\"start\"}")));
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));

    uint32_t baseline = TimerManager.getDuration();
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Disabled),
                      static_cast<int>(TimerManager.parseCommand("{\"duration\":120}")));
    TEST_ASSERT_EQUAL_UINT32(baseline, TimerManager.getDuration());

    // Nothing published to HA from the gated commands.
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(PublishCall::State));
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(PublishCall::Duration));

    // Re-enabling restores command processing (returns Ok).
    SHOW_TIMER = true;
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"action\":\"start\"}")));
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running),
                      static_cast<int>(TimerManager.getState()));
}

// ============================================================================
// U7 — onShowTimerChange resets a running timer on the true→false transition
// Proves: disabling at runtime cleanly stops in-flight timers (no surprise alerts).
// ============================================================================
void test_U7_onShowTimerChange_resets_running_timer(void) {
    TimerManager.setDuration(60);
    TimerManager.start();
    fixture::advance(10000);
    TimerManager.tick();
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running),
                      static_cast<int>(TimerManager.getState()));

    TimerManager.onShowTimerChange(true, false);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_UINT32(60, TimerManager.getRemaining());

    // Non-disable transitions are no-ops.
    TimerManager.setDuration(30);
    TimerManager.start();
    TimerManager.onShowTimerChange(false, true);  // re-enable; running timer untouched
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running),
                      static_cast<int>(TimerManager.getState()));
    TimerManager.onShowTimerChange(true, true);   // no change; untouched
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running),
                      static_cast<int>(TimerManager.getState()));
}

// ============================================================================
// U8 — ReAlert replays the end melody at TIMER_REALERT_INTERVAL
// Proves: ReAlert keeps retriggering single-play replays until cleared.
// ============================================================================
void test_U8_realert_replays_at_interval(void) {
    TimerManager.setFinishedMode(FinishedMode::ReAlert);
    TimerManager.setDuration(2);
    TimerManager.start();

    // Cross zero → initial play.
    fixture::advance(2000);
    TimerManager.tick();
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Finished),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_size_t(1, PeripheryManager.play_calls.size());

    // The previous play left the player "playing". ReAlert skips replay
    // while still playing, so simulate the melody having ended.
    PeripheryManager.__test_set_playing(false);

    // First re-alert after TIMER_REALERT_INTERVAL.
    fixture::advance(static_cast<uint32_t>(TIMER_REALERT_INTERVAL) * 1000U + 100U);
    TimerManager.tick();
    TEST_ASSERT_EQUAL_size_t(2, PeripheryManager.play_calls.size());

    PeripheryManager.__test_set_playing(false);

    // Second re-alert another TIMER_REALERT_INTERVAL later.
    fixture::advance(static_cast<uint32_t>(TIMER_REALERT_INTERVAL) * 1000U + 100U);
    TimerManager.tick();
    TEST_ASSERT_EQUAL_size_t(3, PeripheryManager.play_calls.size());
}

// ============================================================================
// U9 — getIconForState: all slots empty returns empty
// ============================================================================
void test_U9_getIconForState_all_empty(void) {
    TimerManager.setIconIdle("",     false);
    TimerManager.setIconRunning("",  false);
    TimerManager.setIconPaused("",   false);
    TimerManager.setIconFinished("", false);
    TEST_ASSERT_EQUAL_STRING("", TimerManager.getIconForState(TimerState::Idle).c_str());
    TEST_ASSERT_EQUAL_STRING("", TimerManager.getIconForState(TimerState::Running).c_str());
    TEST_ASSERT_EQUAL_STRING("", TimerManager.getIconForState(TimerState::Paused).c_str());
    TEST_ASSERT_EQUAL_STRING("", TimerManager.getIconForState(TimerState::Finished).c_str());
}

// ============================================================================
// U10 — getIconForState: only Idle set inherits for all states
// ============================================================================
void test_U10_getIconForState_only_idle_inherits(void) {
    TimerManager.setIconIdle("64936", false);
    TimerManager.setIconRunning("",   false);
    TimerManager.setIconPaused("",    false);
    TimerManager.setIconFinished("",  false);
    TEST_ASSERT_EQUAL_STRING("64936", TimerManager.getIconForState(TimerState::Idle).c_str());
    TEST_ASSERT_EQUAL_STRING("64936", TimerManager.getIconForState(TimerState::Running).c_str());
    TEST_ASSERT_EQUAL_STRING("64936", TimerManager.getIconForState(TimerState::Paused).c_str());
    TEST_ASSERT_EQUAL_STRING("64936", TimerManager.getIconForState(TimerState::Finished).c_str());
}

// ============================================================================
// U11 — getIconForState: per-state set returns own value; empty falls to Idle
// ============================================================================
void test_U11_getIconForState_per_state_with_idle_fallback(void) {
    TimerManager.setIconIdle("64936",    false);
    TimerManager.setIconRunning("74706", false);
    TimerManager.setIconPaused("",       false);
    TimerManager.setIconFinished("9999", false);
    TEST_ASSERT_EQUAL_STRING("64936", TimerManager.getIconForState(TimerState::Idle).c_str());
    TEST_ASSERT_EQUAL_STRING("74706", TimerManager.getIconForState(TimerState::Running).c_str());
    TEST_ASSERT_EQUAL_STRING("64936", TimerManager.getIconForState(TimerState::Paused).c_str());
    TEST_ASSERT_EQUAL_STRING("9999",  TimerManager.getIconForState(TimerState::Finished).c_str());
}

// ============================================================================
// U12 — reset() from Finished stops sound and returns to Idle
// Proves: the middle short-press dispatch from Finished cleanly dismisses
// the alert (stopSound() exactly once, state → Idle, remaining re-armed,
// state publish == "idle").
// ============================================================================
void test_U12_reset_from_finished_stops_sound_and_returns_idle(void) {
    TimerManager.setDuration(5);
    TimerManager.start();
    fixture::advance(5000);
    TimerManager.tick();
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Finished),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_size_t(1, PeripheryManager.play_calls.size());

    PeripheryManager.__test_set_playing(true);

    TimerManager.reset();

    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_UINT32(5, TimerManager.getRemaining());
    TEST_ASSERT_EQUAL_INT(1, PeripheryManager.stop_calls);
    TEST_ASSERT_FALSE(PeripheryManager.isPlaying());
    const PublishCall *last = fixture::last_publish(PublishCall::State);
    TEST_ASSERT_NOT_NULL(last);
    TEST_ASSERT_EQUAL_STRING("idle", last->state_str.c_str());
}

// ============================================================================
// U13 — start() from Finished stops sound and re-arms to Running
// Proves: the middle long-press dispatch from Finished dismisses the alert
// and immediately re-arms (stopSound() exactly once, state → Running,
// remaining == durationSec, state publish == "running").
// ============================================================================
void test_U13_start_from_finished_stops_sound_and_rearms(void) {
    TimerManager.setDuration(5);
    TimerManager.start();
    fixture::advance(5000);
    TimerManager.tick();
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Finished),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_size_t(1, PeripheryManager.play_calls.size());

    PeripheryManager.__test_set_playing(true);

    TimerManager.start();

    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_UINT32(5, TimerManager.getRemaining());
    TEST_ASSERT_EQUAL_INT(1, PeripheryManager.stop_calls);
    TEST_ASSERT_FALSE(PeripheryManager.isPlaying());
    const PublishCall *last = fixture::last_publish(PublishCall::State);
    TEST_ASSERT_NOT_NULL(last);
    TEST_ASSERT_EQUAL_STRING("running", last->state_str.c_str());
}

// ============================================================================
// U14 — Config wheels respect default TIMER_MAX_DURATION (86400 = 24h)
// HH is capped at 23 when MM=SS=59; +1 past the cap wraps to 0.
// ============================================================================
void test_U14_config_default_cap_wraps_HH_at_23(void) {
    TimerManager.setDuration(86399);
    TimerManager.enterConfigMode();
    TEST_ASSERT_TRUE(TimerManager.isInConfig());
    TEST_ASSERT_EQUAL_UINT8(23, TimerManager.getConfigHH());
    TEST_ASSERT_EQUAL_UINT8(59, TimerManager.getConfigMM());
    TEST_ASSERT_EQUAL_UINT8(59, TimerManager.getConfigSS());
    TEST_ASSERT_EQUAL_UINT8(0,  TimerManager.getConfigField());

    // dynMax(HH | MM=59,SS=59) = (86400-3599)/3600 = 23. cur=23, +1 → 24 > 23 → 0.
    TimerManager.configAdjust(+1);
    TEST_ASSERT_EQUAL_UINT8(0, TimerManager.getConfigHH());

    // From 0, +1 returns to 1 (cap still 23).
    TimerManager.configAdjust(+1);
    TEST_ASSERT_EQUAL_UINT8(1, TimerManager.getConfigHH());
}

// ============================================================================
// U15 — Tight cap (TIMER_MAX_DURATION=3600) caps HH at 1 with MM=SS=0
// And recomputes the cap when other fields move.
// ============================================================================
void test_U15_config_tight_cap_HH_at_1_then_MM_at_0(void) {
    TIMER_MAX_DURATION = 3600;
    TimerManager.setDuration(1);
    TimerManager.enterConfigMode();
    // From setDuration(1): HH=0, MM=0, SS=1, field=HH.

    // Zero out SS so other-field math is clean.
    TimerManager.configCycleField();   // → MM
    TimerManager.configCycleField();   // → SS
    TimerManager.configAdjust(-1);     // SS 1→0
    TEST_ASSERT_EQUAL_UINT8(0, TimerManager.getConfigSS());
    TimerManager.configCycleField();   // → HH

    // dynMax(HH | MM=0,SS=0) = 3600/3600 = 1. 0→1.
    TimerManager.configAdjust(+1);
    TEST_ASSERT_EQUAL_UINT8(1, TimerManager.getConfigHH());

    // cur=1, +1 → 2 > 1 → wrap to 0.
    TimerManager.configAdjust(+1);
    TEST_ASSERT_EQUAL_UINT8(0, TimerManager.getConfigHH());

    // Bring HH back to 1, then cycle to MM. With HH=1, dynMax(MM) = 0 → wrap.
    TimerManager.configAdjust(+1);
    TEST_ASSERT_EQUAL_UINT8(1, TimerManager.getConfigHH());
    TimerManager.configCycleField();   // → MM
    TimerManager.configAdjust(+1);
    TEST_ASSERT_EQUAL_UINT8(0, TimerManager.getConfigMM());
}

// ============================================================================
// U16 — TIMER_MAX_DURATION == 0 means no cap (legacy 99/59/59 wrap)
// ============================================================================
void test_U16_config_no_cap_when_max_is_zero(void) {
    TIMER_MAX_DURATION = 0;
    TimerManager.setDuration(1);
    TimerManager.enterConfigMode();

    // HH at 0 → 99 increments climbs to 99; one more wraps to 0.
    for (int i = 0; i < 99; ++i) TimerManager.configAdjust(+1);
    TEST_ASSERT_EQUAL_UINT8(99, TimerManager.getConfigHH());
    TimerManager.configAdjust(+1);
    TEST_ASSERT_EQUAL_UINT8(0, TimerManager.getConfigHH());
}

// ============================================================================
// U17 — Decrement at 0 wraps to the dynamic cap
// ============================================================================
void test_U17_config_decrement_wraps_to_dynamic_max(void) {
    TIMER_MAX_DURATION = 3600;
    TimerManager.setDuration(1);
    TimerManager.enterConfigMode();

    // Zero SS so otherSec=0 when editing HH.
    TimerManager.configCycleField();  // MM
    TimerManager.configCycleField();  // SS
    TimerManager.configAdjust(-1);    // SS 1→0
    TimerManager.configCycleField();  // HH

    // HH=0, dynMax=1, delta=-1 → wraps to 1.
    TimerManager.configAdjust(-1);
    TEST_ASSERT_EQUAL_UINT8(1, TimerManager.getConfigHH());
}

// ============================================================================
// U18 — Exit-time setDuration backstop never has to clamp (cap held mid-edit)
// ============================================================================
void test_U18_config_exit_value_already_within_cap(void) {
    TIMER_MAX_DURATION = 3600;
    TimerManager.setDuration(1);
    TimerManager.enterConfigMode();

    // Drive HH up to its cap given MM=0,SS=1: dynMax = (3600-1)/3600 = 0.
    TimerManager.configAdjust(+1);
    TEST_ASSERT_EQUAL_UINT8(0, TimerManager.getConfigHH());

    // Now zero SS, then push HH to 1 (now allowed).
    TimerManager.configCycleField();  // MM
    TimerManager.configCycleField();  // SS
    TimerManager.configAdjust(-1);    // SS 1→0
    TimerManager.configCycleField();  // HH
    TimerManager.configAdjust(+1);    // HH 0→1
    TEST_ASSERT_EQUAL_UINT8(1, TimerManager.getConfigHH());

    TimerManager.exitConfigMode();
    TEST_ASSERT_FALSE(TimerManager.isInConfig());
    TEST_ASSERT_EQUAL_UINT32(3600, TimerManager.getDuration());
}

// ============================================================================
// U19 (M2) — enterConfigMode clamps durationSec to 99h to keep HH editable
// ============================================================================
void test_U19_enterConfig_clamps_duration_at_99h(void) {
    TIMER_MAX_DURATION = 0;  // disable the upper cap so we can install a huge value
    TimerManager.setDuration(99UL * 3600UL + 1UL * 3600UL + 30UL);  // 100h00m30s
    TimerManager.enterConfigMode();
    TEST_ASSERT_TRUE(TimerManager.isInConfig());
    TEST_ASSERT_EQUAL_UINT8(99, TimerManager.getConfigHH());
    TEST_ASSERT_EQUAL_UINT32(99UL * 3600UL, TimerManager.getDuration());
}

// ============================================================================
// U20 (H1) — icon name validation: strict whitelist
// ============================================================================
void test_U20_icon_name_validation(void) {
    // Valid: alnum + _ - up to 32 chars
    TimerManager.setIconRunning("Run_01-2", false);
    TEST_ASSERT_EQUAL_STRING("Run_01-2", TimerManager.getIconRunning().c_str());

    // Empty is allowed (clears).
    TimerManager.setIconRunning("", false);
    TEST_ASSERT_EQUAL_STRING("", TimerManager.getIconRunning().c_str());

    // Path traversal: rejected, value unchanged.
    TimerManager.setIconRunning("ok_name", false);
    TimerManager.setIconRunning("../etc/passwd", false);
    TEST_ASSERT_EQUAL_STRING("ok_name", TimerManager.getIconRunning().c_str());

    // Slash: rejected.
    TimerManager.setIconRunning("foo/bar", false);
    TEST_ASSERT_EQUAL_STRING("ok_name", TimerManager.getIconRunning().c_str());

    // Spaces: rejected.
    TimerManager.setIconRunning("has space", false);
    TEST_ASSERT_EQUAL_STRING("ok_name", TimerManager.getIconRunning().c_str());

    // Over-length (33 chars): rejected.
    TimerManager.setIconRunning("aaaaaaaaaabbbbbbbbbbccccccccccddd", false);
    TEST_ASSERT_EQUAL_STRING("ok_name", TimerManager.getIconRunning().c_str());
}

// ============================================================================
// U21 (C4) — parseCommand batches NVS writes into one persist()
// Baseline: setup() calls Preferences.begin() once. A multi-field command
// should add exactly one more begin() call (the persist at the end), not one
// per field.
// ============================================================================
void test_U21_parseCommand_batches_persist(void) {
    int begin_at_start = Preferences::begin_calls;

    TimerManager.parseCommand("{\"duration\":600,\"buzzer\":\"end\",\"finished\":\"hold\","
                              "\"icon_idle\":\"a\",\"icon_running\":\"b\"}");

    int delta = Preferences::begin_calls - begin_at_start;
    // One persist call (begin + 7 puts + end) for all 5 changed fields.
    TEST_ASSERT_EQUAL_INT(1, delta);
    // Values applied:
    TEST_ASSERT_EQUAL_UINT32(600, TimerManager.getDuration());
    TEST_ASSERT_EQUAL(static_cast<int>(BuzzerMode::End),
                      static_cast<int>(TimerManager.getBuzzerMode()));
    TEST_ASSERT_EQUAL(static_cast<int>(FinishedMode::Hold),
                      static_cast<int>(TimerManager.getFinishedMode()));
    TEST_ASSERT_EQUAL_STRING("a", TimerManager.getIconIdle().c_str());
    TEST_ASSERT_EQUAL_STRING("b", TimerManager.getIconRunning().c_str());
}

// ============================================================================
// U22 (M1) — parseCommand while in config: drop partial edit, drain deferred,
// accept command. The partial edit must NOT be committed to durationSec.
// ============================================================================
void test_U22_parseCommand_aborts_config_without_committing_edit(void) {
    TimerManager.setDuration(300);
    TimerManager.enterConfigMode();
    // Move HH up so the partial edit (if committed) would diverge from 300s.
    TimerManager.configAdjust(+1);  // HH 0 -> 1, total would be 3600 + (300%3600)
    TEST_ASSERT_TRUE(TimerManager.isInConfig());

    int drain_before = DisplayManager.drain_calls;

    TimerManager.parseCommand("{\"action\":\"start\"}");

    // Config exited.
    TEST_ASSERT_FALSE(TimerManager.isInConfig());
    // Deferred notifications drained (M1 invariant).
    TEST_ASSERT_EQUAL_INT(drain_before + 1, DisplayManager.drain_calls);
    // The on-device edit was DISCARDED — duration unchanged.
    TEST_ASSERT_EQUAL_UINT32(300, TimerManager.getDuration());
    // The command was honored.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running),
                      static_cast<int>(TimerManager.getState()));
}

// ============================================================================
// U23 — formatHMS emits a trimmed clock string (hours dropped when zero,
// most-significant shown field unpadded, lower fields zero-padded).
// ============================================================================
void test_U23_formatHMS_trimmed(void) {
    TEST_ASSERT_EQUAL_STRING("0:00",     TimerManager_::formatHMS(0).c_str());
    TEST_ASSERT_EQUAL_STRING("0:45",     TimerManager_::formatHMS(45).c_str());
    TEST_ASSERT_EQUAL_STRING("3:00",     TimerManager_::formatHMS(180).c_str());
    TEST_ASSERT_EQUAL_STRING("5:05",     TimerManager_::formatHMS(305).c_str());
    TEST_ASSERT_EQUAL_STRING("1:00:00",  TimerManager_::formatHMS(3600).c_str());
    TEST_ASSERT_EQUAL_STRING("1:01:01",  TimerManager_::formatHMS(3661).c_str());
    TEST_ASSERT_EQUAL_STRING("10:00:00", TimerManager_::formatHMS(36000).c_str());
}

// ============================================================================
// U24 — parseHMS accepts bare seconds, MM:SS, HH:MM:SS, carries out-of-range
// fields, and trims surrounding whitespace.
// ============================================================================
void test_U24_parseHMS_accepts(void) {
    uint32_t s = 0;
    TEST_ASSERT_TRUE(TimerManager_::parseHMS("00:05:00", s)); TEST_ASSERT_EQUAL_UINT32(300, s);
    TEST_ASSERT_TRUE(TimerManager_::parseHMS("3:00", s));     TEST_ASSERT_EQUAL_UINT32(180, s);   // MM:SS
    TEST_ASSERT_TRUE(TimerManager_::parseHMS("90", s));       TEST_ASSERT_EQUAL_UINT32(90, s);    // bare seconds
    TEST_ASSERT_TRUE(TimerManager_::parseHMS("3:90", s));     TEST_ASSERT_EQUAL_UINT32(270, s);   // carry, no 0-59 cap
    TEST_ASSERT_TRUE(TimerManager_::parseHMS(" 1:00:00 ", s));TEST_ASSERT_EQUAL_UINT32(3600, s);  // trims
    TEST_ASSERT_TRUE(TimerManager_::parseHMS("0:45", s));     TEST_ASSERT_EQUAL_UINT32(45, s);
}

// ============================================================================
// U25 — parseHMS rejects empty fields, >2 colons, non-numeric, and empty input.
// ============================================================================
void test_U25_parseHMS_rejects(void) {
    uint32_t s = 12345;  // sentinel; must be left untouched on reject
    TEST_ASSERT_FALSE(TimerManager_::parseHMS("aa:bb", s));
    TEST_ASSERT_FALSE(TimerManager_::parseHMS("5:", s));
    TEST_ASSERT_FALSE(TimerManager_::parseHMS(":30", s));
    TEST_ASSERT_FALSE(TimerManager_::parseHMS("1:2:3:4", s));
    TEST_ASSERT_FALSE(TimerManager_::parseHMS("", s));
    TEST_ASSERT_FALSE(TimerManager_::parseHMS("   ", s));
    TEST_ASSERT_EQUAL_UINT32(12345, s);
}

// ============================================================================
// U26 — parseCommand "duration" accepts a clock string or a numeric value;
// both resolve to the same seconds. Bad string is ignored (duration unchanged).
// ============================================================================
void test_U26_parseCommand_duration_string_and_number(void) {
    TimerManager.parseCommand("{\"duration\":\"00:05:00\"}");
    TEST_ASSERT_EQUAL_UINT32(300, TimerManager.getDuration());

    TimerManager.parseCommand("{\"duration\":600}");
    TEST_ASSERT_EQUAL_UINT32(600, TimerManager.getDuration());

    TimerManager.parseCommand("{\"duration\":\"3:00\"}");   // MM:SS
    TEST_ASSERT_EQUAL_UINT32(180, TimerManager.getDuration());

    // Malformed string is ignored: duration stays at the last good value.
    TimerManager.parseCommand("{\"duration\":\"banana\"}");
    TEST_ASSERT_EQUAL_UINT32(180, TimerManager.getDuration());
}

// ============================================================================
// U27 — config round-trip is unchanged after unifying onto the shared helpers
// (regression guard: enter decomposes seconds, exit recomposes them).
// ============================================================================
void test_U27_config_roundtrip_unchanged(void) {
    TimerManager.setDuration(3661);  // 1:01:01
    TimerManager.enterConfigMode();
    TEST_ASSERT_EQUAL_UINT8(1, TimerManager.getConfigHH());
    TEST_ASSERT_EQUAL_UINT8(1, TimerManager.getConfigMM());
    TEST_ASSERT_EQUAL_UINT8(1, TimerManager.getConfigSS());
    TimerManager.exitConfigMode();
    TEST_ASSERT_EQUAL_UINT32(3661, TimerManager.getDuration());
}

// ============================================================================
// U28 — isValidDuration: range gate used by the reject-everywhere policy.
// ============================================================================
void test_U28_isValidDuration(void) {
    TIMER_MAX_DURATION = 86400;
    TEST_ASSERT_FALSE(TimerManager_::isValidDuration(0));
    TEST_ASSERT_TRUE (TimerManager_::isValidDuration(1));
    TEST_ASSERT_TRUE (TimerManager_::isValidDuration(86400));
    TEST_ASSERT_FALSE(TimerManager_::isValidDuration(86401));

    TIMER_MAX_DURATION = 0;  // 0 = no upper cap
    TEST_ASSERT_TRUE (TimerManager_::isValidDuration(999999));
    TEST_ASSERT_FALSE(TimerManager_::isValidDuration(0));

    TIMER_MAX_DURATION = 86400;  // restore default for later tests
}

// ============================================================================
// U29 — parseCommand is always strict: Ok / BadJson / BadField, and out-of-range
// is REJECTED (not clamped). Invalid input leaves duration unchanged.
// ============================================================================
void test_U29_parseCommand_strict_results(void) {
    SHOW_TIMER = true;
    TIMER_MAX_DURATION = 86400;

    // Valid: string and numeric both apply.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"duration\":\"00:05:00\"}")));
    TEST_ASSERT_EQUAL_UINT32(300, TimerManager.getDuration());
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"duration\":600}")));
    TEST_ASSERT_EQUAL_UINT32(600, TimerManager.getDuration());

    // BadJson: unparseable / empty.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadJson),
                      static_cast<int>(TimerManager.parseCommand("{garbage")));
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadJson),
                      static_cast<int>(TimerManager.parseCommand("")));

    // BadField: malformed duration string — duration unchanged.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"duration\":\"banana\"}")));
    TEST_ASSERT_EQUAL_UINT32(600, TimerManager.getDuration());

    // BadField: out-of-range numeric is REJECTED, NOT clamped to MAX.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"duration\":99999}")));
    TEST_ASSERT_EQUAL_UINT32(600, TimerManager.getDuration());

    // BadField: zero, and an unknown enum value.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"duration\":0}")));
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"buzzer\":\"nope\"}")));
    TEST_ASSERT_EQUAL_UINT32(600, TimerManager.getDuration());
}

// ============================================================================
// U30 — Atomicity: a command with one bad field applies NOTHING (the valid
// action in the same payload must not run).
// ============================================================================
void test_U30_parseCommand_atomic_reject(void) {
    SHOW_TIMER = true;
    TIMER_MAX_DURATION = 86400;
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"action\":\"start\",\"buzzer\":\"nope\"}")));
    // The action did NOT run — still Idle.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));
}

// ============================================================================
// U31 — Config-abort is atomic (#11): a rejected command leaves an in-progress
// on-device edit intact; a valid command aborts config and applies.
// ============================================================================
void test_U31_config_abort_only_on_valid_command(void) {
    SHOW_TIMER = true;
    TIMER_MAX_DURATION = 86400;
    TimerManager.setDuration(300);
    TimerManager.enterConfigMode();
    TEST_ASSERT_TRUE(TimerManager.isInConfig());

    // Rejected command: config edit untouched.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"duration\":\"banana\"}")));
    TEST_ASSERT_TRUE(TimerManager.isInConfig());

    // Valid command: config aborts and the value applies.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"duration\":\"00:01:00\"}")));
    TEST_ASSERT_FALSE(TimerManager.isInConfig());
    TEST_ASSERT_EQUAL_UINT32(60, TimerManager.getDuration());
}

// ============================================================================
// U36 — parseCommand accepts the three tuning-knob keys in range and writes
// them to the awtrix-namespace globals. Per ADR-0003 these keys exist so the
// timer command surface has parity with the on-device TIMER menu.
// ============================================================================
void test_U36_parseCommand_tuning_keys_accepted_in_range(void) {
    SHOW_TIMER = true;

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"finished_hold\":42,\"realert_interval\":30,\"countdown_seconds\":5}")));
    TEST_ASSERT_EQUAL_UINT16(42, TIMER_FINISHED_HOLD);
    TEST_ASSERT_EQUAL_UINT16(30, TIMER_REALERT_INTERVAL);
    TEST_ASSERT_EQUAL_UINT16(5,  TIMER_COUNTDOWN_SECONDS);

    // Range boundaries — all min/max should accept.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"finished_hold\":1,\"realert_interval\":5,\"countdown_seconds\":0}")));
    TEST_ASSERT_EQUAL_UINT16(1, TIMER_FINISHED_HOLD);
    TEST_ASSERT_EQUAL_UINT16(5, TIMER_REALERT_INTERVAL);
    TEST_ASSERT_EQUAL_UINT16(0, TIMER_COUNTDOWN_SECONDS);

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"finished_hold\":300,\"realert_interval\":300,\"countdown_seconds\":30}")));
    TEST_ASSERT_EQUAL_UINT16(300, TIMER_FINISHED_HOLD);
    TEST_ASSERT_EQUAL_UINT16(300, TIMER_REALERT_INTERVAL);
    TEST_ASSERT_EQUAL_UINT16(30,  TIMER_COUNTDOWN_SECONDS);
}

// ============================================================================
// U37 — Out-of-range tuning values are REJECTED atomically per ADR-0001: the
// whole command is rejected and no field (including a valid action) applies.
// ============================================================================
void test_U37_parseCommand_tuning_keys_atomic_reject_out_of_range(void) {
    SHOW_TIMER = true;
    TIMER_FINISHED_HOLD     = 10;
    TIMER_REALERT_INTERVAL  = 15;
    TIMER_COUNTDOWN_SECONDS = 3;

    // finished_hold below min (1).
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"finished_hold\":0}")));
    TEST_ASSERT_EQUAL_UINT16(10, TIMER_FINISHED_HOLD);

    // finished_hold above max (300).
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"finished_hold\":301}")));
    TEST_ASSERT_EQUAL_UINT16(10, TIMER_FINISHED_HOLD);

    // realert_interval below min (5).
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"realert_interval\":4}")));
    TEST_ASSERT_EQUAL_UINT16(15, TIMER_REALERT_INTERVAL);

    // countdown_seconds above max (30).
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"countdown_seconds\":31}")));
    TEST_ASSERT_EQUAL_UINT16(3, TIMER_COUNTDOWN_SECONDS);

    // Non-numeric type → BadField.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"finished_hold\":\"ten\"}")));
    TEST_ASSERT_EQUAL_UINT16(10, TIMER_FINISHED_HOLD);

    // Atomicity: one bad tuning field rejects the whole command — paired action
    // does not run, paired good fields don't apply.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"action\":\"start\",\"finished_hold\":50,\"countdown_seconds\":99}")));
    TEST_ASSERT_EQUAL_UINT16(10, TIMER_FINISHED_HOLD);
    TEST_ASSERT_EQUAL_UINT16(3,  TIMER_COUNTDOWN_SECONDS);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));
}

// ============================================================================
// U38 — A tuning-key command persists via saveSettings(); a command with only
// modes/duration/action does NOT (those go through TimerManager.persist()).
// ============================================================================
void test_U38_parseCommand_tuning_change_persists_via_saveSettings(void) {
    SHOW_TIMER = true;

    saveSettings_calls = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"finished_hold\":42}")));
    TEST_ASSERT_EQUAL_INT(1, saveSettings_calls);

    // Mode-only / duration-only commands do not touch awtrix Settings.
    saveSettings_calls = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"buzzer\":\"off\",\"duration\":120}")));
    TEST_ASSERT_EQUAL_INT(0, saveSettings_calls);

    // Rejected command does not call saveSettings.
    saveSettings_calls = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"finished_hold\":999}")));
    TEST_ASSERT_EQUAL_INT(0, saveSettings_calls);
}

// ============================================================================
// U39 (ADR-0004) — Four behavior parameters accepted within their principled
// ranges, applied to the globals.
// ============================================================================
void test_U39_parseCommand_behavior_params_accepted_in_range(void) {
    SHOW_TIMER = true;

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"max_duration\":3600,\"button_step\":5,"
                          "\"remaining_publish_interval\":2,\"app_config_timeout\":60}")));
    TEST_ASSERT_EQUAL_UINT32(3600, TIMER_MAX_DURATION);
    TEST_ASSERT_EQUAL_UINT32(5,    TIMER_STEP);
    TEST_ASSERT_EQUAL_UINT16(2,    TIMER_PUBLISH_INTERVAL);
    TEST_ASSERT_EQUAL_UINT16(60,   TIMER_CONFIG_TIMEOUT);

    // Lower bounds.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"max_duration\":1,\"button_step\":1,"
                          "\"remaining_publish_interval\":1,\"app_config_timeout\":5}")));
    TEST_ASSERT_EQUAL_UINT32(1,  TIMER_MAX_DURATION);
    TEST_ASSERT_EQUAL_UINT32(1,  TIMER_STEP);
    TEST_ASSERT_EQUAL_UINT16(1,  TIMER_PUBLISH_INTERVAL);
    TEST_ASSERT_EQUAL_UINT16(5,  TIMER_CONFIG_TIMEOUT);

    // Reset max_duration so it doesn't reject other tests' duration commands.
    TIMER_MAX_DURATION = 86400;

    // Upper bounds.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"max_duration\":604800,\"button_step\":99,"
                          "\"remaining_publish_interval\":60,\"app_config_timeout\":300}")));
    TEST_ASSERT_EQUAL_UINT32(604800, TIMER_MAX_DURATION);
    TEST_ASSERT_EQUAL_UINT32(99,     TIMER_STEP);
    TEST_ASSERT_EQUAL_UINT16(60,     TIMER_PUBLISH_INTERVAL);
    TEST_ASSERT_EQUAL_UINT16(300,    TIMER_CONFIG_TIMEOUT);
}

// ============================================================================
// U40 (ADR-0004) — Out-of-range behavior parameters are rejected atomically.
// ============================================================================
void test_U40_parseCommand_behavior_params_atomic_reject(void) {
    SHOW_TIMER = true;
    TIMER_MAX_DURATION     = 86400;
    TIMER_STEP             = 1;
    TIMER_PUBLISH_INTERVAL = 1;
    TIMER_CONFIG_TIMEOUT   = 30;

    // max_duration below floor.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"max_duration\":0}")));
    TEST_ASSERT_EQUAL_UINT32(86400, TIMER_MAX_DURATION);

    // max_duration above ceiling.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"max_duration\":604801}")));
    TEST_ASSERT_EQUAL_UINT32(86400, TIMER_MAX_DURATION);

    // button_step out of range.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"button_step\":100}")));
    TEST_ASSERT_EQUAL_UINT32(1, TIMER_STEP);

    // remaining_publish_interval out of range.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"remaining_publish_interval\":0}")));
    TEST_ASSERT_EQUAL_UINT16(1, TIMER_PUBLISH_INTERVAL);

    // app_config_timeout out of range.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"app_config_timeout\":4}")));
    TEST_ASSERT_EQUAL_UINT16(30, TIMER_CONFIG_TIMEOUT);

    // Non-numeric is rejected.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"max_duration\":\"big\"}")));
    TEST_ASSERT_EQUAL_UINT32(86400, TIMER_MAX_DURATION);
}

// ============================================================================
// U41 (ADR-0004) — Melody filenames and progress-bar options.
// ============================================================================
void test_U41_parseCommand_melody_and_bar(void) {
    SHOW_TIMER = true;
    TIMER_MELODY_TICK = "timer_tick";
    TIMER_MELODY_END  = "timer_end";
    TIMER_BAR_ENABLED = true;
    TIMER_BAR_COLOR   = 0;

    // Valid custom names apply.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"melody_tick\":\"custom_tick\",\"melody_end\":\"jingle\"}")));
    TEST_ASSERT_TRUE(TIMER_MELODY_TICK == "custom_tick");
    TEST_ASSERT_TRUE(TIMER_MELODY_END  == "jingle");

    // Empty string resets to canonical defaults.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"melody_tick\":\"\",\"melody_end\":\"\"}")));
    TEST_ASSERT_TRUE(TIMER_MELODY_TICK == "timer_tick");
    TEST_ASSERT_TRUE(TIMER_MELODY_END  == "timer_end");

    // Invalid melody name (path separator) is rejected.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"melody_tick\":\"foo/bar\"}")));
    TEST_ASSERT_TRUE(TIMER_MELODY_TICK == "timer_tick");

    // bar_enabled bool round-trip.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_enabled\":false}")));
    TEST_ASSERT_FALSE(TIMER_BAR_ENABLED);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_enabled\":true}")));
    TEST_ASSERT_TRUE(TIMER_BAR_ENABLED);

    // bar_enabled non-bool rejected.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_enabled\":\"yes\"}")));
    TEST_ASSERT_TRUE(TIMER_BAR_ENABLED);

    // bar_color numeric.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_color\":16711680}")));
    TEST_ASSERT_EQUAL_UINT32(0xFF0000, TIMER_BAR_COLOR);

    // bar_color hex string with leading '#'.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_color\":\"#00FF00\"}")));
    TEST_ASSERT_EQUAL_UINT32(0x00FF00, TIMER_BAR_COLOR);

    // bar_color hex string without '#'.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_color\":\"0000FF\"}")));
    TEST_ASSERT_EQUAL_UINT32(0x0000FF, TIMER_BAR_COLOR);

    // bar_color malformed string rejected.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_color\":\"notahex\"}")));
    TEST_ASSERT_EQUAL_UINT32(0x0000FF, TIMER_BAR_COLOR);

    // bar_color hex string must be exactly 6 digits wrong lengths
    // are rejected and leave the previous color unchanged (atomic reject).
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_color\":\"#abc\"}")));
    TEST_ASSERT_EQUAL_UINT32(0x0000FF, TIMER_BAR_COLOR);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_color\":\"\"}")));
    TEST_ASSERT_EQUAL_UINT32(0x0000FF, TIMER_BAR_COLOR);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_color\":\"12345\"}")));
    TEST_ASSERT_EQUAL_UINT32(0x0000FF, TIMER_BAR_COLOR);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_color\":\"1234567\"}")));
    TEST_ASSERT_EQUAL_UINT32(0x0000FF, TIMER_BAR_COLOR);

    // Exactly-6-digit forms succeed, with and without leading '#'.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_color\":\"FFAA00\"}")));
    TEST_ASSERT_EQUAL_UINT32(0xFFAA00, TIMER_BAR_COLOR);
    TIMER_BAR_COLOR = 0x0000FF;
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_color\":\"#FFAA00\"}")));
    TEST_ASSERT_EQUAL_UINT32(0xFFAA00, TIMER_BAR_COLOR);

    // Numeric one past 0xFFFFFF is out of range and rejected (color unchanged).
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_color\":16777216}")));
    TEST_ASSERT_EQUAL_UINT32(0xFFAA00, TIMER_BAR_COLOR);
}

// ============================================================================
// U42 (ADR-0001 addendum) — In a multi-key payload, max_duration is evaluated
// before duration so {max_duration, duration} is judged against the in-payload
// ceiling, not the pre-payload one. Atomic-reject is preserved for inconsistent
// payloads and single-key duration behavior is unchanged.
// ============================================================================
void test_U42_parseCommand_multi_key_validation_ordering(void) {
    SHOW_TIMER = true;

    // Case 1: payload raises ceiling and sets a duration within the new ceiling
    // in one atomic call. Pre-payload ceiling would have rejected duration=9000.
    TIMER_MAX_DURATION = 5000;
    TimerManager.setDuration(100);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"max_duration\":10000,\"duration\":9000}")));
    TEST_ASSERT_EQUAL_UINT32(10000, TIMER_MAX_DURATION);
    TEST_ASSERT_EQUAL_UINT32(9000,  TimerManager.getDuration());

    // Case 2: payload is internally inconsistent (duration > in-payload ceiling).
    // Atomic-reject: neither key applies.
    TIMER_MAX_DURATION = 5000;
    TimerManager.setDuration(100);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"max_duration\":100,\"duration\":9000}")));
    TEST_ASSERT_EQUAL_UINT32(5000, TIMER_MAX_DURATION);
    TEST_ASSERT_EQUAL_UINT32(100,  TimerManager.getDuration());

    // Case 3: single-key duration above the live ceiling is still rejected
    // (no in-payload ceiling to fall back on).
    TIMER_MAX_DURATION = 5000;
    TimerManager.setDuration(100);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"duration\":9000}")));
    TEST_ASSERT_EQUAL_UINT32(5000, TIMER_MAX_DURATION);
    TEST_ASSERT_EQUAL_UINT32(100,  TimerManager.getDuration());

    // Reset for downstream tests.
    TIMER_MAX_DURATION = 86400;
}

// ============================================================================
// U43 — bar_enabled is a STRICT bool. Only JSON true/false are
// accepted; every other JSON shape (integer 0/1, float, numeric/word string,
// null, object, array) is rejected with BadField and leaves TIMER_BAR_ENABLED
// ArduinoJson is<bool>() coercion quirks
// ============================================================================
void test_U43_parseCommand_bar_enabled_strict_bool(void) {
    SHOW_TIMER = true;

    // Every non-bool JSON shape is rejected and leaves the prior value intact.
    // Prior value held at `true` throughout so a sloppy coercion to false shows.
    TIMER_BAR_ENABLED = true;
    const char *rejected[] = {
        "{\"bar_enabled\":1}",       // integer truthy
        "{\"bar_enabled\":0}",       // integer falsy
        "{\"bar_enabled\":1.0}",     // float
        "{\"bar_enabled\":\"yes\"}", // arbitrary truthy string
        "{\"bar_enabled\":\"true\"}",// literal bool-word string (not a JSON bool)
        "{\"bar_enabled\":null}",    // null is not false
        "{\"bar_enabled\":{}}",      // object
        "{\"bar_enabled\":[]}",      // array
    };
    for (const char *cmd : rejected) {
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(TimerCmdResult::BadField),
                                  static_cast<int>(TimerManager.parseCommand(cmd)),
                                  cmd);
        TEST_ASSERT_TRUE_MESSAGE(TIMER_BAR_ENABLED, cmd);  // unchanged
    }

    // Only genuine JSON booleans apply.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_enabled\":false}")));
    TEST_ASSERT_FALSE(TIMER_BAR_ENABLED);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_enabled\":true}")));
    TEST_ASSERT_TRUE(TIMER_BAR_ENABLED);
}

// ============================================================================
// U32–U35 — Timer HA Presence descriptor table invariants.
// The table (src/TimerHa.h) is the single source of truth that MQTTManager's
// discovery setup AND teardown both read, so it cannot drift. These host tests
// lock the contract that was previously only checked by maintainer-only E2E.
// ============================================================================

static int count_ha_options(const char *opts) {
    if (!opts || !*opts) return 0;
    int n = 1;
    for (const char *p = opts; *p; ++p) if (*p == ';') ++n;
    return n;
}

// U32 — exactly one descriptor per slot, in slot order, with a sane component
// and a "%s"-bearing unique-id format.
void test_U32_descriptor_table_well_formed(void) {
    TEST_ASSERT_EQUAL_UINT(8, (unsigned)TIMER_HA_DESCRIPTOR_COUNT);
    for (size_t i = 0; i < TIMER_HA_DESCRIPTOR_COUNT; ++i) {
        const TimerHaDescriptor &d = TIMER_HA_DESCRIPTORS[i];
        // Row i must describe slot i (setup/teardown index the array by slot).
        TEST_ASSERT_EQUAL_UINT((unsigned)i, (unsigned)d.slot);

        const bool validComponent =
            strcmp(d.component, "text")   == 0 ||
            strcmp(d.component, "sensor") == 0 ||
            strcmp(d.component, "select") == 0 ||
            strcmp(d.component, "button") == 0;
        TEST_ASSERT_TRUE_MESSAGE(validComponent, d.component);

        TEST_ASSERT_NOT_NULL(d.idFormat);
        TEST_ASSERT_NOT_NULL(strstr(d.idFormat, "%s"));
        TEST_ASSERT_NOT_NULL(d.icon);
        TEST_ASSERT_NOT_NULL(d.name);
    }
}

// U33 — every entity's unique-id format is distinct (no two entities collide).
void test_U33_descriptor_ids_unique(void) {
    for (size_t i = 0; i < TIMER_HA_DESCRIPTOR_COUNT; ++i)
        for (size_t j = i + 1; j < TIMER_HA_DESCRIPTOR_COUNT; ++j)
            TEST_ASSERT_TRUE_MESSAGE(
                strcmp(TIMER_HA_DESCRIPTORS[i].idFormat,
                       TIMER_HA_DESCRIPTORS[j].idFormat) != 0,
                TIMER_HA_DESCRIPTORS[i].idFormat);
}

// U34 — only selects carry options; only sensors carry unit + device class.
void test_U34_descriptor_type_specific_fields(void) {
    for (size_t i = 0; i < TIMER_HA_DESCRIPTOR_COUNT; ++i) {
        const TimerHaDescriptor &d = TIMER_HA_DESCRIPTORS[i];
        if (strcmp(d.component, "select") == 0)
            TEST_ASSERT_NOT_NULL(d.options);
        else
            TEST_ASSERT_NULL(d.options);
        if (strcmp(d.component, "sensor") != 0) {
            TEST_ASSERT_NULL(d.unit);
            TEST_ASSERT_NULL(d.deviceClass);
        }
    }
}

// U35 — the HA select option lists cannot silently diverge from the enums they
// surface: one option per enum value (Candidate-B link). If a new BuzzerMode /
// FinishedMode value is added, its last index grows and this forces the option
// list to grow with it.
void test_U35_select_options_match_enums(void) {
    const TimerHaDescriptor &buz = timerHaDescriptor(TimerHaEntity::Buzzer);
    const TimerHaDescriptor &fin = timerHaDescriptor(TimerHaEntity::Finished);
    TEST_ASSERT_EQUAL_INT((int)BuzzerMode::Countdown + 1, count_ha_options(buz.options));
    TEST_ASSERT_EQUAL_INT((int)FinishedMode::ReAlert + 1, count_ha_options(fin.options));
}

// ============================================================================
// D1–D6 — Timer rendering, via the display-free TimerView model (src/TimerView).
// The renderer's decision logic (per-state text, the compact display format,
// progress-bar geometry, the Finished blink, the config underline) used to live
// inside TimerApp in Apps.cpp — uncompiled on the host — so these were deferred.
// TimerView::compute() reads the same TimerManager state the tests already drive
// and returns a pure description of what to draw, so they run on the host now.
// Still at the painter boundary (not covered here): font-metric text centering
// and the icon-file (.jpg/.gif) lookup, both of which need real display I/O.
// ============================================================================

// D1 — Idle shows the configured duration as compact text, with no bar.
void test_D1_view_idle_shows_duration_no_bar(void) {
    TimerManager.setDuration(300);
    TimerView v = TimerViewModel::compute(0);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerView::Screen::Time), static_cast<int>(v.screen));
    TEST_ASSERT_EQUAL_STRING("5:00", v.text);
    TEST_ASSERT_TRUE(v.showText);
    TEST_ASSERT_FALSE(v.showBar);
}

// D2 — Running shows the remaining time with a bar.
void test_D2_view_running_shows_remaining_with_bar(void) {
    TimerManager.setDuration(300);
    TimerManager.start();
    fixture::advance(60000);            // 60s elapsed -> 240s remaining
    TimerManager.tick();
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running),
                      static_cast<int>(TimerManager.getState()));
    TimerView v = TimerViewModel::compute(0);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerView::Screen::Time), static_cast<int>(v.screen));
    TEST_ASSERT_EQUAL_STRING("4:00", v.text);
    TEST_ASSERT_TRUE(v.showText);
    TEST_ASSERT_TRUE(v.showBar);
}

// D3 — Finished blinks "0:00" at the 500 ms cadence; no bar.
void test_D3_view_finished_blinks_0_00(void) {
    TimerManager.setDuration(5);
    TimerManager.start();
    fixture::advance(5000);
    TimerManager.tick();
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Finished),
                      static_cast<int>(TimerManager.getState()));

    // nowMs drives the blink directly (independent of the run clock).
    TimerView on  = TimerViewModel::compute(0);
    TimerView off = TimerViewModel::compute(500);
    TimerView on2 = TimerViewModel::compute(1000);

    TEST_ASSERT_EQUAL(static_cast<int>(TimerView::Screen::Finished), static_cast<int>(on.screen));
    TEST_ASSERT_EQUAL_STRING("0:00", on.text);
    TEST_ASSERT_TRUE(on.showText);
    TEST_ASSERT_FALSE(off.showText);
    TEST_ASSERT_TRUE(on2.showText);
    TEST_ASSERT_FALSE(on.showBar);
}

// D4 — Progress bar geometry: right-anchored, drains from the left
// (barStartX + barLen == 32, the right edge at column 31), and a sub-1-cell
// remaining draws no bar.
void test_D4_view_bar_geometry_right_anchored(void) {
    // Half remaining: 23 * 50/100 = 11 cells, anchored right.
    TimerManager.setDuration(100);
    TimerManager.start();
    fixture::advance(50000);
    TimerManager.tick();
    TEST_ASSERT_EQUAL_UINT32(50, TimerManager.getRemaining());
    TimerView half = TimerViewModel::compute(0);
    TEST_ASSERT_TRUE(half.showBar);
    TEST_ASSERT_EQUAL_UINT8(11, half.barLen);
    TEST_ASSERT_EQUAL_INT16(21, half.barStartX);                 // 9 + (23 - 11)
    TEST_ASSERT_EQUAL_INT16(32, half.barStartX + half.barLen);   // right edge anchored

    // Full remaining: full-length bar starting at the bar origin.
    TimerManager.reset();
    TimerManager.start();
    TimerView full = TimerViewModel::compute(0);
    TEST_ASSERT_EQUAL_UINT8(23, full.barLen);
    TEST_ASSERT_EQUAL_INT16(9, full.barStartX);
    TEST_ASSERT_EQUAL_INT16(32, full.barStartX + full.barLen);

    // Tiny remaining (1s of 100): 23 * 1/100 == 0 cells -> no bar drawn.
    fixture::advance(99000);
    TimerManager.tick();
    TEST_ASSERT_EQUAL_UINT32(1, TimerManager.getRemaining());
    TimerView tiny = TimerViewModel::compute(0);
    TEST_ASSERT_FALSE(tiny.showBar);
}

// D5 — Config screen: HH:MM:SS centered over the full panel, field underline
// tracks configCycleField, no bar.
void test_D5_view_config_screen(void) {
    TimerManager.setDuration(3661);     // 1:01:01
    TimerManager.enterConfigMode();
    TimerView v = TimerViewModel::compute(0);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerView::Screen::Config), static_cast<int>(v.screen));
    TEST_ASSERT_EQUAL_STRING("01:01:01", v.text);
    TEST_ASSERT_TRUE(v.showText);
    TEST_ASSERT_TRUE(v.showUnderline);
    TEST_ASSERT_EQUAL_UINT8(0, v.underlineField);   // HH highlighted first
    TEST_ASSERT_EQUAL_INT16(0, v.textRegionX0);     // centered over the full 32px panel
    TEST_ASSERT_EQUAL_INT16(32, v.textRegionW);
    TEST_ASSERT_FALSE(v.showBar);

    TimerManager.configCycleField();                // HH -> MM
    TimerView v2 = TimerViewModel::compute(0);
    TEST_ASSERT_EQUAL_UINT8(1, v2.underlineField);
}

// D6 — formatTimerDisplay is the compact on-screen format (drops seconds past
// 1h to fit 24px), distinct from formatHMS (the wire string, always seconds).
void test_D6_formatTimerDisplay_vs_wire_string(void) {
    char b[12];
    TimerViewModel::formatTimerDisplay(45, b, sizeof(b));    TEST_ASSERT_EQUAL_STRING("0:45", b);
    TimerViewModel::formatTimerDisplay(305, b, sizeof(b));   TEST_ASSERT_EQUAL_STRING("5:05", b);
    TimerViewModel::formatTimerDisplay(3600, b, sizeof(b));  TEST_ASSERT_EQUAL_STRING("1:00", b);
    TimerViewModel::formatTimerDisplay(3661, b, sizeof(b));  TEST_ASSERT_EQUAL_STRING("1:01", b);  // seconds dropped
    TimerViewModel::formatTimerDisplay(36000, b, sizeof(b)); TEST_ASSERT_EQUAL_STRING("10:00", b);
    // Contrast: the wire string for the same value keeps seconds.
    TEST_ASSERT_EQUAL_STRING("1:01:01", TimerManager_::formatHMS(3661).c_str());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_U1_setDuration_clamps_low_and_high);
    RUN_TEST(test_U2_parseCommand_null_empty_garbage_are_noops);
    RUN_TEST(test_U3_start_from_idle_publishes_state_then_remaining);
    RUN_TEST(test_U4_tick_crosses_zero_enters_finished);
    RUN_TEST(test_U5_autoclear_returns_to_idle_after_hold);
    RUN_TEST(test_U6_parseCommand_noop_when_disabled);
    RUN_TEST(test_U7_onShowTimerChange_resets_running_timer);
    RUN_TEST(test_U8_realert_replays_at_interval);
    RUN_TEST(test_U9_getIconForState_all_empty);
    RUN_TEST(test_U10_getIconForState_only_idle_inherits);
    RUN_TEST(test_U11_getIconForState_per_state_with_idle_fallback);
    RUN_TEST(test_U12_reset_from_finished_stops_sound_and_returns_idle);
    RUN_TEST(test_U13_start_from_finished_stops_sound_and_rearms);
    RUN_TEST(test_U14_config_default_cap_wraps_HH_at_23);
    RUN_TEST(test_U15_config_tight_cap_HH_at_1_then_MM_at_0);
    RUN_TEST(test_U16_config_no_cap_when_max_is_zero);
    RUN_TEST(test_U17_config_decrement_wraps_to_dynamic_max);
    RUN_TEST(test_U18_config_exit_value_already_within_cap);
    RUN_TEST(test_U19_enterConfig_clamps_duration_at_99h);
    RUN_TEST(test_U20_icon_name_validation);
    RUN_TEST(test_U21_parseCommand_batches_persist);
    RUN_TEST(test_U22_parseCommand_aborts_config_without_committing_edit);
    RUN_TEST(test_U23_formatHMS_trimmed);
    RUN_TEST(test_U24_parseHMS_accepts);
    RUN_TEST(test_U25_parseHMS_rejects);
    RUN_TEST(test_U26_parseCommand_duration_string_and_number);
    RUN_TEST(test_U27_config_roundtrip_unchanged);
    RUN_TEST(test_U28_isValidDuration);
    RUN_TEST(test_U29_parseCommand_strict_results);
    RUN_TEST(test_U30_parseCommand_atomic_reject);
    RUN_TEST(test_U31_config_abort_only_on_valid_command);
    RUN_TEST(test_U36_parseCommand_tuning_keys_accepted_in_range);
    RUN_TEST(test_U37_parseCommand_tuning_keys_atomic_reject_out_of_range);
    RUN_TEST(test_U38_parseCommand_tuning_change_persists_via_saveSettings);
    RUN_TEST(test_U39_parseCommand_behavior_params_accepted_in_range);
    RUN_TEST(test_U40_parseCommand_behavior_params_atomic_reject);
    RUN_TEST(test_U41_parseCommand_melody_and_bar);
    RUN_TEST(test_U42_parseCommand_multi_key_validation_ordering);
    RUN_TEST(test_U43_parseCommand_bar_enabled_strict_bool);
    RUN_TEST(test_U32_descriptor_table_well_formed);
    RUN_TEST(test_U33_descriptor_ids_unique);
    RUN_TEST(test_U34_descriptor_type_specific_fields);
    RUN_TEST(test_U35_select_options_match_enums);
    RUN_TEST(test_D1_view_idle_shows_duration_no_bar);
    RUN_TEST(test_D2_view_running_shows_remaining_with_bar);
    RUN_TEST(test_D3_view_finished_blinks_0_00);
    RUN_TEST(test_D4_view_bar_geometry_right_anchored);
    RUN_TEST(test_D5_view_config_screen);
    RUN_TEST(test_D6_formatTimerDisplay_vs_wire_string);
    return UNITY_END();
}
