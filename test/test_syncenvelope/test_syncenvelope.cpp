// Unit tests for SyncEnvelope (PRD #28, extracted from TimerManager per ADR-0023 —
// the third cut after PeerRegistry/ADR-0021 and SyncSeenCache/ADR-0022).
//
// The whole point of the extraction: these exercise the inbound sync GATE directly,
// as a pure table-driven decision — no TimerManager singleton, no uniqueID /
// TIMER_SYNC_FOLLOW globals, no MQTT/Preferences stubs. Contrast the gating cases in
// test_timer (test_S4), which had to stand up the full 4,794-line fixture. The dedup
// step is NOT here: it is the shell's stateful guard (SyncSeenCache), tested in
// test_syncseen — classify owns only the four pure gates.

#include <unity.h>
#include <ArduinoFake.h>
#include <ArduinoJson.h>

#include "../../src/SyncEnvelope.h"

using SyncEnvelope::Context;
using SyncEnvelope::Decision;

void setUp(void) {}
void tearDown(void) {}

// Helper: classify a JSON literal against a {ownId, follow} context.
static Decision classifyJson(const char *json, const char *ownId, bool follow) {
    StaticJsonDocument<512> doc;
    deserializeJson(doc, json);
    return SyncEnvelope::classify(doc.as<JsonVariantConst>(), Context{String(ownId), follow});
}

// ============================================================================
// SE1 — a packet without a _sync envelope is not a sync packet -> Ignore.
// ============================================================================
void test_SE1_no_sync_envelope_ignored(void) {
    Decision d = classifyJson("{\"action\":\"start\"}", "awtrix_me", true);
    TEST_ASSERT_EQUAL(Decision::Ignore, d.kind);
}

// ============================================================================
// SE2 — an empty src is malformed -> Ignore.
// ============================================================================
void test_SE2_empty_src_ignored(void) {
    Decision d = classifyJson("{\"_sync\":{\"src\":\"\",\"seq\":1,\"tgt\":\"all\"},\"action\":\"start\"}",
                              "awtrix_me", true);
    TEST_ASSERT_EQUAL(Decision::Ignore, d.kind);
}

// ============================================================================
// SE3 — a packet from our own uniqueID is an echo -> Ignore (before any gate).
// ============================================================================
void test_SE3_own_echo_ignored(void) {
    Decision d = classifyJson("{\"_sync\":{\"src\":\"awtrix_me\",\"seq\":1,\"tgt\":\"all\"},\"action\":\"start\"}",
                              "awtrix_me", true);
    TEST_ASSERT_EQUAL(Decision::Ignore, d.kind);
}

// ============================================================================
// SE4 — a presence beacon harvests the sender UNGATED: HarvestPresence even with
// follow=false and no tgt, carrying the sender's src for the registry.
// ============================================================================
void test_SE4_presence_harvested_ungated(void) {
    Decision d = classifyJson("{\"_sync\":{\"src\":\"awtrix_peer\",\"seq\":1},\"presence\":true}",
                              "awtrix_me", /*follow=*/false);
    TEST_ASSERT_EQUAL(Decision::HarvestPresence, d.kind);
    TEST_ASSERT_EQUAL_STRING("awtrix_peer", d.src.c_str());
}

// ============================================================================
// SE5 — a command with follow=false is dropped by the consent gate -> Ignore,
// even though it targets "all".
// ============================================================================
void test_SE5_follow_off_command_ignored(void) {
    Decision d = classifyJson("{\"_sync\":{\"src\":\"awtrix_peer\",\"seq\":1,\"tgt\":\"all\"},\"action\":\"start\"}",
                              "awtrix_me", /*follow=*/false);
    TEST_ASSERT_EQUAL(Decision::Ignore, d.kind);
}

// ============================================================================
// SE6 — follow=true + tgt "all" -> Apply, carrying src and seq (the dedup key).
// ============================================================================
void test_SE6_follow_on_all_applies(void) {
    Decision d = classifyJson("{\"_sync\":{\"src\":\"awtrix_peer\",\"seq\":7,\"tgt\":\"all\"},\"action\":\"start\"}",
                              "awtrix_me", true);
    TEST_ASSERT_EQUAL(Decision::Apply, d.kind);
    TEST_ASSERT_EQUAL_STRING("awtrix_peer", d.src.c_str());
    TEST_ASSERT_EQUAL_UINT32(7, d.seq);
}

// ============================================================================
// SE7 — follow=true + tgt array containing ownId -> Apply.
// ============================================================================
void test_SE7_follow_on_array_hit_applies(void) {
    Decision d = classifyJson(
        "{\"_sync\":{\"src\":\"awtrix_peer\",\"seq\":2,\"tgt\":[\"awtrix_x\",\"awtrix_me\"]},\"action\":\"start\"}",
        "awtrix_me", true);
    TEST_ASSERT_EQUAL(Decision::Apply, d.kind);
}

// ============================================================================
// SE8 — follow=true but tgt array does NOT contain ownId -> Ignore.
// ============================================================================
void test_SE8_follow_on_array_miss_ignored(void) {
    Decision d = classifyJson(
        "{\"_sync\":{\"src\":\"awtrix_peer\",\"seq\":2,\"tgt\":[\"awtrix_x\",\"awtrix_y\"]},\"action\":\"start\"}",
        "awtrix_me", true);
    TEST_ASSERT_EQUAL(Decision::Ignore, d.kind);
}

