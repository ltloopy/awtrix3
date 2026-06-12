// Starter unit tests U1–U8 for TimerManager. Proves every layer of the
// native test scaffolding: ArduinoFake millis mocking, recording-mock
// ordering (MQTT), stateful fakes (notifications, PeripheryManager),
// Preferences round-trip, and the AutoClear lifecycle end-to-end.

#include <unity.h>
#include <ArduinoFake.h>
#include <ArduinoJson.h>
#include <string.h>

#include "fixture.h"
#include "../../src/TimerEnums.h"
#include "../../src/TimerHa.h"
#include "../../src/TimerView.h"
#include "../../src/TimerSettings.h"
#include "../../src/TimerMenu.h"
#include "../../src/TimerConfigEditor.h"
#include "Preferences.h"

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
// Proves: Globals + Preferences fake wired correctly; each clamp publishes the
// clamped value, as trimmed HMS, on the canonical duration topic (issue #34).
// ============================================================================
void test_U1_setDuration_clamps_low_and_high(void) {
    TimerManager.setDuration(0);
    TEST_ASSERT_EQUAL_UINT32(1, TimerManager.getDuration());
    const PublishCall *low = fixture::last_publish(fixture::TIMER_DURATION_TOPIC);
    TEST_ASSERT_NOT_NULL(low);
    TEST_ASSERT_EQUAL_STRING("0:01", low->payload.c_str());

    TimerManager.setDuration(99999);
    TEST_ASSERT_EQUAL_UINT32(TIMER_MAX_DURATION, TimerManager.getDuration());
    const PublishCall *high = fixture::last_publish(fixture::TIMER_DURATION_TOPIC);
    TEST_ASSERT_NOT_NULL(high);
    TEST_ASSERT_EQUAL_STRING("24:00:00", high->payload.c_str());   // fixture cap 86400 s

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
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_STATE_TOPIC));

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

    // The contract: "running" on the state topic MUST precede the remaining
    // publish. Find the index of the first publish on each topic post-start.
    int stateIdx = -1, remIdx = -1;
    for (size_t i = 0; i < MQTTManager.recorded.size(); ++i) {
        const auto &c = MQTTManager.recorded[i];
        if (stateIdx < 0 && c.topic == fixture::TIMER_STATE_TOPIC && c.payload == "running") stateIdx = (int)i;
        if (remIdx   < 0 && c.topic == fixture::TIMER_REMAINING_TOPIC && c.payload == "300") remIdx = (int)i;
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

    // Assert: state returned to Idle; last publish on the state topic == "idle".
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));
    const PublishCall *last = fixture::last_publish(fixture::TIMER_STATE_TOPIC);
    TEST_ASSERT_NOT_NULL(last);
    TEST_ASSERT_EQUAL_STRING("idle", last->payload.c_str());
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
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_STATE_TOPIC));
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_DURATION_TOPIC));

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
    const PublishCall *last = fixture::last_publish(fixture::TIMER_STATE_TOPIC);
    TEST_ASSERT_NOT_NULL(last);
    TEST_ASSERT_EQUAL_STRING("idle", last->payload.c_str());
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
    const PublishCall *last = fixture::last_publish(fixture::TIMER_STATE_TOPIC);
    TEST_ASSERT_NOT_NULL(last);
    TEST_ASSERT_EQUAL_STRING("running", last->payload.c_str());
}

// ============================================================================
// U14–U17 — the deterministic config-wheel cap/wrap math was retargeted to drive
// TimerConfigEditor directly when that editor was extracted from TimerManager
// (ADR-0011). It now lives as test_CE2..test_CE5 near the bottom of this file:
//   U14 (default 24h cap, HH wraps at 23)            -> test_CE2
//   U15 (tight 3600 cap, per-field recompute)        -> test_CE3
//   U16 (no cap when TIMER_MAX_DURATION == 0)         -> test_CE4
//   U17 (decrement at 0 wraps to the dynamic cap)     -> test_CE5
// The TimerManager forwarder roundtrip is still covered end-to-end by U18/U19/
// U22/U27/U31 below, which exercise enterConfigMode/exitConfigMode + run-state.
// ============================================================================

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
                          "{\"max_duration\":3600,"
                          "\"remaining_publish_interval\":2,\"app_config_timeout\":60}")));
    TEST_ASSERT_EQUAL_UINT32(3600, TIMER_MAX_DURATION);
    TEST_ASSERT_EQUAL_UINT16(2,    TIMER_PUBLISH_INTERVAL);
    TEST_ASSERT_EQUAL_UINT16(60,   TIMER_CONFIG_TIMEOUT);

    // Lower bounds.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"max_duration\":1,"
                          "\"remaining_publish_interval\":1,\"app_config_timeout\":5}")));
    TEST_ASSERT_EQUAL_UINT32(1,  TIMER_MAX_DURATION);
    TEST_ASSERT_EQUAL_UINT16(1,  TIMER_PUBLISH_INTERVAL);
    TEST_ASSERT_EQUAL_UINT16(5,  TIMER_CONFIG_TIMEOUT);

    // Reset max_duration so it doesn't reject other tests' duration commands.
    TIMER_MAX_DURATION = 86400;

    // Upper bounds.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"max_duration\":604800,"
                          "\"remaining_publish_interval\":60,\"app_config_timeout\":300}")));
    TEST_ASSERT_EQUAL_UINT32(604800, TIMER_MAX_DURATION);
    TEST_ASSERT_EQUAL_UINT16(60,     TIMER_PUBLISH_INTERVAL);
    TEST_ASSERT_EQUAL_UINT16(300,    TIMER_CONFIG_TIMEOUT);
}

// ============================================================================
// U40 (ADR-0004) — Out-of-range behavior parameters are rejected atomically.
// ============================================================================
void test_U40_parseCommand_behavior_params_atomic_reject(void) {
    SHOW_TIMER = true;
    TIMER_MAX_DURATION     = 86400;
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

    // icon_enabled bool round-trip.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"icon_enabled\":false}")));
    TEST_ASSERT_FALSE(TIMER_ICON_ENABLED);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"icon_enabled\":true}")));
    TEST_ASSERT_TRUE(TIMER_ICON_ENABLED);

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
// U50 — icon_enabled is a STRICT bool (same contract as bar_enabled, U43).
// Only JSON true/false are accepted; every other JSON shape (integer 0/1, float,
// numeric/word string, null, object, array) is rejected with BadField and leaves
// TIMER_ICON_ENABLED intact.
// ============================================================================
void test_U50_parseCommand_icon_enabled_strict_bool(void) {
    SHOW_TIMER = true;

    // Every non-bool JSON shape is rejected and leaves the prior value intact.
    // Prior value held at `true` throughout so a sloppy coercion to false shows.
    TIMER_ICON_ENABLED = true;
    const char *rejected[] = {
        "{\"icon_enabled\":1}",       // integer truthy
        "{\"icon_enabled\":0}",       // integer falsy
        "{\"icon_enabled\":1.0}",     // float
        "{\"icon_enabled\":\"yes\"}", // arbitrary truthy string
        "{\"icon_enabled\":\"true\"}",// literal bool-word string (not a JSON bool)
        "{\"icon_enabled\":null}",    // null is not false
        "{\"icon_enabled\":{}}",      // object
        "{\"icon_enabled\":[]}",      // array
    };
    for (const char *cmd : rejected) {
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(TimerCmdResult::BadField),
                                  static_cast<int>(TimerManager.parseCommand(cmd)),
                                  cmd);
        TEST_ASSERT_TRUE_MESSAGE(TIMER_ICON_ENABLED, cmd);  // unchanged
    }

    // Only genuine JSON booleans apply.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"icon_enabled\":false}")));
    TEST_ASSERT_FALSE(TIMER_ICON_ENABLED);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"icon_enabled\":true}")));
    TEST_ASSERT_TRUE(TIMER_ICON_ENABLED);
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
    // The "`;`"-joined option strings are derived from the codec table's HA column
    // (ADR-0010) and MUST stay byte-identical so existing HA automations/dashboards
    // keep matching the same option labels in the same order.
    TEST_ASSERT_EQUAL_STRING("Off;End;Countdown", buz.options);
    TEST_ASSERT_EQUAL_STRING("Auto-clear;Hold;Re-alert", fin.options);
}

// U54 — create/teardown id symmetry. Both discovery setup and teardown derive
// each entity's unique id through the one TimerHa helper, so they cannot drift.
// For every row, the "create-path" derivation (by slot) and the "teardown-path"
// derivation (by row) agree with each other and with the row's idFormat applied
// to the MAC. The helper reads idFormat from the row it is handed, so the id
// depends only on the row, never its array position — reordering the table moves
// no entity's topic. (Replaces the old hand-ordered TIMER_HA_ID_BUFFERS macro.)
void test_U54_timer_ha_ids_create_teardown_symmetric(void) {
    const char *mac = "a1b2c3";
    for (size_t i = 0; i < TIMER_HA_DESCRIPTOR_COUNT; ++i) {
        const TimerHaDescriptor &d = TIMER_HA_DESCRIPTORS[i];
        char createPath[40], teardownPath[40], expected[40];
        formatTimerHaEntityId(timerHaDescriptor(d.slot), mac, createPath, sizeof createPath);
        formatTimerHaEntityId(TIMER_HA_DESCRIPTORS[i], mac, teardownPath, sizeof teardownPath);
        snprintf(expected, sizeof expected, d.idFormat, mac);
        TEST_ASSERT_EQUAL_STRING(expected, createPath);
        TEST_ASSERT_EQUAL_STRING(createPath, teardownPath);
    }
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

// Builds a TimerSnapshot from the live TimerManager, mirroring the single
// production mapping in TimerApp (src/Apps.cpp). Used by the D-tests that drive
// the manager through real start()/tick()/setDuration()/config arithmetic and
// then assert the view of that exact state.
static TimerSnapshot mgrSnapshot(unsigned long nowMs, bool iconEnabled = true) {
    return TimerSnapshot{
        TimerManager.getState(), TimerManager.getDuration(), TimerManager.getRemaining(),
        TimerManager.getRunDuration(), TimerManager.isInConfig(), TimerManager.getConfigField(),
        TimerManager.getConfigHH(), TimerManager.getConfigMM(), TimerManager.getConfigSS(),
        iconEnabled, nowMs};
}

// D1 — Idle shows the configured duration as compact text, with no bar.
void test_D1_view_idle_shows_duration_no_bar(void) {
    const TimerSnapshot s{TimerState::Idle, 300, 0, 0, false, 0, 0, 0, 0, true, 0};
    TimerView v = TimerViewModel::compute(s);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerView::Screen::Time), static_cast<int>(v.screen));
    TEST_ASSERT_EQUAL_STRING("5:00", v.text);
    TEST_ASSERT_TRUE(v.showText);
    TEST_ASSERT_FALSE(v.showBar);
}

