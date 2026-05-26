// Starter unit tests U1–U8 for TimerManager. Proves every layer of the
// native test scaffolding: ArduinoFake millis mocking, recording-mock
// ordering (MQTT), stateful fakes (notifications, PeripheryManager),
// Preferences round-trip, and the AutoClear lifecycle end-to-end.

#include <unity.h>
#include <ArduinoFake.h>
#include <string.h>

#include "fixture.h"
#include "../../src/TimerHa.h"

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
// D1–D4, I1 — TODO: requires native DisplayManager.cpp test infrastructure
// (currently DisplayManager is stubbed). Tracked separately.
// ============================================================================

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
    RUN_TEST(test_U32_descriptor_table_well_formed);
    RUN_TEST(test_U33_descriptor_ids_unique);
    RUN_TEST(test_U34_descriptor_type_specific_fields);
    RUN_TEST(test_U35_select_options_match_enums);
    return UNITY_END();
}
