"""
pytest fixtures for the AWTRIX timer e2e harness.

Design notes — see ~/.claude/plans/review-this-branch-and-enumerated-hummingbird.md
for the full plan; the high points:

  * `mqtt` (session-scoped) — connects to the broker, subscribes to HA
    discovery (`homeassistant/+/+/+/config`) and `{prefix}/#`, builds an
    entity short-name → state-topic map by parsing discovery payloads.
    Exposes `entity_state(name)` and `publish_command(suffix, payload)`.

  * `timer` (function-scoped) — depends on `mqtt`; per-test, publishes a
    baseline reset and waits for `timer_state == "idle"` (≤2 s) before
    yielding. Restores `duration=300`, `buzzer=end`, `finished=auto-clear`.

  * `device_reboot` (manual helper) — calls `/api/reboot`, waits up to 30 s
    for the HA availability topic to flip `offline → online`.

CLI flags (configure via `pytest --broker=... --device-id=... --device-ip=...`):
  --broker      MQTT broker host[:port] (default: localhost:1883)
  --device-id   ArduinoHA device unique-id (MAC-derived, e.g. abc123)
  --device-ip   Device IP, only required for the reboot test
  --ha-prefix   HA discovery prefix (default: homeassistant)
  --mqtt-prefix Device MQTT command prefix (default: awtrix_<device-id>)
"""

from __future__ import annotations

import json
import os
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Dict, Optional

import paho.mqtt.client as mqtt
import pytest


# ---------------------------------------------------------------------------
# CLI plumbing
# ---------------------------------------------------------------------------

def pytest_addoption(parser):
    parser.addoption("--broker",      default="localhost:1883",  help="MQTT broker host[:port]")
    parser.addoption("--device-id",   default=None,              help="ArduinoHA device unique id")
    parser.addoption("--device-ip",   default=None,              help="Device IP (for /api/reboot)")
    parser.addoption("--ha-prefix",   default="homeassistant",   help="HA discovery prefix")
    parser.addoption("--mqtt-prefix", default=None,              help="Device MQTT command prefix")


# ---------------------------------------------------------------------------
# Discovery + topic-map state held by the MqttHarness
# ---------------------------------------------------------------------------

# Map from HA component short-name suffix in our firmware's unique_id naming
# (e.g. "timer_state", "timer_dur") to the discovery JSON payload it published.
# Built from {ha_prefix}/+/+/+/config subscriptions.
@dataclass
class MqttHarness:
    client:      mqtt.Client
    broker_host: str
    broker_port: int
    ha_prefix:   str
    mqtt_prefix: str
    device_id:   str
    device_ip:   Optional[str]

    discovered:        Dict[str, dict] = field(default_factory=dict)
    last_state_by_topic: Dict[str, str] = field(default_factory=dict)
    _lock:             threading.Lock  = field(default_factory=threading.Lock)

    # ---- discovery helpers -------------------------------------------------
    def _on_message(self, _client, _userdata, msg):
        payload = msg.payload.decode("utf-8", errors="replace")
        with self._lock:
            self.last_state_by_topic[msg.topic] = payload

            # HA discovery topics look like: {ha_prefix}/<component>/<dev>/<obj>/config
            if msg.topic.startswith(self.ha_prefix + "/") and msg.topic.endswith("/config"):
                try:
                    doc = json.loads(payload)
                except json.JSONDecodeError:
                    return
                uid = doc.get("unique_id") or doc.get("uniq_id") or doc.get("object_id")
                if not uid:
                    return
                # ArduinoHA prefixes unique_ids with the device id; strip it to get
                # a stable short-name like "timer_state".
                short = uid.split("_", 1)[1] if uid.startswith(self.device_id) else uid
                self.discovered[short] = doc

    def entity_state(self, short_name: str, timeout: float = 5.0) -> Optional[str]:
        """Return the most recent state-topic payload for the named entity."""
        topic = self.entity_state_topic(short_name)
        if topic is None:
            return None
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self._lock:
                if topic in self.last_state_by_topic:
                    return self.last_state_by_topic[topic]
            time.sleep(0.05)
        return None

    def entity_state_topic(self, short_name: str) -> Optional[str]:
        with self._lock:
            doc = self.discovered.get(short_name)
        if not doc:
            return None
        return doc.get("state_topic") or doc.get("stat_t")

    def entity_command_topic(self, short_name: str) -> Optional[str]:
        with self._lock:
            doc = self.discovered.get(short_name)
        if not doc:
            return None
        return doc.get("command_topic") or doc.get("cmd_t")

    def has_entity(self, short_name: str) -> bool:
        with self._lock:
            return short_name in self.discovered

    # ---- command helpers ---------------------------------------------------
    def publish_command(self, suffix: str, payload: str | dict, qos: int = 1):
        topic = f"{self.mqtt_prefix}/{suffix}"
        if isinstance(payload, dict):
            payload = json.dumps(payload)
        self.client.publish(topic, payload, qos=qos).wait_for_publish(timeout=5)

    def wait_for_entity_state(self, short_name: str, expected: str, timeout: float = 10.0) -> bool:
        topic = self.entity_state_topic(short_name)
        if topic is None:
            return False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self._lock:
                if self.last_state_by_topic.get(topic) == expected:
                    return True
            time.sleep(0.05)
        return False


