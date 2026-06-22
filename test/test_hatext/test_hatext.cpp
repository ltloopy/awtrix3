// Host suite for the vendored ArduinoHA library's opt-in JSON-attributes
// capability on HAText (PRD #66 / issue #67), the THIRD device type to adopt the
// capability after HASelect (HA1-HA3) and HASensor (HS1-HS3). Uses the library's
// own ARDUINOHA_TEST harness (PubSubClientMock + HAMqtt + HADevice) to drive a
// real connect and assert the exact discovery payload the broker would see, plus
// the retained attributes publish. The opt-in is generic library mechanics only:
// no formatHMS, no Timer dependency.
//
// HAText's discovery shape differs from HASensor's: it has a command topic
// (cmd_t) and no device-class / unit-of-measurement. The json_attr_t topic is
// appended after both data topics, mirroring how HASensor appends it after its
// single state topic.
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

// The discovery payload a Duration-shaped text entity (name + icon, no retain,
// device availability off) emits today. Pinned so any change to the serializer
// that perturbs an existing HAText entity fails here.
static const char *EXPECTED_DISABLED_CONFIG =
    "{\"name\":\"Duration\",\"uniq_id\":\"uniqueText\",\"ic\":\"mdi:timer\","
    "\"dev\":{\"ids\":\"testId\"},"
    "\"stat_t\":\"testData/testId/uniqueText/stat_t\","
    "\"cmd_t\":\"testData/testId/uniqueText/cmd_t\"}";

// Same payload with the json_attributes_topic appended — the ONLY delta the
// capability introduces when enabled.
static const char *EXPECTED_ENABLED_CONFIG =
    "{\"name\":\"Duration\",\"uniq_id\":\"uniqueText\",\"ic\":\"mdi:timer\","
    "\"dev\":{\"ids\":\"testId\"},"
    "\"stat_t\":\"testData/testId/uniqueText/stat_t\","
    "\"cmd_t\":\"testData/testId/uniqueText/cmd_t\","
    "\"json_attr_t\":\"testData/testId/uniqueText/json_attr_t\"}";

void setUp(void) {
    ArduinoFakeReset();
    When(Method(ArduinoFake(), millis)).AlwaysReturn(0);
}
void tearDown(void) {}

struct Harness {
    PubSubClientMock *mock;  // what the broker saw
    HAText *text;            // the connected Duration-shaped text entity
};

// Builds a connected Duration-shaped text entity. `jsonAttrs` toggles the opt-in
// capability. Everything is leaked on purpose (see file header).
static Harness connectText(bool jsonAttrs) {
    PubSubClientMock *mock = new PubSubClientMock();
    HADevice *device = new HADevice("testId");
    HAMqtt *mqtt = new HAMqtt(mock, *device);
    mqtt->setDataPrefix("testData");
    mqtt->begin("testHost");

    HAText *text = new HAText("uniqueText");
    text->setName("Duration");
    text->setIcon("mdi:timer");
    if (jsonAttrs) {
        text->setJsonAttributes(true);
    }

    // connect: publishes discovery config (flushed message index 0) then an
    // empty initial state (index 1) — HAText publishes its state on connect,
    // unlike HASensor. The config remains the first flushed message.
    mqtt->loop();
    return {mock, text};
}

// ============================================================================
// HT1 — a text entity with attributes DISABLED (the default) emits the same
// discovery payload as today: no json_attr_t, byte-identical to EXPECTED.
// Proves no regression to any existing HAText entity.
// ============================================================================
void test_HT1_disabled_text_config_is_unchanged(void) {
    PubSubClientMock *mock = connectText(false).mock;

    // config (index 0) + empty initial state (index 1).
    TEST_ASSERT_EQUAL_UINT8(2, mock->getFlushedMessagesNb());
    const MqttMessage *m = mock->getFlushedMessages()[0];
    TEST_ASSERT_EQUAL_STRING(EXPECTED_DISABLED_CONFIG, m->buffer);
    TEST_ASSERT_NULL(strstr(m->buffer, "json_attr_t"));
}

// ============================================================================
// HT2 — setJsonAttributes(true) adds the json_attributes_topic to the discovery
// payload, and nothing else: the payload is the disabled one with json_attr_t
// appended. Proves HA is told to read attributes from the entity's own topic.
// ============================================================================
void test_HT2_enabled_text_advertises_json_attr_topic(void) {
    PubSubClientMock *mock = connectText(true).mock;

    // config (index 0) + empty initial state (index 1).
    TEST_ASSERT_EQUAL_UINT8(2, mock->getFlushedMessagesNb());
    const MqttMessage *m = mock->getFlushedMessages()[0];
    TEST_ASSERT_EQUAL_STRING(EXPECTED_ENABLED_CONFIG, m->buffer);
    TEST_ASSERT_NOT_NULL(
        strstr(m->buffer, "\"json_attr_t\":\"testData/testId/uniqueText/json_attr_t\""));
}

// ============================================================================
// HT3 — publishJsonAttributes() publishes the given JSON object retained on the
// text entity's json_attributes data topic, riding the same data-topic machinery
// as setState. Proves the retained + existing data-topic path for the carrier.
// ============================================================================
void test_HT3_publishJsonAttributes_is_retained_on_attr_topic(void) {
    Harness h = connectText(true);

    TEST_ASSERT_TRUE(h.text->publishJsonAttributes("{\"max_duration\":\"24:00:00\"}"));

    // The attributes publish is the flushed message after config (0) and the
    // empty initial state (1), so it lands at index 2.
    PubSubClientMock *mock = h.mock;
    TEST_ASSERT_EQUAL_UINT8(3, mock->getFlushedMessagesNb());
    const MqttMessage *m = mock->getFlushedMessages()[2];
    TEST_ASSERT_EQUAL_STRING("testData/testId/uniqueText/json_attr_t", m->topic);
    TEST_ASSERT_EQUAL_STRING("{\"max_duration\":\"24:00:00\"}", m->buffer);
    TEST_ASSERT_TRUE(m->retained);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_HT1_disabled_text_config_is_unchanged);
    RUN_TEST(test_HT2_enabled_text_advertises_json_attr_topic);
    RUN_TEST(test_HT3_publishJsonAttributes_is_retained_on_attr_topic);
    return UNITY_END();
}