// ============================================================================
// SE9 — the FOLLOWER invariant (the Q2 footgun guard). classify reads only the
// SENDER's tgt + the receiver's {ownId, follow}; it has NO notion of the
// receiver's own send-target list. A pure follower (empty local targets) targeted
// by a peer still Applies. There is no `targets` field in Context to break this.
// ============================================================================
void test_SE9_follower_applies_regardless_of_local_targets(void) {
    // Context carries only {ownId, follow} — a local target list cannot even be
    // expressed here, so a follower with no targets obeys a peer that addresses it.
    Decision d = classifyJson(
        "{\"_sync\":{\"src\":\"awtrix_leader\",\"seq\":1,\"tgt\":[\"awtrix_me\"]},\"action\":\"start\"}",
        "awtrix_me", true);
    TEST_ASSERT_EQUAL(Decision::Apply, d.kind);
}

// ============================================================================
// SE10 — targetsMe directly: "all" covers anyone; array membership (with
// trimming); a non-covering list does not.
// ============================================================================
void test_SE10_targetsMe(void) {
    StaticJsonDocument<256> doc;
    deserializeJson(doc,
        "{\"all\":\"all\",\"hit\":[\" awtrix_me \",\"awtrix_x\"],\"miss\":[\"awtrix_x\"],\"empty\":[]}");
    TEST_ASSERT_TRUE(SyncEnvelope::targetsMe(doc["all"], "awtrix_me"));
    TEST_ASSERT_TRUE(SyncEnvelope::targetsMe(doc["hit"], "awtrix_me"));   // trimmed match
    TEST_ASSERT_FALSE(SyncEnvelope::targetsMe(doc["miss"], "awtrix_me"));
    TEST_ASSERT_FALSE(SyncEnvelope::targetsMe(doc["empty"], "awtrix_me"));
}

// ============================================================================
// SE11 — build (bare): just {src, seq}, no tgt. The presence-beacon shape.
// ============================================================================
void test_SE11_build_bare(void) {
    StaticJsonDocument<128> doc;
    JsonObject sync = doc.createNestedObject("_sync");
    SyncEnvelope::build(sync, "awtrix_me", 5);
    TEST_ASSERT_EQUAL_STRING("awtrix_me", sync["src"].as<const char *>());
    TEST_ASSERT_EQUAL_UINT32(5, sync["seq"].as<uint32_t>());
    TEST_ASSERT_FALSE(sync.containsKey("tgt"));
}

// ============================================================================
// SE12 — build (targeted): "all" -> tgt string "all".
// ============================================================================
void test_SE12_build_targets_all(void) {
    StaticJsonDocument<256> doc;
    JsonObject sync = doc.createNestedObject("_sync");
    SyncEnvelope::build(sync, "awtrix_me", 5, "all");
    TEST_ASSERT_TRUE(sync["tgt"].is<const char *>());
    TEST_ASSERT_EQUAL_STRING("all", sync["tgt"].as<const char *>());
}

// ============================================================================
// SE13 — build (targeted): a CSV -> a tgt id array, whitespace/empty ids skipped.
// ============================================================================
void test_SE13_build_targets_csv(void) {
    StaticJsonDocument<256> doc;
    JsonObject sync = doc.createNestedObject("_sync");
    SyncEnvelope::build(sync, "awtrix_me", 5, " awtrix_a , awtrix_b ,, ");
    JsonArrayConst tgt = sync["tgt"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL(2, tgt.size());
    TEST_ASSERT_EQUAL_STRING("awtrix_a", tgt[0].as<const char *>());
    TEST_ASSERT_EQUAL_STRING("awtrix_b", tgt[1].as<const char *>());
}

// ============================================================================
// SE14 — build (targeted) with an empty list -> an empty tgt array (sends to nobody).
// ============================================================================
void test_SE14_build_targets_empty(void) {
    StaticJsonDocument<256> doc;
    JsonObject sync = doc.createNestedObject("_sync");
    SyncEnvelope::build(sync, "awtrix_me", 5, "");
    TEST_ASSERT_TRUE(sync["tgt"].is<JsonArrayConst>());
    TEST_ASSERT_EQUAL(0, sync["tgt"].as<JsonArrayConst>().size());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_SE1_no_sync_envelope_ignored);
    RUN_TEST(test_SE2_empty_src_ignored);
    RUN_TEST(test_SE3_own_echo_ignored);
    RUN_TEST(test_SE4_presence_harvested_ungated);
    RUN_TEST(test_SE5_follow_off_command_ignored);
    RUN_TEST(test_SE6_follow_on_all_applies);
    RUN_TEST(test_SE7_follow_on_array_hit_applies);
    RUN_TEST(test_SE8_follow_on_array_miss_ignored);
    RUN_TEST(test_SE9_follower_applies_regardless_of_local_targets);
    RUN_TEST(test_SE10_targetsMe);
    RUN_TEST(test_SE11_build_bare);
    RUN_TEST(test_SE12_build_targets_all);
    RUN_TEST(test_SE13_build_targets_csv);
    RUN_TEST(test_SE14_build_targets_empty);
    return UNITY_END();
}
