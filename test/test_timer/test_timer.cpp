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
#include "../../src/TimerMenuNav.h"
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
// U55 (#43) — a payload rejected mid-validation persists nothing
// The atomic-reject path exits before the apply block, so the "timer"
// namespace must see no flush and keep its prior values.
// ============================================================================
void test_U55_parseCommand_rejected_payload_persists_nothing(void) {
    int begin_at_start = Preferences::begin_calls;

    // duration is valid; buzzer is not -> whole command rejected.
    TimerCmdResult r = TimerManager.parseCommand("{\"duration\":600,\"buzzer\":\"bogus\"}");

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField), static_cast<int>(r));
    TEST_ASSERT_EQUAL_INT(0, Preferences::begin_calls - begin_at_start);  // no NVS flush
    TEST_ASSERT_EQUAL_UINT32(300, TimerManager.getDuration());            // RAM untouched too
}

// ============================================================================
// U56 (#43) — an all-no-op payload causes no NVS write
// Every value equals current state (setUp defaults), so the deep setters'
// equality-skip leaves nothing dirty and the batch must not flush.
// ============================================================================
void test_U56_parseCommand_noop_payload_does_not_write_nvs(void) {
    int begin_at_start = Preferences::begin_calls;

    TimerCmdResult r = TimerManager.parseCommand(
        "{\"duration\":300,\"buzzer\":\"end\",\"finished\":\"auto-clear\","
        "\"icon_idle\":\"\",\"icon_running\":\"\"}");

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok), static_cast<int>(r));
    TEST_ASSERT_EQUAL_INT(0, Preferences::begin_calls - begin_at_start);  // nothing dirtied -> no flush
}

// ============================================================================
// U57 (#43) — after a multi-key payload the stored "timer"-namespace values
// are correct. Proven by reboot round-trip (setup() reloads from Preferences,
// same precedent as U1), covering every member-backed key in one batch.
// ============================================================================
void test_U57_parseCommand_multikey_stores_correct_values(void) {
    TimerCmdResult r = TimerManager.parseCommand(
        "{\"duration\":600,\"buzzer\":\"countdown\",\"finished\":\"hold\","
        "\"icon_idle\":\"a\",\"icon_running\":\"b\",\"icon_paused\":\"c\",\"icon_finished\":\"d\"}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok), static_cast<int>(r));

    // Simulate reboot: setup() reloads the "timer" namespace from Preferences.
    TimerManager.setup();

    TEST_ASSERT_EQUAL_UINT32(600, TimerManager.getDuration());
    TEST_ASSERT_EQUAL(static_cast<int>(BuzzerMode::Countdown),
                      static_cast<int>(TimerManager.getBuzzerMode()));
    TEST_ASSERT_EQUAL(static_cast<int>(FinishedMode::Hold),
                      static_cast<int>(TimerManager.getFinishedMode()));
    TEST_ASSERT_EQUAL_STRING("a", TimerManager.getIconIdle().c_str());
    TEST_ASSERT_EQUAL_STRING("b", TimerManager.getIconRunning().c_str());
    TEST_ASSERT_EQUAL_STRING("c", TimerManager.getIconPaused().c_str());
    TEST_ASSERT_EQUAL_STRING("d", TimerManager.getIconFinished().c_str());
}

// ============================================================================
// U58 (#44) — a table-only payload whose value equals current state causes no
// "awtrix" write; a genuinely changed value still flushes exactly once.
// ============================================================================
void test_U58_parseCommand_noop_table_value_skips_awtrix_write(void) {
    // Equals the setUp default (TIMER_FINISHED_HOLD = 10) -> nothing changed.
    saveSettings_calls = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"finished_hold\":10}")));
    TEST_ASSERT_EQUAL_INT(0, saveSettings_calls);
    TEST_ASSERT_EQUAL_UINT16(10, TIMER_FINISHED_HOLD);

    // A real change still flushes the "awtrix" namespace once.
    saveSettings_calls = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"finished_hold\":42}")));
    TEST_ASSERT_EQUAL_INT(1, saveSettings_calls);
    TEST_ASSERT_EQUAL_UINT16(42, TIMER_FINISHED_HOLD);
}

// ============================================================================
// U59 (#44) — a mixed payload (member-backed + table keys) yields exactly one
// flush per NVS namespace: one "timer" flush (Preferences begin/end) and one
// "awtrix" flush (saveSettings), with correct final stored values in both.
// ============================================================================
void test_U59_parseCommand_mixed_payload_one_flush_per_namespace(void) {
    saveSettings_calls = 0;
    int begin_at_start = Preferences::begin_calls;

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"buzzer\":\"countdown\",\"icon_idle\":\"a\",\"finished_hold\":42}")));

    TEST_ASSERT_EQUAL_INT(1, Preferences::begin_calls - begin_at_start);  // one "timer" flush
    TEST_ASSERT_EQUAL_INT(1, saveSettings_calls);                          // one "awtrix" flush

    // Table half landed in its storage global (snapshotted by saveSettings).
    TEST_ASSERT_EQUAL_UINT16(42, TIMER_FINISHED_HOLD);

    // Member half landed in the "timer" namespace: reboot round-trip (same
    // precedent as U57 — setup() reloads from Preferences).
    TimerManager.setup();
    TEST_ASSERT_EQUAL(static_cast<int>(BuzzerMode::Countdown),
                      static_cast<int>(TimerManager.getBuzzerMode()));
    TEST_ASSERT_EQUAL_STRING("a", TimerManager.getIconIdle().c_str());
}

// ============================================================================
// One-shot override core (PRD #99 / issue #100). A payload-level boolean `save`
// (default true) makes a command one-shot: save:false applies its config for the
// current run only and reverts to the saved settings when the timer next returns
// to Idle (reset or auto-clear), writing nothing to flash.
// ============================================================================

// OS1 (AC1, run-state key) — save:false duration runs once; after reset() the
// working duration is the previously-saved default (300), not the one-off value.
void test_OS1_save_false_duration_reverts_on_reset(void) {
    TimerCmdResult r = TimerManager.parseCommand(
        "{\"duration\":900,\"action\":\"start\",\"save\":false}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok), static_cast<int>(r));
    TEST_ASSERT_EQUAL_UINT32(900, TimerManager.getDuration());          // live one-shot value
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running),
                      static_cast<int>(TimerManager.getState()));

    TimerManager.reset();
    TEST_ASSERT_EQUAL_UINT32(300, TimerManager.getDuration());          // reverted to saved default
}

// OS2 (AC1, table key) — save:false finished_hold runs once; after reset() the
// working value is the previously-saved default (10).
void test_OS2_save_false_table_key_reverts_on_reset(void) {
    TimerCmdResult r = TimerManager.parseCommand(
        "{\"finished_hold\":99,\"action\":\"start\",\"save\":false}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok), static_cast<int>(r));
    TEST_ASSERT_EQUAL_UINT16(99, TIMER_FINISHED_HOLD);                  // live one-shot value

    TimerManager.reset();
    TEST_ASSERT_EQUAL_UINT16(10, TIMER_FINISHED_HOLD);                  // reverted to saved default
}

// OS9 (AC1, member-config key) — save:false buzzer runs once; after reset() the
// buzzer mode reverts to the saved default (End). Exercises the second descriptor
// table's restore path.
void test_OS9_save_false_member_key_reverts_on_reset(void) {
    TimerCmdResult r = TimerManager.parseCommand(
        "{\"buzzer\":\"countdown\",\"action\":\"start\",\"save\":false}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok), static_cast<int>(r));
    TEST_ASSERT_EQUAL(static_cast<int>(BuzzerMode::Countdown),
                      static_cast<int>(TimerManager.getBuzzerMode()));  // live one-shot value

    TimerManager.reset();
    TEST_ASSERT_EQUAL(static_cast<int>(BuzzerMode::End),
                      static_cast<int>(TimerManager.getBuzzerMode()));  // reverted to saved default
}

// OS3 (AC2) — a one-shot apply performs no NVS write: neither the member-backed
// "timer" namespace (Preferences begin) nor the table-backed "awtrix" namespace
// (saveSettings) is flushed.
void test_OS3_save_false_writes_no_nvs(void) {
    saveSettings_calls = 0;
    int begin_at_start = Preferences::begin_calls;

    TimerCmdResult r = TimerManager.parseCommand(
        "{\"duration\":900,\"finished_hold\":99,\"buzzer\":\"countdown\",\"save\":false}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok), static_cast<int>(r));

    TEST_ASSERT_EQUAL_INT(0, saveSettings_calls);                       // no "awtrix" flush
    TEST_ASSERT_EQUAL_INT(0, Preferences::begin_calls - begin_at_start);// no "timer" flush
}

// OS4 (AC3, auto-clear path) — the tick auto-clear transition reverts the override
// via the same returnToIdle() seam reset() uses. A one-shot duration runs once and
// reverts to the saved default when AutoClear returns the timer to Idle.
void test_OS4_autoclear_reverts_override(void) {
    TimerCmdResult r = TimerManager.parseCommand(
        "{\"duration\":1,\"action\":\"start\",\"save\":false}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok), static_cast<int>(r));
    TEST_ASSERT_EQUAL_UINT32(1, TimerManager.getDuration());            // live one-shot value

    fixture::advance(1000);
    TimerManager.tick();                                               // cross zero -> Finished
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Finished),
                      static_cast<int>(TimerManager.getState()));

    fixture::advance(10500);                                           // > saved finished_hold (10s)
    TimerManager.tick();                                              // AutoClear -> Idle
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_UINT32(300, TimerManager.getDuration());         // reverted to saved default
}

// OS5 (AC4) — a normal (save:true) command arriving mid-override becomes the new
// saved baseline; a later revert restores that new truth, not the original.
void test_OS5_normal_command_mid_override_rebaselines(void) {
    saveSettings_calls = 0;
    // One-shot finished_hold=99 (saved default 10) starts an override and, being
    // one-shot, writes nothing to flash.
    TimerManager.parseCommand("{\"finished_hold\":99,\"action\":\"start\",\"save\":false}");
    TEST_ASSERT_EQUAL_UINT16(99, TIMER_FINISHED_HOLD);
    TEST_ASSERT_EQUAL_INT(0, saveSettings_calls);                       // override suppressed the flush

    // A normal config command (save defaults true) mid-override commits + rebaselines.
    TimerCmdResult r = TimerManager.parseCommand("{\"countdown_seconds\":7}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok), static_cast<int>(r));
    TEST_ASSERT_EQUAL_UINT16(7, TIMER_COUNTDOWN_SECONDS);
    TEST_ASSERT_GREATER_THAN_INT(0, saveSettings_calls);               // rebaseline flushed to NVS

    // The later revert must NOT undo the promoted values.
    TimerManager.reset();
    TEST_ASSERT_EQUAL_UINT16(99, TIMER_FINISHED_HOLD);                  // promoted, not reverted to 10
    TEST_ASSERT_EQUAL_UINT16(7, TIMER_COUNTDOWN_SECONDS);
}

// OS6 (AC5) — a non-boolean `save` rejects the whole payload atomically (ADR-0001);
// nothing applies, even valid sibling fields.
void test_OS6_non_boolean_save_atomic_reject(void) {
    TimerCmdResult r = TimerManager.parseCommand("{\"finished_hold\":99,\"save\":\"yes\"}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField), static_cast<int>(r));
    TEST_ASSERT_EQUAL_UINT16(10, TIMER_FINISHED_HOLD);                  // nothing applied

    r = TimerManager.parseCommand("{\"save\":1}");                      // a number is not a bool
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField), static_cast<int>(r));
}

// OS7 (AC6) — save:false carrying only action/duration is harmless: the action
// runs, the one-off duration is live, and nothing breaks.
void test_OS7_save_false_action_duration_harmless(void) {
    TimerCmdResult r = TimerManager.parseCommand(
        "{\"action\":\"start\",\"duration\":120,\"save\":false}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok), static_cast<int>(r));
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_UINT32(120, TimerManager.getDuration());

    r = TimerManager.parseCommand("{\"action\":\"reset\",\"save\":false}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok), static_cast<int>(r));
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_UINT32(300, TimerManager.getDuration());         // one-off duration reverted on Idle
}

// OS8 (mechanism) — a config command never broadcasts on its own (run-scoped config
// mirror, ADR-0018), whether one-shot or normal: config only travels bundled with a
// start. A bare config edit emits no packet either way.
void test_OS8_config_edit_never_broadcasts(void) {
    TIMER_SYNC_TARGETS = "all";                                        // enable propagation
    int before = fixture::sync_packet_count();

    TimerManager.parseCommand("{\"finished_hold\":99,\"save\":false}");
    TEST_ASSERT_EQUAL_INT(before, fixture::sync_packet_count());        // one-shot config: no packet

    TimerManager.parseCommand("{\"finished_hold\":42}");               // normal config: also no packet
    TEST_ASSERT_EQUAL_INT(before, fixture::sync_packet_count());
}

// ============================================================================
// Honest observation carriers under one-shot override (PRD #99 / issue #101).
// While a save:false run is active, every observation/sync carrier keeps reporting
// the SAVED configuration, never the one-off values; run-state stays live.
// ============================================================================

// HC1 (AC1) — GET /api/timer's `config` mirror shows the SAVED values during an
// active override, while the top-level run-state stays effective (the live one-shot
// duration; top-level buzzer/finished are the effective run values too).
void test_HC1_get_config_mirror_saved_during_override(void) {
    // saved defaults: finished_hold=10, buzzer=end, duration=300.
    TimerManager.parseCommand(
        "{\"duration\":900,\"finished_hold\":99,\"buzzer\":\"countdown\","
        "\"action\":\"start\",\"save\":false}");

    DynamicJsonDocument doc(2048);
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));
    JsonObject config = doc["config"].as<JsonObject>();
    TEST_ASSERT_FALSE(config.isNull());

    // config mirror = SAVED values (not the one-off override).
    TEST_ASSERT_EQUAL_UINT16(10, config["finished_hold"].as<uint16_t>());
    TEST_ASSERT_EQUAL_STRING("end", config["buzzer"]);

    // top-level run-state = EFFECTIVE (live one-shot) values.
    TEST_ASSERT_EQUAL_UINT32(900, doc["duration"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING("countdown", doc["buzzer"]);
}

// HC2 (AC3) — a Home Assistant attribute bag, republished (e.g. on reconnect) while
// an override is active, serializes the SAVED config, not the one-off values.
void test_HC2_ha_attribute_bag_saved_during_override(void) {
    // saved countdown_seconds=3; a one-shot run overrides it (and buzzer).
    TimerManager.parseCommand(
        "{\"countdown_seconds\":25,\"buzzer\":\"countdown\",\"action\":\"start\",\"save\":false}");

    MQTTManager.__test_reset();                                 // clear, then simulate a reconnect republish
    TimerManager.publishAttributeGroup(TimerHaEntity::Buzzer);

    const PublishCall *bag = fixture::last_publish(fixture::TIMER_BUZZER_ATTR_TOPIC);
    TEST_ASSERT_NOT_NULL(bag);
    DynamicJsonDocument doc(512);
    TEST_ASSERT_FALSE(deserializeJson(doc, bag->payload));
    TEST_ASSERT_EQUAL_UINT16(3, doc["countdown_seconds"].as<uint16_t>());   // saved, not the one-off 25
}

// HC3 (AC2 + AC4) — a one-shot (save:false) start emits ONE combined packet whose
// config snapshot carries the leader's EFFECTIVE values (run-scoped config mirror,
// ADR-0018), so a follower mirrors the leader's one-off run. The carrier reports
// EFFECTIVE config (not saved) precisely so a leader's own one-shot run propagates;
// the follower applies it one-shot and reverts on its own return to Idle.
void test_HC3_oneshot_start_propagates_effective_config(void) {
    TIMER_SYNC_TARGETS = "all";                                 // enable propagation

    TimerManager.parseCommand(
        "{\"duration\":900,\"finished_hold\":99,\"action\":\"start\",\"save\":false}");

    // Exactly one packet: the combined start carrying the live one-shot duration AND
    // the effective config snapshot (finished_hold = the one-off 99).
    TEST_ASSERT_EQUAL_INT(1, fixture::sync_packet_count());
    DynamicJsonDocument pkt(2048);
    TEST_ASSERT_FALSE(deserializeJson(pkt, fixture::last_sync_payload()));
    TEST_ASSERT_EQUAL_STRING("start", pkt["action"]);
    TEST_ASSERT_EQUAL_UINT32(900, pkt["duration"].as<uint32_t>());   // one-shot duration reaches followers
    TEST_ASSERT_EQUAL_UINT16(99, pkt["finished_hold"].as<uint16_t>()); // EFFECTIVE config rides the start
}

// ============================================================================
// Inline RTTTL melodies on melody_end / melody_tick (PRD #99 / issue #102).
// A melody key accepts either a bare saved file-name token (as today) or an inline
// RTTTL tune; an inline tune is always one-shot, played for the run only and never
// written to flash, and never appears in the config mirror or HA bags.
// ============================================================================

// IM1 (AC1) — the pure RTTTL classifier/validator: bare-name vs inline detection,
// valid vs invalid inline syntax, and the edge cases (colons, empty, oversize).
void test_IM1_rtttl_classifier_and_validator(void) {
    // Classifier: bare file-name tokens are NOT inline (no colon).
    TEST_ASSERT_FALSE(timerMelodyIsInline("timer_end"));
    TEST_ASSERT_FALSE(timerMelodyIsInline("alarm-1"));
    TEST_ASSERT_FALSE(timerMelodyIsInline(""));                       // empty = bare (reset-to-default)
    // Classifier: anything carrying a colon is treated as an inline tune.
    TEST_ASSERT_TRUE(timerMelodyIsInline("alarm:d=4,o=5,b=120:c,e,g"));
    TEST_ASSERT_TRUE(timerMelodyIsInline(":d=4,o=5,b=120:c"));        // empty name still inline
    TEST_ASSERT_TRUE(timerMelodyIsInline("alarm:c,e,g"));             // malformed but inline-intent

    // Validator: well-formed inline tunes pass.
    TEST_ASSERT_TRUE(timerMelodyValidateInline("alarm:d=4,o=5,b=120:c,e,g"));
    TEST_ASSERT_TRUE(timerMelodyValidateInline("x:b=120:c"));         // minimal control section
    TEST_ASSERT_TRUE(timerMelodyValidateInline(":d=4,o=5,b=120:c"));  // empty name allowed

    // Validator: malformed inline tunes fail.
    TEST_ASSERT_FALSE(timerMelodyValidateInline("alarm:c,e,g"));      // only one colon
    TEST_ASSERT_FALSE(timerMelodyValidateInline("alarm::"));          // empty control + notes
    TEST_ASSERT_FALSE(timerMelodyValidateInline("alarm::c"));         // empty control section
    TEST_ASSERT_FALSE(timerMelodyValidateInline("alarm:d=4,o=5,b=120:")); // empty notes section
    TEST_ASSERT_FALSE(timerMelodyValidateInline("alarm:foo=4:c"));    // control without d/o/b
    TEST_ASSERT_FALSE(timerMelodyValidateInline("a:b:c:d"));          // three colons

    // Validator: oversize is rejected.
    String big = "x:d=4,o=5,b=120:";
    for (int i = 0; i < 300; i++) big += "c,";
    TEST_ASSERT_FALSE(timerMelodyValidateInline(big));
}

// IM2 (AC2) — an inline melody_end plays for the run, never overwrites the saved
// bare name, writes nothing to flash, and reverts on return to Idle (the next run
// uses the saved melody again).
void test_IM2_inline_melody_end_plays_reverts_no_persist(void) {
    const char *tune = "alarm:d=4,o=5,b=120:c,e,g";
    saveSettings_calls = 0;
    int begin_before = Preferences::begin_calls;

    String cmd = String("{\"duration\":1,\"melody_end\":\"") + tune + "\",\"action\":\"start\"}";
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(cmd.c_str())));

    TEST_ASSERT_EQUAL_STRING("timer_end", TIMER_MELODY_END.c_str());    // saved name not overwritten
    TEST_ASSERT_EQUAL_INT(0, saveSettings_calls);                       // one-shot: no "awtrix" flush
    TEST_ASSERT_EQUAL_INT(0, Preferences::begin_calls - begin_before);  // one-shot: no "timer" flush

    // Run to finish -> enterFinished plays the resolved end melody (the inline tune).
    PeripheryManager.__test_reset();
    fixture::advance(1000);
    TimerManager.tick();
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Finished),
                      static_cast<int>(TimerManager.getState()));
    bool playedInline = false;
    for (auto &p : PeripheryManager.play_calls) if (p == String(tune)) playedInline = true;
    TEST_ASSERT_TRUE(playedInline);

    // Revert on Idle: the next run plays the SAVED melody, not the inline tune.
    TimerManager.reset();
    PeripheryManager.__test_reset();
    TimerManager.setDuration(1);
    TimerManager.start();
    fixture::advance(1000);
    TimerManager.tick();
    bool inlineCarriedOver = false;
    for (auto &p : PeripheryManager.play_calls) if (p == String(tune)) inlineCarriedOver = true;
    TEST_ASSERT_FALSE(inlineCarriedOver);
}