// D2 — Running shows the remaining time with a bar. Drives the real run clock so
// the snapshot carries the same remaining (240) the manager computes.
void test_D2_view_running_shows_remaining_with_bar(void) {
    TimerManager.setDuration(300);
    TimerManager.start();
    fixture::advance(60000);            // 60s elapsed -> 240s remaining
    TimerManager.tick();
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running),
                      static_cast<int>(TimerManager.getState()));
    TimerView v = TimerViewModel::compute(mgrSnapshot(0));
    TEST_ASSERT_EQUAL(static_cast<int>(TimerView::Screen::Time), static_cast<int>(v.screen));
    TEST_ASSERT_EQUAL_STRING("4:00", v.text);
    TEST_ASSERT_TRUE(v.showText);
    TEST_ASSERT_TRUE(v.showBar);
}

// D3 — Finished blinks "0:00" at the 500 ms cadence; no bar. The blink is a pure
// function of nowMs, so three snapshots differing only in nowMs cover it.
void test_D3_view_finished_blinks_0_00(void) {
    const TimerSnapshot base{TimerState::Finished, 5, 0, 5, false, 0, 0, 0, 0, true, 0};
    TimerSnapshot s_on = base, s_off = base, s_on2 = base;
    s_on.nowMs = 0; s_off.nowMs = 500; s_on2.nowMs = 1000;

    TimerView on  = TimerViewModel::compute(s_on);
    TimerView off = TimerViewModel::compute(s_off);
    TimerView on2 = TimerViewModel::compute(s_on2);

    TEST_ASSERT_EQUAL(static_cast<int>(TimerView::Screen::Finished), static_cast<int>(on.screen));
    TEST_ASSERT_EQUAL_STRING("0:00", on.text);
    TEST_ASSERT_TRUE(on.showText);
    TEST_ASSERT_FALSE(off.showText);
    TEST_ASSERT_TRUE(on2.showText);
    TEST_ASSERT_FALSE(on.showBar);
}

// D4 — Progress bar geometry: right-anchored, drains from the left
// (barStartX + barLen == 32, the right edge at column 31), and a sub-1-cell
// remaining draws no bar. Geometry depends only on (remaining, runDuration,
// iconEnabled), so each case is an explicit Running snapshot.
void test_D4_view_bar_geometry_right_anchored(void) {
    // Half remaining: 23 * 50/100 = 11 cells, anchored right.
    const TimerSnapshot s_half{TimerState::Running, 100, 50, 100, false, 0, 0, 0, 0, true, 0};
    TimerView half = TimerViewModel::compute(s_half);
    TEST_ASSERT_TRUE(half.showBar);
    TEST_ASSERT_EQUAL_UINT8(11, half.barLen);
    TEST_ASSERT_EQUAL_INT16(21, half.barStartX);                 // 9 + (23 - 11)
    TEST_ASSERT_EQUAL_INT16(32, half.barStartX + half.barLen);   // right edge anchored

    // Full remaining: full-length bar starting at the bar origin.
    const TimerSnapshot s_full{TimerState::Running, 100, 100, 100, false, 0, 0, 0, 0, true, 0};
    TimerView full = TimerViewModel::compute(s_full);
    TEST_ASSERT_EQUAL_UINT8(23, full.barLen);
    TEST_ASSERT_EQUAL_INT16(9, full.barStartX);
    TEST_ASSERT_EQUAL_INT16(32, full.barStartX + full.barLen);

    // Tiny remaining (1s of 100): 23 * 1/100 == 0 cells -> no bar drawn.
    const TimerSnapshot s_tiny{TimerState::Running, 100, 1, 100, false, 0, 0, 0, 0, true, 0};
    TimerView tiny = TimerViewModel::compute(s_tiny);
    TEST_ASSERT_FALSE(tiny.showBar);
}

// D5 — Config screen: HH:MM:SS centered over the full panel, field underline
// tracks configCycleField, no bar. Drives the manager so the snapshot carries
// the real config-buffer decomposition (3661 -> 01:01:01) and field cursor.
void test_D5_view_config_screen(void) {
    TimerManager.setDuration(3661);     // 1:01:01
    TimerManager.enterConfigMode();
    TimerView v = TimerViewModel::compute(mgrSnapshot(0));
    TEST_ASSERT_EQUAL(static_cast<int>(TimerView::Screen::Config), static_cast<int>(v.screen));
    TEST_ASSERT_EQUAL_STRING("01:01:01", v.text);
    TEST_ASSERT_TRUE(v.showText);
    TEST_ASSERT_TRUE(v.showUnderline);
    TEST_ASSERT_EQUAL_UINT8(0, v.underlineField);   // HH highlighted first
    TEST_ASSERT_EQUAL_INT16(0, v.textRegionX0);     // centered over the full 32px panel
    TEST_ASSERT_EQUAL_INT16(32, v.textRegionW);
    TEST_ASSERT_FALSE(v.showBar);

    TimerManager.configCycleField();                // HH -> MM
    TimerView v2 = TimerViewModel::compute(mgrSnapshot(0));
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

// D7 — icon_enabled gates showIcon and reflows text + bar to the full panel.
// Icon on (default): text region 8..32, bar in the 23px region right of the icon.
// Icon off: showIcon false, text region 0..32, bar spans the full 32px panel.
void test_D7_view_icon_disabled_reflows_text_and_bar(void) {
    // Running, full remaining -> full bar. The two cases differ only in iconEnabled.
    const TimerSnapshot base{TimerState::Running, 100, 100, 100, false, 0, 0, 0, 0, true, 0};

    // Icon on (the default).
    TimerSnapshot s_on = base; s_on.iconEnabled = true;
    TimerView on = TimerViewModel::compute(s_on);
    TEST_ASSERT_TRUE(on.showIcon);
    TEST_ASSERT_EQUAL_INT16(8, on.textRegionX0);
    TEST_ASSERT_EQUAL_INT16(24, on.textRegionW);
    TEST_ASSERT_EQUAL_UINT8(23, on.barLen);
    TEST_ASSERT_EQUAL_INT16(9, on.barStartX);
    TEST_ASSERT_EQUAL_INT16(32, on.barStartX + on.barLen);

    // Icon off: everything reflows to the full panel, bar still right-anchored.
    TimerSnapshot s_off = base; s_off.iconEnabled = false;
    TimerView off = TimerViewModel::compute(s_off);
    TEST_ASSERT_FALSE(off.showIcon);
    TEST_ASSERT_EQUAL_INT16(0, off.textRegionX0);
    TEST_ASSERT_EQUAL_INT16(32, off.textRegionW);
    TEST_ASSERT_EQUAL_UINT8(32, off.barLen);
    TEST_ASSERT_EQUAL_INT16(0, off.barStartX);
    TEST_ASSERT_EQUAL_INT16(32, off.barStartX + off.barLen);
}

// D8 — Editing the duration while Running buffers the progress bar (API-17).
// The bar divides by the duration snapshotted at start, so a mid-run edit leaves
// the in-progress bar (and countdown) untouched; the new value only re-arms on
// the next start/reset. Contrast: the buggy live-denominator gave 23*50/600 == 1.
void test_D8_view_duration_edit_while_running_buffers_bar(void) {
    TimerManager.setDuration(100);
    TimerManager.start();
    fixture::advance(50000);            // 50s elapsed -> 50s remaining
    TimerManager.tick();
    TimerView before = TimerViewModel::compute(mgrSnapshot(0));
    TEST_ASSERT_EQUAL_UINT8(11, before.barLen);   // 23 * 50/100, mirrors D4

    TimerManager.setDuration(600);      // edit duration mid-run

    // Countdown unaffected; config edit applied; run-duration snapshot unchanged.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_UINT32(50, TimerManager.getRemaining());
    TEST_ASSERT_EQUAL_UINT32(600, TimerManager.getDuration());
    TEST_ASSERT_EQUAL_UINT32(100, TimerManager.getRunDuration());

    // The snapshot carries duration=600 but runDuration=100; the bar divides by the
    // run snapshot, so it stays 11 rather than snapping to 23*50/600.
    const TimerSnapshot mid = mgrSnapshot(0);
    TEST_ASSERT_EQUAL_UINT32(600, mid.duration);
    TEST_ASSERT_EQUAL_UINT32(100, mid.runDuration);
    TimerView after = TimerViewModel::compute(mid);
    TEST_ASSERT_TRUE(after.showBar);
    TEST_ASSERT_EQUAL_UINT8(11, after.barLen);

    // The new duration re-arms on the next reset/start.
    TimerManager.reset();
    TimerManager.start();
    TEST_ASSERT_EQUAL_UINT32(600, TimerManager.getRunDuration());
    TimerView rearmed = TimerViewModel::compute(mgrSnapshot(0));
    TEST_ASSERT_EQUAL_UINT8(23, rearmed.barLen);  // full bar at the new duration
}

// ============================================================================
// U44 — getStateJson() observation snapshot: shape + field values per state
// Proves: the read-only GET /api/timer surface reports the live timer state,
// using computeCurrentRemaining() (not the throttled cache), with the canonical
// enum spellings and stable JSON shape.
// ============================================================================
void test_U44_getStateJson_idle_snapshot(void) {
    TimerManager.setDuration(300);
    StaticJsonDocument<512> doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));
    TEST_ASSERT_EQUAL_STRING("idle", doc["state"]);
    TEST_ASSERT_TRUE(doc["enabled"].as<bool>());
    TEST_ASSERT_EQUAL_UINT32(300, doc["remaining"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING("5:00", doc["remaining_str"]);
    TEST_ASSERT_EQUAL_UINT32(300, doc["duration"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING("5:00", doc["duration_str"]);
    // Defaults: buzzer=end, finished=auto-clear.
    TEST_ASSERT_EQUAL_STRING("end", doc["buzzer"]);
    TEST_ASSERT_EQUAL_STRING("auto-clear", doc["finished"]);
}

void test_U45_getStateJson_running_is_wallclock_fresh(void) {
    TimerManager.setDuration(300);
    TimerManager.start();
    fixture::advance(126000);   // 126 s elapsed; NO tick() — proves live compute,
                                // not the cached remainingSec the publish path uses.
    StaticJsonDocument<512> doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));
    TEST_ASSERT_EQUAL_STRING("running", doc["state"]);
    TEST_ASSERT_EQUAL_UINT32(174, doc["remaining"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING("2:54", doc["remaining_str"]);
}

void test_U46_getStateJson_paused_frozen(void) {
    TimerManager.setDuration(300);
    TimerManager.start();
    fixture::advance(100000);
    TimerManager.pause();
    uint32_t frozen = TimerManager.getRemaining();
    fixture::advance(50000);    // time passes while paused; remaining must not move.
    StaticJsonDocument<512> doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));
    TEST_ASSERT_EQUAL_STRING("paused", doc["state"]);
    TEST_ASSERT_EQUAL_UINT32(frozen, doc["remaining"].as<uint32_t>());
}

void test_U47_getStateJson_finished_zero(void) {
    TimerManager.setDuration(5);
    TimerManager.start();
    fixture::advance(5000);
    TimerManager.tick();
    StaticJsonDocument<512> doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));
    TEST_ASSERT_EQUAL_STRING("finished", doc["state"]);
    TEST_ASSERT_EQUAL_UINT32(0, doc["remaining"].as<uint32_t>());
}

