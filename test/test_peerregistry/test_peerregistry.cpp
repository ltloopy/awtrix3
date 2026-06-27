// Unit tests for PeerRegistry (#111 / ADR-0019, extracted per ADR-0021).
//
// The whole point of the extraction: these exercise the LAN peer set DIRECTLY,
// with an injected `nowMs` and an injected own-id — no TimerManager singleton, no
// uniqueID/AP_MODE/ServerManager globals, no MQTT/Preferences stubs. Contrast the
// peer cases in test_timer, which had to stand up the full fixture.

#include <unity.h>
#include <ArduinoFake.h>

#include "../../src/PeerRegistry.h"

// kPeerMax is a private detail of PeerRegistry; mirror it here so the eviction
// test can fill the set. Kept in sync by the P5 assertions (a drift would make
// either the "still full" or the "evicted" assertion fail).
static constexpr int kPeerMax = 16;

void setUp(void) {}
void tearDown(void) {}

// ============================================================================
// P1 — the clock's own id is never stored (echoed own beacons are dropped)
// ============================================================================
void test_P1_own_id_excluded(void) {
    PeerRegistry reg;
    reg.setOwnId("self");
    reg.record("self", 1000);
    TEST_ASSERT_EQUAL_INT(0, reg.count());
    TEST_ASSERT_FALSE(reg.has("self"));
}

// ============================================================================
// P2 — a fresh peer is recorded and observable
// ============================================================================
void test_P2_record_adds_peer(void) {
    PeerRegistry reg;
    reg.setOwnId("self");
    reg.record("peer-a", 1000);
    TEST_ASSERT_EQUAL_INT(1, reg.count());
    TEST_ASSERT_TRUE(reg.has("peer-a"));
    TEST_ASSERT_FALSE(reg.has("peer-b"));
}

// ============================================================================
// P3 — re-recording an existing peer refreshes lastSeen (no duplicate row)
// Proof: without the refresh the entry would age out; with it, it survives.
// ============================================================================
void test_P3_record_refreshes_last_seen(void) {
    PeerRegistry reg;
    reg.setOwnId("self");
    reg.record("peer-a", 1000);
    reg.record("peer-a", 200000);          // same id, much later
    TEST_ASSERT_EQUAL_INT(1, reg.count()); // refreshed, not duplicated

    // 250000 - 200000 = 50000 <= TTL(100000): stays only if lastSeen was updated.
    reg.prune(250000);
    TEST_ASSERT_EQUAL_INT(1, reg.count());
    TEST_ASSERT_TRUE(reg.has("peer-a"));
}

// ============================================================================
// P4 — entries past the TTL (~3 missed beacons) age out on prune
// ============================================================================
void test_P4_prune_drops_stale(void) {
    PeerRegistry reg;
    reg.setOwnId("self");
    reg.record("peer-a", 1000);
    reg.record("peer-b", 1000);

    reg.prune(1000 + 100001);   // just past kPeerTtlMs for both
    TEST_ASSERT_EQUAL_INT(0, reg.count());
    TEST_ASSERT_FALSE(reg.has("peer-a"));
}

// ============================================================================
// P4b — prune keeps fresh, drops stale, and compacts the survivors
// ============================================================================
void test_P4b_prune_keeps_fresh_compacts(void) {
    PeerRegistry reg;
    reg.setOwnId("self");
    reg.record("stale", 1000);
    reg.record("fresh", 90000);

    reg.prune(101500);   // stale: 100500 > TTL (drop); fresh: 11500 <= TTL (keep)
    TEST_ASSERT_EQUAL_INT(1, reg.count());
    TEST_ASSERT_TRUE(reg.has("fresh"));
    TEST_ASSERT_FALSE(reg.has("stale"));
}

// ============================================================================
// P5 — when full, a new peer overwrites the STALEST slot (freshest survive)
// ============================================================================
void test_P5_eviction_overwrites_stalest(void) {
    PeerRegistry reg;
    reg.setOwnId("self");

    // Fill to capacity; id "p00" is the stalest (smallest lastSeen).
    for (int i = 0; i < kPeerMax; ++i) {
        char id[8];
        snprintf(id, sizeof(id), "p%02d", i);
        reg.record(String(id), (unsigned long)(1000 + i));   // strictly increasing
    }
    TEST_ASSERT_EQUAL_INT(kPeerMax, reg.count());
    TEST_ASSERT_TRUE(reg.has("p00"));

    // One more: still full, stalest ("p00") evicted, newcomer present.
    reg.record("newcomer", 99999);
    TEST_ASSERT_EQUAL_INT(kPeerMax, reg.count());
    TEST_ASSERT_FALSE(reg.has("p00"));
    TEST_ASSERT_TRUE(reg.has("newcomer"));
    TEST_ASSERT_TRUE(reg.has("p01"));   // the next-stalest survived
}

// ============================================================================
// P6 — ids() returns the set SORTED ascending, and respects the cap
// ============================================================================
void test_P6_ids_sorted_and_capped(void) {
    PeerRegistry reg;
    reg.setOwnId("self");
    reg.record("charlie", 1000);
    reg.record("alpha",   1000);
    reg.record("bravo",   1000);

    String out[8];
    size_t n = reg.ids(out, 8);
    TEST_ASSERT_EQUAL_UINT32(3, n);
    TEST_ASSERT_EQUAL_STRING("alpha", out[0].c_str());
    TEST_ASSERT_EQUAL_STRING("bravo", out[1].c_str());
    TEST_ASSERT_EQUAL_STRING("charlie", out[2].c_str());

    // A small buffer still gets the sorted PREFIX (not the discovery order).
    String two[2];
    size_t m = reg.ids(two, 2);
    TEST_ASSERT_EQUAL_UINT32(2, m);
    TEST_ASSERT_EQUAL_STRING("alpha", two[0].c_str());
    TEST_ASSERT_EQUAL_STRING("bravo", two[1].c_str());
}

// ============================================================================
// P7 — clear() empties the set but retains the injected own-id
// ============================================================================
void test_P7_clear_empties_but_keeps_own_id(void) {
    PeerRegistry reg;
    reg.setOwnId("self");
    reg.record("peer-a", 1000);
    reg.record("peer-b", 1000);
    TEST_ASSERT_EQUAL_INT(2, reg.count());

    reg.clear();
    TEST_ASSERT_EQUAL_INT(0, reg.count());
    TEST_ASSERT_FALSE(reg.has("peer-a"));

    // Own id still excluded after a clear (it was retained, not wiped).
    reg.record("self", 2000);
    TEST_ASSERT_EQUAL_INT(0, reg.count());
}

// ============================================================================
// P8 — an empty/blank src is never recorded
// ============================================================================
void test_P8_empty_src_ignored(void) {
    PeerRegistry reg;
    reg.setOwnId("self");
    reg.record("", 1000);
    TEST_ASSERT_EQUAL_INT(0, reg.count());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_P1_own_id_excluded);
    RUN_TEST(test_P2_record_adds_peer);
    RUN_TEST(test_P3_record_refreshes_last_seen);
    RUN_TEST(test_P4_prune_drops_stale);
    RUN_TEST(test_P4b_prune_keeps_fresh_compacts);
    RUN_TEST(test_P5_eviction_overwrites_stalest);
    RUN_TEST(test_P6_ids_sorted_and_capped);
    RUN_TEST(test_P7_clear_empties_but_keeps_own_id);
    RUN_TEST(test_P8_empty_src_ignored);
    return UNITY_END();
}