// IM3 (AC3) — an inline melody_tick plays during the countdown for the run only and
// never persists.
void test_IM3_inline_melody_tick_plays_no_persist(void) {
    const char *tune = "beep:d=16,o=6,b=200:c,c";
    saveSettings_calls = 0;

    String cmd = String("{\"duration\":5,\"melody_tick\":\"") + tune +
                 "\",\"buzzer\":\"countdown\",\"countdown_seconds\":5,\"action\":\"start\"}";
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(cmd.c_str())));

    TEST_ASSERT_EQUAL_STRING("timer_tick", TIMER_MELODY_TICK.c_str());  // saved name not overwritten
    TEST_ASSERT_EQUAL_INT(0, saveSettings_calls);                       // one-shot: no flush

    // Advance one second into the countdown window -> plays the inline tick tune.
    PeripheryManager.__test_reset();
    PeripheryManager.__test_set_playing(false);
    fixture::advance(1000);
    TimerManager.tick();
    bool played = false;
    for (auto &p : PeripheryManager.play_calls) if (p == String(tune)) played = true;
    TEST_ASSERT_TRUE(played);
}

// IM4 (AC4) — a bare file-name token still resolves and persists exactly as before.
void test_IM4_bare_melody_name_still_persists(void) {
    saveSettings_calls = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"melody_end\":\"custom_alarm\"}")));
    TEST_ASSERT_EQUAL_STRING("custom_alarm", TIMER_MELODY_END.c_str()); // stored to the name global
    TEST_ASSERT_GREATER_THAN_INT(0, saveSettings_calls);               // persisted (save:true default)
}

// IM5 (AC5) — an inline tune never appears in the GET config mirror; the mirror
// shows the SAVED bare melody name throughout the inline-melody override.
void test_IM5_inline_never_in_config_mirror(void) {
    const char *tune = "alarm:d=4,o=5,b=120:c,e,g";
    String cmd = String("{\"melody_end\":\"") + tune + "\",\"action\":\"start\",\"save\":false}";
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(cmd.c_str())));

    DynamicJsonDocument doc(2048);
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));
    JsonObject config = doc["config"].as<JsonObject>();
    TEST_ASSERT_EQUAL_STRING("timer_end", config["melody_end"]);       // saved name, never the tune
}

// IM6 (AC6) — a malformed inline RTTTL rejects the whole payload atomically; no
// sibling field is applied.
void test_IM6_malformed_inline_atomic_reject(void) {
    // ':' present (inline-classified) but only one colon -> malformed.
    TimerCmdResult r = TimerManager.parseCommand(
        "{\"finished_hold\":50,\"melody_end\":\"alarm:c,e,g\"}");
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField), static_cast<int>(r));
    TEST_ASSERT_EQUAL_UINT16(10, TIMER_FINISHED_HOLD);                  // sibling not applied (atomic)
    TEST_ASSERT_EQUAL_STRING("timer_end", TIMER_MELODY_END.c_str());    // melody unchanged
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

    // Member-only commands (every member-backed kind: both enum modes, an icon,
    // duration) do not touch awtrix Settings even when they change values (#44).
    saveSettings_calls = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"buzzer\":\"off\",\"finished\":\"hold\",\"icon_idle\":\"x\",\"duration\":120}")));
    TEST_ASSERT_EQUAL_INT(0, saveSettings_calls);

    // Rejected command does not call saveSettings.
    saveSettings_calls = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"finished_hold\":999}")));
    TEST_ASSERT_EQUAL_INT(0, saveSettings_calls);
}

// ============================================================================
// U39 (ADR-0004) — Behavior parameters accepted within their principled ranges,
// applied to the globals. (app_config_timeout was removed in PRD #83 / #88.)
// ============================================================================
void test_U39_parseCommand_behavior_params_accepted_in_range(void) {
    SHOW_TIMER = true;

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"max_duration\":3600,"
                          "\"remaining_publish_interval\":2}")));
    TEST_ASSERT_EQUAL_UINT32(3600, TIMER_MAX_DURATION);
    TEST_ASSERT_EQUAL_UINT16(2,    TIMER_PUBLISH_INTERVAL);

    // Lower bounds.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"max_duration\":1,"
                          "\"remaining_publish_interval\":1}")));
    TEST_ASSERT_EQUAL_UINT32(1,  TIMER_MAX_DURATION);
    TEST_ASSERT_EQUAL_UINT16(1,  TIMER_PUBLISH_INTERVAL);

    // Reset max_duration so it doesn't reject other tests' duration commands.
    TIMER_MAX_DURATION = 86400;

    // Upper bounds.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"max_duration\":604800,"
                          "\"remaining_publish_interval\":60}")));
    TEST_ASSERT_EQUAL_UINT32(604800, TIMER_MAX_DURATION);
    TEST_ASSERT_EQUAL_UINT16(60,     TIMER_PUBLISH_INTERVAL);

    // Back-compat: an inbound command still carrying the removed app_config_timeout
    // key is accepted and the key silently ignored (unknown keys are not errors).
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand(
                          "{\"max_duration\":3600,\"app_config_timeout\":60}")));
    TEST_ASSERT_EQUAL_UINT32(3600, TIMER_MAX_DURATION);
}

// ============================================================================
// U40 (ADR-0004) — Out-of-range behavior parameters are rejected atomically.
// ============================================================================
void test_U40_parseCommand_behavior_params_atomic_reject(void) {
    SHOW_TIMER = true;
    TIMER_MAX_DURATION     = 86400;
    TIMER_PUBLISH_INTERVAL = 1;

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

    // The removed app_config_timeout key is now unknown: even an out-of-range value
    // is accepted (silently ignored), not rejected (back-compat, #88).
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"app_config_timeout\":4}")));

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
    TIMER_BAR_BG_COLOR = 0;

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

    // bar_bg_color (ADR-0020) reuses parseBarColor: numeric, hex with/without '#',
    // malformed/out-of-range rejected, and it is INDEPENDENT of bar_color.
    TIMER_BAR_COLOR = 0xFFAA00;   // sentinel: must stay untouched by bg edits
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_bg_color\":255}")));
    TEST_ASSERT_EQUAL_UINT32(0x0000FF, TIMER_BAR_BG_COLOR);
    TEST_ASSERT_EQUAL_UINT32(0xFFAA00, TIMER_BAR_COLOR);    // foreground unchanged
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_bg_color\":\"#202020\"}")));
    TEST_ASSERT_EQUAL_UINT32(0x202020, TIMER_BAR_BG_COLOR);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_bg_color\":\"00AA55\"}")));
    TEST_ASSERT_EQUAL_UINT32(0x00AA55, TIMER_BAR_BG_COLOR);
    // 0 is a valid literal (black = no track), not rejected.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_bg_color\":0}")));
    TEST_ASSERT_EQUAL_UINT32(0, TIMER_BAR_BG_COLOR);
    // Malformed string and out-of-range number rejected (atomic reject; value kept).
    TIMER_BAR_BG_COLOR = 0x123456;
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_bg_color\":\"notahex\"}")));
    TEST_ASSERT_EQUAL_UINT32(0x123456, TIMER_BAR_BG_COLOR);
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
                      static_cast<int>(TimerManager.parseCommand("{\"bar_bg_color\":16777216}")));
    TEST_ASSERT_EQUAL_UINT32(0x123456, TIMER_BAR_BG_COLOR);
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
    // 8 original entities + the two sync-control entities (issue #110): the Follow
    // switch and the static Off/All Targets select.
    TEST_ASSERT_EQUAL_UINT(10, (unsigned)TIMER_HA_DESCRIPTOR_COUNT);
    for (size_t i = 0; i < TIMER_HA_DESCRIPTOR_COUNT; ++i) {
        const TimerHaDescriptor &d = TIMER_HA_DESCRIPTORS[i];
        // Row i must describe slot i (setup/teardown index the array by slot).
        TEST_ASSERT_EQUAL_UINT((unsigned)i, (unsigned)d.slot);

        const bool validComponent =
            strcmp(d.component, "text")   == 0 ||
            strcmp(d.component, "sensor") == 0 ||
            strcmp(d.component, "select") == 0 ||
            strcmp(d.component, "switch") == 0 ||
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

// U60 — haRegistrationAtCap encodes ArduinoHA's HAMqtt::addDeviceType off-by-one
// (`_devicesTypesNb + 1 >= _maxDevicesTypesNb`), so the effective capacity is
// maxEntities - 1. This is the root cause of issue #125: at max=34 the 34th add
// (registered==33) is rejected, which silently dropped the two Timer sync-control
// entities once the inventory reached 35. The DEBUG_MODE registration warning and
// the cap math both lean on this; pin the boundary so the off-by-one can't drift.
void test_U60_ha_registration_off_by_one(void) {
    // Old cap of 34: effective 33. The 33rd add (registered==32) still fits; the
    // 34th (registered==33) is the one that was dropped.
    TEST_ASSERT_FALSE(haRegistrationAtCap(32, 34));
    TEST_ASSERT_TRUE(haRegistrationAtCap(33, 34));
    // Raised cap of 40: effective 39. The 35-entity inventory fits with headroom;
    // only at the effective cap does the next add get rejected.
    TEST_ASSERT_FALSE(haRegistrationAtCap(35, 40));
    TEST_ASSERT_TRUE(haRegistrationAtCap(39, 40));
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
    // Background track (ADR-0020): full trough, icon on -> (9, 23), regardless of drain.
    TEST_ASSERT_TRUE(half.showBarTrack);
    TEST_ASSERT_EQUAL_INT16(9, half.barTrackStartX);
    TEST_ASSERT_EQUAL_UINT8(23, half.barTrackLen);

    // Full remaining: full-length bar starting at the bar origin.
    const TimerSnapshot s_full{TimerState::Running, 100, 100, 100, false, 0, 0, 0, 0, true, 0};
    TimerView full = TimerViewModel::compute(s_full);
    TEST_ASSERT_EQUAL_UINT8(23, full.barLen);
    TEST_ASSERT_EQUAL_INT16(9, full.barStartX);
    TEST_ASSERT_EQUAL_INT16(32, full.barStartX + full.barLen);

    // Tiny remaining (1s of 100): 23 * 1/100 == 0 cells -> no foreground bar...
    const TimerSnapshot s_tiny{TimerState::Running, 100, 1, 100, false, 0, 0, 0, 0, true, 0};
    TimerView tiny = TimerViewModel::compute(s_tiny);
    TEST_ASSERT_FALSE(tiny.showBar);
    // ...but the background trough PERSISTS through the final stretch (ADR-0020).
    TEST_ASSERT_TRUE(tiny.showBarTrack);
    TEST_ASSERT_EQUAL_INT16(9, tiny.barTrackStartX);
    TEST_ASSERT_EQUAL_UINT8(23, tiny.barTrackLen);

    // Icon hidden: both the bar and its trough reflow to the full 32px panel (0, 32).
    const TimerSnapshot s_noicon{TimerState::Running, 100, 50, 100, false, 0, 0, 0, 0, false, 0};
    TimerView noicon = TimerViewModel::compute(s_noicon);
    TEST_ASSERT_TRUE(noicon.showBarTrack);
    TEST_ASSERT_EQUAL_INT16(0, noicon.barTrackStartX);
    TEST_ASSERT_EQUAL_UINT8(32, noicon.barTrackLen);

    // Idle: no bar region active, so no trough either.
    const TimerSnapshot s_idle{TimerState::Idle, 100, 0, 0, false, 0, 0, 0, 0, true, 0};
    TimerView idle = TimerViewModel::compute(s_idle);
    TEST_ASSERT_FALSE(idle.showBarTrack);
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
    // 2048: the GET body now carries the nested config mirror (PRD #73), past the
    // old 512-byte buffer. The top-level field assertions below are unchanged.
    StaticJsonDocument<2048> doc;
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
    StaticJsonDocument<2048> doc;
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
    StaticJsonDocument<2048> doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));
    TEST_ASSERT_EQUAL_STRING("paused", doc["state"]);
    TEST_ASSERT_EQUAL_UINT32(frozen, doc["remaining"].as<uint32_t>());
}

void test_U47_getStateJson_finished_zero(void) {
    TimerManager.setDuration(5);
    TimerManager.start();
    fixture::advance(5000);
    TimerManager.tick();
    StaticJsonDocument<2048> doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));
    TEST_ASSERT_EQUAL_STRING("finished", doc["state"]);
    TEST_ASSERT_EQUAL_UINT32(0, doc["remaining"].as<uint32_t>());
}

void test_U48_getStateJson_disabled_still_200_shape(void) {
    SHOW_TIMER = false;
    StaticJsonDocument<2048> doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));
    TEST_ASSERT_FALSE(doc["enabled"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("idle", doc["state"]);   // disabled timer is forced Idle
}

void test_U49_getStateJson_enum_canonical_spellings(void) {
    TimerManager.parseCommand("{\"buzzer\":\"countdown\",\"finished\":\"re-alert\"}");
    StaticJsonDocument<2048> doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));
    TEST_ASSERT_EQUAL_STRING("countdown", doc["buzzer"]);
    TEST_ASSERT_EQUAL_STRING("re-alert", doc["finished"]);
    // The parser also accepts the non-hyphen alias on input; output stays canonical.
    TimerManager.parseCommand("{\"finished\":\"autoclear\"}");
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));
    TEST_ASSERT_EQUAL_STRING("auto-clear", doc["finished"]);
}

// ============================================================================
// U60 (#74) — getStateJson() gains a nested `config` object alongside the
// existing top-level run-state summary. The config mirror is built from the two
// descriptor tables (timerBuildFullConfig); this pins its presence as a JSON
// object and that the stable 8-field top-level summary is untouched (back-compat).
// ============================================================================
void test_U60_getStateJson_has_nested_config_object(void) {
    DynamicJsonDocument doc(2048);
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));

    // The existing 8 top-level fields are present and unchanged (U44 contract).
    TEST_ASSERT_TRUE(doc.containsKey("state"));
    TEST_ASSERT_TRUE(doc.containsKey("enabled"));
    TEST_ASSERT_TRUE(doc.containsKey("remaining"));
    TEST_ASSERT_TRUE(doc.containsKey("remaining_str"));
    TEST_ASSERT_TRUE(doc.containsKey("duration"));
    TEST_ASSERT_TRUE(doc.containsKey("duration_str"));
    TEST_ASSERT_TRUE(doc.containsKey("buzzer"));
    TEST_ASSERT_TRUE(doc.containsKey("finished"));

    // The new nested config object.
    TEST_ASSERT_TRUE(doc.containsKey("config"));
    TEST_ASSERT_TRUE(doc["config"].is<JsonObject>());
}