void test_U48_getStateJson_disabled_still_200_shape(void) {
    SHOW_TIMER = false;
    StaticJsonDocument<512> doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));
    TEST_ASSERT_FALSE(doc["enabled"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("idle", doc["state"]);   // disabled timer is forced Idle
}

void test_U49_getStateJson_enum_canonical_spellings(void) {
    TimerManager.parseCommand("{\"buzzer\":\"countdown\",\"finished\":\"re-alert\"}");
    StaticJsonDocument<512> doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));
    TEST_ASSERT_EQUAL_STRING("countdown", doc["buzzer"]);
    TEST_ASSERT_EQUAL_STRING("re-alert", doc["finished"]);
    // The parser also accepts the non-hyphen alias on input; output stays canonical.
    TimerManager.parseCommand("{\"finished\":\"autoclear\"}");
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));
    TEST_ASSERT_EQUAL_STRING("auto-clear", doc["finished"]);
}

// ============================================================================
// Propagation surface (device-to-device timer sync). See CONTEXT.md and
// docs/adr/0006-timer-multi-device-sync.md. Helpers parse the recorded UDP
// payload(s) emitted via the ServerManager stub.
// ============================================================================

// S1 — sync_follow is a strict bool and sync_targets is a validated string, both
// under the ADR-0001 atomic-reject contract: a bad shape leaves BOTH globals
// untouched. Setting either never itself broadcasts (local identity, not config).
void test_S1_sync_settings_validation_atomic_reject(void) {
    SHOW_TIMER = true;

    TIMER_SYNC_FOLLOW = true;   // held true so a sloppy coercion to false shows
    const char *follow_rejected[] = {
        "{\"sync_follow\":1}", "{\"sync_follow\":0}", "{\"sync_follow\":\"yes\"}",
        "{\"sync_follow\":null}", "{\"sync_follow\":{}}", "{\"sync_follow\":[]}",
    };
    for (const char *cmd : follow_rejected) {
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(TimerCmdResult::BadField),
                                  static_cast<int>(TimerManager.parseCommand(cmd)), cmd);
        TEST_ASSERT_TRUE_MESSAGE(TIMER_SYNC_FOLLOW, cmd);  // unchanged
    }

    TIMER_SYNC_TARGETS = "keepme";
    const char *targets_rejected[] = {
        "{\"sync_targets\":5}",                 // not a string
        "{\"sync_targets\":\"a b\"}",          // space inside token
        "{\"sync_targets\":\"ok,\"}",          // empty trailing token
        "{\"sync_targets\":\"good,bad!\"}",    // invalid char
    };
    for (const char *cmd : targets_rejected) {
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(TimerCmdResult::BadField),
                                  static_cast<int>(TimerManager.parseCommand(cmd)), cmd);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("keepme", TIMER_SYNC_TARGETS.c_str(), cmd);
    }

    // Valid forms apply and persist; none of them broadcast.
    ServerManager.__test_reset();
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"sync_follow\":false,\"sync_targets\":\"all\"}")));
    TEST_ASSERT_FALSE(TIMER_SYNC_FOLLOW);
    TEST_ASSERT_EQUAL_STRING("all", TIMER_SYNC_TARGETS.c_str());
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"sync_targets\":\"awtrix_ab12,awtrix_cd34\"}")));
    TEST_ASSERT_EQUAL_STRING("awtrix_ab12,awtrix_cd34", TIMER_SYNC_TARGETS.c_str());
    TEST_ASSERT_EQUAL_INT(0, fixture::sync_packet_count());  // sync_* never propagates
}

// S2 — a local start emits exactly one run-state packet carrying action+duration
// and NO config keys (the duration-is-run-state invariant; a start never clobbers
// a peer's config).
void test_S2_local_start_emits_runstate_only(void) {
    SHOW_TIMER = true;
    TIMER_SYNC_TARGETS = "all";

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"duration\":300,\"action\":\"start\"}")));
    TEST_ASSERT_EQUAL_INT(1, fixture::sync_packet_count());

    StaticJsonDocument<1024> doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, fixture::last_sync_payload()));
    TEST_ASSERT_EQUAL_STRING("awtrix_self", doc["_sync"]["src"]);
    TEST_ASSERT_EQUAL_STRING("all", doc["_sync"]["tgt"]);
    TEST_ASSERT_EQUAL_STRING("start", doc["action"]);
    TEST_ASSERT_EQUAL_UINT32(300, doc["duration"].as<uint32_t>());
    TEST_ASSERT_FALSE(doc.containsKey("buzzer"));     // no config rides with run-state
    TEST_ASSERT_FALSE(doc.containsKey("bar_color"));
}

// S3 — a local config edit emits one full-snapshot config packet with NO action,
// NO duration, and NO sync_* (local identity is never propagated).
void test_S3_local_config_emits_snapshot_only(void) {
    SHOW_TIMER = true;
    TIMER_SYNC_TARGETS = "awtrix_peer";

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"buzzer\":\"countdown\",\"bar_color\":\"#FF0000\"}")));
    TEST_ASSERT_EQUAL_INT(1, fixture::sync_packet_count());

    StaticJsonDocument<2048> doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, fixture::last_sync_payload()));
    TEST_ASSERT_EQUAL_STRING("awtrix_self", doc["_sync"]["src"]);
    TEST_ASSERT_EQUAL_STRING("awtrix_peer", doc["_sync"]["tgt"][0]);   // array form
    TEST_ASSERT_EQUAL_STRING("countdown", doc["buzzer"]);
    TEST_ASSERT_EQUAL_UINT32(0xFF0000, doc["bar_color"].as<uint32_t>());
    TEST_ASSERT_FALSE(doc.containsKey("action"));
    TEST_ASSERT_FALSE(doc.containsKey("duration"));
    TEST_ASSERT_FALSE(doc.containsKey("sync_follow"));
    TEST_ASSERT_FALSE(doc.containsKey("sync_targets"));
}

// S4 — applySyncCommand gating: own-src echo ignored; follow consent required;
// targeting honored (all / member / non-member); duplicate (src,seq) dropped.
void test_S4_applySyncCommand_gating(void) {
    SHOW_TIMER = true;
    fixture::advance(1000);   // millis() > 0 so the dedup TTL math is well-defined

    // Own echo: a packet from this clock's own uniqueID is ignored.
    TIMER_SYNC_FOLLOW = true;
    TimerManager.applySyncCommand(
        "{\"_sync\":{\"src\":\"awtrix_self\",\"seq\":1,\"tgt\":\"all\"},\"action\":\"start\",\"duration\":300}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle), static_cast<int>(TimerManager.getState()));

    // Follow gate off: targeted packet from a peer is ignored.
    TIMER_SYNC_FOLLOW = false;
    TimerManager.applySyncCommand(
        "{\"_sync\":{\"src\":\"awtrix_o\",\"seq\":2,\"tgt\":\"all\"},\"action\":\"start\",\"duration\":300}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle), static_cast<int>(TimerManager.getState()));

    // Follow on but not targeted: ignored.
    TIMER_SYNC_FOLLOW = true;
    TimerManager.applySyncCommand(
        "{\"_sync\":{\"src\":\"awtrix_o\",\"seq\":3,\"tgt\":[\"awtrix_other\"]},\"action\":\"start\",\"duration\":300}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle), static_cast<int>(TimerManager.getState()));

    // Targeted by member list: applies.
    TimerManager.applySyncCommand(
        "{\"_sync\":{\"src\":\"awtrix_o\",\"seq\":4,\"tgt\":[\"awtrix_self\"]},\"action\":\"start\",\"duration\":300}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running), static_cast<int>(TimerManager.getState()));

    // Dedup: re-deliver seq 4 (the 3x redundant send) — dropped, no re-application.
    TimerManager.reset();
    TimerManager.applySyncCommand(
        "{\"_sync\":{\"src\":\"awtrix_o\",\"seq\":4,\"tgt\":[\"awtrix_self\"]},\"action\":\"start\",\"duration\":300}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle), static_cast<int>(TimerManager.getState()));

    // A new seq from the same src is accepted.
    TimerManager.applySyncCommand(
        "{\"_sync\":{\"src\":\"awtrix_o\",\"seq\":5,\"tgt\":\"all\"},\"action\":\"start\",\"duration\":300}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running), static_cast<int>(TimerManager.getState()));
}

// S5 — one-hop guard: an inbound command is applied but NEVER re-broadcast, even
// when this clock is itself configured to command peers.
void test_S5_remote_apply_does_not_rebroadcast(void) {
    SHOW_TIMER = true;
    TIMER_SYNC_FOLLOW  = true;
    TIMER_SYNC_TARGETS = "all";   // this clock would broadcast its own local actions

    TimerManager.applySyncCommand(
        "{\"_sync\":{\"src\":\"awtrix_o\",\"seq\":9,\"tgt\":\"all\"},\"action\":\"start\",\"duration\":120}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running), static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_INT(0, fixture::sync_packet_count());  // applied, not relayed
}

// S6 — sync off (empty target list) suppresses all broadcasting.
void test_S6_sync_off_never_broadcasts(void) {
    SHOW_TIMER = true;
    TIMER_SYNC_TARGETS = "";   // default

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"duration\":300,\"action\":\"start\"}")));
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"buzzer\":\"end\"}")));
    TEST_ASSERT_EQUAL_INT(0, fixture::sync_packet_count());
}

// ============================================================================
// U51 — Editing duration while Paused resets the timer to Idle with the new
// duration (remaining follows the new full duration; state publish == "idle").
// ============================================================================
void test_U51_setDuration_while_paused_resets_to_idle(void) {
    TimerManager.setDuration(300);
    TimerManager.start();
    fixture::advance(60000);            // 60s elapsed -> 240 remaining
    TimerManager.tick();
    TimerManager.pause();
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Paused),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_UINT32(240, TimerManager.getRemaining());

    // Update duration while paused -> reset to Idle with the new full duration.
    TimerManager.setDuration(600);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_UINT32(600, TimerManager.getRemaining());
    TEST_ASSERT_EQUAL_UINT32(600, TimerManager.getDuration());

    const PublishCall *st = fixture::last_publish(fixture::TIMER_STATE_TOPIC);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_EQUAL_STRING("idle", st->payload.c_str());
    const PublishCall *rem = fixture::last_publish(fixture::TIMER_REMAINING_TOPIC);
    TEST_ASSERT_NOT_NULL(rem);
    TEST_ASSERT_EQUAL_STRING("600", rem->payload.c_str());
}

// ============================================================================
// U52 — tick() delegates config-mode timing to the editor (issue #23): with the
// editor active, 30 s of no input auto-applies the edit and exits to Idle. Proves
// the timeout path runs through editor.tick() -> exitConfigMode -> setDuration.
// ============================================================================
void test_U52_tick_delegates_config_timeout_autoapplies_and_exits(void) {
    TimerManager.setDuration(300);      // 00:05:00
    TimerManager.enterConfigMode();     // millis()==0 seeds the idle clock
    TEST_ASSERT_TRUE(TimerManager.isInConfig());

    TimerManager.configAdjust(+1);      // HH 0 -> 1 (edit -> 01:05:00 = 3900s)
    TEST_ASSERT_EQUAL_UINT8(1, TimerManager.getConfigHH());

    // 30 s of no input: tick() must auto-apply through the editor and exit to Idle.
    fixture::advance(30000);
    TimerManager.tick();

    TEST_ASSERT_FALSE(TimerManager.isInConfig());
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_UINT32(3900, TimerManager.getDuration());   // committed via setDuration
}