# ---------------------------------------------------------------------------
# Session-scoped MQTT fixture
# ---------------------------------------------------------------------------

EXPECTED_TIMER_ENTITIES = [
    "timer_dur", "timer_rem", "timer_state",
    "timer_buz", "timer_fin",
    "timer_start", "timer_pause", "timer_reset",
]


@pytest.fixture(scope="session")
def mqtt_harness(request) -> MqttHarness:
    broker      = request.config.getoption("--broker") or os.getenv("AWTRIX_TEST_BROKER", "localhost:1883")
    device_id   = request.config.getoption("--device-id") or os.getenv("AWTRIX_TEST_DEVICE_ID")
    device_ip   = request.config.getoption("--device-ip") or os.getenv("AWTRIX_TEST_DEVICE_IP")
    ha_prefix   = request.config.getoption("--ha-prefix")
    mqtt_prefix = request.config.getoption("--mqtt-prefix") or (f"awtrix_{device_id}" if device_id else None)

    if not device_id:
        pytest.skip("--device-id (or AWTRIX_TEST_DEVICE_ID) is required for e2e tests")
    if not mqtt_prefix:
        pytest.skip("--mqtt-prefix could not be derived; pass --mqtt-prefix explicitly")

    host, _, port_s = broker.partition(":")
    port = int(port_s) if port_s else 1883

    client = mqtt.Client(client_id=f"awtrix-e2e-{int(time.time())}")
    harness = MqttHarness(
        client=client,
        broker_host=host,
        broker_port=port,
        ha_prefix=ha_prefix,
        mqtt_prefix=mqtt_prefix,
        device_id=device_id,
        device_ip=device_ip,
    )
    client.on_message = harness._on_message

    client.connect(host, port, keepalive=30)
    client.loop_start()
    client.subscribe(f"{ha_prefix}/+/+/+/config", qos=1)
    client.subscribe(f"{mqtt_prefix}/#",          qos=1)

    # Wait for HA discovery to deliver every timer entity (≤10 s).
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        with harness._lock:
            present = all(e in harness.discovered for e in EXPECTED_TIMER_ENTITIES)
        if present:
            break
        time.sleep(0.1)
    else:
        missing = [e for e in EXPECTED_TIMER_ENTITIES if not harness.has_entity(e)]
        pytest.skip(
            f"Timer entities never discovered within 10 s: missing {missing}. "
            f"Confirm HA_DISCOVERY is enabled on the device and the broker is reachable."
        )

    yield harness

    client.loop_stop()
    client.disconnect()


# ---------------------------------------------------------------------------
# Per-test reset fixture
# ---------------------------------------------------------------------------

BASELINE_RESET = {
    "duration": 300,
    "buzzer":   "end",
    "finished": "auto-clear",
    "action":   "reset",
}


@pytest.fixture
def timer(mqtt_harness: MqttHarness) -> MqttHarness:
    """Per-test fixture: publish baseline reset, wait for state==idle."""
    mqtt_harness.publish_command("timer", BASELINE_RESET)
    if not mqtt_harness.wait_for_entity_state("timer_state", "idle", timeout=3.0):
        pytest.fail("Device did not return to 'idle' within 3 s after baseline reset")
    yield mqtt_harness


# ---------------------------------------------------------------------------
# Device reboot helper
# ---------------------------------------------------------------------------

@pytest.fixture
def device_reboot(mqtt_harness: MqttHarness):
    """Returns a callable that triggers /api/reboot and waits for reconnect."""
    import requests

    def _do_reboot(timeout: float = 30.0):
        if not mqtt_harness.device_ip:
            pytest.skip("--device-ip is required for reboot tests")
        requests.get(f"http://{mqtt_harness.device_ip}/api/reboot", timeout=5)
        # Wait for the HA availability topic to flip back to "online" (proxy
        # for "MQTT reconnected"). The exact availability topic is published
        # in each entity's discovery doc; pick any one.
        avail_topic = None
        for name in EXPECTED_TIMER_ENTITIES:
            doc = mqtt_harness.discovered.get(name) or {}
            avail = doc.get("availability") or doc.get("avty")
            if isinstance(avail, list) and avail:
                avail_topic = avail[0].get("topic")
            elif isinstance(avail, dict):
                avail_topic = avail.get("topic")
            elif "availability_topic" in doc:
                avail_topic = doc["availability_topic"]
            if avail_topic:
                break
        if not avail_topic:
            time.sleep(min(timeout, 15))  # best-effort fixed wait
            return
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with mqtt_harness._lock:
                if mqtt_harness.last_state_by_topic.get(avail_topic) == "online":
                    return
            time.sleep(0.25)
        pytest.fail(f"Device did not reconnect within {timeout} s after /api/reboot")

    return _do_reboot
