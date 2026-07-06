// Unit tests for SyncTargetsDebounce (issue #199 / PRD #192) — the Targets-select
// republish debounce decision carved out of TimerHaHost::refreshTargets, a deliberate
// sibling of SyncSeenCache/ADR-0022.
//
// The whole point of the extraction: these exercise the settle-window state machine
// DIRECTLY, with an injected `nowMs` — no TimerHaHost, no ArduinoHA carriers, no MQTT.
// The TD1–TD4 cases are the four transition-table rows (one per Action); TD5–TD9 pin
// the consequences the table encodes (revert-cancel, no window-restart on drift, seed
// baseline, window anchored at first sighting, unsigned wrap).

#include <unity.h>
#include <ArduinoFake.h>
#include <limits.h>

#include "../../src/SyncTargetsDebounce.h"

using Action = SyncTargetsDebounce::Action;

// Unity has no enum formatter; compare as ints so a failure prints the actual Action.
#define ASSERT_ACTION(expected, actual) TEST_ASSERT_EQUAL_INT((int)(expected), (int)(actual))

void setUp(void) {}
void tearDown(void) {}

// ============================================================================
// TD1 — Row 1: cur == sig ⇒ Unchanged. seed() adopted the baseline, so the same
// options later is no change (and no spurious first republish).
// ============================================================================
void test_TD1_same_sig_unchanged(void) {
    SyncTargetsDebounce d;
    d.seed("Off;All");
    ASSERT_ACTION(Action::Unchanged, d.step("Off;All", 1000));
}

// ============================================================================
// TD2 — Row 2: cur != sig with the window closed ⇒ StartWindow (first sighting
// of a membership change opens the settle window; nothing is adopted yet).
// ============================================================================
void test_TD2_change_starts_window(void) {
    SyncTargetsDebounce d;
    d.seed("Off;All");
    ASSERT_ACTION(Action::StartWindow, d.step("Off;All;awtrix_p", 1000));
}

// ============================================================================
// TD3 — Row 3: cur != sig with the window open and now - sinceMs < 3000 ⇒ Waiting
// (the change has not settled yet; state is untouched).
// ============================================================================
void test_TD3_within_window_waiting(void) {
    SyncTargetsDebounce d;
    d.seed("Off;All");
    ASSERT_ACTION(Action::StartWindow, d.step("Off;All;awtrix_p", 1000));
    ASSERT_ACTION(Action::Waiting,     d.step("Off;All;awtrix_p", 1000 + 2999));
}

// ============================================================================
// TD4 — Row 4: cur != sig with the window open and now - sinceMs >= 3000 ⇒
// Republish, adopting cur as the new signature in the same atomic step (pinned
// at the exact boundary: 2999 is TD3's Waiting, 3000 republishes). After the
// adoption the same options are Unchanged — the caller publishes exactly once.
// ============================================================================
void test_TD4_settled_window_republishes_and_adopts(void) {
    SyncTargetsDebounce d;
    d.seed("Off;All");
    ASSERT_ACTION(Action::StartWindow, d.step("Off;All;awtrix_p", 1000));
    ASSERT_ACTION(Action::Republish,   d.step("Off;All;awtrix_p", 1000 + 3000));
    ASSERT_ACTION(Action::Unchanged,   d.step("Off;All;awtrix_p", 1000 + 3001));
}

// ============================================================================
// TD5 — revert-cancel: row 1 clears the dirty flag even mid-window, so membership
// flipping back to the published set cancels the pending republish. The next
// change gets a FRESH window (StartWindow, not a Republish off the stale anchor).
// ============================================================================
void test_TD5_revert_cancels_pending_window(void) {
    SyncTargetsDebounce d;
    d.seed("Off;All");
    ASSERT_ACTION(Action::StartWindow, d.step("Off;All;awtrix_p", 1000));
    ASSERT_ACTION(Action::Unchanged,   d.step("Off;All",          2000));  // revert: cancels
    // Were the window still open, 5000 - 1000 >= 3000 would republish; it must not.
    ASSERT_ACTION(Action::StartWindow, d.step("Off;All;awtrix_p", 5000));
}

