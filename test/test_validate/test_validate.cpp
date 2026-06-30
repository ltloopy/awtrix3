// Unit tests for TimerCommand::classify (#143) -- the PURE atomic-reject command
// validator (ADR-0001), extracted from TimerManager::parseCommand and mirroring the
// SyncEnvelope sibling (ADR-0023).
//
// The whole point of the extraction: these exercise the validation DECISION directly,
// as a pure mutate-nothing pass over the packet + a Context -- no TimerManager
// singleton, no TIMER_MAX_DURATION / _remoteApply globals (the Context carries them),
// no MQTT/Preferences stubs. classify links only the dependency-light descriptor-table
// family ([env:native_validate]). Contrast the same cases in test_timer, which stand up
// the full 4,700-line fixture.

#include <unity.h>
#include <ArduinoFake.h>
#include <ArduinoJson.h>

#include "../../src/TimerCommand.h"

using TimerCommand::Context;
using TimerCommand::Plan;
using TimerCommand::Action;

void setUp(void) {}
void tearDown(void) {}

// Default ceiling for tests that don't exercise the max_duration cross-field.
static constexpr uint32_t kSavedMax = 86400;

static Plan classifyJson(const char *json, uint32_t savedMax = kSavedMax, bool remoteApply = false) {
    StaticJsonDocument<1024> doc;
    deserializeJson(doc, json);
    return TimerCommand::classify(doc.as<JsonObjectConst>(), Context{savedMax, remoteApply});
}

// Row index of a settings/member key, so assertions don't hardcode table order.
static int tableIndex(const char *cmdKey) {
    for (size_t i = 0; i < TIMER_SETTINGS_DESC_COUNT; ++i)
        if (strcmp(TIMER_SETTINGS_DESCS[i].cmdKey, cmdKey) == 0) return (int)i;
    return -1;
}
static int memberIndex(const char *cmdKey) {
    for (size_t i = 0; i < TIMER_MEMBER_VALIDATOR_COUNT; ++i)
        if (strcmp(TIMER_MEMBER_VALIDATORS[i].cmdKey, cmdKey) == 0) return (int)i;
    return -1;
}

// ============================================================================
// V1 -- a fully valid config command classifies ok, staging values + presence.
// ============================================================================
void test_V1_valid_config_ok(void) {
    Plan p = classifyJson("{\"finished_hold\":30,\"buzzer\":\"end\"}");
    TEST_ASSERT_TRUE(p.ok);
    int fh = tableIndex("finished_hold");
    TEST_ASSERT_TRUE(p.tablePresent[fh]);
    TEST_ASSERT_EQUAL_UINT32(30, p.tableStaged[fh].num);
    int bz = memberIndex("buzzer");
    TEST_ASSERT_TRUE(p.memberPresent[bz]);
}

// ============================================================================
// V2 -- the first invalid TABLE field rejects the whole command (atomic).
// finished_hold range is 1..300; 9999 is out of range.
// ============================================================================
void test_V2_bad_table_field_rejected(void) {
    Plan p = classifyJson("{\"finished_hold\":9999,\"buzzer\":\"end\"}");
    TEST_ASSERT_FALSE(p.ok);
}

// ============================================================================
// V3 -- an invalid action verb rejects the whole command.
// ============================================================================
void test_V3_bad_action_rejected(void) {
    Plan p = classifyJson("{\"action\":\"frobnicate\"}");
    TEST_ASSERT_FALSE(p.ok);
}

// ============================================================================
// V4 -- an invalid MEMBER field (unknown buzzer mode) rejects atomically.
// ============================================================================
void test_V4_bad_member_field_rejected(void) {
    Plan p = classifyJson("{\"buzzer\":\"nonsense\"}");
    TEST_ASSERT_FALSE(p.ok);
}

// ============================================================================
// V5 -- the ADR-0001 addendum at the classify level: a command that raises
// max_duration AND sets a duration above the OLD ceiling but within the NEW one is
// accepted, with durationSec carrying the new value (the shell's apply ordering then
// makes it stick -- regression-tested in test_timer).
// ============================================================================
void test_V5_raise_max_then_in_range_duration_ok(void) {
    // old ceiling 300; raise to 7200; set duration 3600 (was out of range, now in).
    Plan p = classifyJson("{\"max_duration\":7200,\"duration\":3600}", /*savedMax=*/300);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_TRUE(p.haveDuration);
    TEST_ASSERT_EQUAL_UINT32(3600, p.durationSec);
}

// ============================================================================
// V6 -- a duration above the effective ceiling (no raise) is rejected, not clamped.
// ============================================================================
void test_V6_duration_over_ceiling_rejected(void) {
    Plan p = classifyJson("{\"duration\":3600}", /*savedMax=*/300);
    TEST_ASSERT_FALSE(p.ok);
}

// ============================================================================
// V7 -- save:false makes the command one-shot; save:true (default) does not.
// ============================================================================
void test_V7_save_flag_drives_oneshot(void) {
    Plan off = classifyJson("{\"duration\":60,\"save\":false}");
    TEST_ASSERT_TRUE(off.ok);
    TEST_ASSERT_TRUE(off.oneShot);

    Plan on = classifyJson("{\"duration\":60,\"save\":true}");
    TEST_ASSERT_TRUE(on.ok);
    TEST_ASSERT_FALSE(on.oneShot);

    Plan dflt = classifyJson("{\"duration\":60}");
    TEST_ASSERT_FALSE(dflt.oneShot);
}