// ============================================================================
// U61 (#74) — run-state ↔ config boundary in the GET body. `duration` is
// top-level only (run-state, never config; excluded from the member-config
// table for that reason). `buzzer`/`finished` appear BOTH at top level (legacy
// back-compat) AND inside config (the mirror is self-contained). `action` is
// never persisted, so it is absent from config.
// ============================================================================
void test_U61_getStateJson_config_boundary_placement(void) {
    DynamicJsonDocument doc(2048);
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));
    JsonObject config = doc["config"].as<JsonObject>();
    TEST_ASSERT_FALSE(config.isNull());

    // duration is run-state: top-level only, absent from config.
    TEST_ASSERT_TRUE (doc.containsKey("duration"));
    TEST_ASSERT_FALSE(config.containsKey("duration"));
    TEST_ASSERT_FALSE(config.containsKey("duration_str"));
    TEST_ASSERT_FALSE(config.containsKey("action"));

    // buzzer/finished appear in both places.
    TEST_ASSERT_TRUE(doc.containsKey("buzzer"));
    TEST_ASSERT_TRUE(doc.containsKey("finished"));
    TEST_ASSERT_TRUE(config.containsKey("buzzer"));
    TEST_ASSERT_TRUE(config.containsKey("finished"));
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

// S2 — a local start emits exactly ONE combined packet carrying action+duration
// AND the leader's effective config snapshot (run-scoped config mirror, ADR-0018):
// the config rides WITH the start so a follower mirrors the run.
void test_S2_local_start_emits_combined_packet(void) {
    SHOW_TIMER = true;
    TIMER_SYNC_TARGETS = "all";

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"duration\":300,\"action\":\"start\"}")));
    TEST_ASSERT_EQUAL_INT(1, fixture::sync_packet_count());   // one combined packet, not two

    DynamicJsonDocument doc(2048);
    TEST_ASSERT_FALSE(deserializeJson(doc, fixture::last_sync_payload()));
    TEST_ASSERT_EQUAL_STRING("awtrix_self", doc["_sync"]["src"]);
    TEST_ASSERT_EQUAL_STRING("all", doc["_sync"]["tgt"]);
    TEST_ASSERT_EQUAL_STRING("start", doc["action"]);
    TEST_ASSERT_EQUAL_UINT32(300, doc["duration"].as<uint32_t>());
    // The effective config snapshot now rides WITH the start (the combined packet).
    TEST_ASSERT_TRUE(doc.containsKey("buzzer"));
    TEST_ASSERT_TRUE(doc.containsKey("bar_color"));
    // sync_* (local identity) is never in the snapshot.
    TEST_ASSERT_FALSE(doc.containsKey("sync_follow"));
    TEST_ASSERT_FALSE(doc.containsKey("sync_targets"));
}

// S3 — a local config edit emits NO sync packet (run-scoped config mirror, ADR-0018):
// config no longer converges on a config edit; it only travels bundled with a start.
void test_S3_local_config_edit_emits_nothing(void) {
    SHOW_TIMER = true;
    TIMER_SYNC_TARGETS = "awtrix_peer";

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"buzzer\":\"countdown\",\"bar_color\":\"#FF0000\"}")));
    TEST_ASSERT_EQUAL_INT(0, fixture::sync_packet_count());   // a config edit propagates nothing
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

// S7 — a peer-applied config command is now ALWAYS one-shot on the receiver
// (receiver-forced one-shot, issue #108 / ADR-0018): the value applies to the
// effective layer (RAM live) so the run mirrors the leader, but the HA attribute
// republish is SUPPRESSED — the follower's attribute bag keeps reporting its OWN
// SAVED config (carriers stay honest under an override, ADR-0017 §3), and reverts
// on return to Idle. The value is never persisted; one-hop still holds (no relay).
void test_S7_remote_config_is_oneshot_attribute_bag_stays_saved(void) {
    SHOW_TIMER = true;
    TIMER_SYNC_FOLLOW  = true;
    TIMER_SYNC_TARGETS = "all";   // this clock would relay its own local edits
    fixture::advance(1000);       // millis() > 0 so the dedup TTL math is well-defined

    TimerManager.applySyncCommand(
        "{\"_sync\":{\"src\":\"awtrix_o\",\"seq\":7,\"tgt\":\"all\"},\"realert_interval\":99}");

    TEST_ASSERT_EQUAL_UINT16(99, TIMER_REALERT_INTERVAL);   // applied to the EFFECTIVE layer
    // Carriers stay honest under the receiver-forced override: no attribute republish.
    TEST_ASSERT_NULL(fixture::last_publish(fixture::TIMER_FINISHED_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_FINISHED_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(0, fixture::sync_packet_count());  // one-hop: applied, never relayed
}

// ============================================================================
// Run-scoped config mirror + one-shot sync receive (issue #108 / ADR-0018).
// A config edit never propagates; a start bundles the leader's effective config;
// a follower applies a received command ONE-SHOT and reverts on return to Idle.
// ============================================================================

// SR1 — pause and reset propagate run-state ONLY (no config snapshot rides them);
// only a start carries the combined config mirror.
void test_SR1_pause_reset_propagate_runstate_only(void) {
    SHOW_TIMER = true;
    TIMER_SYNC_TARGETS = "all";
    TimerManager.parseCommand("{\"duration\":300,\"action\":\"start\"}");   // start: combined
    ServerManager.__test_reset();

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"action\":\"pause\"}")));
    TEST_ASSERT_EQUAL_INT(1, fixture::sync_packet_count());
    DynamicJsonDocument p(2048);
    TEST_ASSERT_FALSE(deserializeJson(p, fixture::last_sync_payload()));
    TEST_ASSERT_EQUAL_STRING("pause", p["action"]);
    TEST_ASSERT_FALSE(p.containsKey("buzzer"));      // no config rides pause
    TEST_ASSERT_FALSE(p.containsKey("bar_color"));

    ServerManager.__test_reset();
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"action\":\"reset\"}")));
    TEST_ASSERT_EQUAL_INT(1, fixture::sync_packet_count());
    DynamicJsonDocument r(2048);
    TEST_ASSERT_FALSE(deserializeJson(r, fixture::last_sync_payload()));
    TEST_ASSERT_EQUAL_STRING("reset", r["action"]);
    TEST_ASSERT_FALSE(r.containsKey("buzzer"));      // no config rides reset
}

// SR2 — a bare duration edit propagates run-state (duration only), no config.
void test_SR2_bare_duration_edit_propagates_duration_only(void) {
    SHOW_TIMER = true;
    TIMER_SYNC_TARGETS = "all";

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"duration\":600}")));
    TEST_ASSERT_EQUAL_INT(1, fixture::sync_packet_count());
    DynamicJsonDocument d(2048);
    TEST_ASSERT_FALSE(deserializeJson(d, fixture::last_sync_payload()));
    TEST_ASSERT_FALSE(d.containsKey("action"));
    TEST_ASSERT_EQUAL_UINT32(600, d["duration"].as<uint32_t>());
    TEST_ASSERT_FALSE(d.containsKey("buzzer"));       // no config rides a duration edit
}

// SR3 — receiver-forced one-shot: a follower applies a received start (with config)
// during the run, but NEVER persists it — even though the packet carries no `save`
// flag — and reverts to its OWN saved config/duration on return to Idle, writing
// nothing to flash. Saved finished_hold default 10, saved duration 300.
void test_SR3_follower_applies_oneshot_and_reverts(void) {
    SHOW_TIMER = true;
    TIMER_SYNC_FOLLOW = true;
    fixture::advance(1000);

    saveSettings_calls = 0;
    int begin_at_start = Preferences::begin_calls;

    // Leader sends a combined start carrying foreign effective config + duration.
    TimerManager.applySyncCommand(
        "{\"_sync\":{\"src\":\"awtrix_o\",\"seq\":1,\"tgt\":\"all\"},"
        "\"action\":\"start\",\"duration\":900,\"finished_hold\":99}");

    // The run mirrors the leader during the run.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_UINT32(900, TimerManager.getDuration());
    TEST_ASSERT_EQUAL_UINT16(99, TIMER_FINISHED_HOLD);   // effective during the run

    // Nothing was written to flash (no persist while following one-shot).
    TEST_ASSERT_EQUAL_INT(0, saveSettings_calls);                       // no "awtrix" flush
    TEST_ASSERT_EQUAL_INT(0, Preferences::begin_calls - begin_at_start);// no "timer" flush

    // On return to Idle the follower reverts to its OWN saved config/duration.
    TimerManager.reset();
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));
    TEST_ASSERT_EQUAL_UINT32(300, TimerManager.getDuration());   // own saved duration
    TEST_ASSERT_EQUAL_UINT16(10, TIMER_FINISHED_HOLD);           // own saved config restored
}

// SR4 — a follower forces one-shot even when the leader's packet says save:true:
// the receive path overrides it, so a synced run is never persisted regardless of
// the leader's flag, and reverts to the follower's own saved config.
void test_SR4_follower_forces_oneshot_regardless_of_save(void) {
    SHOW_TIMER = true;
    TIMER_SYNC_FOLLOW = true;
    fixture::advance(1000);

    saveSettings_calls = 0;
    int begin_at_start = Preferences::begin_calls;

    TimerManager.applySyncCommand(
        "{\"_sync\":{\"src\":\"awtrix_o\",\"seq\":2,\"tgt\":\"all\"},"
        "\"action\":\"start\",\"duration\":120,\"finished_hold\":77,\"save\":true}");

    TEST_ASSERT_EQUAL_UINT16(77, TIMER_FINISHED_HOLD);   // effective during the run
    TEST_ASSERT_EQUAL_INT(0, saveSettings_calls);                       // save:true ignored on receive
    TEST_ASSERT_EQUAL_INT(0, Preferences::begin_calls - begin_at_start);

    TimerManager.reset();
    TEST_ASSERT_EQUAL_UINT16(10, TIMER_FINISHED_HOLD);   // reverted to own saved
}

// ============================================================================
// Peer presence subsystem (#111 / ADR-0019). A presence beacon + a bounded peer
// registry keyed by stable uniqueID, harvested UNGATED (presence is informational,
// never a command) so a clock learns which peers are on the LAN.
// ============================================================================

// PP1 — a received presence packet adds/refreshes the sender in the registry and
// changes NO timer state. Harvest is ungated: neither follow nor targeting apply.
void test_PP1_presence_harvest_ungated_no_timer_state(void) {
    SHOW_TIMER = true;
    fixture::advance(1000);

    // Follow OFF and the packet carries no tgt at all: a real command would be
    // dropped by the gates, but presence is harvested regardless.
    TIMER_SYNC_FOLLOW = false;
    TEST_ASSERT_EQUAL_INT(0, TimerManager.peerCount());

    TimerManager.applySyncCommand(
        "{\"_sync\":{\"src\":\"awtrix_o\",\"seq\":1},\"presence\":true}");

    TEST_ASSERT_EQUAL_INT(1, TimerManager.peerCount());
    TEST_ASSERT_TRUE(TimerManager.hasPeer("awtrix_o"));
    // No timer state changed.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle), static_cast<int>(TimerManager.getState()));

    // A later beacon from the same peer refreshes lastSeen without duplicating.
    fixture::advance(5000);
    TimerManager.applySyncCommand(
        "{\"_sync\":{\"src\":\"awtrix_o\",\"seq\":2},\"presence\":true}");
    TEST_ASSERT_EQUAL_INT(1, TimerManager.peerCount());
}

// PP2 — presence is never treated as a command: even a packet that ALSO carries
// action/duration is harvested as presence only, applying NO timer state, with no
// follow/target gate. (presence:true short-circuits before parseCommand.)
void test_PP2_presence_never_treated_as_command(void) {
    SHOW_TIMER = true;
    TIMER_SYNC_FOLLOW = true;          // gates would otherwise let a command through
    fixture::advance(1000);

    TimerManager.applySyncCommand(
        "{\"_sync\":{\"src\":\"awtrix_o\",\"seq\":1,\"tgt\":\"all\"},"
        "\"presence\":true,\"action\":\"start\",\"duration\":300}");

    TEST_ASSERT_TRUE(TimerManager.hasPeer("awtrix_o"));
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle), static_cast<int>(TimerManager.getState()));
}

// PP3 — the registry excludes the clock's own id and is bounded (~16). Own-src
// presence is dropped (own echo); flooding distinct peers never exceeds the bound.
void test_PP3_registry_excludes_self_and_is_bounded(void) {
    SHOW_TIMER = true;
    fixture::advance(1000);

    // Own id is never registered (own echo, like a real command).
    TimerManager.applySyncCommand(
        "{\"_sync\":{\"src\":\"awtrix_self\",\"seq\":1},\"presence\":true}");
    TEST_ASSERT_EQUAL_INT(0, TimerManager.peerCount());
    TEST_ASSERT_FALSE(TimerManager.hasPeer("awtrix_self"));

    // Flood 20 distinct peers; the registry caps at the bound (16).
    for (int i = 0; i < 20; ++i) {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "{\"_sync\":{\"src\":\"awtrix_p%02d\",\"seq\":%d},\"presence\":true}", i, i + 10);
        TimerManager.applySyncCommand(buf);
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT(16, TimerManager.peerCount());
    TEST_ASSERT_EQUAL_INT(16, TimerManager.peerCount());
}

// PP4 — peers age out after the TTL (~3 missed beacons, ~100s). tickPresence(now)
// prunes entries not seen within the TTL.
void test_PP4_peers_age_out_past_ttl(void) {
    SHOW_TIMER = true;
    fixture::advance(1000);

    TimerManager.applySyncCommand(
        "{\"_sync\":{\"src\":\"awtrix_o\",\"seq\":1},\"presence\":true}");
    TEST_ASSERT_EQUAL_INT(1, TimerManager.peerCount());

    // Still within TTL: a prune keeps the peer.
    fixture::advance(50000);
    TimerManager.tickPresence(fixture::virtual_now());
    TEST_ASSERT_EQUAL_INT(1, TimerManager.peerCount());
    TEST_ASSERT_TRUE(TimerManager.hasPeer("awtrix_o"));

    // Past TTL (>100s since last seen): the peer ages out.
    fixture::advance(60000);   // ~110s total since harvest
    TimerManager.tickPresence(fixture::virtual_now());
    TEST_ASSERT_EQUAL_INT(0, TimerManager.peerCount());
    TEST_ASSERT_FALSE(TimerManager.hasPeer("awtrix_o"));
}