// ============================================================================
// U53 — when the editor is NOT active, tick() runs the run-state machine: the
// config branch never intercepts a Running countdown (issue #23 delegation guard).
// ============================================================================
void test_U53_tick_runs_runstate_when_not_in_config(void) {
    TimerManager.setDuration(10);
    TimerManager.start();
    TEST_ASSERT_FALSE(TimerManager.isInConfig());
    TEST_ASSERT_EQUAL_UINT32(10, TimerManager.getRemaining());

    fixture::advance(4000);
    TimerManager.tick();                // run-state countdown, not the config branch

    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_UINT32(6, TimerManager.getRemaining());
}

// ============================================================================
// T1 — TIMER_SETTINGS_DESCS: UIntRange validators reject/accept at the exact
// boundaries, reached directly (not through the 300-line parseCommand).
// ============================================================================
void test_T1_table_uintrange_boundaries(void) {
    StaticJsonDocument<64> j;
    TcValue out;

    const TimerSettingDesc *fh = timerSettingByCmdKey("finished_hold");  // 1..300
    TEST_ASSERT_NOT_NULL(fh);
    j["v"] = 0;   TEST_ASSERT_FALSE(timerSettingParse(*fh, j["v"], out));
    j["v"] = 1;   TEST_ASSERT_TRUE (timerSettingParse(*fh, j["v"], out)); TEST_ASSERT_EQUAL_UINT32(1,   out.num);
    j["v"] = 300; TEST_ASSERT_TRUE (timerSettingParse(*fh, j["v"], out)); TEST_ASSERT_EQUAL_UINT32(300, out.num);
    j["v"] = 301; TEST_ASSERT_FALSE(timerSettingParse(*fh, j["v"], out));

    const TimerSettingDesc *cd = timerSettingByCmdKey("countdown_seconds");  // 0..30
    TEST_ASSERT_NOT_NULL(cd);
    j["v"] = 0;  TEST_ASSERT_TRUE (timerSettingParse(*cd, j["v"], out)); TEST_ASSERT_EQUAL_UINT32(0, out.num);
    j["v"] = 30; TEST_ASSERT_TRUE (timerSettingParse(*cd, j["v"], out));
    j["v"] = 31; TEST_ASSERT_FALSE(timerSettingParse(*cd, j["v"], out));
}

// ============================================================================
// T2 — Strict JSON types per check kind: Bool rows demand a real bool; numeric
// rows reject bool/string. (Same contract the legacy parseCommand enforced.)
// ============================================================================
void test_T2_table_strict_types(void) {
    StaticJsonDocument<64> j;
    TcValue out;

    const TimerSettingDesc *ie = timerSettingByCmdKey("icon_enabled");  // Bool
    j["v"] = true;   TEST_ASSERT_TRUE (timerSettingParse(*ie, j["v"], out)); TEST_ASSERT_TRUE(out.b);
    j["v"] = 1;      TEST_ASSERT_FALSE(timerSettingParse(*ie, j["v"], out));  // int is not bool
    j["v"] = "true"; TEST_ASSERT_FALSE(timerSettingParse(*ie, j["v"], out));

    const TimerSettingDesc *fh = timerSettingByCmdKey("finished_hold");  // numeric
    j["v"] = true;  TEST_ASSERT_FALSE(timerSettingParse(*fh, j["v"], out));   // bool is not number
    j["v"] = "50";  TEST_ASSERT_FALSE(timerSettingParse(*fh, j["v"], out));   // string is not number
}

// ============================================================================
// T3 — Bespoke validators: bar_color (int or #RRGGBB) and sync_targets (""/all/
// comma-list) live in the table via fn pointers.
// ============================================================================
void test_T3_table_bespoke_validators(void) {
    StaticJsonDocument<64> j;
    TcValue out;

    const TimerSettingDesc *bc = timerSettingByCmdKey("bar_color");
    j["v"] = 0xABCDEF;  TEST_ASSERT_TRUE (timerSettingParse(*bc, j["v"], out)); TEST_ASSERT_EQUAL_UINT32(0xABCDEF, out.num);
    j["v"] = 0x1000000; TEST_ASSERT_FALSE(timerSettingParse(*bc, j["v"], out));  // > 0xFFFFFF
    j["v"] = "#FF8800"; TEST_ASSERT_TRUE (timerSettingParse(*bc, j["v"], out)); TEST_ASSERT_EQUAL_UINT32(0xFF8800, out.num);
    j["v"] = "00aa55";  TEST_ASSERT_TRUE (timerSettingParse(*bc, j["v"], out)); TEST_ASSERT_EQUAL_UINT32(0x00AA55, out.num);
    j["v"] = "GGGGGG";  TEST_ASSERT_FALSE(timerSettingParse(*bc, j["v"], out));  // non-hex
    j["v"] = "12345";   TEST_ASSERT_FALSE(timerSettingParse(*bc, j["v"], out));  // wrong length

    const TimerSettingDesc *st = timerSettingByCmdKey("sync_targets");
    j["v"] = "";                       TEST_ASSERT_TRUE (timerSettingParse(*st, j["v"], out));
    j["v"] = "all";                    TEST_ASSERT_TRUE (timerSettingParse(*st, j["v"], out)); TEST_ASSERT_EQUAL_STRING("all", out.str.c_str());
    j["v"] = "awtrix_ab12,awtrix_cd";  TEST_ASSERT_TRUE (timerSettingParse(*st, j["v"], out));
    j["v"] = "bad token!";             TEST_ASSERT_FALSE(timerSettingParse(*st, j["v"], out));
    j["v"] = 5;                        TEST_ASSERT_FALSE(timerSettingParse(*st, j["v"], out));  // not a string
}

// ============================================================================
// T4 — NVS round-trip across the whole table (newly testable: was uncovered in
// Globals.cpp). Save -> clobber globals -> load restores every type.
// ============================================================================
void test_T4_table_nvs_roundtrip(void) {
    Preferences p;

    TIMER_FINISHED_HOLD = 123;
    TIMER_MAX_DURATION  = 4242;
    TIMER_BAR_ENABLED   = false;
    TIMER_BAR_COLOR     = 0x112233;
    TIMER_MELODY_TICK   = "mytick";
    TIMER_SYNC_FOLLOW   = true;
    TIMER_SYNC_TARGETS  = "all";
    timerSettingsSaveNvs(p);

    TIMER_FINISHED_HOLD = 1;
    TIMER_MAX_DURATION  = 1;
    TIMER_BAR_ENABLED   = true;
    TIMER_BAR_COLOR     = 0;
    TIMER_MELODY_TICK   = "x";
    TIMER_SYNC_FOLLOW   = false;
    TIMER_SYNC_TARGETS  = "";
    timerSettingsLoadNvs(p);

    TEST_ASSERT_EQUAL_UINT16(123,      TIMER_FINISHED_HOLD);
    TEST_ASSERT_EQUAL_UINT32(4242,     TIMER_MAX_DURATION);
    TEST_ASSERT_FALSE(TIMER_BAR_ENABLED);
    TEST_ASSERT_EQUAL_UINT32(0x112233, TIMER_BAR_COLOR);
    TEST_ASSERT_EQUAL_STRING("mytick", TIMER_MELODY_TICK.c_str());
    TEST_ASSERT_TRUE(TIMER_SYNC_FOLLOW);
    TEST_ASSERT_EQUAL_STRING("all",    TIMER_SYNC_TARGETS.c_str());
}

// ============================================================================
// T5 — dev.json is per-key best-effort: a valid key applies, an out-of-range
// sibling is skipped (not atomic-rejected), and other keys still land.
// ============================================================================
void test_T5_devjson_best_effort(void) {
    // Defaults after reset_all(): finished_hold=10, realert_interval=15.
    StaticJsonDocument<256> doc;
    doc["timer_finished_hold"]   = 50;        // valid -> applied
    doc["timer_realert_interval"] = 9999;     // out of range -> skipped
    doc["timer_bar_color"]       = "#0000FF"; // valid hex string -> applied
    timerSettingsLoadDevJson(doc.as<JsonObjectConst>());

    TEST_ASSERT_EQUAL_UINT16(50,       TIMER_FINISHED_HOLD);
    TEST_ASSERT_EQUAL_UINT16(15,       TIMER_REALERT_INTERVAL);  // unchanged (best-effort skip)
    TEST_ASSERT_EQUAL_UINT32(0x0000FF, TIMER_BAR_COLOR);
}

// ============================================================================
// T6 — Snapshot membership = inSnapshot column: config-block table keys are
// emitted; the local-identity sync keys are excluded (ADR-0006).
// ============================================================================
void test_T6_snapshot_excludes_local_identity(void) {
    StaticJsonDocument<512> doc;
    timerSettingsBuildSnapshot(doc);

    TEST_ASSERT_TRUE(doc.containsKey("finished_hold"));
    TEST_ASSERT_TRUE(doc.containsKey("bar_color"));
    TEST_ASSERT_TRUE(doc.containsKey("melody_tick"));
    TEST_ASSERT_TRUE(doc.containsKey("max_duration"));

    TEST_ASSERT_FALSE(doc.containsKey("sync_follow"));
    TEST_ASSERT_FALSE(doc.containsKey("sync_targets"));
}

// ============================================================================
// Member-backed config table (TIMER_MEMBER_CONFIG_DESCS) — the config block's
// SECOND table (B1). One hook row per member-backed key, routing through
// TimerManager's deep setters. See docs/adr/0009.
// ============================================================================

// T7 — the member-config table is well-formed: exactly the six B1 keys, every hook
// present, and disjoint from TIMER_SETTINGS_DESCS (the two halves never share a
// cmdKey). This pins the membership the old kMemberConfigKeys list guaranteed.
void test_T7_member_config_table_well_formed(void) {
    TEST_ASSERT_EQUAL_UINT32(6, (uint32_t)TIMER_MEMBER_CONFIG_DESC_COUNT);

    const char *expected[] = {"buzzer", "finished", "icon_idle",
                              "icon_running", "icon_paused", "icon_finished"};
    for (size_t e = 0; e < 6; ++e) {
        int hits = 0;
        for (size_t i = 0; i < TIMER_MEMBER_CONFIG_DESC_COUNT; ++i)
            if (strcmp(TIMER_MEMBER_CONFIG_DESCS[i].cmdKey, expected[e]) == 0) ++hits;
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, hits, expected[e]);   // present exactly once
    }

    for (size_t i = 0; i < TIMER_MEMBER_CONFIG_DESC_COUNT; ++i) {
        const TimerMemberConfigDesc &d = TIMER_MEMBER_CONFIG_DESCS[i];
        TEST_ASSERT_NOT_NULL(d.cmdKey);
        TEST_ASSERT_NOT_NULL((void *)d.validate);
        TEST_ASSERT_NOT_NULL((void *)d.apply);
        TEST_ASSERT_NOT_NULL((void *)d.emit);
        TEST_ASSERT_NULL(timerSettingByCmdKey(d.cmdKey));   // not also a declarative row
    }
}

