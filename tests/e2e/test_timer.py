"""
Starter harness tests H1 (discovery) and H2 (end-to-end 5 s timer).

Follow-ups H3–H8 are enumerated in the plan file at
~/.claude/plans/review-this-branch-and-enumerated-hummingbird.md and should
be filed as separate issues + PRs.

This suite is contract-only by design (see plan Q12) — it proves things the
native unit tests can't:

  * ArduinoHA actually publishes to the expected MQTT topics
  * The MQTT round-trip works against a real broker
  * The firmware's tick() loop runs fast enough on real FreeRTOS
"""

import time

from conftest import EXPECTED_TIMER_ENTITIES


# ---------------------------------------------------------------------------
# H1 — Discovery
# ---------------------------------------------------------------------------

def test_H1_discovery_emits_all_timer_entities(mqtt_harness):
    """All 7 expected timer entities discovered with valid state topics."""
    missing = [e for e in EXPECTED_TIMER_ENTITIES if not mqtt_harness.has_entity(e)]
    assert not missing, f"Discovery missing entities: {missing}"

    # Every state-bearing entity (skip the action buttons) must advertise a
    # state_topic — without it, no test can observe its state.
    state_bearing = ["timer_dur", "timer_rem", "timer_state", "timer_buz", "timer_fin"]
    for e in state_bearing:
        assert mqtt_harness.entity_state_topic(e), (
            f"Entity {e!r} discovered but has no state_topic in its discovery payload"
        )

    # Each timer button must advertise a command_topic — otherwise H3 can't
    # exercise the HA-button path.
    for e in ["timer_start", "timer_pause", "timer_reset"]:
        assert mqtt_harness.entity_command_topic(e), (
            f"Entity {e!r} discovered but has no command_topic in its discovery payload"
        )


# ---------------------------------------------------------------------------
# H2 — End-to-end 5 s timer
# ---------------------------------------------------------------------------

def test_H2_end_to_end_five_second_timer(timer):
    """Publish a 5 s timer, observe state transitions on real hardware."""
    start = time.monotonic()
    timer.publish_command("timer", {"duration": 5, "action": "start"})

    assert timer.wait_for_entity_state("timer_state", "running", timeout=2.0), \
        "Device did not report 'running' within 2 s of receiving start command"

    # Expect Finished between 5 and 7 wall-clock seconds (±2 s slack for
    # FreeRTOS scheduling and MQTT publish latency).
    assert timer.wait_for_entity_state("timer_state", "finished", timeout=8.0), \
        "Device did not report 'finished' within 8 s for a 5 s timer"

    elapsed = time.monotonic() - start
    assert 4.5 <= elapsed <= 8.0, (
        f"5 s timer reported 'finished' at {elapsed:.2f} s; expected 4.5–8.0 s"
    )
