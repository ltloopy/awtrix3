# Timer e2e harness

End-to-end tests for the AWTRIX timer feature. Maintainer-only local tool —
runs against a real flashed Ulanzi TC001 + an MQTT broker + (optionally)
Home Assistant.

**Not** run in CI. Maintainers run this pre-merge for any timer-touching PR
and paste results into the PR description.

## Prerequisites

- Python 3.10+
- Docker (for the local mosquitto broker; or use your own)
- A Ulanzi TC001 flashed with the branch under test, on the same LAN
- The device must have HA discovery enabled (`HA_DISCOVERY=true` global)

## Setup

```bash
cd tests/e2e
pip install -r requirements.txt
docker compose up -d                 # start local mosquitto on :1883
```

Configure the device to publish to your test broker (via the firmware's web
UI). Confirm HA discovery topics start appearing under `homeassistant/+/+/+/config`.

## Run

```bash
# Minimum: device id (MAC-derived, the suffix after "awtrix_" in the topic prefix)
pytest --device-id=abc123def456

# Full form
pytest \
  --broker=localhost:1883 \
  --device-id=abc123def456 \
  --device-ip=192.168.1.42 \
  --ha-prefix=homeassistant \
  --mqtt-prefix=awtrix_abc123def456
```

Environment variable equivalents: `AWTRIX_TEST_BROKER`, `AWTRIX_TEST_DEVICE_ID`,
`AWTRIX_TEST_DEVICE_IP`.

`--device-ip` is only required for tests that call `/api/reboot` (H6 in the
plan; not in the starter set).

## Starter tests

| Test | What it proves |
| --- | --- |
| H1 | HA discovery emits all 7 timer entities with the right state/command topics |
| H2 | A 5 s timer publishes `running` within 1 s and `finished` within 5–7 wall-clock seconds |

Follow-up tests (H3–H8: button parity, AutoClear timing, Hold dismissal,
persistence across reboot, auto-switch, invalid-JSON safety) are enumerated
in the plan file at
`~/.claude/plans/review-this-branch-and-enumerated-hummingbird.md`.

## Expected runtime

~30 s for the 2 starter tests. The full 8-test suite (after H3–H8 land) runs
~3–5 minutes because tests with the AutoClear/Hold lifecycle wait for the
real `TIMER_FINISHED_HOLD` window (10 s) on hardware — those constants are
not MQTT-tunable today (see plan Q10).

## Troubleshooting

**`Timer entities never discovered within 10 s`**
- Confirm HA discovery is enabled on the device.
- Confirm the device is publishing to the broker you're testing against.
- Run `mosquitto_sub -h localhost -t 'homeassistant/#' -v` and verify
  config topics appear.

**Tests hang on `wait_for_entity_state`**
- The device probably isn't receiving the command. Verify the
  `--mqtt-prefix` matches what the device subscribes to.
- Subscribe to `{prefix}/timer` with `mosquitto_sub` and confirm the
  command payload arrives.