// T8 — member rows validate (accept good / reject bad) and round-trip through the
// snapshot: after a config command the member snapshot emits the live values, and
// the table half (timerSettingsBuildSnapshot) does NOT carry them.
void test_T8_member_config_validate_and_snapshot_roundtrip(void) {
    SHOW_TIMER = true;

    const TimerMemberConfigDesc *buz = nullptr, *icon = nullptr;
    for (size_t i = 0; i < TIMER_MEMBER_CONFIG_DESC_COUNT; ++i) {
        if (strcmp(TIMER_MEMBER_CONFIG_DESCS[i].cmdKey, "buzzer")    == 0) buz  = &TIMER_MEMBER_CONFIG_DESCS[i];
        if (strcmp(TIMER_MEMBER_CONFIG_DESCS[i].cmdKey, "icon_idle") == 0) icon = &TIMER_MEMBER_CONFIG_DESCS[i];
    }
    TEST_ASSERT_NOT_NULL(buz);
    TEST_ASSERT_NOT_NULL(icon);

    // validate: pure (no global touched). Enum accepts canonical, rejects junk; icon
    // rejects an over-long name (> 32 chars).
    StaticJsonDocument<256> in;
    in["buzzer"]   = "countdown";
    in["bad"]      = "nope";
    in["longicon"] = "this_icon_name_is_far_too_long_to_be_valid_xx";
    TcValue v;
    TEST_ASSERT_TRUE (buz->validate(in["buzzer"], v));
    TEST_ASSERT_FALSE(buz->validate(in["bad"], v));
    TEST_ASSERT_FALSE(icon->validate(in["longicon"], v));

    // round-trip: a config command sets the live values; the member snapshot emits them.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand(
            "{\"buzzer\":\"countdown\",\"finished\":\"re-alert\",\"icon_idle\":\"idle\"}")));

    StaticJsonDocument<512> snap;
    timerMemberConfigBuildSnapshot(snap);
    TEST_ASSERT_EQUAL_STRING("countdown", snap["buzzer"]);
    TEST_ASSERT_EQUAL_STRING("re-alert",  snap["finished"]);
    TEST_ASSERT_EQUAL_STRING("idle",      snap["icon_idle"]);

    // disjoint in the snapshot too: the declarative table half omits the member keys.
    StaticJsonDocument<512> tbl;
    timerSettingsBuildSnapshot(tbl);
    TEST_ASSERT_FALSE(tbl.containsKey("buzzer"));
    TEST_ASSERT_FALSE(tbl.containsKey("icon_idle"));
}

// ============================================================================
// TIMER menu slot table (src/TimerMenu.cpp). The on-device menu's label/adjust
// logic, host-testable for the first time (MenuManager itself isn't host-built).
// Slot order: 0 buzzer, 1 countdown, 2 finished, 3 clear(hold), 4 alert(realert),
// 5 icon, 6 bar. See docs/adr/0008.
// ============================================================================

// M1 — table is well-formed: 7 slots, every slot labels, and each table-backed
// slot's cmdKey resolves to a descriptor whose type matches the slot kind (so the
// menu can't reference a key the settings table doesn't back, ADR-0007).
void test_M1_slot_table_well_formed(void) {
    TEST_ASSERT_EQUAL_UINT32(7, (uint32_t)TIMER_MENU_SLOT_COUNT);
    for (uint8_t i = 0; i < TIMER_MENU_SLOT_COUNT; ++i) {
        TEST_ASSERT_TRUE(timerMenuLabel(i).length() > 0);
        const TimerMenuSlot &s = TIMER_MENU_SLOTS[i];
        switch (s.kind) {
            case TimerMenuKind::EnumCycle:
                TEST_ASSERT_NOT_NULL(s.codec);
                TEST_ASSERT_TRUE(s.labelCount > 0);
                TEST_ASSERT_NOT_NULL((void *)s.getEnum);
                TEST_ASSERT_NOT_NULL((void *)s.setEnum);
                break;
            case TimerMenuKind::SteppedRange: {
                const TimerSettingDesc *d = timerSettingByCmdKey(s.cmdKey);
                TEST_ASSERT_NOT_NULL(d);
                TEST_ASSERT_TRUE(d->type == TcType::U16);
                TEST_ASSERT_TRUE(s.step > 0);
                break;
            }
            case TimerMenuKind::BoolToggle: {
                const TimerSettingDesc *d = timerSettingByCmdKey(s.cmdKey);
                TEST_ASSERT_NOT_NULL(d);
                TEST_ASSERT_TRUE(d->type == TcType::Bool);
                break;
            }
        }
    }
}

// M2 — enum slots cycle both ways and route through the real TimerManager setter
// (proven by the recorded MQTT publish), not a raw member poke.
void test_M2_enum_cycle_wraps_and_routes_via_setter(void) {
    TimerManager.setBuzzerMode(BuzzerMode::Off);
    MQTTManager.__test_reset();

    timerMenuAdjust(0, +1);   // Off -> End
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BuzzerMode::End, (uint8_t)TimerManager.getBuzzerMode());
    const PublishCall *buz = fixture::last_publish(fixture::TIMER_BUZZER_TOPIC);
    TEST_ASSERT_NOT_NULL(buz);
    TEST_ASSERT_EQUAL_STRING("end", buz->payload.c_str());

    timerMenuAdjust(0, +1);   // End -> Countdown
    timerMenuAdjust(0, +1);   // Countdown -> Off (wrap)
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BuzzerMode::Off, (uint8_t)TimerManager.getBuzzerMode());

    timerMenuAdjust(0, -1);   // Off -> Countdown (wrap backward)
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BuzzerMode::Countdown, (uint8_t)TimerManager.getBuzzerMode());

    // finished slot routes through its setter too (proven on the wire: the
    // row's publish hook puts the codec string on the canonical topic).
    TimerManager.setFinishedMode(FinishedMode::AutoClear);
    MQTTManager.__test_reset();
    timerMenuAdjust(2, +1);   // AutoClear -> Hold
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FinishedMode::Hold, (uint8_t)TimerManager.getFinishedMode());
    const PublishCall *fin = fixture::last_publish(fixture::TIMER_FINISHED_TOPIC);
    TEST_ASSERT_NOT_NULL(fin);
    TEST_ASSERT_EQUAL_STRING("hold", fin->payload.c_str());
}

// M3 — stepped ranges saturate at the descriptor bounds (no overshoot/underflow).
void test_M3_stepped_range_saturates(void) {
    // finished_hold (slot 3): step 5, lo 1, hi 300.
    TIMER_FINISHED_HOLD = 298;
    timerMenuAdjust(3, +1);                                  // 298 (+5 -> 303 > 300) -> cap
    TEST_ASSERT_EQUAL_UINT16(300, TIMER_FINISHED_HOLD);
    timerMenuAdjust(3, +1);
    TEST_ASSERT_EQUAL_UINT16(300, TIMER_FINISHED_HOLD);
    TIMER_FINISHED_HOLD = 4;
    timerMenuAdjust(3, -1);                                  // 4 (>=1+5? no) -> lo
    TEST_ASSERT_EQUAL_UINT16(1, TIMER_FINISHED_HOLD);

    // countdown_seconds (slot 1): step 1, lo 0, hi 30.
    TIMER_COUNTDOWN_SECONDS = 30;
    timerMenuAdjust(1, +1);
    TEST_ASSERT_EQUAL_UINT16(30, TIMER_COUNTDOWN_SECONDS);
    TIMER_COUNTDOWN_SECONDS = 0;
    timerMenuAdjust(1, -1);
    TEST_ASSERT_EQUAL_UINT16(0, TIMER_COUNTDOWN_SECONDS);
    timerMenuAdjust(1, +1);
    TEST_ASSERT_EQUAL_UINT16(1, TIMER_COUNTDOWN_SECONDS);
}

// M4 — bool toggles flip on either button.
void test_M4_bool_toggle_flips_both_directions(void) {
    TIMER_ICON_ENABLED = true;
    timerMenuAdjust(5, +1);
    TEST_ASSERT_FALSE(TIMER_ICON_ENABLED);
    timerMenuAdjust(5, -1);
    TEST_ASSERT_TRUE(TIMER_ICON_ENABLED);

    TIMER_BAR_ENABLED = false;
    timerMenuAdjust(6, +1);
    TEST_ASSERT_TRUE(TIMER_BAR_ENABLED);
    timerMenuAdjust(6, -1);
    TEST_ASSERT_FALSE(TIMER_BAR_ENABLED);
}

// M5 — the stepped clamp bounds ARE the descriptor's lo/hi (single source, ADR-0007):
// for every SteppedRange slot, saturate up == hi and down == lo.
void test_M5_stepped_clamps_to_descriptor_bounds(void) {
    for (uint8_t i = 0; i < TIMER_MENU_SLOT_COUNT; ++i) {
        const TimerMenuSlot &s = TIMER_MENU_SLOTS[i];
        if (s.kind != TimerMenuKind::SteppedRange) continue;
        const TimerSettingDesc *d = timerSettingByCmdKey(s.cmdKey);
        TEST_ASSERT_NOT_NULL(d);
        uint16_t &v = *static_cast<uint16_t *>(d->storage);

        v = (uint16_t)d->hi;
        timerMenuAdjust(i, +1);
        TEST_ASSERT_EQUAL_UINT16((uint16_t)d->hi, v);   // cannot exceed hi

        v = (uint16_t)d->lo;
        timerMenuAdjust(i, -1);
        TEST_ASSERT_EQUAL_UINT16((uint16_t)d->lo, v);   // cannot drop below lo
    }
}

// M6 — label formatting matches the on-screen strings.
void test_M6_label_formatting(void) {
    TIMER_FINISHED_HOLD = 10;
    TEST_ASSERT_EQUAL_STRING("CLEAR 10", timerMenuLabel(3).c_str());
    TimerManager.setBuzzerMode(BuzzerMode::End);
    TEST_ASSERT_EQUAL_STRING("BZR END", timerMenuLabel(0).c_str());
    TIMER_ICON_ENABLED = true;
    TEST_ASSERT_EQUAL_STRING("ICON ON", timerMenuLabel(5).c_str());
    TIMER_COUNTDOWN_SECONDS = 3;
    TEST_ASSERT_EQUAL_STRING("CDOWN 3", timerMenuLabel(1).c_str());
}

// M7 — an enum adjust applies + publishes live but DEFERS the NVS write; the write
// happens only on the commit (persistConfig). Proven via Preferences::begin_calls
// (persist() brackets a begin("timer")). See ADR-0008.
void test_M7_enum_adjust_defers_persist_until_commit(void) {
    TimerManager.setBuzzerMode(BuzzerMode::End);   // known starting point (default persist)
    int before = Preferences::begin_calls;

    timerMenuAdjust(0, +1);   // End -> Countdown, persist deferred
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BuzzerMode::Countdown, (uint8_t)TimerManager.getBuzzerMode());
    TEST_ASSERT_EQUAL_INT(before, Preferences::begin_calls);   // no "timer"-ns write yet

    TimerManager.persistConfig();
    TEST_ASSERT_TRUE(Preferences::begin_calls > before);       // commit flushed it
}

