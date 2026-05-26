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

import pytest

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


# ---------------------------------------------------------------------------
# H9 — Duration as a clock string (HH:MM:SS) round-trips and normalizes
# ---------------------------------------------------------------------------

def test_H9_duration_clock_string_round_trip(timer):
    """A "HH:MM:SS" duration is accepted; timer_dur reports the trimmed value."""
    # Clock-string duration starts a 10-minute timer; the duration entity
    # reflects the trimmed canonical form "10:00".
    timer.publish_command("timer", {"duration": "00:10:00", "action": "start"})
    assert timer.wait_for_entity_state("timer_dur", "10:00", timeout=3.0), \
        "timer_dur did not report the trimmed clock string '10:00'"
    assert timer.wait_for_entity_state("timer_state", "running", timeout=2.0), \
        "Device did not report 'running' after a clock-string duration"

    # Reset back to idle so the next assertion isn't racing the countdown.
    timer.publish_command("timer", {"action": "reset"})

    # Legacy numeric seconds still work and normalize to the trimmed clock form.
    timer.publish_command("timer", {"duration": 120})
    assert timer.wait_for_entity_state("timer_dur", "2:00", timeout=3.0), \
        "Numeric duration 120 did not normalize to '2:00' on timer_dur"


# ---------------------------------------------------------------------------
# H10 — Invalid duration is rejected (not clamped) on the MQTT/entity surfaces
# ---------------------------------------------------------------------------

def test_H10_invalid_duration_rejected_not_clamped(timer):
    """Out-of-range and malformed durations leave the previous valid time in place."""
    dur_cmd = timer.entity_command_topic("timer_dur")
    assert dur_cmd, "timer_dur has no command_topic"

    # Baseline a known valid duration via the HA entity command topic.
    timer.client.publish(dur_cmd, "10:00", qos=1).wait_for_publish(timeout=5)
    assert timer.wait_for_entity_state("timer_dur", "10:00", timeout=3.0), \
        "entity command path did not set duration to 10:00"

    # Out-of-range via the entity: must revert to 10:00, never clamp to 24:00:00.
    timer.client.publish(dur_cmd, "30:00:00", qos=1).wait_for_publish(timeout=5)
    assert not timer.wait_for_entity_state("timer_dur", "24:00:00", timeout=2.0), \
        "out-of-range entry clamped to 24:00:00 instead of reverting"
    assert timer.entity_state("timer_dur", timeout=3.0) == "10:00", \
        "out-of-range entry did not leave the previous valid time in place"

    # Malformed via the entity: also reverts.
    timer.client.publish(dur_cmd, "banana", qos=1).wait_for_publish(timeout=5)
    assert timer.entity_state("timer_dur", timeout=3.0) == "10:00"

    # Out-of-range via the MQTT command topic: silently ignored, value unchanged.
    timer.publish_command("timer", {"duration": 99999})
    assert not timer.wait_for_entity_state("timer_dur", "27:46:39", timeout=2.0), \
        "MQTT out-of-range command was applied instead of rejected"
    assert timer.entity_state("timer_dur", timeout=3.0) == "10:00"


# ---------------------------------------------------------------------------
# H11 — POST /api/timer returns 200 / 400 (requires --device-ip)
# ---------------------------------------------------------------------------

def test_H11_http_status_codes(timer):
    """The HTTP API surfaces validation as status codes."""
    import requests

    if not timer.device_ip:
        pytest.skip("--device-ip is required for HTTP status-code tests")
    url = f"http://{timer.device_ip}/api/timer"

    # Valid → 200 OK.
    r = requests.post(url, data='{"duration":"5:00","action":"reset"}', timeout=5)
    assert r.status_code == 200, f"valid command returned {r.status_code}: {r.text!r}"

    # Malformed JSON → 400 ErrorParsingJson.
    r = requests.post(url, data="{bad json", timeout=5)
    assert r.status_code == 400 and "ErrorParsingJson" in r.text, \
        f"malformed JSON returned {r.status_code}: {r.text!r}"

    # Out-of-range duration → 400 InvalidValue (rejected, not clamped).
    r = requests.post(url, data='{"duration":"30:00:00"}', timeout=5)
    assert r.status_code == 400 and "InvalidValue" in r.text, \
        f"out-of-range returned {r.status_code}: {r.text!r}"

    # Malformed duration value → 400 InvalidValue.
    r = requests.post(url, data='{"duration":"banana"}', timeout=5)
    assert r.status_code == 400 and "InvalidValue" in r.text, \
        f"malformed value returned {r.status_code}: {r.text!r}"

    # (409 TimerDisabled requires SHOW_TIMER=false; exercised manually — see
    # TIMER_TEST_PLAN.md.)