// PP5 — the beacon is emitted periodically on a real network but NEVER in AP mode.
void test_PP5_beacon_periodic_not_in_ap_mode(void) {
    SHOW_TIMER = true;
    TIMER_SYNC_TARGETS = "";     // beacon is unconditional, independent of sync targets
    fixture::advance(1000);

    // AP mode: no beacon, ever.
    AP_MODE = true;
    TimerManager.tickPresence(fixture::virtual_now());
    fixture::advance(60000);
    TimerManager.tickPresence(fixture::virtual_now());
    TEST_ASSERT_EQUAL_INT(0, fixture::sync_packet_count());

    // Real network: first tick emits a beacon, then it throttles to the period.
    AP_MODE = false;
    ServerManager.__test_reset();
    TimerManager.tickPresence(fixture::virtual_now());
    TEST_ASSERT_EQUAL_INT(1, fixture::sync_packet_count());

    DynamicJsonDocument doc(256);
    TEST_ASSERT_FALSE(deserializeJson(doc, fixture::last_sync_payload()));
    TEST_ASSERT_TRUE(doc["presence"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("awtrix_self", doc["_sync"]["src"]);

    // Within the period: no new beacon.
    fixture::advance(5000);
    TimerManager.tickPresence(fixture::virtual_now());
    TEST_ASSERT_EQUAL_INT(1, fixture::sync_packet_count());

    // After the period (~30s) elapses: another beacon.
    fixture::advance(30000);
    TimerManager.tickPresence(fixture::virtual_now());
    TEST_ASSERT_EQUAL_INT(2, fixture::sync_packet_count());
}

// ============================================================================
// Dynamic HA Targets select (#112). The select's options are built at runtime
// from the peer registry — "Off;All" plus each currently-discovered peer id
// (sorted) — and command/state map through the CURRENT id<->index list. These
// DT tests cover the pure option-build + mapping contract (TimerHa) and the
// sorted peer-id accessor (TimerManager) the select consumes.
// ============================================================================

// DT1 — with no peers discovered, the option list is just the two base entries.
void test_DT1_build_options_empty_registry(void) {
    char buf[256];
    timerSyncTargetsBuildOptions(nullptr, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("Off;All", buf);
}

// DT2 — each discovered peer id appends to the base options, in the given order.
void test_DT2_build_options_with_sorted_ids(void) {
    const char *ids[] = {"awtrix_aa", "awtrix_bb"};
    char buf[256];
    timerSyncTargetsBuildOptions(ids, 2, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("Off;All;awtrix_aa;awtrix_bb", buf);
}

// DT3 — forward map: the two base values reflect to their fixed indices.
void test_DT3_forward_map_off_all(void) {
    const char *ids[] = {"awtrix_aa"};
    TEST_ASSERT_EQUAL_INT(0, timerSyncTargetsIndexForValue("", ids, 1));
    TEST_ASSERT_EQUAL_INT(1, timerSyncTargetsIndexForValue("all", ids, 1));
    // No peers and a base value: still resolves (n==0 path).
    TEST_ASSERT_EQUAL_INT(0, timerSyncTargetsIndexForValue("", nullptr, 0));
    TEST_ASSERT_EQUAL_INT(1, timerSyncTargetsIndexForValue("all", nullptr, 0));
}

// DT4 — forward map: a present single id maps to its position; an absent id or a
// multi-id CSV are both unknown (-1, no option selected).
void test_DT4_forward_map_present_absent_csv(void) {
    const char *ids[] = {"awtrix_aa", "awtrix_bb", "awtrix_cc"};
    TEST_ASSERT_EQUAL_INT(2, timerSyncTargetsIndexForValue("awtrix_aa", ids, 3));
    TEST_ASSERT_EQUAL_INT(3, timerSyncTargetsIndexForValue("awtrix_bb", ids, 3));
    TEST_ASSERT_EQUAL_INT(4, timerSyncTargetsIndexForValue("awtrix_cc", ids, 3));
    // An id no longer in the registry -> unknown.
    TEST_ASSERT_EQUAL_INT(-1, timerSyncTargetsIndexForValue("awtrix_zz", ids, 3));
    // A multi-id CSV cannot be a single select option -> unknown.
    TEST_ASSERT_EQUAL_INT(-1, timerSyncTargetsIndexForValue("awtrix_aa,awtrix_bb", ids, 3));
}

// DT5 — reverse map: index resolves to its sync_targets value; out-of-range fails.
void test_DT5_reverse_map_index_to_value(void) {
    const char *ids[] = {"awtrix_aa", "awtrix_bb"};
    char buf[64];
    TEST_ASSERT_TRUE(timerSyncTargetsValueForIndex(0, ids, 2, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf);
    TEST_ASSERT_TRUE(timerSyncTargetsValueForIndex(1, ids, 2, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("all", buf);
    TEST_ASSERT_TRUE(timerSyncTargetsValueForIndex(2, ids, 2, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("awtrix_aa", buf);
    TEST_ASSERT_TRUE(timerSyncTargetsValueForIndex(3, ids, 2, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("awtrix_bb", buf);
    // Past the last peer index -> out of range, false (BadField), buf untouched.
    TEST_ASSERT_FALSE(timerSyncTargetsValueForIndex(4, ids, 2, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("awtrix_bb", buf);
}

// DT6 — the registry accessor the select consumes returns the current peer ids
// sorted ascending (so the option list is stable regardless of discovery order).
void test_DT6_peer_ids_sorted(void) {
    SHOW_TIMER = true;
    fixture::advance(1000);

    // Harvest three peers out of order via presence beacons (ungated).
    TimerManager.applySyncCommand("{\"_sync\":{\"src\":\"awtrix_cc\",\"seq\":1},\"presence\":true}");
    TimerManager.applySyncCommand("{\"_sync\":{\"src\":\"awtrix_aa\",\"seq\":1},\"presence\":true}");
    TimerManager.applySyncCommand("{\"_sync\":{\"src\":\"awtrix_bb\",\"seq\":1},\"presence\":true}");

    String ids[8];
    size_t n = TimerManager.peerIds(ids, 8);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("awtrix_aa", ids[0].c_str());
    TEST_ASSERT_EQUAL_STRING("awtrix_bb", ids[1].c_str());
    TEST_ASSERT_EQUAL_STRING("awtrix_cc", ids[2].c_str());

    // The accessor never writes past the provided capacity.
    String two[2];
    TEST_ASSERT_EQUAL_INT(2, TimerManager.peerIds(two, 2));
    TEST_ASSERT_EQUAL_STRING("awtrix_aa", two[0].c_str());
    TEST_ASSERT_EQUAL_STRING("awtrix_bb", two[1].c_str());
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

    // bar_bg_color (ADR-0020) shares the same bespoke parser (parseBarColor).
    const TimerSettingDesc *bbc = timerSettingByCmdKey("bar_bg_color");
    TEST_ASSERT_NOT_NULL(bbc);
    j["v"] = 0x202020;  TEST_ASSERT_TRUE (timerSettingParse(*bbc, j["v"], out)); TEST_ASSERT_EQUAL_UINT32(0x202020, out.num);
    j["v"] = 0;         TEST_ASSERT_TRUE (timerSettingParse(*bbc, j["v"], out)); TEST_ASSERT_EQUAL_UINT32(0, out.num);  // 0 = black (no track)
    j["v"] = "#0044AA"; TEST_ASSERT_TRUE (timerSettingParse(*bbc, j["v"], out)); TEST_ASSERT_EQUAL_UINT32(0x0044AA, out.num);
    j["v"] = "nothex";  TEST_ASSERT_FALSE(timerSettingParse(*bbc, j["v"], out));

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
    TIMER_BAR_BG_COLOR  = 0x445566;
    TIMER_MELODY_TICK   = "mytick";
    TIMER_SYNC_FOLLOW   = true;
    TIMER_SYNC_TARGETS  = "all";
    timerSettingsSaveNvs(p);

    TIMER_FINISHED_HOLD = 1;
    TIMER_MAX_DURATION  = 1;
    TIMER_BAR_ENABLED   = true;
    TIMER_BAR_COLOR     = 0;
    TIMER_BAR_BG_COLOR  = 0;
    TIMER_MELODY_TICK   = "x";
    TIMER_SYNC_FOLLOW   = false;
    TIMER_SYNC_TARGETS  = "";
    timerSettingsLoadNvs(p);

    TEST_ASSERT_EQUAL_UINT16(123,      TIMER_FINISHED_HOLD);
    TEST_ASSERT_EQUAL_UINT32(4242,     TIMER_MAX_DURATION);
    TEST_ASSERT_FALSE(TIMER_BAR_ENABLED);
    TEST_ASSERT_EQUAL_UINT32(0x112233, TIMER_BAR_COLOR);
    TEST_ASSERT_EQUAL_UINT32(0x445566, TIMER_BAR_BG_COLOR);
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
    TEST_ASSERT_TRUE(doc.containsKey("bar_bg_color"));
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
// TIMER menu slot table (src/TimerMenu.cpp). The on-device menu's name/value/adjust
// logic, host-testable for the first time (MenuManager itself isn't host-built).
// Slot order: 0 DURATION, 1 buzzer, 2 countdown, 3 finished, 4 autoclear(hold),
// 5 realert, 6 icon, 7 bar, 8 MAIN (navigation). See docs/adr/0008 and 0016.
// ============================================================================

// Shared cmdKey-resolution guard for the slot-table integrity tests (issue #49):
// resolves a table-backed slot's cmdKey exactly the way the production menu code
// does (timerSettingByCmdKey); on a miss, formats a diagnostic into msg naming
// the slot index, its item name, and the unresolved key.
static const TimerSettingDesc *resolveSlotCmdKey(const TimerMenuSlot &s, size_t idx,
                                                 char *msg, size_t msgLen) {
    const TimerSettingDesc *d = s.cmdKey ? timerSettingByCmdKey(s.cmdKey) : nullptr;
    if (!d)
        snprintf(msg, msgLen, "slot %u (%s): cmdKey \"%s\" not in TIMER_SETTINGS_DESCS",
                 (unsigned)idx, s.name ? s.name : "?", s.cmdKey ? s.cmdKey : "(null)");
    return d;
}

// M1 — table is well-formed: 7 slots, every slot labels, and each table-backed
// slot's cmdKey resolves to a descriptor whose type matches the slot kind (so the
// menu can't reference a key the settings table doesn't back, ADR-0007).
void test_M1_slot_table_well_formed(void) {
    TEST_ASSERT_EQUAL_UINT32(9, (uint32_t)TIMER_MENU_SLOT_COUNT);
    for (uint8_t i = 0; i < TIMER_MENU_SLOT_COUNT; ++i) {
        // Every list row has a non-empty name (the list label).
        TEST_ASSERT_TRUE(timerMenuName(i).length() > 0);
        const TimerMenuSlot &s = TIMER_MENU_SLOTS[i];
        switch (s.kind) {
            case TimerMenuKind::Duration:
                // No storage row; the value is the HH:MM:SS clock (non-empty).
                TEST_ASSERT_NULL(s.cmdKey);
                TEST_ASSERT_NULL(s.codec);
                TEST_ASSERT_TRUE(timerMenuValue(i).length() > 0);
                break;
            case TimerMenuKind::EnumCycle:
                TEST_ASSERT_NOT_NULL(s.codec);
                TEST_ASSERT_TRUE(s.labelCount > 0);
                TEST_ASSERT_NOT_NULL((void *)s.getEnum);
                TEST_ASSERT_NOT_NULL((void *)s.setEnum);
                TEST_ASSERT_TRUE(timerMenuValue(i).length() > 0);
                break;
            case TimerMenuKind::SteppedRange: {
                char msg[120];
                const TimerSettingDesc *d = resolveSlotCmdKey(s, i, msg, sizeof(msg));
                TEST_ASSERT_NOT_NULL_MESSAGE(d, msg);
                TEST_ASSERT_TRUE(d->type == TcType::U16);
                TEST_ASSERT_TRUE(s.step > 0);
                break;
            }
            case TimerMenuKind::BoolToggle: {
                char msg[120];
                const TimerSettingDesc *d = resolveSlotCmdKey(s, i, msg, sizeof(msg));
                TEST_ASSERT_NOT_NULL_MESSAGE(d, msg);
                TEST_ASSERT_TRUE(d->type == TcType::Bool);
                break;
            }
            case TimerMenuKind::Navigation:
                // The MAIN row carries no value and no cmdKey/codec.
                TEST_ASSERT_EQUAL_STRING("", timerMenuValue(i).c_str());
                TEST_ASSERT_NULL(s.cmdKey);
                TEST_ASSERT_NULL(s.codec);
                break;
        }
    }
    // DURATION is the first row (PRD #83 / issue #86).
    TEST_ASSERT_TRUE(TIMER_MENU_SLOTS[0].kind == TimerMenuKind::Duration);
    TEST_ASSERT_EQUAL_STRING("DURATION", timerMenuName(0).c_str());
    // The lone Navigation row (MAIN) sits last -- the TimerMenuNav back-to-main
    // invariant the device relies on.
    TEST_ASSERT_TRUE(TIMER_MENU_SLOTS[TIMER_MENU_SLOT_COUNT - 1].kind ==
                     TimerMenuKind::Navigation);
    TEST_ASSERT_EQUAL_STRING("MAIN", timerMenuName(TIMER_MENU_SLOT_COUNT - 1).c_str());
}

// M10 — negative proof for the M1 resolution guard (issue #49): a deliberately
// unresolvable cmdKey is caught by the same resolveSlotCmdKey path the M1 table
// walk uses, and the diagnostic names the slot (index + label prefix) and the
// offending key. The synthetic slot lives only here; TIMER_MENU_SLOTS is untouched.
void test_M10_unresolved_cmdkey_guard_fires_and_names_slot(void) {
    const TimerMenuSlot bogus = {TimerMenuKind::BoolToggle, "BOGUS", "no_such_key",
                                 0, nullptr, 0, nullptr, nullptr};
    char msg[120] = "";
    const TimerSettingDesc *d = resolveSlotCmdKey(bogus, 99, msg, sizeof(msg));
    TEST_ASSERT_NULL(d);                                // the guard fires...
    TEST_ASSERT_NOT_NULL(strstr(msg, "slot 99"));       // ...naming the slot...
    TEST_ASSERT_NOT_NULL(strstr(msg, "BOGUS"));
    TEST_ASSERT_NOT_NULL(strstr(msg, "no_such_key"));   // ...and the key.
}

// M2 — enum slots cycle both ways and route through the real TimerManager setter
// (proven by the recorded MQTT publish), not a raw member poke.
void test_M2_enum_cycle_wraps_and_routes_via_setter(void) {
    TimerManager.setBuzzerMode(BuzzerMode::Off);
    MQTTManager.__test_reset();

    timerMenuAdjust(1, +1);   // buzzer (slot 1): Off -> End
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BuzzerMode::End, (uint8_t)TimerManager.getBuzzerMode());
    const PublishCall *buz = fixture::last_publish(fixture::TIMER_BUZZER_TOPIC);
    TEST_ASSERT_NOT_NULL(buz);
    TEST_ASSERT_EQUAL_STRING("end", buz->payload.c_str());

    timerMenuAdjust(1, +1);   // End -> Countdown
    timerMenuAdjust(1, +1);   // Countdown -> Off (wrap)
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BuzzerMode::Off, (uint8_t)TimerManager.getBuzzerMode());

    timerMenuAdjust(1, -1);   // Off -> Countdown (wrap backward)
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BuzzerMode::Countdown, (uint8_t)TimerManager.getBuzzerMode());

    // finished slot routes through its setter too (proven on the wire: the
    // row's publish hook puts the codec string on the canonical topic).
    TimerManager.setFinishedMode(FinishedMode::AutoClear);
    MQTTManager.__test_reset();
    timerMenuAdjust(3, +1);   // finished (slot 3): AutoClear -> Hold
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FinishedMode::Hold, (uint8_t)TimerManager.getFinishedMode());
    const PublishCall *fin = fixture::last_publish(fixture::TIMER_FINISHED_TOPIC);
    TEST_ASSERT_NOT_NULL(fin);
    TEST_ASSERT_EQUAL_STRING("hold", fin->payload.c_str());
}

// M3 — stepped ranges saturate at the descriptor bounds (no overshoot/underflow).
void test_M3_stepped_range_saturates(void) {
    // finished_hold (slot 4): step 5, lo 1, hi 300.
    TIMER_FINISHED_HOLD = 298;
    timerMenuAdjust(4, +1);                                  // 298 (+5 -> 303 > 300) -> cap
    TEST_ASSERT_EQUAL_UINT16(300, TIMER_FINISHED_HOLD);
    timerMenuAdjust(4, +1);
    TEST_ASSERT_EQUAL_UINT16(300, TIMER_FINISHED_HOLD);
    TIMER_FINISHED_HOLD = 4;
    timerMenuAdjust(4, -1);                                  // 4 (>=1+5? no) -> lo
    TEST_ASSERT_EQUAL_UINT16(1, TIMER_FINISHED_HOLD);

    // countdown_seconds (slot 2): step 1, lo 0, hi 30.
    TIMER_COUNTDOWN_SECONDS = 30;
    timerMenuAdjust(2, +1);
    TEST_ASSERT_EQUAL_UINT16(30, TIMER_COUNTDOWN_SECONDS);
    TIMER_COUNTDOWN_SECONDS = 0;
    timerMenuAdjust(2, -1);
    TEST_ASSERT_EQUAL_UINT16(0, TIMER_COUNTDOWN_SECONDS);
    timerMenuAdjust(2, +1);
    TEST_ASSERT_EQUAL_UINT16(1, TIMER_COUNTDOWN_SECONDS);
}

// M4 — bool toggles flip on either button.
void test_M4_bool_toggle_flips_both_directions(void) {
    TIMER_ICON_ENABLED = true;
    timerMenuAdjust(6, +1);
    TEST_ASSERT_FALSE(TIMER_ICON_ENABLED);
    timerMenuAdjust(6, -1);
    TEST_ASSERT_TRUE(TIMER_ICON_ENABLED);

    TIMER_BAR_ENABLED = false;
    timerMenuAdjust(7, +1);
    TEST_ASSERT_TRUE(TIMER_BAR_ENABLED);
    timerMenuAdjust(7, -1);
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

// M6 — the name vs bare-value accessors. The list shows the item NAME; the leaf
// shows the BARE value only (no prefix), per PRD #83.
void test_M6_name_and_bare_value(void) {
    // Names are the list labels (DURATION first, MAIN last).
    TEST_ASSERT_EQUAL_STRING("DURATION",  timerMenuName(0).c_str());
    TEST_ASSERT_EQUAL_STRING("BUZZER",    timerMenuName(1).c_str());
    TEST_ASSERT_EQUAL_STRING("COUNTDOWN", timerMenuName(2).c_str());
    TEST_ASSERT_EQUAL_STRING("FINISH",    timerMenuName(3).c_str());
    TEST_ASSERT_EQUAL_STRING("CLEAR DELAY",       timerMenuName(4).c_str());
    TEST_ASSERT_EQUAL_STRING("RE-ALERT INTERVAL", timerMenuName(5).c_str());
    TEST_ASSERT_EQUAL_STRING("ICON",              timerMenuName(6).c_str());
    TEST_ASSERT_EQUAL_STRING("PROGRESS BAR",      timerMenuName(7).c_str());
    TEST_ASSERT_EQUAL_STRING("MAIN",      timerMenuName(8).c_str());

    // Bare values: no prefix.
    TIMER_FINISHED_HOLD = 10;
    TEST_ASSERT_EQUAL_STRING("10", timerMenuValue(4).c_str());
    TimerManager.setBuzzerMode(BuzzerMode::End);
    TEST_ASSERT_EQUAL_STRING("END", timerMenuValue(1).c_str());
    TIMER_ICON_ENABLED = true;
    TEST_ASSERT_EQUAL_STRING("ON", timerMenuValue(6).c_str());
    TIMER_COUNTDOWN_SECONDS = 3;
    TEST_ASSERT_EQUAL_STRING("3", timerMenuValue(2).c_str());
    // DURATION value is the zero-padded HH:MM:SS clock.
    TimerManager.setDuration(305);   // 0h 5m 5s
    TEST_ASSERT_EQUAL_STRING("00:05:05", timerMenuValue(0).c_str());
}

// M7 — an enum adjust applies + publishes live but DEFERS the NVS write; the write
// happens only on the long-press commit (the PersistBatch guard, #45). Proven via
// Preferences::begin_calls (persist() brackets a begin("timer")). See ADR-0008.
void test_M7_enum_adjust_defers_persist_until_commit(void) {
    TimerManager.setBuzzerMode(BuzzerMode::End);   // known starting point (default persist)
    int before = Preferences::begin_calls;

    timerMenuAdjust(1, +1);   // buzzer (slot 1): End -> Countdown, persist deferred
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BuzzerMode::Countdown, (uint8_t)TimerManager.getBuzzerMode());
    TEST_ASSERT_EQUAL_INT(before, Preferences::begin_calls);   // no "timer"-ns write yet

    {
        TimerManager_::PersistBatch batch(TimerManager);       // the commit seam
    }
    TEST_ASSERT_TRUE(Preferences::begin_calls > before);       // commit flushed it
}

// M8 — the two enum slots source their on-screen label from the codec table's
// menu column (single source of truth, #19), proven through the public
// timerMenuLabel() for every enum value. Guards against the menu re-growing a
// private label copy that could drift from the codec table.
void test_M8_enum_labels_source_from_codec(void) {
    for (uint8_t i = 0; i < (uint8_t)BuzzerMode::COUNT; ++i) {
        TimerManager.setBuzzerMode((BuzzerMode)i);
        TEST_ASSERT_EQUAL_STRING(TIMER_BUZZER_CODEC[i].menu, timerMenuValue(1).c_str());
    }
    for (uint8_t i = 0; i < (uint8_t)FinishedMode::COUNT; ++i) {
        TimerManager.setFinishedMode((FinishedMode)i);
        TEST_ASSERT_EQUAL_STRING(TIMER_FINISHED_CODEC[i].menu, timerMenuValue(3).c_str());
    }
    // The codec menu column is now the BARE value (no "BZR "/"FIN " prefix).
    TEST_ASSERT_EQUAL_STRING("END",   TIMER_BUZZER_CODEC[(int)BuzzerMode::End].menu);
    // FINISH on-device labels are plain language (PRD #96): auto-clear -> CLEAR,
    // re-alert -> RE-ALERT. The wire/ha carrier columns are unchanged (ADR-0010).
    TEST_ASSERT_EQUAL_STRING("CLEAR",    TIMER_FINISHED_CODEC[(int)FinishedMode::AutoClear].menu);
    TEST_ASSERT_EQUAL_STRING("RE-ALERT", TIMER_FINISHED_CODEC[(int)FinishedMode::ReAlert].menu);
}

// M9 — the TIMER-menu long-press commit is ONE PersistBatch window (#45): enum
// edits deferred during scroll ("timer" ns) and table-row edits ("awtrix" ns)
// persist together at scope exit, exactly one flush per namespace. Mirrors
// MenuManager's TimerConfigMenu commit seam (device-only; stubbed here).
void test_M9_menu_commit_one_persistbatch_flushes_both_namespaces(void) {
    saveSettings_calls = 0;
    int before = Preferences::begin_calls;

    timerMenuAdjust(1, +1);   // buzzer (slot 1) End -> Countdown: applies live, persist deferred
    timerMenuAdjust(4, +1);   // finished_hold (slot 4) 10 -> 15: table row, RAM only
    TEST_ASSERT_EQUAL_INT(0, Preferences::begin_calls - before);   // no "timer" write during scroll
    TEST_ASSERT_EQUAL_INT(0, saveSettings_calls);                  // no "awtrix" write during scroll

    {   // the long-press commit: scope exit flushes both namespaces together
        TimerManager_::PersistBatch batch(TimerManager);
        batch.markTableDirty();
    }
    TEST_ASSERT_EQUAL_INT(1, Preferences::begin_calls - before);   // one "timer" flush
    TEST_ASSERT_EQUAL_INT(1, saveSettings_calls);                  // one "awtrix" flush
    TEST_ASSERT_EQUAL_UINT16(15, TIMER_FINISHED_HOLD);

    // The deferred enum edit actually reached NVS: reboot round-trip (same
    // precedent as U57 — setup() reloads from Preferences).
    TimerManager.setup();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BuzzerMode::Countdown, (uint8_t)TimerManager.getBuzzerMode());
}

// M11 — the TIMER-menu long-press commit refreshes every HA attribute group
// (issue #60): after the PersistBatch commit, the commit republishes every
// carrier's attribute object, so an on-device edit of a menu knob (finished_hold/
// realert_interval/countdown_seconds/icon/bar) refreshes its HA attribute
// immediately instead of waiting for the next reconnect — closing the staleness
// gap. The menu commit no longer broadcasts config to peers (run-scoped config
// mirror, ADR-0018 superseding ADR-0006): config travels only bundled with a
// `start`. Mirrors MenuManager's commitTimerMenu seam (device-only; reproduced
// here in production order), pinning the attribute refresh and the no-broadcast.
void test_M11_menu_commit_republishes_all_attribute_groups(void) {
    TIMER_SYNC_TARGETS = "all";   // sync on, yet a menu config commit still emits nothing

    timerMenuAdjust(4, +1);   // finished_hold (slot 4) 10 -> 15: a menu knob, live in RAM, NVS deferred

    {   // the long-press commit window, then the attribute refresh (no peer broadcast)
        TimerManager_::PersistBatch batch(TimerManager);
        batch.markTableDirty();
    }
    TimerManager.publishAllAttributeGroups();

    // A config edit no longer propagates to peers (ADR-0018).
    TEST_ASSERT_EQUAL_INT(0, fixture::sync_packet_count());
    // ... and every carrier's attribute object was refreshed exactly once,
    // including the Duration text entity (PRD #66 / issue #68).
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_FINISHED_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_BUZZER_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_STATE_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_REMAINING_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_DURATION_ATTR_TOPIC));
    // The just-committed knob's new value is in its carrier's refreshed bag.
    const PublishCall *fin = fixture::last_publish(fixture::TIMER_FINISHED_ATTR_TOPIC);
    TEST_ASSERT_NOT_NULL(fin);
    TEST_ASSERT_NOT_NULL(strstr(fin->payload.c_str(), "\"finished_hold\":15"));
}

// ============================================================================
// TIMER menu navigation state machine (src/TimerMenuNav.cpp). The display-free
// drill-in interaction model: list/leaf focus, selected index with wrap, and the
// input->outcome mapping the device layer acts on (PRD #83 / issues #85, #86). The
// nine list rows are the nine slots (DURATION at 0, 6 value rows, MAIN at index 8).
// See docs/adr/0016.
// ============================================================================

// The on-screen label the device renders for the current cursor: the item NAME in
// List focus, the BARE VALUE in Editing focus -- mirrors MenuManager.menutext().
static String navCurrentLabel(const TimerMenuNav &nav) {
    return nav.focus() == TimerNavFocus::List
               ? timerMenuName(nav.index())
               : timerMenuValue(nav.index());
}

// N1 — list navigation walks every row and WRAPS in both directions; focus stays
// List throughout and the reported label is the item name.
void test_N1_list_navigation_wraps(void) {
    TimerMenuNav nav(TIMER_MENU_SLOT_COUNT, TIMER_MENU_SLOT_COUNT - 1);
    TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::List);
    TEST_ASSERT_EQUAL_UINT8(0, nav.index());
    TEST_ASSERT_EQUAL_STRING("DURATION", navCurrentLabel(nav).c_str());

    // Walk right through all nine rows and wrap back to 0.
    for (uint8_t i = 1; i < TIMER_MENU_SLOT_COUNT; ++i) {
        TEST_ASSERT_TRUE(nav.navigate(+1) == TimerNavOutcome::None);
        TEST_ASSERT_EQUAL_UINT8(i, nav.index());
        TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::List);
    }
    TEST_ASSERT_EQUAL_STRING("MAIN", navCurrentLabel(nav).c_str());  // last row
    nav.navigate(+1);
    TEST_ASSERT_EQUAL_UINT8(0, nav.index());                        // wrapped

    // Left from the first row lands on the last (MAIN).
    nav.navigate(-1);
    TEST_ASSERT_EQUAL_UINT8(TIMER_MENU_SLOT_COUNT - 1, nav.index());
}

// N2 — short press on a value row drills into its leaf (focus -> Editing); the
// reported label switches from the item name to the bare value.
void test_N2_select_value_row_enters_leaf(void) {
    TimerManager.setBuzzerMode(BuzzerMode::End);
    TimerMenuNav nav(TIMER_MENU_SLOT_COUNT, TIMER_MENU_SLOT_COUNT - 1);
    nav.navigate(+1);          // to BUZZER (slot 1)
    TEST_ASSERT_EQUAL_STRING("BUZZER", navCurrentLabel(nav).c_str());

    TEST_ASSERT_TRUE(nav.select() == TimerNavOutcome::EnterLeaf);
    TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::Editing);
    TEST_ASSERT_EQUAL_UINT8(1, nav.index());
    TEST_ASSERT_EQUAL_STRING("END", navCurrentLabel(nav).c_str());  // bare value
}

// N3 — inside a value leaf, left/right yields AdjustValue (the caller mutates the
// value via timerMenuAdjust) and the cursor index does NOT move.
void test_N3_leaf_navigate_adjusts_value(void) {
    TimerMenuNav nav(TIMER_MENU_SLOT_COUNT, TIMER_MENU_SLOT_COUNT - 1);
    nav.navigate(+1);          // to BUZZER (slot 1)
    nav.select();              // enter leaf
    TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::Editing);

    TEST_ASSERT_TRUE(nav.navigate(+1) == TimerNavOutcome::AdjustValue);
    TEST_ASSERT_EQUAL_UINT8(1, nav.index());   // still on BUZZER
    TEST_ASSERT_TRUE(nav.navigate(-1) == TimerNavOutcome::AdjustValue);
    TEST_ASSERT_EQUAL_UINT8(1, nav.index());
}

// N4 — short press inside a value leaf confirms and returns to the list.
void test_N4_leaf_select_confirms_back_to_list(void) {
    TimerMenuNav nav(TIMER_MENU_SLOT_COUNT, TIMER_MENU_SLOT_COUNT - 1);
    nav.navigate(+1);          // to BUZZER (slot 1)
    nav.select();              // enter BUZZER leaf
    TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::Editing);

    TEST_ASSERT_TRUE(nav.select() == TimerNavOutcome::ConfirmBackToList);
    TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::List);
    TEST_ASSERT_EQUAL_UINT8(1, nav.index());
}

// N5 — long press inside a value leaf saves and returns to the list; the value is
// already live in RAM so no separate commit happens here.
void test_N5_leaf_back_returns_to_list(void) {
    TimerMenuNav nav(TIMER_MENU_SLOT_COUNT, TIMER_MENU_SLOT_COUNT - 1);
    nav.navigate(+1);          // to BUZZER (slot 1)
    nav.select();              // enter BUZZER leaf
    TEST_ASSERT_TRUE(nav.back() == TimerNavOutcome::BackToList);
    TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::List);
}

