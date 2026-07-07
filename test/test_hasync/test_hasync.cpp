// Host suite for the discovery payloads of the two writable sync-control entities
// added in issue #110: the Follow switch (HASwitch, sync_follow) and the static
// Off/All Targets select (HASelect, sync_targets). Uses the vendored ArduinoHA
// library's own ARDUINOHA_TEST harness (PubSubClientMock + HAMqtt + HADevice) to
// drive a real connect and assert the EXACT discovery payload the broker would see
// for each entity. This is the native_ha coverage AC5 calls for — the switch is the
// Timer's first HASwitch, so its discovery config has never been pinned before.
//
// The harness objects are intentionally leaked (heap-allocated, never deleted):
// PubSubClientMock's teardown frees its realloc'd message array with `delete`,
// which the Windows UCRT heap flags as corruption. The processes are one-shot, so
// leaking the few-object harness keeps the vendored mock untouched (see the
// matching note in test_haselect).
#include <unity.h>
#include <ArduinoFake.h>
#include <ArduinoHA.h>
#include <cstring>

using namespace fakeit;

// The Follow switch's discovery payload: a command-driven on/off entity with name
// + icon, riding its own state + command topics. No retain/optimistic/device-class
// (none are set), no json_attr (the switch is a control, not an attribute carrier).
static const char *EXPECTED_SWITCH_CONFIG =
    "{\"name\":\"Timer sync follow\",\"uniq_id\":\"uniqSyncFollow\","
    "\"ic\":\"mdi:account-sync\",\"dev\":{\"ids\":\"testId\"},"
    "\"stat_t\":\"testData/testId/uniqSyncFollow/stat_t\","
    "\"cmd_t\":\"testData/testId/uniqSyncFollow/cmd_t\"}";

// The Targets select's discovery payload: a static two-option select (Off/All) with
// name + icon, on its own state + command topics. The option list is exactly the
// static Off/All this slice ships (no dynamic peers yet).
static const char *EXPECTED_SELECT_CONFIG =
    "{\"name\":\"Timer sync targets\",\"uniq_id\":\"uniqSyncTargets\","
    "\"ic\":\"mdi:target-account\",\"options\":[\"Off\",\"All\"],"
    "\"dev\":{\"ids\":\"testId\"},"
    "\"stat_t\":\"testData/testId/uniqSyncTargets/stat_t\","
    "\"cmd_t\":\"testData/testId/uniqSyncTargets/cmd_t\"}";

void setUp(void) {
    ArduinoFakeReset();
    When(Method(ArduinoFake(), millis)).AlwaysReturn(0);
}
void tearDown(void) {}

// Builds a connected HADevice/HAMqtt over a fresh mock, returning the mock so the
// flushed discovery messages can be asserted. Everything is leaked on purpose.
static PubSubClientMock *connectMqtt(HABaseDeviceType *(*build)()) {
    PubSubClientMock *mock = new PubSubClientMock();
    HADevice *device = new HADevice("testId");
    HAMqtt *mqtt = new HAMqtt(mock, *device);
    mqtt->setDataPrefix("testData");
    mqtt->begin("testHost");
    build();
    mqtt->loop();  // connect + publish discovery config (flushed message index 0)
    return mock;
}

static HABaseDeviceType *buildSyncFollowSwitch() {
    HASwitch *sw = new HASwitch("uniqSyncFollow");
    sw->setName("Timer sync follow");
    sw->setIcon("mdi:account-sync");
    return sw;
}

static HABaseDeviceType *buildSyncTargetsSelect() {
    HASelect *sel = new HASelect("uniqSyncTargets");
    sel->setOptions("Off;All");
    sel->setName("Timer sync targets");
    sel->setIcon("mdi:target-account");
    return sel;
}

// ============================================================================
// SYNC1 — the Follow switch's discovery payload advertises name + icon and its
// own state + command topics (so HA can command it), byte-identical to EXPECTED.
// Pins the Timer's first HASwitch discovery config.
// ============================================================================
void test_SYNC1_follow_switch_discovery_config(void) {
    PubSubClientMock *mock = connectMqtt(buildSyncFollowSwitch);

    // The switch flushes its discovery config (index 0) followed by its initial
    // state (index 1, "OFF") on connect; the config is the first message.
    TEST_ASSERT_TRUE(mock->getFlushedMessagesNb() >= 1);
    const MqttMessage *m = mock->getFlushedMessages()[0];
    TEST_ASSERT_EQUAL_STRING(EXPECTED_SWITCH_CONFIG, m->buffer);
    // It is commandable (has a command topic) — the write path HA toggles.
    TEST_ASSERT_NOT_NULL(strstr(m->buffer, "\"cmd_t\":\"testData/testId/uniqSyncFollow/cmd_t\""));
}

// ============================================================================
// SYNC2 — the Targets select's discovery payload carries exactly the static
// Off/All options, name + icon, and its command topic, byte-identical to EXPECTED.
// ============================================================================
void test_SYNC2_targets_select_discovery_config(void) {
    PubSubClientMock *mock = connectMqtt(buildSyncTargetsSelect);

    TEST_ASSERT_EQUAL_UINT8(1, mock->getFlushedMessagesNb());
    const MqttMessage *m = mock->getFlushedMessages()[0];
    TEST_ASSERT_EQUAL_STRING(EXPECTED_SELECT_CONFIG, m->buffer);
    // Static two-option list only (no dynamic peers in this slice).
    TEST_ASSERT_NOT_NULL(strstr(m->buffer, "\"options\":[\"Off\",\"All\"]"));
    TEST_ASSERT_NOT_NULL(strstr(m->buffer, "\"cmd_t\":\"testData/testId/uniqSyncTargets/cmd_t\""));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_SYNC1_follow_switch_discovery_config);
    RUN_TEST(test_SYNC2_targets_select_discovery_config);
    return UNITY_END();
}