// M8 — the two enum slots source their on-screen label from the codec table's
// menu column (single source of truth, #19), proven through the public
// timerMenuLabel() for every enum value. Guards against the menu re-growing a
// private label copy that could drift from the codec table.
void test_M8_enum_labels_source_from_codec(void) {
    for (uint8_t i = 0; i < (uint8_t)BuzzerMode::COUNT; ++i) {
        TimerManager.setBuzzerMode((BuzzerMode)i);
        TEST_ASSERT_EQUAL_STRING(TIMER_BUZZER_CODEC[i].menu, timerMenuLabel(0).c_str());
    }
    for (uint8_t i = 0; i < (uint8_t)FinishedMode::COUNT; ++i) {
        TimerManager.setFinishedMode((FinishedMode)i);
        TEST_ASSERT_EQUAL_STRING(TIMER_FINISHED_CODEC[i].menu, timerMenuLabel(2).c_str());
    }
}

// ============================================================================
// TIMER per-enum codec table (src/TimerEnums.cpp). The fifth descriptor-table
// family member: one row per enum value, indexed by the enum's numeric value,
// carrying {wire, menu, ha, aliases}. The wire column is the MQTT/HTTP/sync
// contract consumed by parse*/toString in TimerManager; menu/ha columns are
// rewired by the two follow-up issues. See docs/adr/0010.
// ============================================================================

// T9 — both codec tables are well-formed: exactly COUNT rows (also a static_assert
// in TimerEnums.cpp, checked here at runtime too), and every row carries a
// non-empty wire / menu / ha string. Aliases are optional (nullptr allowed).
void test_T9_codec_tables_well_formed(void) {
    TEST_ASSERT_EQUAL_UINT32((uint32_t)BuzzerMode::COUNT,
                             (uint32_t)TIMER_BUZZER_CODEC_COUNT);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)FinishedMode::COUNT,
                             (uint32_t)TIMER_FINISHED_CODEC_COUNT);

    for (size_t i = 0; i < TIMER_BUZZER_CODEC_COUNT; ++i) {
        const TimerEnumCodec &r = TIMER_BUZZER_CODEC[i];
        TEST_ASSERT_NOT_NULL(r.wire); TEST_ASSERT_TRUE(strlen(r.wire) > 0);
        TEST_ASSERT_NOT_NULL(r.menu); TEST_ASSERT_TRUE(strlen(r.menu) > 0);
        TEST_ASSERT_NOT_NULL(r.ha);   TEST_ASSERT_TRUE(strlen(r.ha)   > 0);
    }
    for (size_t i = 0; i < TIMER_FINISHED_CODEC_COUNT; ++i) {
        const TimerEnumCodec &r = TIMER_FINISHED_CODEC[i];
        TEST_ASSERT_NOT_NULL(r.wire); TEST_ASSERT_TRUE(strlen(r.wire) > 0);
        TEST_ASSERT_NOT_NULL(r.menu); TEST_ASSERT_TRUE(strlen(r.menu) > 0);
        TEST_ASSERT_NOT_NULL(r.ha);   TEST_ASSERT_TRUE(strlen(r.ha)   > 0);
    }
}

// T10 — the wire-string contract round-trips through the table-backed parse/
// toString, for EVERY enum value: parse(toString(v)) == v, and toString(v) is the
// row's canonical wire spelling. Plus every alias the current parser accepts still
// parses, and parsing is case-insensitive (arbitrary case round-trips).
void test_T10_codec_roundtrip_aliases_and_case(void) {
    // buzzer: parse(toString(v)) == v for all values; toString == table wire.
    for (uint8_t i = 0; i < (uint8_t)BuzzerMode::COUNT; ++i) {
        BuzzerMode v = (BuzzerMode)i;
        TimerManager.setBuzzerMode(v);
        const char *s = TimerManager.buzzerModeString();
        TEST_ASSERT_EQUAL_STRING(TIMER_BUZZER_CODEC[i].wire, s);
        BuzzerMode back;
        TEST_ASSERT_TRUE(TimerManager_::parseBuzzerMode(String(s), back));
        TEST_ASSERT_EQUAL_UINT8(i, (uint8_t)back);
    }
    // finished: same.
    for (uint8_t i = 0; i < (uint8_t)FinishedMode::COUNT; ++i) {
        FinishedMode v = (FinishedMode)i;
        TimerManager.setFinishedMode(v);
        const char *s = TimerManager.finishedModeString();
        TEST_ASSERT_EQUAL_STRING(TIMER_FINISHED_CODEC[i].wire, s);
        FinishedMode back;
        TEST_ASSERT_TRUE(TimerManager_::parseFinishedMode(String(s), back));
        TEST_ASSERT_EQUAL_UINT8(i, (uint8_t)back);
    }

    // Every alias declared in the tables still parses to its row's enum value.
    for (uint8_t i = 0; i < (uint8_t)BuzzerMode::COUNT; ++i) {
        if (!TIMER_BUZZER_CODEC[i].aliases) continue;
        BuzzerMode back;
        TEST_ASSERT_TRUE(TimerManager_::parseBuzzerMode(String(TIMER_BUZZER_CODEC[i].aliases), back));
        TEST_ASSERT_EQUAL_UINT8(i, (uint8_t)back);
    }
    for (uint8_t i = 0; i < (uint8_t)FinishedMode::COUNT; ++i) {
        if (!TIMER_FINISHED_CODEC[i].aliases) continue;
        FinishedMode back;
        TEST_ASSERT_TRUE(TimerManager_::parseFinishedMode(String(TIMER_FINISHED_CODEC[i].aliases), back));
        TEST_ASSERT_EQUAL_UINT8(i, (uint8_t)back);
    }

    // The specific legacy aliases the old parser accepted, spelled out explicitly.
    FinishedMode f;
    TEST_ASSERT_TRUE(TimerManager_::parseFinishedMode("autoclear", f));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FinishedMode::AutoClear, (uint8_t)f);
    TEST_ASSERT_TRUE(TimerManager_::parseFinishedMode("realert", f));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FinishedMode::ReAlert, (uint8_t)f);

    // Case-insensitive: arbitrary case parses for canonical, hyphenated, and aliases.
    BuzzerMode b;
    TEST_ASSERT_TRUE(TimerManager_::parseBuzzerMode("COUNTDOWN", b));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BuzzerMode::Countdown, (uint8_t)b);
    TEST_ASSERT_TRUE(TimerManager_::parseFinishedMode("Auto-Clear", f));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FinishedMode::AutoClear, (uint8_t)f);
    TEST_ASSERT_TRUE(TimerManager_::parseFinishedMode("ReAlert", f));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FinishedMode::ReAlert, (uint8_t)f);

    // Junk is still rejected.
    TEST_ASSERT_FALSE(TimerManager_::parseBuzzerMode("nope", b));
    TEST_ASSERT_FALSE(TimerManager_::parseFinishedMode("nope", f));
}

// ============================================================================
// TimerConfigEditor — direct-drive tests for the extracted duration editor.
// These exercise the pure value object in isolation (no TimerManager singleton,
// no DisplayManager). The cap math reads the TIMER_MAX_DURATION global, so each
// test sets it explicitly. See docs/adr/0011-timer-config-editor-extraction.md.
// ============================================================================

// CE1 — enter() decomposes a duration into HH/MM/SS, starts on the HH field,
// and becomes active. (Mirrors the U14 setup, now on the editor directly.)
void test_CE1_enter_decomposes_and_activates(void) {
    TimerConfigEditor ed;
    TEST_ASSERT_FALSE(ed.isActive());
    ed.enter(86399);   // 23:59:59
    TEST_ASSERT_TRUE(ed.isActive());
    TEST_ASSERT_EQUAL_UINT8(23, ed.hh());
    TEST_ASSERT_EQUAL_UINT8(59, ed.mm());
    TEST_ASSERT_EQUAL_UINT8(59, ed.ss());
    TEST_ASSERT_EQUAL_UINT8(0,  ed.field());
}

// CE2 — adjust() wraps the HH field at the dynamic cap. With MM=SS=59 and the
// default 24h max, dynMax(HH) = (86400-3599)/3600 = 23, so +1 past 23 wraps to 0.
void test_CE2_adjust_default_cap_wraps_HH_at_23(void) {
    TIMER_MAX_DURATION = 86400;
    TimerConfigEditor ed;
    ed.enter(86399);   // 23:59:59, field=HH
    TEST_ASSERT_EQUAL_UINT8(23, ed.hh());

    ed.adjust(+1);     // 23 -> 24 > cap 23 -> 0
    TEST_ASSERT_EQUAL_UINT8(0, ed.hh());

    ed.adjust(+1);     // 0 -> 1
    TEST_ASSERT_EQUAL_UINT8(1, ed.hh());
}

// CE3 — a tight cap (TIMER_MAX_DURATION=3600) caps HH at 1 with MM=SS=0, and the
// cap recomputes when other fields move: with HH=1, dynMax(MM) collapses to 0.
void test_CE3_adjust_tight_cap_recomputes_per_field(void) {
    TIMER_MAX_DURATION = 3600;
    TimerConfigEditor ed;
    ed.enter(1);            // 00:00:01, field=HH

    ed.cycleField();        // -> MM
    ed.cycleField();        // -> SS
    ed.adjust(-1);          // SS 1 -> 0
    TEST_ASSERT_EQUAL_UINT8(0, ed.ss());
    ed.cycleField();        // -> HH

    ed.adjust(+1);          // dynMax(HH | MM=0,SS=0) = 1: 0 -> 1
    TEST_ASSERT_EQUAL_UINT8(1, ed.hh());
    ed.adjust(+1);          // 1 -> 2 > cap 1 -> wrap to 0
    TEST_ASSERT_EQUAL_UINT8(0, ed.hh());

    ed.adjust(+1);          // back to 1
    ed.cycleField();        // -> MM; with HH=1, dynMax(MM) = 0 -> wrap to 0
    ed.adjust(+1);
    TEST_ASSERT_EQUAL_UINT8(0, ed.mm());
}

// CE4 — TIMER_MAX_DURATION == 0 disables the cap (legacy 99/59/59 wrap).
void test_CE4_adjust_no_cap_when_max_is_zero(void) {
    TIMER_MAX_DURATION = 0;
    TimerConfigEditor ed;
    ed.enter(1);
    for (int i = 0; i < 99; ++i) ed.adjust(+1);
    TEST_ASSERT_EQUAL_UINT8(99, ed.hh());
    ed.adjust(+1);          // 99 -> wrap to 0
    TEST_ASSERT_EQUAL_UINT8(0, ed.hh());
}

// CE5 — decrement at 0 wraps to the dynamic cap, not stay pinned at 0.
void test_CE5_adjust_decrement_wraps_to_dynamic_max(void) {
    TIMER_MAX_DURATION = 3600;
    TimerConfigEditor ed;
    ed.enter(1);
    ed.cycleField();        // MM
    ed.cycleField();        // SS
    ed.adjust(-1);          // SS 1 -> 0
    ed.cycleField();        // HH
    ed.adjust(-1);          // HH 0, dynMax=1, -1 -> wrap to 1
    TEST_ASSERT_EQUAL_UINT8(1, ed.hh());
}