// N6 — short press on MAIN returns to the main menu (and commits), from EITHER
// entry origin (PRD #83 / issue #87: MAIN always goes to the main menu).
void test_N6_select_main_goes_to_main_menu(void) {
    for (uint8_t o = 0; o < 2; ++o) {
        TimerNavOrigin origin = o == 0 ? TimerNavOrigin::Menu : TimerNavOrigin::App;
        TimerMenuNav nav(TIMER_MENU_SLOT_COUNT, TIMER_MENU_SLOT_COUNT - 1, origin);
        // walk to MAIN (last row)
        for (uint8_t i = 0; i < TIMER_MENU_SLOT_COUNT - 1; ++i) nav.navigate(+1);
        TEST_ASSERT_TRUE(nav.onMain());
        TEST_ASSERT_TRUE(nav.select() == TimerNavOutcome::GoToMainMenu);
        TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::List);  // not a leaf
    }
}

// N7 — long press OUT of the list is context-aware by entry origin: Menu -> back
// to the main menu, App -> exit the menu (back to the Timer app).
void test_N7_list_back_is_context_aware(void) {
    TimerMenuNav fromMenu(TIMER_MENU_SLOT_COUNT, TIMER_MENU_SLOT_COUNT - 1,
                          TimerNavOrigin::Menu);
    TEST_ASSERT_TRUE(fromMenu.back() == TimerNavOutcome::GoToMainMenu);

    TimerMenuNav fromApp(TIMER_MENU_SLOT_COUNT, TIMER_MENU_SLOT_COUNT - 1,
                         TimerNavOrigin::App);
    TEST_ASSERT_TRUE(fromApp.back() == TimerNavOutcome::ExitMenu);
}

// N8 — enter() (re)opens the list: List focus, index 0, fresh origin -- the device
// uses it whenever the TIMER menu is opened, even after a prior leaf edit.
void test_N8_enter_resets_to_list_top(void) {
    TimerMenuNav nav(TIMER_MENU_SLOT_COUNT, TIMER_MENU_SLOT_COUNT - 1,
                     TimerNavOrigin::Menu);
    nav.navigate(+1);
    nav.select();              // now Editing on BUZZER (slot 1)
    TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::Editing);

    nav.enter(TIMER_MENU_SLOT_COUNT, TIMER_MENU_SLOT_COUNT - 1, TimerNavOrigin::App);
    TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::List);
    TEST_ASSERT_EQUAL_UINT8(0, nav.index());
    TEST_ASSERT_TRUE(nav.origin() == TimerNavOrigin::App);
}

// N9 — the Idle-only gating decision is host-testable: timerMenuLeafKind maps the
// DURATION slot to an editable wheel only while Idle, read-only otherwise; every
// other slot is a plain Value leaf (PRD #83 user story 27).
void test_N9_duration_leaf_kind_gated_by_state(void) {
    TEST_ASSERT_TRUE(timerMenuLeafKind(0, TimerState::Idle) == TimerNavLeaf::DurationEditable);
    TEST_ASSERT_TRUE(timerMenuLeafKind(0, TimerState::Running) == TimerNavLeaf::DurationReadOnly);
    TEST_ASSERT_TRUE(timerMenuLeafKind(0, TimerState::Paused) == TimerNavLeaf::DurationReadOnly);
    TEST_ASSERT_TRUE(timerMenuLeafKind(0, TimerState::Finished) == TimerNavLeaf::DurationReadOnly);
    // A value row is always a plain Value leaf regardless of state.
    TEST_ASSERT_TRUE(timerMenuLeafKind(1, TimerState::Idle) == TimerNavLeaf::Value);
    TEST_ASSERT_TRUE(timerMenuLeafKind(1, TimerState::Running) == TimerNavLeaf::Value);
}

// N10 — the editable DURATION leaf: short press cycles the H/M/S field (stays in
// the leaf), left/right steps the field, and a long press commits and returns to
// the list.
void test_N10_duration_editable_leaf_inputs(void) {
    TimerMenuNav nav(TIMER_MENU_SLOT_COUNT, TIMER_MENU_SLOT_COUNT - 1);
    // Drill into DURATION (slot 0) as an editable wheel.
    TEST_ASSERT_TRUE(nav.select(TimerNavLeaf::DurationEditable) == TimerNavOutcome::EnterLeaf);
    TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::Editing);

    // Short press cycles the field and STAYS in the leaf (not confirm-back).
    TEST_ASSERT_TRUE(nav.select() == TimerNavOutcome::CycleField);
    TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::Editing);

    // left/right step the active field.
    TEST_ASSERT_TRUE(nav.navigate(+1) == TimerNavOutcome::AdjustField);
    TEST_ASSERT_TRUE(nav.navigate(-1) == TimerNavOutcome::AdjustField);
    TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::Editing);

    // Long press commits the duration and returns to the list.
    TEST_ASSERT_TRUE(nav.back() == TimerNavOutcome::CommitDuration);
    TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::List);
    TEST_ASSERT_EQUAL_UINT8(0, nav.index());
}

// N11 — the read-only DURATION leaf (Running/Paused): left/right are no-ops and any
// press returns to the list; nothing is committed.
void test_N11_duration_readonly_leaf_inputs(void) {
    TimerMenuNav nav(TIMER_MENU_SLOT_COUNT, TIMER_MENU_SLOT_COUNT - 1);
    TEST_ASSERT_TRUE(nav.select(TimerNavLeaf::DurationReadOnly) == TimerNavOutcome::EnterLeaf);
    TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::Editing);

    // left/right do nothing.
    TEST_ASSERT_TRUE(nav.navigate(+1) == TimerNavOutcome::None);
    TEST_ASSERT_TRUE(nav.navigate(-1) == TimerNavOutcome::None);
    TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::Editing);

    // A short press returns to the list (no CommitDuration, no field cycle).
    TEST_ASSERT_TRUE(nav.select() == TimerNavOutcome::BackToList);
    TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::List);

    // And so does a long press (re-entered).
    nav.select(TimerNavLeaf::DurationReadOnly);
    TEST_ASSERT_TRUE(nav.back() == TimerNavOutcome::BackToList);
    TEST_ASSERT_TRUE(nav.focus() == TimerNavFocus::List);
}

// ============================================================================
// TIMER per-enum codec table (src/TimerEnums.cpp). The fifth descriptor-table
// family member: one row per enum value, indexed by the enum's numeric value,
// carrying {wire, menu, ha, aliases}. The wire column is the MQTT/HTTP/sync
// contract consumed by parse*/toString in TimerManager; menu/ha columns are
// the bare on-device leaf value and the HA option label. See docs/adr/0010.
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
// Attribute-group projection (PRD #57 / issue #58). The shared per-row emit, the
// key->carrier attribute-group table, and the carrier bag builder. Pure data +
// pure functions, asserted on the host without ArduinoHA.
// ============================================================================