// ============================================================================
// TD6 — mid-window drift does NOT restart the window: row 3 compares cur to the
// PUBLISHED sig, never the first-sighted value. A→B@t0, →C@t0+2s ⇒ still Waiting
// (a first-sighted-value comparison would restart the window on C), then one
// Republish at t0+3s adopting the LATEST options C.
// ============================================================================
void test_TD6_drift_does_not_restart_window(void) {
    SyncTargetsDebounce d;
    d.seed("Off;All");                                                       // A
    ASSERT_ACTION(Action::StartWindow, d.step("Off;All;awtrix_p", 1000));    // B @ t0
    ASSERT_ACTION(Action::Waiting,     d.step("Off;All;awtrix_q", 3000));    // C @ t0+2s
    ASSERT_ACTION(Action::Republish,   d.step("Off;All;awtrix_q", 4000));    // t0+3s: settles
    ASSERT_ACTION(Action::Unchanged,   d.step("Off;All;awtrix_q", 4001));    // C was adopted
}

// ============================================================================
// TD7 — seed() adopts the options AND closes the window: the freshly-seeded
// baseline is Unchanged (no spurious first republish — TD1 pins the plain case),
// and a window left open before seed() is dead — the next change opens a fresh
// one instead of republishing off the stale anchor.
// ============================================================================
void test_TD7_seed_adopts_and_closes_window(void) {
    SyncTargetsDebounce d;
    d.seed("Off;All");
    ASSERT_ACTION(Action::StartWindow, d.step("Off;All;awtrix_p", 1000));  // window opens
    d.seed("Off;All;awtrix_p");                                            // baseline mid-window
    ASSERT_ACTION(Action::Unchanged,   d.step("Off;All;awtrix_p", 2000));  // adopted by seed
    // Were the pre-seed window still open, 9000 - 1000 >= 3000 would republish.
    ASSERT_ACTION(Action::StartWindow, d.step("Off;All",          9000));
}

// ============================================================================
// TD8 — the window start is recorded at the FIRST sighting of the change and only
// there: a Waiting step must not slide the anchor. B@1000, B@2000 (Waiting),
// B@4000 ⇒ Republish because 4000 - 1000 >= 3000; an anchor slid to 2000 would
// still be Waiting (4000 - 2000 < 3000).
// ============================================================================
void test_TD8_window_anchored_at_first_sighting(void) {
    SyncTargetsDebounce d;
    d.seed("Off;All");
    ASSERT_ACTION(Action::StartWindow, d.step("Off;All;awtrix_p", 1000));
    ASSERT_ACTION(Action::Waiting,     d.step("Off;All;awtrix_p", 2000));
    ASSERT_ACTION(Action::Republish,   d.step("Off;All;awtrix_p", 4000));
}

// ============================================================================
// TD9 — unsigned (now - sinceMs) wrap arithmetic: a window opened just before the
// millis() rollover still measures its true width after now wraps past zero.
// start + delta reduces mod 2^N for any unsigned long width, so `now` is
// numerically SMALLER than the anchor here; the unsigned subtraction still yields
// delta exactly, where a `now >= since + 3000` comparison would misfire.
// ============================================================================
void test_TD9_window_survives_millis_wrap(void) {
    SyncTargetsDebounce d;
    const unsigned long start = ULONG_MAX - 999;  // 1000 ticks before rollover
    d.seed("Off;All");
    ASSERT_ACTION(Action::StartWindow, d.step("Off;All;awtrix_p", start));
    ASSERT_ACTION(Action::Waiting,     d.step("Off;All;awtrix_p", start + 2000));  // wrapped: now == 1000
    ASSERT_ACTION(Action::Republish,   d.step("Off;All;awtrix_p", start + 3000));  // wrapped: now == 2000
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_TD1_same_sig_unchanged);
    RUN_TEST(test_TD2_change_starts_window);
    RUN_TEST(test_TD3_within_window_waiting);
    RUN_TEST(test_TD4_settled_window_republishes_and_adopts);
    RUN_TEST(test_TD5_revert_cancels_pending_window);
    RUN_TEST(test_TD6_drift_does_not_restart_window);
    RUN_TEST(test_TD7_seed_adopts_and_closes_window);
    RUN_TEST(test_TD8_window_anchored_at_first_sighting);
    RUN_TEST(test_TD9_window_survives_millis_wrap);
    return UNITY_END();
}