// ============================================================================
// V8 -- a remote-applied command is ALWAYS one-shot, even with save:true (ADR-0018):
// a follower mirrors but never persists. classify reads remoteApply from the Context.
// ============================================================================
void test_V8_remote_apply_forces_oneshot(void) {
    Plan p = classifyJson("{\"action\":\"start\",\"save\":true}", kSavedMax, /*remoteApply=*/true);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_TRUE(p.oneShot);
}

// ============================================================================
// V9 -- an inline RTTTL melody is staged out of the bare-name table (tablePresent
// cleared), surfaced via inline*, and forces the command one-shot (#102).
// ============================================================================
void test_V9_inline_melody_staged_and_oneshot(void) {
    Plan p = classifyJson("{\"melody_end\":\"beep:d=4,o=5,b=120:c\"}");
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_TRUE(p.haveInlineEnd);
    TEST_ASSERT_EQUAL_STRING("beep:d=4,o=5,b=120:c", p.inlineEnd.c_str());
    TEST_ASSERT_FALSE(p.tablePresent[tableIndex("melody_end")]);   // excluded from bare-name store
    TEST_ASSERT_TRUE(p.oneShot);
}

// ============================================================================
// V10 -- a malformed inline melody (only the colon discriminator, no valid RTTTL
// structure) rejects the whole command (BadField, atomic-reject).
// ============================================================================
void test_V10_bad_inline_melody_rejected(void) {
    Plan p = classifyJson("{\"melody_end\":\"a:b\"}");   // two sections, no d=/o=/b= token, missing notes split
    TEST_ASSERT_FALSE(p.ok);
}

// ============================================================================
// V11 -- the action verb is parsed into the Plan (no re-parse in apply).
// ============================================================================
void test_V11_action_parsed(void) {
    TEST_ASSERT_EQUAL(Action::Start, classifyJson("{\"action\":\"start\"}").action);
    TEST_ASSERT_EQUAL(Action::Pause, classifyJson("{\"action\":\"PAUSE\"}").action);   // case-insensitive
    TEST_ASSERT_EQUAL(Action::Reset, classifyJson("{\"action\":\"reset\"}").action);
    TEST_ASSERT_EQUAL(Action::None,  classifyJson("{\"duration\":60}").action);
}

// ============================================================================
// V12 -- configInCommand: true for a config-block table key (finished_hold) and for a
// member key (buzzer); FALSE for a pure run-state command and for sync-only keys
// (sync_follow is inSnapshot=false, local identity).
// ============================================================================
void test_V12_config_in_command(void) {
    TEST_ASSERT_TRUE(classifyJson("{\"finished_hold\":30}").configInCommand);
    TEST_ASSERT_TRUE(classifyJson("{\"buzzer\":\"end\"}").configInCommand);
    TEST_ASSERT_FALSE(classifyJson("{\"action\":\"start\"}").configInCommand);
    TEST_ASSERT_FALSE(classifyJson("{\"duration\":60}").configInCommand);
    TEST_ASSERT_FALSE(classifyJson("{\"sync_follow\":true}").configInCommand);
}

// ============================================================================
// V13 -- attrCarrierDirty: finished_hold maps to the Finished HA carrier.
// ============================================================================
void test_V13_attr_carrier_dirty(void) {
    Plan p = classifyJson("{\"finished_hold\":30}");
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_TRUE(p.attrCarrierDirty[(size_t)TimerHaEntity::Finished]);
    TEST_ASSERT_FALSE(p.attrCarrierDirty[(size_t)TimerHaEntity::Buzzer]);
}

// ============================================================================
// V14 -- a non-boolean save flag rejects the whole command (atomic-reject).
// ============================================================================
void test_V14_non_bool_save_rejected(void) {
    Plan p = classifyJson("{\"duration\":60,\"save\":\"yes\"}");
    TEST_ASSERT_FALSE(p.ok);
}

// ============================================================================
// V15 -- a duration HH:MM:SS string is parsed to seconds (the MQTT/HA string path).
// ============================================================================
void test_V15_duration_string_parsed(void) {
    Plan p = classifyJson("{\"duration\":\"00:05:00\"}");
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL_UINT32(300, p.durationSec);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_V1_valid_config_ok);
    RUN_TEST(test_V2_bad_table_field_rejected);
    RUN_TEST(test_V3_bad_action_rejected);
    RUN_TEST(test_V4_bad_member_field_rejected);
    RUN_TEST(test_V5_raise_max_then_in_range_duration_ok);
    RUN_TEST(test_V6_duration_over_ceiling_rejected);
    RUN_TEST(test_V7_save_flag_drives_oneshot);
    RUN_TEST(test_V8_remote_apply_forces_oneshot);
    RUN_TEST(test_V9_inline_melody_staged_and_oneshot);
    RUN_TEST(test_V10_bad_inline_melody_rejected);
    RUN_TEST(test_V11_action_parsed);
    RUN_TEST(test_V12_config_in_command);
    RUN_TEST(test_V13_attr_carrier_dirty);
    RUN_TEST(test_V14_non_bool_save_rejected);
    RUN_TEST(test_V15_duration_string_parsed);
    return UNITY_END();
}