// T11 — the shared "storage -> JSON value" emit (timerSettingEmitValue) writes
// each row's live value under its cmdKey, dispatched by type, IGNORING snapshot
// membership (so the state bag can later mix snapshot and non-snapshot rows).
// This is the single helper both the config snapshot and the attribute builder
// reuse, so the two can never disagree about a value. Pins by-type serialization
// once: uint (U16/U32), bool, string.
void test_T11_setting_emit_value_by_type(void) {
    TIMER_REALERT_INTERVAL = 42;   // U16
    TIMER_MAX_DURATION     = 7200; // U32
    TIMER_ICON_ENABLED     = false;// Bool
    TIMER_MELODY_TICK      = "tick_a"; // Str
    TIMER_SYNC_TARGETS     = "all";    // Str, inSnapshot=false (membership ignored)

    StaticJsonDocument<256> doc;
    timerSettingEmitValue(*timerSettingByCmdKey("realert_interval"), doc);
    timerSettingEmitValue(*timerSettingByCmdKey("max_duration"),     doc);
    timerSettingEmitValue(*timerSettingByCmdKey("icon_enabled"),     doc);
    timerSettingEmitValue(*timerSettingByCmdKey("melody_tick"),      doc);
    timerSettingEmitValue(*timerSettingByCmdKey("sync_targets"),     doc);

    TEST_ASSERT_EQUAL_UINT32(42,   doc["realert_interval"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(7200, doc["max_duration"].as<uint32_t>());
    TEST_ASSERT_FALSE(doc["icon_enabled"].as<bool>());
    TEST_ASSERT_TRUE(doc["icon_enabled"].is<bool>());   // bool stays a JSON bool
    TEST_ASSERT_EQUAL_STRING("tick_a", doc["melody_tick"].as<const char *>());
    TEST_ASSERT_EQUAL_STRING("all",    doc["sync_targets"].as<const char *>());
}

// T12 — the finished-select carrier's attribute bag (timerBuildAttributeGroup):
// exactly {realert_interval, finished_hold}, carrying the live values, and NOTHING
// else (no buzzer keys cross over). realert_interval is first so the folded payload
// is a superset of the legacy {"realert_interval":N} (PRD #57: realert_interval
// unchanged from the user's perspective).
void test_T12_attribute_group_finished_bag(void) {
    TIMER_REALERT_INTERVAL = 99;
    TIMER_FINISHED_HOLD    = 7;

    StaticJsonDocument<256> doc;
    timerBuildAttributeGroup(TimerHaEntity::Finished, doc);

    TEST_ASSERT_EQUAL_UINT32(99, doc["realert_interval"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(7,  doc["finished_hold"].as<uint32_t>());
    TEST_ASSERT_EQUAL_INT(2, (int)doc.as<JsonObjectConst>().size());
}

// T13 — the buzzer-select carrier's attribute bag: exactly {countdown_seconds,
// melody_tick, melody_end} with live values, and only those (no finished keys).
void test_T13_attribute_group_buzzer_bag(void) {
    TIMER_COUNTDOWN_SECONDS = 5;
    TIMER_MELODY_TICK       = "tk";
    TIMER_MELODY_END        = "nd";

    StaticJsonDocument<256> doc;
    timerBuildAttributeGroup(TimerHaEntity::Buzzer, doc);

    TEST_ASSERT_EQUAL_UINT32(5, doc["countdown_seconds"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING("tk", doc["melody_tick"].as<const char *>());
    TEST_ASSERT_EQUAL_STRING("nd", doc["melody_end"].as<const char *>());
    TEST_ASSERT_EQUAL_INT(3, (int)doc.as<JsonObjectConst>().size());
}

// T14 — the attribute-group table is well-formed: every row maps to a real
// settings row (the cmdKey resolves via timerSettingByCmdKey), and the lit-up
// carriers are exactly the buzzer/finished selects, the state/remaining sensors
// (PRD #57 / issue #59), plus the Duration text entity (PRD #66 / issue #68
// extended the opt-in to HAText) — the carrier set grew four -> five.
void test_T14_attribute_group_table_well_formed(void) {
    TEST_ASSERT_TRUE(TIMER_ATTR_GROUP_DESC_COUNT >= 5);
    for (size_t i = 0; i < TIMER_ATTR_GROUP_DESC_COUNT; ++i) {
        const TimerAttrGroupDesc &g = TIMER_ATTR_GROUP_DESCS[i];
        TEST_ASSERT_NOT_NULL(g.cmdKey);
        TEST_ASSERT_NOT_NULL_MESSAGE(timerSettingByCmdKey(g.cmdKey), g.cmdKey);
        // The lit-up carriers: the two HASelects, the two HASensors, the HAText.
        TEST_ASSERT_TRUE(g.carrier == TimerHaEntity::Buzzer   ||
                         g.carrier == TimerHaEntity::Finished ||
                         g.carrier == TimerHaEntity::State    ||
                         g.carrier == TimerHaEntity::Remaining||
                         g.carrier == TimerHaEntity::Duration);
    }
}

// T15 — the state-sensor carrier's attribute bag (timerBuildAttributeGroup):
// exactly the eight config-view keys in table order, carrying live values, with
// bar_color / bar_bg_color rendered as the human "#RRGGBB" string via their per-row
// formatters (deliberately different from the raw-int config-snapshot form).
// sync_follow / sync_targets ride here as read-only attributes despite
// inSnapshot=false. (app_config_timeout was removed in PRD #83 / #88; bar_bg_color
// added in ADR-0020.)
void test_T15_attribute_group_state_bag(void) {
    TIMER_MAX_DURATION     = 7200;
    TIMER_PUBLISH_INTERVAL = 5;
    TIMER_ICON_ENABLED     = false;
    TIMER_BAR_ENABLED      = true;
    TIMER_BAR_COLOR        = 0xFF8800;
    TIMER_BAR_BG_COLOR     = 0x202020;
    TIMER_SYNC_FOLLOW      = true;
    TIMER_SYNC_TARGETS     = "all";

    StaticJsonDocument<512> doc;
    timerBuildAttributeGroup(TimerHaEntity::State, doc);

    TEST_ASSERT_EQUAL_UINT32(7200, doc["max_duration"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(5,    doc["remaining_publish_interval"].as<uint32_t>());
    TEST_ASSERT_TRUE(doc["app_config_timeout"].isNull());   // key gone
    TEST_ASSERT_FALSE(doc["icon_enabled"].as<bool>());
    TEST_ASSERT_TRUE(doc["bar_enabled"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("#FF8800", doc["bar_color"].as<const char *>());
    TEST_ASSERT_EQUAL_STRING("#202020", doc["bar_bg_color"].as<const char *>());
    TEST_ASSERT_TRUE(doc["sync_follow"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("all", doc["sync_targets"].as<const char *>());
    TEST_ASSERT_EQUAL_INT(8, (int)doc.as<JsonObjectConst>().size());
}

// T16 — the remaining-sensor carrier's attribute bag: exactly
// {remaining_publish_interval} (that sensor's own cadence), live value, nothing
// else. The key is shared with the state bag (T15) — one settings row, two carriers.
void test_T16_attribute_group_remaining_bag(void) {
    TIMER_PUBLISH_INTERVAL = 9;

    StaticJsonDocument<256> doc;
    timerBuildAttributeGroup(TimerHaEntity::Remaining, doc);

    TEST_ASSERT_EQUAL_UINT32(9, doc["remaining_publish_interval"].as<uint32_t>());
    TEST_ASSERT_EQUAL_INT(1, (int)doc.as<JsonObjectConst>().size());
}

// T17 — the bar_color formatter renders the human attribute string: "default"
// when 0 (follow text color, ADR-0004), else uppercase "#RRGGBB". This is the
// only formatter row; it deliberately differs from the raw-int config snapshot.
void test_T17_bar_color_formatter_renders_default_or_hex(void) {
    const TimerSettingDesc *d = timerSettingByCmdKey("bar_color");
    TEST_ASSERT_NOT_NULL(d);

    TIMER_BAR_COLOR = 0;
    StaticJsonDocument<512> off;
    timerBuildAttributeGroup(TimerHaEntity::State, off);
    TEST_ASSERT_EQUAL_STRING("default", off["bar_color"].as<const char *>());

    TIMER_BAR_COLOR = 0x00FF00;
    StaticJsonDocument<512> green;
    timerBuildAttributeGroup(TimerHaEntity::State, green);
    TEST_ASSERT_EQUAL_STRING("#00FF00", green["bar_color"].as<const char *>());
}

// T17b — the bar_bg_color formatter (ADR-0020) renders "none" when 0 (black = no
// track, a LITERAL off, NOT the foreground's text-color sentinel) else uppercase
// "#RRGGBB". The "none" vs bar_color's "default" label is the deliberate asymmetry.
void test_T17b_bar_bg_color_formatter_renders_none_or_hex(void) {
    const TimerSettingDesc *d = timerSettingByCmdKey("bar_bg_color");
    TEST_ASSERT_NOT_NULL(d);

    TIMER_BAR_BG_COLOR = 0;
    StaticJsonDocument<512> off;
    timerBuildAttributeGroup(TimerHaEntity::State, off);
    TEST_ASSERT_EQUAL_STRING("none", off["bar_bg_color"].as<const char *>());

    TIMER_BAR_BG_COLOR = 0x202020;
    StaticJsonDocument<512> set;
    timerBuildAttributeGroup(TimerHaEntity::State, set);
    TEST_ASSERT_EQUAL_STRING("#202020", set["bar_bg_color"].as<const char *>());
}

// T18 — the Duration text entity's attribute bag (PRD #66 / issue #68): exactly
// {max_duration}, rendered in the carrier-NATIVE clock form the Duration entity's
// own state speaks (formatHMS), NOT the raw seconds the state sensor keeps. This
// is the first projected key whose representation differs per carrier: T15 pins
// max_duration as the raw number 86400 on the State carrier; here the SAME key on
// the Duration carrier is the clock STRING "24:00:00". The formatter reuses the
// exact formatHMS the Duration state uses, including its trimming (drop the hours
// group when zero), so the edge value 3600 renders "1:00:00".
void test_T18_attribute_group_duration_bag(void) {
    TIMER_MAX_DURATION = 86400;   // the default cap

    StaticJsonDocument<256> doc;
    timerBuildAttributeGroup(TimerHaEntity::Duration, doc);

    TEST_ASSERT_EQUAL_STRING("24:00:00", doc["max_duration"].as<const char *>());
    TEST_ASSERT_TRUE(doc["max_duration"].is<const char *>());   // clock STRING, not a number
    TEST_ASSERT_EQUAL_INT(1, (int)doc.as<JsonObjectConst>().size());

    // Edge value: 3600 s -> the trimmed clock form "1:00:00" (hours group kept,
    // byte-identical in style to formatHMS, which the Duration state also uses).
    TIMER_MAX_DURATION = 3600;
    StaticJsonDocument<256> edge;
    timerBuildAttributeGroup(TimerHaEntity::Duration, edge);
    TEST_ASSERT_EQUAL_STRING("1:00:00", edge["max_duration"].as<const char *>());
}

// T19 (#74) — drift guard for the GET /api/timer config mirror. The pure
// projection timerBuildFullConfig walks BOTH descriptor tables, so its output
// must carry every persisted config key. For every TIMER_SETTINGS_DESCS row
// (including the sync-role keys sync_follow/sync_targets, which are NOT in the
// propagation snapshot) and every TIMER_MEMBER_CONFIG_DESCS row, the produced
// config object contains that cmdKey. Walking the tables means a future config
// key forces a corresponding read entry here or this test fails -- the read/write
// parity cannot silently regress. duration (run-state) and action (not
// persisted) are deliberately absent.
void test_T19_full_config_mirrors_every_persisted_key(void) {
    DynamicJsonDocument cfg(2048);
    timerBuildFullConfig(cfg);

    for (size_t i = 0; i < TIMER_SETTINGS_DESC_COUNT; ++i) {
        const char *k = TIMER_SETTINGS_DESCS[i].cmdKey;
        TEST_ASSERT_TRUE_MESSAGE(cfg.containsKey(k), k);
    }
    for (size_t i = 0; i < TIMER_MEMBER_CONFIG_DESC_COUNT; ++i) {
        const char *k = TIMER_MEMBER_CONFIG_DESCS[i].cmdKey;
        TEST_ASSERT_TRUE_MESSAGE(cfg.containsKey(k), k);
    }

    // The run-state / non-persisted keys are NOT config.
    TEST_ASSERT_FALSE(cfg.containsKey("duration"));
    TEST_ASSERT_FALSE(cfg.containsKey("action"));
}

// T20 (#75) — the two deliberate carrier-native renderings on the GET config
// mirror, following this endpoint's raw+_str duration precedent. bar_color is
// rendered as the human string ("default" when 0 / follow text color, ADR-0004,
// else uppercase "#RRGGBB") instead of the raw int. max_duration keeps its raw
// seconds AND gains a sibling max_duration_str in the trimmed clock form (reusing
// formatHMS, hours group kept). Every other key stays raw.
void test_T20_full_config_carrier_native_renderings(void) {
    // bar_color: "default" when 0.
    TIMER_BAR_COLOR = 0;
    {
        DynamicJsonDocument cfg(2048);
        timerBuildFullConfig(cfg);
        TEST_ASSERT_TRUE(cfg["bar_color"].is<const char *>());
        TEST_ASSERT_EQUAL_STRING("default", cfg["bar_color"].as<const char *>());
    }
    // bar_color: uppercase "#RRGGBB" when set.
    TIMER_BAR_COLOR = 0x00FF00;
    {
        DynamicJsonDocument cfg(2048);
        timerBuildFullConfig(cfg);
        TEST_ASSERT_EQUAL_STRING("#00FF00", cfg["bar_color"].as<const char *>());
    }
    // bar_bg_color (ADR-0020): "none" when 0, uppercase "#RRGGBB" when set.
    TIMER_BAR_BG_COLOR = 0;
    {
        DynamicJsonDocument cfg(2048);
        timerBuildFullConfig(cfg);
        TEST_ASSERT_TRUE(cfg["bar_bg_color"].is<const char *>());
        TEST_ASSERT_EQUAL_STRING("none", cfg["bar_bg_color"].as<const char *>());
    }
    TIMER_BAR_BG_COLOR = 0x202020;
    {
        DynamicJsonDocument cfg(2048);
        timerBuildFullConfig(cfg);
        TEST_ASSERT_EQUAL_STRING("#202020", cfg["bar_bg_color"].as<const char *>());
    }
    // max_duration: raw seconds kept, plus the trimmed clock-string sibling.
    TIMER_MAX_DURATION = 86400;
    {
        DynamicJsonDocument cfg(2048);
        timerBuildFullConfig(cfg);
        TEST_ASSERT_FALSE(cfg["max_duration"].is<const char *>());   // still a number
        TEST_ASSERT_EQUAL_UINT32(86400, cfg["max_duration"].as<uint32_t>());
        TEST_ASSERT_EQUAL_STRING("24:00:00", cfg["max_duration_str"].as<const char *>());
    }
    // Edge value: 3600 s -> "1:00:00" (hours group kept), raw unchanged.
    TIMER_MAX_DURATION = 3600;
    {
        DynamicJsonDocument cfg(2048);
        timerBuildFullConfig(cfg);
        TEST_ASSERT_EQUAL_UINT32(3600, cfg["max_duration"].as<uint32_t>());
        TEST_ASSERT_EQUAL_STRING("1:00:00", cfg["max_duration_str"].as<const char *>());
    }
}

// T21 (#75) — value spot-check round-trip: set representative knobs via
// parseCommand (a number, a color, a CSV sync target, an icon, a melody, a
// toggle) then GET them back under config. Proves the read-after-write contract
// over HTTP alone, including both carrier-native renderings. sync_targets and
// the melody/icon read back as their raw strings; the toggle as a raw bool.
void test_T21_get_config_value_spotchecks_via_parsecommand(void) {
    SHOW_TIMER = true;
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand(
            "{\"max_duration\":7200,\"bar_color\":\"#112233\",\"sync_targets\":\"abc,def\","
            "\"icon_running\":\"run\",\"melody_tick\":\"tick2\",\"bar_enabled\":false}")));

    DynamicJsonDocument doc(2048);
    TEST_ASSERT_FALSE(deserializeJson(doc, TimerManager.getStateJson()));
    JsonObject c = doc["config"].as<JsonObject>();
    TEST_ASSERT_FALSE(c.isNull());

    TEST_ASSERT_EQUAL_UINT32(7200, c["max_duration"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING("2:00:00", c["max_duration_str"].as<const char *>());
    TEST_ASSERT_EQUAL_STRING("#112233", c["bar_color"].as<const char *>());
    TEST_ASSERT_EQUAL_STRING("abc,def", c["sync_targets"].as<const char *>());
    TEST_ASSERT_EQUAL_STRING("run", c["icon_running"].as<const char *>());
    TEST_ASSERT_EQUAL_STRING("tick2", c["melody_tick"].as<const char *>());
    TEST_ASSERT_FALSE(c["bar_enabled"].as<bool>());
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

// CE9 — a held right button auto-repeats +1 on the current field: the hold clock
// starts at the first tick the button is observed pressed; the first step fires
// once the 500 ms long-press threshold is crossed, then one step per 250 ms cadence
// window, and nothing in between. Field is SS (no cap interference).
void test_CE9_held_button_autorepeats_at_cadence(void) {
    TIMER_MAX_DURATION = 0;        // disable cap: SS wraps at 59, predictable +1 steps
    TimerConfigEditor ed;
    ed.enter(0);                   // 00:00:00, field=HH
    ed.cycleField(); ed.cycleField();   // -> SS

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
    TimerConfigEditor ed;
    ed.enter(5);                   // 00:00:05, field=HH
    ed.cycleField(); ed.cycleField();   // -> SS = 5

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

// ============================================================================
// W12 — full wire refresh (issue #41, closing PRD #28): publishAllWire() puts
// every declared wire artifact — state, remaining, duration, buzzer, finished,
// icons aggregate — on its canonical topic EXACTLY once with the current value
// as payload. "Every" pins that the connect / discovery-enable republish is
// derived from the member table (a new published row cannot be silently
// skipped); "exactly once" pins the shared-icons-hook dedupe. The total count
// pins that nothing else rides along. No order assertion: boot-republish order
// is not a contract anyone consumes.
// ============================================================================
void test_W12_publishAllWire_each_artifact_exactly_once(void) {
    // Seed non-default values through parseCommand, then clear the recording
    // so only the refresh itself is counted.
    TimerManager.parseCommand(
        "{\"duration\":3661,\"buzzer\":\"countdown\",\"finished\":\"hold\",\"icon_idle\":\"clock\"}");
    MQTTManager.__test_reset();

    TimerManager.publishAllWire();

    TEST_ASSERT_EQUAL_INT(6, (int)MQTTManager.recorded.size());
    const char *topics[] = {
        fixture::TIMER_STATE_TOPIC,    fixture::TIMER_REMAINING_TOPIC,
        fixture::TIMER_DURATION_TOPIC, fixture::TIMER_BUZZER_TOPIC,
        fixture::TIMER_FINISHED_TOPIC, fixture::TIMER_ICONS_TOPIC};
    for (const char *t : topics)
        TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(t));

    TEST_ASSERT_EQUAL_STRING("idle",      fixture::last_publish(fixture::TIMER_STATE_TOPIC)->payload.c_str());
    TEST_ASSERT_EQUAL_STRING("3661",      fixture::last_publish(fixture::TIMER_REMAINING_TOPIC)->payload.c_str());
    TEST_ASSERT_EQUAL_STRING("1:01:01",   fixture::last_publish(fixture::TIMER_DURATION_TOPIC)->payload.c_str());
    TEST_ASSERT_EQUAL_STRING("countdown", fixture::last_publish(fixture::TIMER_BUZZER_TOPIC)->payload.c_str());
    TEST_ASSERT_EQUAL_STRING("hold",      fixture::last_publish(fixture::TIMER_FINISHED_TOPIC)->payload.c_str());
    TEST_ASSERT_EQUAL_STRING(
        "{\"idle\":\"clock\",\"running\":\"\",\"paused\":\"\",\"finished\":\"\"}",
        fixture::last_publish(fixture::TIMER_ICONS_TOPIC)->payload.c_str());
}

// ============================================================================
// W13 — the finished carrier's attribute bag on the wire (PRD #57, generalizing
// issue #51): publishAttributeGroup(Finished) puts the folded
// {realert_interval, finished_hold} object — live values, not defaults — on the
// select's json_attr_t topic via the wire seam. realert_interval is preserved
// (PRD #57: unchanged from the user's perspective). The expected topic is the
// fixture literal, spelled independently of the TimerHa builders.
// ============================================================================
void test_W13_publish_finished_group_folds_realert_and_hold(void) {
    TIMER_REALERT_INTERVAL = 42;   // non-default values, to prove they read live
    TIMER_FINISHED_HOLD    = 7;

    TimerManager.publishAttributeGroup(TimerHaEntity::Finished);

    const PublishCall *attr = fixture::last_publish(fixture::TIMER_FINISHED_ATTR_TOPIC);
    TEST_ASSERT_NOT_NULL(attr);
    TEST_ASSERT_EQUAL_STRING("{\"realert_interval\":42,\"finished_hold\":7}", attr->payload.c_str());
    // Exactly one publish, and only on the attributes topic (not the state topic).
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_FINISHED_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_FINISHED_TOPIC));
}

// ============================================================================
// W14 — editing a finished-carrier key republishes EXACTLY that carrier's bag
// (PRD #57, generalizing issue #52): a config command carrying realert_interval
// makes parseCommand republish the finished attribute object, reflecting the
// JUST-applied value (republish after apply, not before), and touches no other
// attribute topic. Same parseCommand-driven validate→apply→publish chain as W6.
// ============================================================================
void test_W14_realert_interval_change_republishes_finished_group(void) {
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"realert_interval\":42}")));

    const PublishCall *attr = fixture::last_publish(fixture::TIMER_FINISHED_ATTR_TOPIC);
    TEST_ASSERT_NOT_NULL(attr);
    TEST_ASSERT_EQUAL_STRING("{\"realert_interval\":42,\"finished_hold\":10}", attr->payload.c_str());
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_FINISHED_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_BUZZER_ATTR_TOPIC));
}

// ============================================================================
// W15 — the republish is keyed precisely on a mapped attribute key (PRD #57): a
// config command that edits only keys NOT projected as attributes (here a
// duration run-state edit) leaves BOTH carriers' attribute topics silent. Pins
// that the trigger is "a mapped key applied", not "any config change", so
// unrelated edits don't churn the retained attributes.
// ============================================================================
void test_W15_edit_without_attribute_key_republishes_nothing(void) {
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"duration\":120}")));

    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_FINISHED_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_BUZZER_ATTR_TOPIC));
}

// ============================================================================
// W16 — the buzzer carrier's attribute bag on the wire (PRD #57 / issue #58):
// publishAttributeGroup(Buzzer) puts {countdown_seconds, melody_tick, melody_end}
// — live values — on the buzzer select's json_attr_t topic, and only there.
// ============================================================================
void test_W16_publish_buzzer_group_emits_countdown_and_melodies(void) {
    TIMER_COUNTDOWN_SECONDS = 5;
    TIMER_MELODY_TICK       = "tk";
    TIMER_MELODY_END        = "nd";

    TimerManager.publishAttributeGroup(TimerHaEntity::Buzzer);

    const PublishCall *attr = fixture::last_publish(fixture::TIMER_BUZZER_ATTR_TOPIC);
    TEST_ASSERT_NOT_NULL(attr);
    TEST_ASSERT_EQUAL_STRING(
        "{\"countdown_seconds\":5,\"melody_tick\":\"tk\",\"melody_end\":\"nd\"}",
        attr->payload.c_str());
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_BUZZER_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_BUZZER_TOPIC));
}