// CE6 — cycleField rotates HH -> MM -> SS -> HH.
void test_CE6_cycleField_rotates(void) {
    TimerConfigEditor ed;
    ed.enter(0);
    TEST_ASSERT_EQUAL_UINT8(0, ed.field());
    ed.cycleField(); TEST_ASSERT_EQUAL_UINT8(1, ed.field());
    ed.cycleField(); TEST_ASSERT_EQUAL_UINT8(2, ed.field());
    ed.cycleField(); TEST_ASSERT_EQUAL_UINT8(0, ed.field());
}

// CE7 — exit() recomposes HH/MM/SS back to seconds and deactivates. The edited
// 01:00:00 round-trips to 3600 with no clamping needed (cap held during the edit).
void test_CE7_exit_recomposes_and_deactivates(void) {
    TIMER_MAX_DURATION = 3600;
    TimerConfigEditor ed;
    ed.enter(1);
    ed.cycleField();        // MM
    ed.cycleField();        // SS
    ed.adjust(-1);          // SS 1 -> 0
    ed.cycleField();        // HH
    ed.adjust(+1);          // HH 0 -> 1  (now 01:00:00)
    TEST_ASSERT_EQUAL_UINT32(3600, ed.exit());
    TEST_ASSERT_FALSE(ed.isActive());
}

// ============================================================================
// TimerConfigEditor::tick — config-mode timing in isolation (issue #23).
// These drive the editor's auto-repeat + 30 s no-input timeout with injected
// time and button state, no TimerManager singleton / PeripheryManager / display.
// See docs/adr/0012-timer-config-timing-in-editor.md.
// ============================================================================

// CE8 — 30 s of no input makes tick() report TimedOut so the caller auto-applies.
// noteInput(0) seeds the idle clock; the window is TIMER_CONFIG_TIMEOUT seconds.
void test_CE8_tick_times_out_after_no_input(void) {
    TIMER_CONFIG_TIMEOUT = 30;
    TimerConfigEditor ed;
    ed.enter(60);
    ed.noteInput(0);

    TimerConfigEditor::ButtonState none;
    TEST_ASSERT_EQUAL(TimerConfigEditor::TickOutcome::Active,
                      ed.tick(29000, none));   // 29 s < 30 s window
    TEST_ASSERT_EQUAL(TimerConfigEditor::TickOutcome::TimedOut,
                      ed.tick(30000, none));   // 30 s elapsed
}

// CE9 — a held right button auto-repeats +1 on the current field: the hold clock
// starts at the first tick the button is observed pressed; the first step fires
// once the 500 ms long-press threshold is crossed, then one step per 250 ms cadence
// window, and nothing in between. Field is SS (no cap interference).
void test_CE9_held_button_autorepeats_at_cadence(void) {
    TIMER_MAX_DURATION = 0;        // disable cap: SS wraps at 59, predictable +1 steps
    TIMER_CONFIG_TIMEOUT = 30;     // well above the cadence under test
    TimerConfigEditor ed;
    ed.enter(0);                   // 00:00:00, field=HH
    ed.cycleField(); ed.cycleField();   // -> SS
    ed.noteInput(1000);

    TimerConfigEditor::ButtonState right; right.rightPressed = true;

    ed.tick(1000, right);  TEST_ASSERT_EQUAL_UINT8(0, ed.ss());   // press observed: hold clock starts
    ed.tick(1499, right);  TEST_ASSERT_EQUAL_UINT8(0, ed.ss());   // 499 ms held < 500: no step
    ed.tick(1500, right);  TEST_ASSERT_EQUAL_UINT8(1, ed.ss());   // threshold crossed: first step
    ed.tick(1700, right);  TEST_ASSERT_EQUAL_UINT8(1, ed.ss());   // +200 ms < 250: no step
    ed.tick(1750, right);  TEST_ASSERT_EQUAL_UINT8(2, ed.ss());   // +250 ms: next step
    ed.tick(2000, right);  TEST_ASSERT_EQUAL_UINT8(3, ed.ss());   // +250 ms: next step
}

// CE10 — the left button auto-repeats -1, and releasing the button clears the hold
// clock so a re-press must wait the full long-press threshold again (no instant step).
void test_CE10_left_decrements_and_release_rewaits(void) {
    TIMER_MAX_DURATION = 0;
    TIMER_CONFIG_TIMEOUT = 30;
    TimerConfigEditor ed;
    ed.enter(5);                   // 00:00:05, field=HH
    ed.cycleField(); ed.cycleField();   // -> SS = 5
    ed.noteInput(1000);

    TimerConfigEditor::ButtonState left;  left.leftPressed  = true;
    TimerConfigEditor::ButtonState none;

    ed.tick(1000, left);  TEST_ASSERT_EQUAL_UINT8(5, ed.ss());   // press observed
    ed.tick(1500, left);  TEST_ASSERT_EQUAL_UINT8(4, ed.ss());   // threshold: first -1 step
    ed.tick(1750, left);  TEST_ASSERT_EQUAL_UINT8(3, ed.ss());   // +250 ms: next -1 step

    ed.tick(1800, none);  TEST_ASSERT_EQUAL_UINT8(3, ed.ss());   // released: hold clock cleared

    // Re-press: must hold the full threshold again before stepping (no instant step).
    ed.tick(1900, left);  TEST_ASSERT_EQUAL_UINT8(3, ed.ss());   // press re-observed
    ed.tick(2399, left);  TEST_ASSERT_EQUAL_UINT8(3, ed.ss());   // 499 ms held < 500: no step
    ed.tick(2400, left);  TEST_ASSERT_EQUAL_UINT8(2, ed.ss());   // threshold crossed: step
}

// CE11 — holding a button past the 30 s window never times out: each auto-repeat
// counts as input and resets the idle clock, so the editor stays Active throughout
// (matching the on-device "holding to scrub keeps the edit alive" behaviour).
void test_CE11_hold_suppresses_timeout(void) {
    TIMER_MAX_DURATION = 0;
    TIMER_CONFIG_TIMEOUT = 30;
    TimerConfigEditor ed;
    ed.enter(0);
    ed.cycleField(); ed.cycleField();   // -> SS
    ed.noteInput(1000);

    TimerConfigEditor::ButtonState right; right.rightPressed = true;

    // Tick at a steady on-device-like cadence across well past 30 s of holding.
    for (unsigned long now = 1000; now <= 40000; now += 200) {
        TEST_ASSERT_EQUAL(TimerConfigEditor::TickOutcome::Active, ed.tick(now, right));
    }
}

// CE12 — regression guard for issue #25: ButtonState is constructible from two button
// reads. The paren-init form below has no matching overload until the explicit
// (bool,bool) constructor exists (paren aggregate-init is C++20, off here), so this
// pins the constructor that lets the device's gnu++11 two-arg brace-init compile.
// A default-constructed ButtonState stays {false, false}.
void test_CE12_buttonstate_constructible_from_two_reads(void) {
    TimerConfigEditor::ButtonState b(true, false);
    TEST_ASSERT_TRUE(b.leftPressed);
    TEST_ASSERT_FALSE(b.rightPressed);

    TimerConfigEditor::ButtonState none;
    TEST_ASSERT_FALSE(none.leftPressed);
    TEST_ASSERT_FALSE(none.rightPressed);
}

// ============================================================================
// W1 — wire seam: the canonical ArduinoHA data-topic builder (issue #31)
// The Timer's real external interface is the wire. This pins the exact topic
// shape ArduinoHA emits on device ({dataPrefix}/{deviceUniqueId}/{entityId}/
// stat_t per HASerializer::generateDataTopic + HAStateTopic) so a format
// drift in the shared builder fails here, not in Home Assistant.
// ============================================================================
void test_W1_state_topic_builder_formats_canonical_topic(void) {
    char topic[160];
    formatTimerHaDataTopic("awtrix_self", "a1b2c3d4e5f6", "d4e5f6_timer_state",
                           topic, sizeof(topic));
    TEST_ASSERT_EQUAL_STRING("awtrix_self/a1b2c3d4e5f6/d4e5f6_timer_state/stat_t", topic);
}

// ============================================================================
// W2 — wire seam end-to-end: start() puts payload "running" on the state topic
// Asserts the real wire contract (topic + payload), not the dispatch path:
// after start, the broker would receive "running" on the canonical state
// topic. The expected topic is the fixture literal — spelled independently of
// the builder — so a wrong-topic regression anywhere in the publish path
// (id format, topic format, routing) fails here.
// ============================================================================
void test_W2_start_publishes_running_on_state_topic(void) {
    TimerManager.start();

    const PublishCall *wire = fixture::last_publish(fixture::TIMER_STATE_TOPIC);
    TEST_ASSERT_NOT_NULL(wire);
    TEST_ASSERT_EQUAL_STRING("running", wire->payload.c_str());
}

// ============================================================================
// W3 — wire fidelity across the lifecycle: after start the broker would see
// "running" on the state topic; after AutoClear runs out, "idle" on the SAME
// topic. Together with W2 this is acceptance criterion 4 of issue #31.
// ============================================================================
void test_W3_autoclear_publishes_idle_on_state_topic(void) {
    TimerManager.setDuration(5);
    TimerManager.start();
    const PublishCall *afterStart = fixture::last_publish(fixture::TIMER_STATE_TOPIC);
    TEST_ASSERT_NOT_NULL(afterStart);
    TEST_ASSERT_EQUAL_STRING("running", afterStart->payload.c_str());

    // Run out the countdown (-> Finished), then the AutoClear hold (-> Idle).
    fixture::advance(5000);
    TimerManager.tick();
    fixture::advance(static_cast<uint32_t>(TIMER_FINISHED_HOLD) * 1000U + 100U);
    TimerManager.tick();

    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));
    const PublishCall *afterClear = fixture::last_publish(fixture::TIMER_STATE_TOPIC);
    TEST_ASSERT_NOT_NULL(afterClear);
    TEST_ASSERT_EQUAL_STRING("idle", afterClear->payload.c_str());
}

// ============================================================================
// W4 — wire fidelity for the remaining key (issue #32): start() puts the
// remaining seconds, as a plain decimal string, on the canonical remaining
// topic. Payload format pins the retired HASensorNumber(PrecisionP0) setValue
// path; the expected topic is the fixture literal, spelled independently of
// the TimerHa builders.
// ============================================================================
void test_W4_start_publishes_remaining_seconds_on_remaining_topic(void) {
    TimerManager.start();   // default fixture duration: 300 s

    const PublishCall *wire = fixture::last_publish(fixture::TIMER_REMAINING_TOPIC);
    TEST_ASSERT_NOT_NULL(wire);
    TEST_ASSERT_EQUAL_STRING("300", wire->payload.c_str());
}

