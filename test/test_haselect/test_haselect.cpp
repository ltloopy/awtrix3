// Host suite for the vendored ArduinoHA library's opt-in JSON-attributes
// capability on HASelect (PRD #17 / issue #51). Uses the library's own
// ARDUINOHA_TEST harness (PubSubClientMock + HAMqtt + HADevice) to drive a real
// connect and assert the exact discovery payload the broker would see, plus the
// retained attributes publish. ArduinoFake supplies millis() for the connect.
//
// The harness objects are intentionally leaked (heap-allocated, never deleted):
// PubSubClientMock's teardown frees its realloc'd message array with `delete`,
// which the Windows UCRT heap flags as corruption. The processes are one-shot,
// so leaking the few-object harness keeps the vendored mock untouched.
#include <unity.h>
#include <ArduinoFake.h>
#include <ArduinoHA.h>
#include <cstring>

using namespace fakeit;

// The discovery payload a finished-mode-shaped select (name + icon + options,
// no retain/optimistic, device availability off) emits today. Pinned so any
// change to the serializer that perturbs an existing select fails here.
static const char *EXPECTED_DISABLED_CONFIG =
    "{\"name\":\"Finished\",\"uniq_id\":\"uniqueSel\",\"ic\":\"mdi:bell\","
    "\"options\":[\"A\",\"B\",\"C\"],\"dev\":{\"ids\":\"testId\"},"
    "\"stat_t\":\"testData/testId/uniqueSel/stat_t\","
    "\"cmd_t\":\"testData/testId/uniqueSel/cmd_t\"}";

// Same payload with the json_attributes_topic appended — the ONLY delta the
// capability introduces when enabled.
static const char *EXPECTED_ENABLED_CONFIG =
    "{\"name\":\"Finished\",\"uniq_id\":\"uniqueSel\",\"ic\":\"mdi:bell\","
    "\"options\":[\"A\",\"B\",\"C\"],\"dev\":{\"ids\":\"testId\"},"
    "\"stat_t\":\"testData/testId/uniqueSel/stat_t\","
    "\"cmd_t\":\"testData/testId/uniqueSel/cmd_t\","
    "\"json_attr_t\":\"testData/testId/uniqueSel/json_attr_t\"}";

void setUp(void) {
    ArduinoFakeReset();
    When(Method(ArduinoFake(), millis)).AlwaysReturn(0);
}
void tearDown(void) {}

struct Harness {
    PubSubClientMock *mock;  // what the broker saw
    HASelect *sel;           // the connected finished-mode-shaped select
};

// Builds a connected finished-mode-shaped select. `jsonAttrs` toggles the opt-in
// capability. Everything is leaked on purpose (see file header).
static Harness connectSelect(bool jsonAttrs) {
    PubSubClientMock *mock = new PubSubClientMock();
    HADevice *device = new HADevice("testId");
    HAMqtt *mqtt = new HAMqtt(mock, *device);
    mqtt->setDataPrefix("testData");
    mqtt->begin("testHost");

    HASelect *sel = new HASelect("uniqueSel");
    sel->setOptions("A;B;C");
    sel->setName("Finished");
    sel->setIcon("mdi:bell");
    if (jsonAttrs) {
        sel->setJsonAttributes(true);
    }

    mqtt->loop();  // connect + publish discovery config (flushed message index 0)
    return {mock, sel};
}

// ============================================================================
// HA1 — a select with attributes DISABLED (the default) emits the same
// discovery payload as today: no json_attr_t, byte-identical to EXPECTED.
// Proves no regression to the buzzer select or any other select.
// ============================================================================
void test_HA1_disabled_select_config_is_unchanged(void) {
    PubSubClientMock *mock = connectSelect(false).mock;

    TEST_ASSERT_EQUAL_UINT8(1, mock->getFlushedMessagesNb());
    const MqttMessage *m = mock->getFlushedMessages()[0];
    TEST_ASSERT_EQUAL_STRING(EXPECTED_DISABLED_CONFIG, m->buffer);
    TEST_ASSERT_NULL(strstr(m->buffer, "json_attr_t"));
}

// ============================================================================
// HA2 — setJsonAttributes(true) adds the json_attributes_topic to the discovery
// payload, and nothing else: the payload is the disabled one with json_attr_t
// appended. Proves HA is told to read attributes from the entity's own topic.
// ============================================================================
void test_HA2_enabled_select_advertises_json_attr_topic(void) {
    PubSubClientMock *mock = connectSelect(true).mock;

    TEST_ASSERT_EQUAL_UINT8(1, mock->getFlushedMessagesNb());
    const MqttMessage *m = mock->getFlushedMessages()[0];
    TEST_ASSERT_EQUAL_STRING(EXPECTED_ENABLED_CONFIG, m->buffer);
    TEST_ASSERT_NOT_NULL(
        strstr(m->buffer, "\"json_attr_t\":\"testData/testId/uniqueSel/json_attr_t\""));
}

// ============================================================================
// HA3 — publishJsonAttributes() publishes the given JSON object retained on the
// select's json_attributes data topic, riding the same data-topic machinery as
// the state. Proves AC1's "retained + existing data-topic path".
// ============================================================================
void test_HA3_publishJsonAttributes_is_retained_on_attr_topic(void) {
    Harness h = connectSelect(true);

    TEST_ASSERT_TRUE(h.sel->publishJsonAttributes("{\"realert_interval\":15}"));

    // The attributes publish is the flushed message after the config (index 1).
    PubSubClientMock *mock = h.mock;
    TEST_ASSERT_EQUAL_UINT8(2, mock->getFlushedMessagesNb());
    const MqttMessage *m = mock->getFlushedMessages()[1];
    TEST_ASSERT_EQUAL_STRING("testData/testId/uniqueSel/json_attr_t", m->topic);
    TEST_ASSERT_EQUAL_STRING("{\"realert_interval\":15}", m->buffer);
    TEST_ASSERT_TRUE(m->retained);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_HA1_disabled_select_config_is_unchanged);
    RUN_TEST(test_HA2_enabled_select_advertises_json_attr_topic);
    RUN_TEST(test_HA3_publishJsonAttributes_is_retained_on_attr_topic);
    return UNITY_END();
}