// ============================================================================
// W17 — the full attribute refresh (PRD #57 / issue #58): publishAllAttributeGroups()
// publishes every distinct carrier's bag EXACTLY once on its json_attr_t topic.
// This is what the discovery-enable and reconnect paths call right after the wire
// refresh, so HA never sees an entity with missing attributes. "Each once" pins
// the carrier dedupe; the count pins that nothing else rides along.
// ============================================================================
void test_W17_publishAllAttributeGroups_each_carrier_once(void) {
    TimerManager.publishAllAttributeGroups();

    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_FINISHED_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_BUZZER_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_STATE_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_REMAINING_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_DURATION_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(5, (int)MQTTManager.recorded.size());   // exactly the five carriers
}

// ============================================================================
// W18 — editing a buzzer-carrier key republishes EXACTLY the buzzer bag (PRD
// #57 / issue #58): a config command carrying countdown_seconds (or a melody)
// republishes the buzzer attribute object and leaves the finished topic silent.
// Together with W14 this pins per-carrier isolation through parseCommand.
// ============================================================================
void test_W18_countdown_change_republishes_buzzer_group_only(void) {
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"countdown_seconds\":9}")));

    const PublishCall *attr = fixture::last_publish(fixture::TIMER_BUZZER_ATTR_TOPIC);
    TEST_ASSERT_NOT_NULL(attr);
    TEST_ASSERT_EQUAL_STRING(
        "{\"countdown_seconds\":9,\"melody_tick\":\"timer_tick\",\"melody_end\":\"timer_end\"}",
        attr->payload.c_str());
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_BUZZER_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_FINISHED_ATTR_TOPIC));
}

// ============================================================================
// W19 — the state sensor's attribute bag on the wire (PRD #57 / issue #59):
// publishAttributeGroup(State) puts the eight-key config view — live values,
// bar_color / bar_bg_color as the "#RRGGBB" human string — on the state sensor's
// json_attr_t topic, and only there. Proves the HASensor carrier rides the same
// wire seam. (app_config_timeout removed in PRD #83 / #88; bar_bg_color in ADR-0020.)
// ============================================================================
void test_W19_publish_state_group_emits_full_config_view(void) {
    TIMER_MAX_DURATION     = 7200;
    TIMER_PUBLISH_INTERVAL = 5;
    TIMER_ICON_ENABLED     = false;
    TIMER_BAR_ENABLED      = true;
    TIMER_BAR_COLOR        = 0xFF8800;
    TIMER_BAR_BG_COLOR     = 0x202020;
    TIMER_SYNC_FOLLOW      = true;
    TIMER_SYNC_TARGETS     = "all";

    TimerManager.publishAttributeGroup(TimerHaEntity::State);

    const PublishCall *attr = fixture::last_publish(fixture::TIMER_STATE_ATTR_TOPIC);
    TEST_ASSERT_NOT_NULL(attr);
    TEST_ASSERT_EQUAL_STRING(
        "{\"max_duration\":7200,\"remaining_publish_interval\":5,"
        "\"icon_enabled\":false,\"bar_enabled\":true,"
        "\"bar_color\":\"#FF8800\",\"bar_bg_color\":\"#202020\","
        "\"sync_follow\":true,\"sync_targets\":\"all\"}",
        attr->payload.c_str());
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_STATE_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_STATE_TOPIC));
}

// ============================================================================
// W20 — the remaining sensor's attribute bag on the wire (PRD #57 / issue #59):
// publishAttributeGroup(Remaining) puts {remaining_publish_interval} — that
// sensor's own cadence — on the remaining sensor's json_attr_t topic, and only there.
// ============================================================================
void test_W20_publish_remaining_group_emits_publish_interval(void) {
    TIMER_PUBLISH_INTERVAL = 9;

    TimerManager.publishAttributeGroup(TimerHaEntity::Remaining);

    const PublishCall *attr = fixture::last_publish(fixture::TIMER_REMAINING_ATTR_TOPIC);
    TEST_ASSERT_NOT_NULL(attr);
    TEST_ASSERT_EQUAL_STRING("{\"remaining_publish_interval\":9}", attr->payload.c_str());
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_REMAINING_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_REMAINING_TOPIC));
}

// ============================================================================
// W21 — editing remaining_publish_interval republishes BOTH carriers that map it
// (PRD #57 / issue #59): the key rides the remaining sensor (its cadence) and the
// state sensor (complete config view), so a config command carrying it makes
// parseCommand republish each carrier EXACTLY once, reflecting the just-applied
// value. Pins the multi-carrier fan-out of the table-driven republish loop.
// ============================================================================
void test_W21_publish_interval_change_republishes_both_carriers(void) {
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"remaining_publish_interval\":7}")));

    const PublishCall *rem = fixture::last_publish(fixture::TIMER_REMAINING_ATTR_TOPIC);
    const PublishCall *st  = fixture::last_publish(fixture::TIMER_STATE_ATTR_TOPIC);
    TEST_ASSERT_NOT_NULL(rem);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_EQUAL_STRING("{\"remaining_publish_interval\":7}", rem->payload.c_str());
    TEST_ASSERT_NOT_NULL(strstr(st->payload.c_str(), "\"remaining_publish_interval\":7"));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_REMAINING_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_STATE_ATTR_TOPIC));
    // No other carrier churns.
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_FINISHED_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_BUZZER_ATTR_TOPIC));
}

// ============================================================================
// W22 — editing bar_color republishes the state bag with the "#RRGGBB" human
// string (PRD #57 / issue #59): the formatter is on the wire path too, not just
// the bag builder. Proves the attribute representation differs from the raw-int
// config snapshot end-to-end through parseCommand.
// ============================================================================
void test_W22_bar_color_change_republishes_state_group_as_hex(void) {
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"bar_color\":\"#123ABC\"}")));

    const PublishCall *st = fixture::last_publish(fixture::TIMER_STATE_ATTR_TOPIC);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_NOT_NULL(strstr(st->payload.c_str(), "\"bar_color\":\"#123ABC\""));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_STATE_ATTR_TOPIC));
}

// ============================================================================
// W23 — sync_follow / sync_targets are read-only state-sensor attributes that are
// STILL never propagated to peers (PRD #57 / issue #59, preserving ADR-0006):
// editing sync_follow republishes the state attribute bag (so HA stays current)
// but, being inSnapshot=false local identity, broadcasts NOTHING to peers.
// ============================================================================
void test_W23_sync_follow_edit_republishes_state_but_does_not_propagate(void) {
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"sync_follow\":true}")));

    const PublishCall *st = fixture::last_publish(fixture::TIMER_STATE_ATTR_TOPIC);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_NOT_NULL(strstr(st->payload.c_str(), "\"sync_follow\":true"));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_STATE_ATTR_TOPIC));
    // Local identity: no config (or any) packet goes out to peers.
    TEST_ASSERT_EQUAL_INT(0, fixture::sync_packet_count());
}

// ============================================================================
// W24 — teardown clears each carrier's retained attribute object (issue #60):
// clearAllAttributeGroups() puts an EMPTY (retained) payload on every distinct
// carrier's json_attr_t topic, exactly once, so disabling the Timer leaves no
// orphaned attribute payload behind the pruned discovery config. The mirror of
// publishAllAttributeGroups (W17): the same carriers, an empty payload instead
// of a bag. removeTimerHAEntities() rides this through the wire seam alongside
// the discovery-config teardown (device-only; the seam is asserted here).
// ============================================================================
void test_W24_clearAllAttributeGroups_empties_each_carrier(void) {
    TimerManager.clearAllAttributeGroups();

    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_FINISHED_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_BUZZER_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_STATE_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_REMAINING_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_DURATION_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(5, (int)MQTTManager.recorded.size());   // exactly the five carriers

    // Empty payload == a retained clear (the broker drops the retained object).
    const PublishCall *st = fixture::last_publish(fixture::TIMER_STATE_ATTR_TOPIC);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_EQUAL_STRING("", st->payload.c_str());
}

// ============================================================================
// W25 — re-enabling after teardown leaves no empty/stale attribute (issue #60,
// acceptance criterion 3): a clear (the SHOW_TIMER true->false teardown) followed
// by the existing refresh path (publishAllAttributeGroups, what enableTimerHA-
// Discovery / reconnect run) overwrites each carrier's json_attr_t with a
// populated bag, so the LAST retained value a late HA subscriber reads is the live
// config, never the empty clear.
// ============================================================================
void test_W25_refresh_after_clear_repopulates_attributes(void) {
    TimerManager.clearAllAttributeGroups();      // SHOW_TIMER true->false teardown
    TimerManager.publishAllAttributeGroups();    // SHOW_TIMER false->true refresh

    const PublishCall *st = fixture::last_publish(fixture::TIMER_STATE_ATTR_TOPIC);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_NOT_NULL(strstr(st->payload.c_str(), "\"max_duration\""));
    const PublishCall *fin = fixture::last_publish(fixture::TIMER_FINISHED_ATTR_TOPIC);
    TEST_ASSERT_NOT_NULL(fin);
    TEST_ASSERT_NOT_NULL(strstr(fin->payload.c_str(), "\"realert_interval\""));
    // The Duration carrier (PRD #66 / issue #68) repopulates too — its last
    // retained value is the live cap in clock form, never the empty clear.
    const PublishCall *dur = fixture::last_publish(fixture::TIMER_DURATION_ATTR_TOPIC);
    TEST_ASSERT_NOT_NULL(dur);
    TEST_ASSERT_NOT_NULL(strstr(dur->payload.c_str(), "\"max_duration\""));
}

// ============================================================================
// W26 — editing max_duration republishes BOTH carriers that map it, EACH IN ITS
// OWN REPRESENTATION (PRD #66 / issue #68): max_duration rides the state sensor
// (raw seconds, a JSON number) AND the Duration text entity (the formatHMS clock
// string, a JSON string), so a config command carrying it makes parseCommand
// republish each carrier exactly once, in its carrier-native form. This is the
// single executable guard for the per-carrier representation divergence end-to-
// end through the command path: a refactor that collapsed the Duration form back
// to raw seconds, or dropped either carrier from the fan-out, fails here. Analog
// of W21 (multi-carrier fan-out) and W22 (formatter on the wire path).
void test_W26_max_duration_change_republishes_state_and_duration(void) {
    TIMER_MAX_DURATION = 3600;   // start off-target so the edit is a genuine change

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.parseCommand("{\"max_duration\":86400}")));

    const PublishCall *st  = fixture::last_publish(fixture::TIMER_STATE_ATTR_TOPIC);
    const PublishCall *dur = fixture::last_publish(fixture::TIMER_DURATION_ATTR_TOPIC);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_NOT_NULL(dur);
    // State carrier: raw seconds, a JSON number (unchanged from today's T15/W19).
    TEST_ASSERT_NOT_NULL(strstr(st->payload.c_str(), "\"max_duration\":86400"));
    // Duration carrier: the clock string, a JSON object {"max_duration":"24:00:00"}.
    TEST_ASSERT_EQUAL_STRING("{\"max_duration\":\"24:00:00\"}", dur->payload.c_str());
    // Each mapped carrier republished EXACTLY once.
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_STATE_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(1, fixture::count_publish(fixture::TIMER_DURATION_ATTR_TOPIC));
    // No unrelated carrier churns.
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_FINISHED_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_BUZZER_ATTR_TOPIC));
    TEST_ASSERT_EQUAL_INT(0, fixture::count_publish(fixture::TIMER_REMAINING_ATTR_TOPIC));
}

// ============================================================================
// HA1..HA6 — the display-free timerHaApply(entity, rawValue) adapter routes each
// HA timer callback through parseCommand (issue #109). The adapter builds the
// minimal JSON command its entity represents and hands it to parseCommand, so HA
// edits get the same atomic-reject validation, the same propagation, and the same
// codec strings as the MQTT/HTTP control surface — no deep setters, no duplicated
// parse logic in the HA layer.
// ============================================================================

// HA1 — the buzzer select routes through parseCommand: the selected option index
// maps through the per-enum codec (ADR-0010) to the canonical wire string, so the
// applied mode matches the equivalent {"buzzer":"countdown"} command exactly.
void test_HA1_buzzer_select_routes_through_parsecommand(void) {
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::Buzzer,
            String((int)BuzzerMode::Countdown))));
    TEST_ASSERT_EQUAL(static_cast<int>(BuzzerMode::Countdown),
                      static_cast<int>(TimerManager.getBuzzerMode()));

    // Parity: same observable result as the equivalent parseCommand JSON.
    TimerManager.parseCommand("{\"buzzer\":\"end\"}");
    TEST_ASSERT_EQUAL(static_cast<int>(BuzzerMode::End),
                      static_cast<int>(TimerManager.getBuzzerMode()));
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::Buzzer,
            String((int)BuzzerMode::Countdown))));
    TEST_ASSERT_EQUAL(static_cast<int>(BuzzerMode::Countdown),
                      static_cast<int>(TimerManager.getBuzzerMode()));
}

// HA2 — the finished select routes through parseCommand via the codec wire string.
void test_HA2_finished_select_routes_through_parsecommand(void) {
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::Finished,
            String((int)FinishedMode::ReAlert))));
    TEST_ASSERT_EQUAL(static_cast<int>(FinishedMode::ReAlert),
                      static_cast<int>(TimerManager.getFinishedMode()));
}

// HA3 — the duration text routes its raw HH:MM:SS string through parseCommand,
// which owns the parse/validate (parseHMS + range): a valid clock string applies.
void test_HA3_duration_text_routes_through_parsecommand(void) {
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::Duration, "0:02:30")));
    TEST_ASSERT_EQUAL_UINT32(150, TimerManager.getDuration());
}