// ============================================================================
// W5 — the periodic remaining republish keeps its throttle (issue #32): with
// TIMER_PUBLISH_INTERVAL = 5 s, driving tick() every 250 ms for 12 s of run
// puts exactly two periodic publishes on the remaining topic (at the 5 s and
// 10 s boundaries) — not one per tick or per elapsed second. Routing through
// the wire seam changed HOW the publish is expressed, not WHEN it fires.
// ============================================================================
void test_W5_tick_republishes_remaining_only_at_publish_interval(void) {
    TIMER_PUBLISH_INTERVAL = 5;
    TimerManager.setDuration(60);
    TimerManager.start();   // publishes remaining "60" immediately
    int baseline = fixture::count_publish(fixture::TIMER_REMAINING_TOPIC);

    for (int t = 250; t <= 12000; t += 250) {
        fixture::advance(250);
        TimerManager.tick();
    }

    TEST_ASSERT_EQUAL_INT(2, fixture::count_publish(fixture::TIMER_REMAINING_TOPIC) - baseline);
    const PublishCall *rem = fixture::last_publish(fixture::TIMER_REMAINING_TOPIC);
    TEST_ASSERT_NOT_NULL(rem);
    TEST_ASSERT_EQUAL_STRING("50", rem->payload.c_str());   // 60 s - 10 s boundary
}

// ============================================================================
// W6 — wire fidelity for the finished enum (issue #33, the PRD's marquee
// regression test): setting finished mode to hold puts the canonical codec
// wire string "hold" — never a numeric index, never the HA label "Hold" —
// on the canonical finished topic. Driven through parseCommand so the whole
// validate→apply→publish chain of the member-config row is under test; the
// expected topic is the fixture literal, spelled independently of the
// TimerHa builders, so a wrong-topic regression fails here too.
// ============================================================================
void test_W6_finished_hold_publishes_wire_string_on_finished_topic(void) {
    TimerManager.parseCommand("{\"finished\":\"hold\"}");

    const PublishCall *wire = fixture::last_publish(fixture::TIMER_FINISHED_TOPIC);
    TEST_ASSERT_NOT_NULL(wire);
    TEST_ASSERT_EQUAL_STRING("hold", wire->payload.c_str());
}

// ============================================================================
// W7 — wire fidelity for the buzzer enum (issue #33): a buzzer change puts the
// canonical codec wire string "countdown" — never the enum's numeric index —
// on the canonical buzzer topic. Same parseCommand-driven chain as W6.
// ============================================================================
void test_W7_buzzer_change_publishes_codec_string_on_buzzer_topic(void) {
    TimerManager.parseCommand("{\"buzzer\":\"countdown\"}");

    const PublishCall *wire = fixture::last_publish(fixture::TIMER_BUZZER_TOPIC);
    TEST_ASSERT_NOT_NULL(wire);
    TEST_ASSERT_EQUAL_STRING("countdown", wire->payload.c_str());
}

// ============================================================================
// W8 — topic isolation for the enum keys (issue #33): each key publishes to
// its OWN canonical topic and nothing crosses over, and a no-op set (same
// value again) publishes nothing — the setter equality-skip survives the
// hook routing. Together with W6/W7 this makes a wrong-topic change fail CI.
// ============================================================================
void test_W8_enum_keys_publish_only_their_own_topic_and_skip_noops(void) {
    TimerManager.parseCommand("{\"buzzer\":\"countdown\"}");
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_BUZZER_TOPIC));
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_FINISHED_TOPIC));

    TimerManager.parseCommand("{\"finished\":\"hold\"}");
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_FINISHED_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_BUZZER_TOPIC));

    // No-op sets: same values again -> no further publish on either topic.
    TimerManager.parseCommand("{\"buzzer\":\"countdown\",\"finished\":\"hold\"}");
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_BUZZER_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_FINISHED_TOPIC));
}

// ============================================================================
// W9 — wire fidelity for the duration key (issue #34): a duration change puts
// the trimmed-HMS clock string — the format the retired HAText::setState path
// sent — on the canonical duration topic. Driven through parseCommand so the
// whole validate→apply→publish chain is under test; the expected topic is the
// fixture literal, spelled independently of the TimerHa builders.
// ============================================================================
void test_W9_duration_change_publishes_hms_on_duration_topic(void) {
    TimerManager.parseCommand("{\"duration\":3661}");

    const PublishCall *wire = fixture::last_publish(fixture::TIMER_DURATION_TOPIC);
    TEST_ASSERT_NOT_NULL(wire);
    TEST_ASSERT_EQUAL_STRING("1:01:01", wire->payload.c_str());
}

// ============================================================================
// W10 — wire fidelity for the icons key (issue #34): an icon change puts the
// aggregate four-state JSON — byte-identical to the retired direct
// mqtt.publish() composer — on the plain prefix topic {MQTT_PREFIX}/timer/
// icons (the one published Timer topic that is NOT an HA entity data topic).
// Driven through parseCommand so the member-config row's validate→apply→
// publish chain is under test; expected topic is the fixture literal.
// ============================================================================
void test_W10_icon_change_publishes_aggregate_json_on_icons_topic(void) {
    TimerManager.parseCommand("{\"icon_idle\":\"clock\"}");

    const PublishCall *wire = fixture::last_publish(fixture::TIMER_ICONS_TOPIC);
    TEST_ASSERT_NOT_NULL(wire);
    TEST_ASSERT_EQUAL_STRING(
        "{\"idle\":\"clock\",\"running\":\"\",\"paused\":\"\",\"finished\":\"\"}",
        wire->payload.c_str());
}

// ============================================================================
// W11 — no-op sets stay silent for the migrated keys (issue #34): re-sending
// the current duration and icon values publishes nothing further on either
// topic — the setter equality-skip survives the seam/hook routing. Completes
// the no-op coverage W8 established for the enum keys.
// ============================================================================
void test_W11_noop_duration_and_icon_sets_do_not_publish(void) {
    TimerManager.parseCommand("{\"duration\":120,\"icon_idle\":\"clock\"}");
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_DURATION_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_ICONS_TOPIC));

    // Same values again -> no further publish on either topic.
    TimerManager.parseCommand("{\"duration\":120,\"icon_idle\":\"clock\"}");
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_DURATION_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_ICONS_TOPIC));
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
    // U14–U17 retargeted to TimerConfigEditor (test_CE2..CE5); see note above their defs.
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
    RUN_TEST(test_U50_parseCommand_icon_enabled_strict_bool);
    RUN_TEST(test_U51_setDuration_while_paused_resets_to_idle);
    RUN_TEST(test_U52_tick_delegates_config_timeout_autoapplies_and_exits);
    RUN_TEST(test_U53_tick_runs_runstate_when_not_in_config);
    RUN_TEST(test_U32_descriptor_table_well_formed);
    RUN_TEST(test_U33_descriptor_ids_unique);
    RUN_TEST(test_U34_descriptor_type_specific_fields);
    RUN_TEST(test_U35_select_options_match_enums);
    RUN_TEST(test_U54_timer_ha_ids_create_teardown_symmetric);
    RUN_TEST(test_D1_view_idle_shows_duration_no_bar);
    RUN_TEST(test_D2_view_running_shows_remaining_with_bar);
    RUN_TEST(test_D3_view_finished_blinks_0_00);
    RUN_TEST(test_D4_view_bar_geometry_right_anchored);
    RUN_TEST(test_D5_view_config_screen);
    RUN_TEST(test_D6_formatTimerDisplay_vs_wire_string);
    RUN_TEST(test_D7_view_icon_disabled_reflows_text_and_bar);
    RUN_TEST(test_D8_view_duration_edit_while_running_buffers_bar);
    RUN_TEST(test_U44_getStateJson_idle_snapshot);
    RUN_TEST(test_U45_getStateJson_running_is_wallclock_fresh);
    RUN_TEST(test_U46_getStateJson_paused_frozen);
    RUN_TEST(test_U47_getStateJson_finished_zero);
    RUN_TEST(test_U48_getStateJson_disabled_still_200_shape);
    RUN_TEST(test_U49_getStateJson_enum_canonical_spellings);
    RUN_TEST(test_S1_sync_settings_validation_atomic_reject);
    RUN_TEST(test_S2_local_start_emits_runstate_only);
    RUN_TEST(test_S3_local_config_emits_snapshot_only);
    RUN_TEST(test_S4_applySyncCommand_gating);
    RUN_TEST(test_S5_remote_apply_does_not_rebroadcast);
    RUN_TEST(test_S6_sync_off_never_broadcasts);
    RUN_TEST(test_T1_table_uintrange_boundaries);
    RUN_TEST(test_T2_table_strict_types);
    RUN_TEST(test_T3_table_bespoke_validators);
    RUN_TEST(test_T4_table_nvs_roundtrip);
    RUN_TEST(test_T5_devjson_best_effort);
    RUN_TEST(test_T6_snapshot_excludes_local_identity);
    RUN_TEST(test_T7_member_config_table_well_formed);
    RUN_TEST(test_T8_member_config_validate_and_snapshot_roundtrip);
    RUN_TEST(test_M1_slot_table_well_formed);
    RUN_TEST(test_M2_enum_cycle_wraps_and_routes_via_setter);
    RUN_TEST(test_M3_stepped_range_saturates);
    RUN_TEST(test_M4_bool_toggle_flips_both_directions);
    RUN_TEST(test_M5_stepped_clamps_to_descriptor_bounds);
    RUN_TEST(test_M6_label_formatting);
    RUN_TEST(test_M7_enum_adjust_defers_persist_until_commit);
    RUN_TEST(test_M8_enum_labels_source_from_codec);
    RUN_TEST(test_T9_codec_tables_well_formed);
    RUN_TEST(test_T10_codec_roundtrip_aliases_and_case);
    RUN_TEST(test_CE1_enter_decomposes_and_activates);
    RUN_TEST(test_CE2_adjust_default_cap_wraps_HH_at_23);
    RUN_TEST(test_CE3_adjust_tight_cap_recomputes_per_field);
    RUN_TEST(test_CE4_adjust_no_cap_when_max_is_zero);
    RUN_TEST(test_CE5_adjust_decrement_wraps_to_dynamic_max);
    RUN_TEST(test_CE6_cycleField_rotates);
    RUN_TEST(test_CE7_exit_recomposes_and_deactivates);
    RUN_TEST(test_CE8_tick_times_out_after_no_input);
    RUN_TEST(test_CE9_held_button_autorepeats_at_cadence);
    RUN_TEST(test_CE10_left_decrements_and_release_rewaits);
    RUN_TEST(test_CE11_hold_suppresses_timeout);
    RUN_TEST(test_CE12_buttonstate_constructible_from_two_reads);
    RUN_TEST(test_W1_state_topic_builder_formats_canonical_topic);
    RUN_TEST(test_W2_start_publishes_running_on_state_topic);
    RUN_TEST(test_W3_autoclear_publishes_idle_on_state_topic);
    RUN_TEST(test_W4_start_publishes_remaining_seconds_on_remaining_topic);
    RUN_TEST(test_W5_tick_republishes_remaining_only_at_publish_interval);
    RUN_TEST(test_W6_finished_hold_publishes_wire_string_on_finished_topic);
    RUN_TEST(test_W7_buzzer_change_publishes_codec_string_on_buzzer_topic);
    RUN_TEST(test_W8_enum_keys_publish_only_their_own_topic_and_skip_noops);
    RUN_TEST(test_W9_duration_change_publishes_hms_on_duration_topic);
    RUN_TEST(test_W10_icon_change_publishes_aggregate_json_on_icons_topic);
    RUN_TEST(test_W11_noop_duration_and_icon_sets_do_not_publish);
    return UNITY_END();
}
