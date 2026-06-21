// Host suite for the vendored ArduinoHA library's opt-in JSON-attributes
// capability on HASensor (PRD #57 / issue #59), mirroring the HASelect HA1-HA3
// suite. Uses the library's own ARDUINOHA_TEST harness (PubSubClientMock +
// HAMqtt + HADevice) to drive a real connect and assert the exact discovery
// payload the broker would see, plus the retained attributes publish.
// HASensorNumber inherits buildSerializer from HASensor, so the remaining sensor
// is covered by the same opt-in with no extra library work.
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

// The discovery payload a state-sensor-shaped sensor (name + icon, no device
// class / unit / force-update, device availability off) emits today. Pinned so
// any change to the serializer that perturbs an existing sensor fails here.
static const char *EXPECTED_DISABLED_CONFIG =
    "{\"name\":\"State\",\"uniq_id\":\"uniqueSensor\",\"ic\":\"mdi:timer\","
    "\"dev\":{\"ids\":\"testId\"},"
    "\"stat_t\":\"testData/testId/uniqueSensor/stat_t\"}";

// Same payload with the json_attributes_topic appended — the ONLY delta the
// capability introduces when enabled.
static const char *EXPECTED_ENABLED_CONFIG =
    "{\"name\":\"State\",\"uniq_id\":\"uniqueSensor\",\"ic\":\"mdi:timer\","
    "\"dev\":{\"ids\":\"testId\"},"
    "\"stat_t\":\"testData/testId/uniqueSensor/stat_t\","
    "\"json_attr_t\":\"testData/testId/uniqueSensor/json_attr_t\"}";

void setUp(void) {
    ArduinoFakeReset();
    When(Method(ArduinoFake(), millis)).AlwaysReturn(0);
}
void tearDown(void) {}

struct Harness {
    PubSubClientMock *mock;  // what the broker saw
    HASensor *sensor;        // the connected state-sensor-shaped sensor
};

// Builds a connected state-sensor-shaped sensor. `jsonAttrs` toggles the opt-in
// capability. Everything is leaked on purpose (see file header).
static Harness connectSensor(bool jsonAttrs) {
    PubSubClientMock *mock = new PubSubClientMock();
    HADevice *device = new HADevice("testId");
    HAMqtt *mqtt = new HAMqtt(mock, *device);
    mqtt->setDataPrefix("testData");
    mqtt->begin("testHost");

    HASensor *sensor = new HASensor("uniqueSensor");
    sensor->setName("State");
    sensor->setIcon("mdi:timer");
    if (jsonAttrs) {
        sensor->setJsonAttributes(true);
    }

    mqtt->loop();  // connect + publish discovery config (flushed message index 0)
    return {mock, sensor};
}

// ============================================================================
// HS1 — a sensor with attributes DISABLED (the default) emits the same
// discovery payload as today: no json_attr_t, byte-identical to EXPECTED.
// Proves no regression to any existing sensor (temperature, uptime, etc.).
// ============================================================================
void test_HS1_disabled_sensor_config_is_unchanged(void) {
    PubSubClientMock *mock = connectSensor(false).mock;

    TEST_ASSERT_EQUAL_UINT8(1, mock->getFlushedMessagesNb());
    const MqttMessage *m = mock->getFlushedMessages()[0];
    TEST_ASSERT_EQUAL_STRING(EXPECTED_DISABLED_CONFIG, m->buffer);
    TEST_ASSERT_NULL(strstr(m->buffer, "json_attr_t"));
}

// ============================================================================
// HS2 — setJsonAttributes(true) adds the json_attributes_topic to the discovery
// payload, and nothing else: the payload is the disabled one with json_attr_t
// appended. Proves HA is told to read attributes from the entity's own topic.
// ============================================================================
void test_HS2_enabled_sensor_advertises_json_attr_topic(void) {
    PubSubClientMock *mock = connectSensor(true).mock;

    TEST_ASSERT_EQUAL_UINT8(1, mock->getFlushedMessagesNb());
    const MqttMessage *m = mock->getFlushedMessages()[0];
    TEST_ASSERT_EQUAL_STRING(EXPECTED_ENABLED_CONFIG, m->buffer);
    TEST_ASSERT_NOT_NULL(
        strstr(m->buffer, "\"json_attr_t\":\"testData/testId/uniqueSensor/json_attr_t\""));
}

// ============================================================================
// HS3 — publishJsonAttributes() publishes the given JSON object retained on the
// sensor's json_attributes data topic, riding the same data-topic machinery as
// setValue. Proves the retained + existing data-topic path for the carrier.
// ============================================================================
void test_HS3_publishJsonAttributes_is_retained_on_attr_topic(void) {
    Harness h = connectSensor(true);

    TEST_ASSERT_TRUE(h.sensor->publishJsonAttributes("{\"max_duration\":86400}"));

    // The attributes publish is the flushed message after the config (index 1).
    PubSubClientMock *mock = h.mock;
    TEST_ASSERT_EQUAL_UINT8(2, mock->getFlushedMessagesNb());
    const MqttMessage *m = mock->getFlushedMessages()[1];
    TEST_ASSERT_EQUAL_STRING("testData/testId/uniqueSensor/json_attr_t", m->topic);
    TEST_ASSERT_EQUAL_STRING("{\"max_duration\":86400}", m->buffer);
    TEST_ASSERT_TRUE(m->retained);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_HS1_disabled_sensor_config_is_unchanged);
    RUN_TEST(test_HS2_enabled_sensor_advertises_json_attr_topic);
    RUN_TEST(test_HS3_publishJsonAttributes_is_retained_on_attr_topic);
    return UNITY_END();
}