// HA4 — an invalid duration is rejected wholesale (atomic-reject parity): the
// adapter returns a non-Ok result and the live value is unchanged, so the HA field
// snaps back to the last valid value. Covers malformed AND out-of-range.
void test_HA4_invalid_duration_rejected_and_snaps_back(void) {
    TimerManager.setDuration(300);   // last valid value

    // Malformed clock string -> BadField, nothing applied.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::Duration, "not a time")));
    TEST_ASSERT_EQUAL_UINT32(300, TimerManager.getDuration());

    // Out-of-range (> 24h cap) -> BadField, nothing applied (reject, never clamp).
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::Duration, "30:00:00")));
    TEST_ASSERT_EQUAL_UINT32(300, TimerManager.getDuration());

    // The canonical live value to echo back is the unchanged 300 ("5:00").
    TEST_ASSERT_EQUAL_STRING("5:00", TimerManager_::formatHMS(TimerManager.getDuration()).c_str());
}

// HA5 — the Start/Pause/Reset buttons route through parseCommand action commands,
// producing the same run-state transitions as the equivalent JSON.
void test_HA5_buttons_route_through_parsecommand(void) {
    TimerManager.setDuration(300);

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::Start, "")));
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Running),
                      static_cast<int>(TimerManager.getState()));

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::Pause, "")));
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Paused),
                      static_cast<int>(TimerManager.getState()));

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::Reset, "")));
    TEST_ASSERT_EQUAL(static_cast<int>(TimerState::Idle),
                      static_cast<int>(TimerManager.getState()));
}

// HA6 — HA Start/Pause/Reset propagate run-state to peers exactly like the MQTT
// surface: routing through parseCommand fires broadcastRunState. A Start emits the
// combined run-scoped packet (action+duration), a Pause emits a run-state-only
// packet. This is the propagation parity the issue calls for.
void test_HA6_buttons_propagate_run_state_to_peers(void) {
    SHOW_TIMER = true;
    TIMER_SYNC_TARGETS = "all";   // leader: relays its own local actions
    TimerManager.setDuration(180);

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::Start, "")));
    TEST_ASSERT_EQUAL_INT(1, fixture::sync_packet_count());

    DynamicJsonDocument doc(2048);
    TEST_ASSERT_FALSE(deserializeJson(doc, fixture::last_sync_payload()));
    TEST_ASSERT_EQUAL_STRING("start", doc["action"]);
    TEST_ASSERT_EQUAL_UINT32(180, doc["duration"].as<uint32_t>());

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::Pause, "")));
    TEST_ASSERT_EQUAL_INT(2, fixture::sync_packet_count());
    DynamicJsonDocument doc2(512);
    TEST_ASSERT_FALSE(deserializeJson(doc2, fixture::last_sync_payload()));
    TEST_ASSERT_EQUAL_STRING("pause", doc2["action"]);
}

// ============================================================================
// HA7..HA11 — the two writable sync-control entities (issue #110). The Follow
// switch (sync_follow) and the static Off/All Targets select (sync_targets) both
// route through the timerHaApply(entity, rawValue) adapter into parseCommand, so
// they inherit the same atomic-reject validation and NVS persistence as the
// {prefix}/timer surface. sync_* are local identity (inSnapshot=false) and must
// NEVER propagate to peers.
// ============================================================================

// HA7 — the Follow switch routes a bool through parseCommand: on -> sync_follow
// true, off -> false, each persisting to NVS (the table half saved by parseCommand).
void test_HA7_sync_follow_switch_routes_through_parsecommand(void) {
    TIMER_SYNC_FOLLOW = false;
    saveSettings_calls = 0;

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::SyncFollow, "1")));
    TEST_ASSERT_TRUE(TIMER_SYNC_FOLLOW);
    // Persists: sync_follow is a table-backed ("awtrix") key, so a real change
    // flushes that namespace exactly once (saveSettings).
    TEST_ASSERT_EQUAL_INT(1, saveSettings_calls);

    saveSettings_calls = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::SyncFollow, "0")));
    TEST_ASSERT_FALSE(TIMER_SYNC_FOLLOW);
    TEST_ASSERT_EQUAL_INT(1, saveSettings_calls);
}

// HA8 — the Targets select routes its RESOLVED value through parseCommand (#112):
// MQTTManager maps the option index to a value (Off -> "", All -> "all", a peer
// option -> its id) and hands timerHaApply that value, which persists to NVS.
void test_HA8_sync_targets_select_routes_through_parsecommand(void) {
    TIMER_SYNC_TARGETS = "";
    saveSettings_calls = 0;

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::SyncTargets, "all")));
    TEST_ASSERT_EQUAL_STRING("all", TIMER_SYNC_TARGETS.c_str());
    // Persists: sync_targets is a table-backed key, flushed once on the change.
    TEST_ASSERT_EQUAL_INT(1, saveSettings_calls);

    saveSettings_calls = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::SyncTargets, "")));
    TEST_ASSERT_EQUAL_STRING("", TIMER_SYNC_TARGETS.c_str());
    TEST_ASSERT_EQUAL_INT(1, saveSettings_calls);

    // A discovered peer id is a first-class value now: it applies and persists.
    saveSettings_calls = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::SyncTargets, "awtrix_peer")));
    TEST_ASSERT_EQUAL_STRING("awtrix_peer", TIMER_SYNC_TARGETS.c_str());
    TEST_ASSERT_EQUAL_INT(1, saveSettings_calls);
}

// HA9 — the select-reflection helper maps the current sync_targets value to its
// option index: "" -> Off (0), "all" -> All (1), and a specific-ID CSV (set
// out-of-band via API/dev.json) -> -1 (unknown), so the select shows blank while
// the read-only attribute stays authoritative for the exact value.
void test_HA9_sync_targets_select_index_reflects_value(void) {
    TEST_ASSERT_EQUAL_INT(0,  timerSyncTargetsSelectIndex(""));
    TEST_ASSERT_EQUAL_INT(1,  timerSyncTargetsSelectIndex("all"));
    TEST_ASSERT_EQUAL_INT(-1, timerSyncTargetsSelectIndex("awtrix_ab12,awtrix_cd34"));
    TEST_ASSERT_EQUAL_INT(-1, timerSyncTargetsSelectIndex("awtrix_peer"));
}

// HA10 — an invalid Targets write is rejected wholesale (atomic-reject parity):
// a malformed sync_targets value returns a non-Ok result and changes nothing, so the
// select snaps back to the value actually applied. The bespoke parseSyncTargets
// validator (token charset/length) is the gate now that the value is passed directly.
void test_HA10_invalid_sync_targets_write_rejected(void) {
    TIMER_SYNC_TARGETS = "all";   // last applied value

    // A value with an illegal character is rejected -> BadField, nothing applied.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::SyncTargets, "bad id!")));
    TEST_ASSERT_EQUAL_STRING("all", TIMER_SYNC_TARGETS.c_str());
    // An over-long token (> 32 chars) is rejected too.
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::BadField),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::SyncTargets,
            "awtrix_0123456789012345678901234567890")));
    TEST_ASSERT_EQUAL_STRING("all", TIMER_SYNC_TARGETS.c_str());
    // The applied value still reflects All in the select.
    TEST_ASSERT_EQUAL_INT(1, timerSyncTargetsSelectIndex(TIMER_SYNC_TARGETS.c_str()));
}

// HA11 — applying sync_follow / sync_targets through the HA adapter NEVER
// propagates to peers (local identity, inSnapshot=false): no sync packet goes out.
void test_HA11_sync_control_writes_do_not_propagate(void) {
    SHOW_TIMER = true;
    TIMER_SYNC_FOLLOW = true;
    TIMER_SYNC_TARGETS = "all";   // this clock would relay its own local edits

    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::SyncFollow, "0")));
    TEST_ASSERT_EQUAL(static_cast<int>(TimerCmdResult::Ok),
        static_cast<int>(TimerManager.timerHaApply(TimerHaEntity::SyncTargets, "")));
    TEST_ASSERT_EQUAL_INT(0, fixture::sync_packet_count());
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
    RUN_TEST(test_U55_parseCommand_rejected_payload_persists_nothing);
    RUN_TEST(test_U56_parseCommand_noop_payload_does_not_write_nvs);
    RUN_TEST(test_U57_parseCommand_multikey_stores_correct_values);
    RUN_TEST(test_U58_parseCommand_noop_table_value_skips_awtrix_write);
    RUN_TEST(test_U59_parseCommand_mixed_payload_one_flush_per_namespace);
    RUN_TEST(test_OS1_save_false_duration_reverts_on_reset);
    RUN_TEST(test_OS2_save_false_table_key_reverts_on_reset);
    RUN_TEST(test_OS9_save_false_member_key_reverts_on_reset);
    RUN_TEST(test_OS3_save_false_writes_no_nvs);
    RUN_TEST(test_OS4_autoclear_reverts_override);
    RUN_TEST(test_OS5_normal_command_mid_override_rebaselines);
    RUN_TEST(test_OS6_non_boolean_save_atomic_reject);
    RUN_TEST(test_OS7_save_false_action_duration_harmless);
    RUN_TEST(test_OS8_config_edit_never_broadcasts);
    RUN_TEST(test_HC1_get_config_mirror_saved_during_override);
    RUN_TEST(test_HC2_ha_attribute_bag_saved_during_override);
    RUN_TEST(test_HC3_oneshot_start_propagates_effective_config);
    RUN_TEST(test_IM1_rtttl_classifier_and_validator);
    RUN_TEST(test_IM2_inline_melody_end_plays_reverts_no_persist);
    RUN_TEST(test_IM3_inline_melody_tick_plays_no_persist);
    RUN_TEST(test_IM4_bare_melody_name_still_persists);
    RUN_TEST(test_IM5_inline_never_in_config_mirror);
    RUN_TEST(test_IM6_malformed_inline_atomic_reject);
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
    RUN_TEST(test_U53_tick_runs_runstate_when_not_in_config);
    RUN_TEST(test_U32_descriptor_table_well_formed);
    RUN_TEST(test_U33_descriptor_ids_unique);
    RUN_TEST(test_U34_descriptor_type_specific_fields);
    RUN_TEST(test_U35_select_options_match_enums);
    RUN_TEST(test_U54_timer_ha_ids_create_teardown_symmetric);
    RUN_TEST(test_U60_ha_registration_off_by_one);
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
    RUN_TEST(test_U60_getStateJson_has_nested_config_object);
    RUN_TEST(test_U61_getStateJson_config_boundary_placement);
    RUN_TEST(test_S1_sync_settings_validation_atomic_reject);
    RUN_TEST(test_S2_local_start_emits_combined_packet);
    RUN_TEST(test_S3_local_config_edit_emits_nothing);
    RUN_TEST(test_S4_applySyncCommand_gating);
    RUN_TEST(test_S5_remote_apply_does_not_rebroadcast);
    RUN_TEST(test_S6_sync_off_never_broadcasts);
    RUN_TEST(test_S7_remote_config_is_oneshot_attribute_bag_stays_saved);
    RUN_TEST(test_SR1_pause_reset_propagate_runstate_only);
    RUN_TEST(test_SR2_bare_duration_edit_propagates_duration_only);
    RUN_TEST(test_SR3_follower_applies_oneshot_and_reverts);
    RUN_TEST(test_SR4_follower_forces_oneshot_regardless_of_save);
    RUN_TEST(test_PP1_presence_harvest_ungated_no_timer_state);
    RUN_TEST(test_PP2_presence_never_treated_as_command);
    RUN_TEST(test_PP3_registry_excludes_self_and_is_bounded);
    RUN_TEST(test_PP4_peers_age_out_past_ttl);
    RUN_TEST(test_PP5_beacon_periodic_not_in_ap_mode);
    RUN_TEST(test_DT1_build_options_empty_registry);
    RUN_TEST(test_DT2_build_options_with_sorted_ids);
    RUN_TEST(test_DT3_forward_map_off_all);
    RUN_TEST(test_DT4_forward_map_present_absent_csv);
    RUN_TEST(test_DT5_reverse_map_index_to_value);
    RUN_TEST(test_DT6_peer_ids_sorted);
    RUN_TEST(test_T1_table_uintrange_boundaries);
    RUN_TEST(test_T2_table_strict_types);
    RUN_TEST(test_T3_table_bespoke_validators);
    RUN_TEST(test_T4_table_nvs_roundtrip);
    RUN_TEST(test_T5_devjson_best_effort);
    RUN_TEST(test_T6_snapshot_excludes_local_identity);
    RUN_TEST(test_T7_member_config_table_well_formed);
    RUN_TEST(test_T8_member_config_validate_and_snapshot_roundtrip);
    RUN_TEST(test_M1_slot_table_well_formed);
    RUN_TEST(test_M10_unresolved_cmdkey_guard_fires_and_names_slot);
    RUN_TEST(test_M2_enum_cycle_wraps_and_routes_via_setter);
    RUN_TEST(test_M3_stepped_range_saturates);
    RUN_TEST(test_M4_bool_toggle_flips_both_directions);
    RUN_TEST(test_M5_stepped_clamps_to_descriptor_bounds);
    RUN_TEST(test_M6_name_and_bare_value);
    RUN_TEST(test_M7_enum_adjust_defers_persist_until_commit);
    RUN_TEST(test_M8_enum_labels_source_from_codec);
    RUN_TEST(test_M9_menu_commit_one_persistbatch_flushes_both_namespaces);
    RUN_TEST(test_M11_menu_commit_republishes_all_attribute_groups);
    RUN_TEST(test_N1_list_navigation_wraps);
    RUN_TEST(test_N2_select_value_row_enters_leaf);
    RUN_TEST(test_N3_leaf_navigate_adjusts_value);
    RUN_TEST(test_N4_leaf_select_confirms_back_to_list);
    RUN_TEST(test_N5_leaf_back_returns_to_list);
    RUN_TEST(test_N6_select_main_goes_to_main_menu);
    RUN_TEST(test_N7_list_back_is_context_aware);
    RUN_TEST(test_N8_enter_resets_to_list_top);
    RUN_TEST(test_N9_duration_leaf_kind_gated_by_state);
    RUN_TEST(test_N10_duration_editable_leaf_inputs);
    RUN_TEST(test_N11_duration_readonly_leaf_inputs);
    RUN_TEST(test_T9_codec_tables_well_formed);
    RUN_TEST(test_T10_codec_roundtrip_aliases_and_case);
    RUN_TEST(test_T11_setting_emit_value_by_type);
    RUN_TEST(test_T12_attribute_group_finished_bag);
    RUN_TEST(test_T13_attribute_group_buzzer_bag);
    RUN_TEST(test_T14_attribute_group_table_well_formed);
    RUN_TEST(test_T15_attribute_group_state_bag);
    RUN_TEST(test_T18_attribute_group_duration_bag);
    RUN_TEST(test_T16_attribute_group_remaining_bag);
    RUN_TEST(test_T17_bar_color_formatter_renders_default_or_hex);
    RUN_TEST(test_T17b_bar_bg_color_formatter_renders_none_or_hex);
    RUN_TEST(test_T19_full_config_mirrors_every_persisted_key);
    RUN_TEST(test_T20_full_config_carrier_native_renderings);
    RUN_TEST(test_T21_get_config_value_spotchecks_via_parsecommand);
    RUN_TEST(test_CE1_enter_decomposes_and_activates);
    RUN_TEST(test_CE2_adjust_default_cap_wraps_HH_at_23);
    RUN_TEST(test_CE3_adjust_tight_cap_recomputes_per_field);
    RUN_TEST(test_CE4_adjust_no_cap_when_max_is_zero);
    RUN_TEST(test_CE5_adjust_decrement_wraps_to_dynamic_max);
    RUN_TEST(test_CE6_cycleField_rotates);
    RUN_TEST(test_CE7_exit_recomposes_and_deactivates);
    RUN_TEST(test_CE9_held_button_autorepeats_at_cadence);
    RUN_TEST(test_CE10_left_decrements_and_release_rewaits);
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
    RUN_TEST(test_W12_publishAllWire_each_artifact_exactly_once);
    RUN_TEST(test_W13_publish_finished_group_folds_realert_and_hold);
    RUN_TEST(test_W14_realert_interval_change_republishes_finished_group);
    RUN_TEST(test_W15_edit_without_attribute_key_republishes_nothing);
    RUN_TEST(test_W16_publish_buzzer_group_emits_countdown_and_melodies);
    RUN_TEST(test_W17_publishAllAttributeGroups_each_carrier_once);
    RUN_TEST(test_W18_countdown_change_republishes_buzzer_group_only);
    RUN_TEST(test_W19_publish_state_group_emits_full_config_view);
    RUN_TEST(test_W20_publish_remaining_group_emits_publish_interval);
    RUN_TEST(test_W21_publish_interval_change_republishes_both_carriers);
    RUN_TEST(test_W22_bar_color_change_republishes_state_group_as_hex);
    RUN_TEST(test_W23_sync_follow_edit_republishes_state_but_does_not_propagate);
    RUN_TEST(test_W24_clearAllAttributeGroups_empties_each_carrier);
    RUN_TEST(test_W25_refresh_after_clear_repopulates_attributes);
    RUN_TEST(test_W26_max_duration_change_republishes_state_and_duration);
    RUN_TEST(test_HA1_buzzer_select_routes_through_parsecommand);
    RUN_TEST(test_HA2_finished_select_routes_through_parsecommand);
    RUN_TEST(test_HA3_duration_text_routes_through_parsecommand);
    RUN_TEST(test_HA4_invalid_duration_rejected_and_snaps_back);
    RUN_TEST(test_HA5_buttons_route_through_parsecommand);
    RUN_TEST(test_HA6_buttons_propagate_run_state_to_peers);
    RUN_TEST(test_HA7_sync_follow_switch_routes_through_parsecommand);
    RUN_TEST(test_HA8_sync_targets_select_routes_through_parsecommand);
    RUN_TEST(test_HA9_sync_targets_select_index_reflects_value);
    RUN_TEST(test_HA10_invalid_sync_targets_write_rejected);
    RUN_TEST(test_HA11_sync_control_writes_do_not_propagate);
    return UNITY_END();
}
