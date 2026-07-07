// Unit tests for SyncSeenCache (PRD #28, extracted from TimerManager per ADR-0022,
// a deliberate sibling of PeerRegistry/ADR-0021).
//
// The whole point of the extraction: these exercise the sync dedup set DIRECTLY,
// with an injected `nowMs` — no TimerManager singleton, no uniqueID/TIMER_SYNC_FOLLOW
// globals, no MQTT/Preferences stubs. Contrast the dedup case in test_timer
// (test_S4), which had to stand up the full 4,794-line fixture.

#include <unity.h>
#include <ArduinoFake.h>

#include "../../src/SyncSeenCache.h"

// kMax is a private detail of SyncSeenCache; mirror it here so the FIFO-wrap test
// can fill the ring. Kept in sync by the SS5 assertions (a drift would make either
// the "evicted" or the "still present" assertion fail).
static constexpr int kMax = 8;

void setUp(void) {}
void tearDown(void) {}

// ============================================================================
// SS1 — a first-seen (src,seq) is not a redundant copy (records it, returns false)
// ============================================================================
void test_SS1_first_seen_returns_false(void) {
    SyncSeenCache cache;
    TEST_ASSERT_FALSE(cache.seen("awtrix_o", 1, 1000));
}

// ============================================================================
// SS2 — an immediate re-deliver of the same (src,seq) is a redundant copy.
// This is the "3x redundant send applied once" guarantee in isolation.
// ============================================================================
void test_SS2_immediate_redeliver_deduped(void) {
    SyncSeenCache cache;
    TEST_ASSERT_FALSE(cache.seen("awtrix_o", 4, 1000));  // first
    TEST_ASSERT_TRUE(cache.seen("awtrix_o", 4, 1000));   // 2nd copy: dropped
    TEST_ASSERT_TRUE(cache.seen("awtrix_o", 4, 1000));   // 3rd copy: dropped
}

// ============================================================================
// SS3 — a new seq from the SAME src is accepted (not a redundant copy)
// ============================================================================
void test_SS3_same_src_new_seq_accepted(void) {
    SyncSeenCache cache;
    TEST_ASSERT_FALSE(cache.seen("awtrix_o", 4, 1000));
    TEST_ASSERT_FALSE(cache.seen("awtrix_o", 5, 1000));  // new seq: apply it
}

// ============================================================================
// SS4 — an entry past the TTL ages out: a later re-deliver is applied again.
// (A sender reboot restarts seq; the stale entry must not dedup the new run.)
// ============================================================================
void test_SS4_ttl_age_out(void) {
    SyncSeenCache cache;
    TEST_ASSERT_FALSE(cache.seen("awtrix_o", 4, 1000));
    TEST_ASSERT_TRUE(cache.seen("awtrix_o", 4, 1000 + 2000));   // 2000 <= TTL: still deduped
    TEST_ASSERT_FALSE(cache.seen("awtrix_o", 4, 1000 + 2001));  // 2001 > TTL: aged out
}

// ============================================================================
// SS5 — at the bound, the FIFO ring overwrites the OLDEST slot (round-robin).
// Fill kMax distinct entries, then one more: the first-written is evicted (a
// re-deliver is no longer deduped) while a later one survives.
// ============================================================================
void test_SS5_fifo_wrap_at_bound(void) {
    SyncSeenCache cache;
    for (int i = 0; i < kMax; ++i)
        TEST_ASSERT_FALSE(cache.seen("awtrix_o", (uint32_t)i, 1000));  // fill the ring

    cache.seen("awtrix_o", 999, 1000);   // wraps: overwrites the oldest slot (seq 0)

    // Check the survivor FIRST: a dedup hit returns early WITHOUT recording, so it
    // does not disturb the ring. seq 1 is still present -> deduped (true).
    TEST_ASSERT_TRUE(cache.seen("awtrix_o", 1, 1000));
    // seq 0 was evicted -> re-deliver is treated as fresh (false, re-applied).
    TEST_ASSERT_FALSE(cache.seen("awtrix_o", 0, 1000));
}

// ============================================================================
// SS6 — nowMs==0 folds to 1 so the stored entry is not mistaken for the empty-slot
// sentinel (atMs==0). A copy first seen at boot (t=0) is still deduped on a later
// re-deliver; were atMs stored as 0 the atMs!=0 guard would skip it as empty.
// ============================================================================
void test_SS6_now_zero_sentinel(void) {
    SyncSeenCache cache;
    TEST_ASSERT_FALSE(cache.seen("awtrix_o", 4, 0));  // recorded with atMs folded to 1, not 0
    TEST_ASSERT_TRUE(cache.seen("awtrix_o", 4, 1));   // matchable: the slot is not seen as empty
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_SS1_first_seen_returns_false);
    RUN_TEST(test_SS2_immediate_redeliver_deduped);
    RUN_TEST(test_SS3_same_src_new_seq_accepted);
    RUN_TEST(test_SS4_ttl_age_out);
    RUN_TEST(test_SS5_fifo_wrap_at_bound);
    RUN_TEST(test_SS6_now_zero_sentinel);
    return UNITY_END();
}
