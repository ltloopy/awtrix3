---
plan_version: 1
feature: timer
branch: feat-timer-app
last_updated: 2026-05-18
---

# AWTRIX Smart Clock — Timer App Manual Test Plan

Manual test plan covering all features added by the `feat-timer-app` branch of the AWTRIX Smart Clock firmware. Replaces the prior smoke checklist at `docs/test-plans/timer.md`.

---

## 1. Introduction

### Objective

Validate the Timer App on the `feat-timer-app` branch across every control surface and on real hardware before merge. Confirm parity between control surfaces, correct handling of configuration, persistence semantics, on-device UX, audio behaviour, and graceful handling of invalid input and edge conditions.

### In scope

- `dev.json` Timer configuration keys (12 keys: `show_timer`, `timer_max_duration`, `timer_step`, `timer_publish_interval`, `timer_finished_hold`, `timer_realert_interval`, `timer_countdown_seconds`, `timer_config_timeout`, `timer_icon_idle/running/paused/finished`)
- REST API: `POST /api/timer`, `POST /api/switch` (with `fast` flag), `GET`/`POST /api/settings` `TIMER` key
- MQTT: command topic `{prefix}/timer`, retained status topic `{prefix}/timer/icons`, published `timer_rem` / `timer_state` cadence
- Home Assistant: all 8 discovered entities (`timer_dur`, `timer_rem`, `timer_state`, `timer_buz`, `timer_fin`, `timer_start`, `timer_pause`, `timer_reset`)
- On-device hardware: physical button gestures, display rendering, buzzer audio
- Persistence: NVS round-trip; intentional non-persistence of runtime state
- Edge / boundary / negative cases

### Out of scope

- Pre-existing apps (Time, Date, Temp, Humidity, Battery)
- Firmware OTA, Wi-Fi provisioning, MQTT broker bring-up, HA installation
- Non-timer notifications (only their interaction with the timer queue is tested)
- Web UI styling / theme

### Sign-off requirements

The tester must record the following at the top of the completed plan:

- Device firmware version (shown in web UI footer)
- `git rev-parse HEAD` of the `feat-timer-app` branch at test time
- Hardware variant (Ulanzi TC001 / awtrix2_upgrade)
- MQTT broker + HA versions

The **Status** column on each test row is filled in by the tester with one of: `Pass` (every Expected Result met), `Fail` (one or more Expected Results not met — file a defect per Section 5), or `Skipped` (case not executed — record reason in PR sign-off). A blank Status at sign-off time means the case was not run.

Every **P1** case must record `Pass` or be explicitly waived (`WAIVED: <reason>` in the Status cell) with sign-off recorded in the PR.

---

## 2. Types of Testing Required

Brief definitions tailored to this plan. Each test case may exercise more than one type — the table cases below name the dominant type implicitly by the section they sit in.

| Type | What it proves |
| --- | --- |
| **Functional** | Each control surface produces the documented effect on the documented state machine. |
| **Configuration** | `dev.json` keys load at boot, clamp out-of-range values, override NVS on every boot. |
| **Integration** | API/MQTT/HA share a common payload schema and converge on the same end state. HA discovery round-trips correctly. |
| **UI / Display** | Text centering, progress bar drain direction, 500 ms blink cadence, `fast` flag app-switch animation suppression. |
| **Audio** | Buzzer modes Off / End / Countdown behave per spec; melody file fallback is silent on missing file. |
| **Persistence** | `duration`, `buzzer`, `finished`, and per-state icons survive reboot (NVS). Runtime state (Running/Paused/remaining) intentionally does **not** survive. |
| **Negative / Boundary** | Out-of-range durations clamp, garbage JSON is a no-op, unknown action strings are ignored without crash. |
| **Regression** | Cross-build sanity on `awtrix2_upgrade`; existing apps unaffected by Timer presence. |

---

## 3. Testing Strategy

### Execution order

To minimise wasted setup and surface blockers early:

1. **`DEV-*`** first — boot-time config requires reboot per change; slowest to iterate. Surface blockers before broader testing.
2. **`HW-*`** while at the device — physical button gestures and display/audio observations are easiest while the tester is still in front of the hardware.
3. **`API-*`** — fastest iteration; full per-field payload coverage lives here.
4. **`MQTT-*`** — adds broker dependency; covers MQTT-specific behaviours and one parity smoke against API.
5. **`HA-*`** — adds HA dependency; covers HA-specific concerns (discovery, write-back, availability, retained cleanup).
6. **`EDGE-*`** — last, since they often combine multiple surfaces and assume the basics work.

### Parity principle (avoids duplication)

`/api/timer` and `{prefix}/timer` accept identical JSON payloads. HA buttons map to the same `{"action":"..."}` payloads. To avoid testing the same scenario three times:

- **API section** enumerates every payload field × valid/invalid value once.
- **MQTT section** covers only what MQTT introduces (retained `/timer/icons` topic, reconnect republish, prefix mismatch, invalid JSON safety, publish cadence) plus **one** explicit parity smoke (`MQTT-01`).
- **HA section** covers HA-unique concerns (8-entity discovery, entity options/units, write-back via number/select, `dismiss` button, availability when `SHOW_TIMER=false`, retained discovery cleanup).

### Automated coverage (do **not** duplicate manually)

Unit tests `U1`–`U13` in [`test/test_timer/test_timer.cpp`](test/test_timer/test_timer.cpp) already prove the following on every PR via [`.github/workflows/test.yml`](.github/workflows/test.yml). Skip these from the manual plan:

- Duration clamping math (`0` → `1`, `99999` → `TIMER_MAX_DURATION`)
- `parseCommand` null / empty / garbage / unknown action safety
- State publish ordering: `state` topic before `remaining` on `start`
- `enterFinished()` side effects: end melody plays exactly once; no overlay notification
- Pause toggle (Running ↔ Paused)
- Icon fallback resolution (per-state empty → idle slot)
- `reset()` and `start()` from `Finished` state

E2e starter tests `H1`–`H2` in [`tests/e2e/test_timer.py`](tests/e2e/test_timer.py) prove HA discovery emits all entities and a 5 s timer round-trips state correctly. Maintainers run these pre-merge.

The manual plan focuses on **user-visible integration** that automation cannot reach: display rendering, audio output, hardware buttons, multi-surface command races, GIF animation lifecycle, dev.json boot behaviour.

---

## 4. Test Cases

### Test environment setup

Define these once. Each test row's **Pre-requisites** column references SETUP IDs.

| Setup ID | Description |
| --- | --- |
| `SETUP-BASE` | Device powered on, flashed with `feat-timer-app` branch, Wi-Fi connected, web UI reachable at `http://<IP>/`, serial monitor open at 115200 baud. Default `dev.json` (no overrides) unless a `DEV-*` case mutates it. |
| `SETUP-MQTT` | `SETUP-BASE` plus: MQTT broker reachable; device configured to publish under `<PREFIX>` (= `awtrix_<id>`); `mosquitto_sub -h <BROKER> -t '<PREFIX>/#' -v` running in a side terminal. |
| `SETUP-HA` | `SETUP-MQTT` plus: Home Assistant instance subscribed to `<HA>` discovery prefix (`homeassistant` by default); device boot-completed at least 30 s ago so discovery has flushed. |
| `SETUP-FILES` | `SETUP-BASE` plus: `/ICONS/timer_a.jpg`, `/ICONS/timer_b.gif`, and `/MELODIES/timer_end.txt` already uploaded via web UI file manager. |
| `SETUP-TIMER-IDLE` | Apply `SETUP-BASE`; ensure timer state is `idle` (publish `{"action":"reset"}` or wait for auto-clear). Default `duration=60`, `buzzer=end`, `finished=auto-clear`. |
| `SETUP-TIMER-RUN-30` | `SETUP-TIMER-IDLE` plus: timer started with `duration=30`, currently `running`. |
| `SETUP-TIMER-FINISHED` | `SETUP-TIMER-IDLE` with `finished=hold`, started with `duration=2`, then wait ≥3 s so state is `finished` with notification on screen. |

### Placeholders used in test steps

Defined once; substitute on execution.

| Token | Meaning |
| --- | --- |
| `<IP>` | Device IP (e.g. `192.168.1.42`) |
| `<BROKER>` | MQTT broker host:port (e.g. `localhost:1883`) |
| `<PREFIX>` | Device MQTT prefix (e.g. `awtrix_abc123`) |
| `<HA>` | HA discovery prefix (default `homeassistant`) |
| `<ID>` | Device entity-id suffix used in HA entity ids (e.g. `abc123def456`) |

### Priority

- **P1** — blocker; must pass for merge
- **P2** — high; should pass for merge, can be waived with sign-off
- **P3** — nice-to-have; track as defect but does not block merge

### Status column

Tester fills the rightmost column of every test row with `Pass`, `Fail`, or `Skipped` (see Sign-off requirements above). Default value `_____` is left blank until executed.

---

### 4.1 `DEV-*` — dev.json configuration

`dev.json` is read at boot only ([docs/dev.md](docs/dev.md)). Each `DEV-*` case requires a reboot to take effect.

| Test ID | Test Description | Pre-requisites | Test Steps | Expected Results | Status |
| --- | --- | --- | --- | --- | :---: |
| `DEV-01` | [P1] `show_timer:false` removes Timer from all four surfaces at boot | `SETUP-HA` | 1. Upload `/dev.json` = `{"show_timer": false}` via web UI file manager.<br>2. Power-cycle the device.<br>3. Observe app rotation for one full cycle.<br>4. `mosquitto_sub -h <BROKER> -t '<HA>/#' -v \| grep timer`.<br>5. `curl -X POST http://<IP>/api/timer -d '{"action":"start"}'`. | (3) Timer app absent from rotation. (4) No `timer_*` HA discovery topics present. (5) HTTP 200 with body `OK`, but state remains `idle`; serial log shows the command was ignored. | _____ |
| `DEV-02` | [P1] Removing `show_timer` from `dev.json` restores Timer surfaces | `SETUP-HA` with `DEV-01` applied | 1. Remove `show_timer` key from `/dev.json` (or set to `true`).<br>2. Power-cycle.<br>3. Wait ≥30 s. | Timer app reappears in rotation. All 8 HA entities re-discovered. `POST /api/timer` and `{prefix}/timer` accept commands again. | _____ |
| `DEV-03` | [P1] `timer_max_duration` clamps duration on persist and via API | `SETUP-MQTT` | 1. Upload `{"timer_max_duration": 60}` to `/dev.json`.<br>2. Power-cycle.<br>3. `curl -X POST http://<IP>/api/timer -d '{"duration": 9999}'`.<br>4. `mosquitto_sub -h <BROKER> -t '<PREFIX>/stats/timer_dur'`.<br>5. Power-cycle again, observe persisted value. | (3) HTTP 200. (4) Published duration = 60 (clamped). (5) After reboot, duration is still 60. | _____ |
| `DEV-04` | [P2] `timer_publish_interval=0` disables `timer_rem` periodic publishes | `SETUP-MQTT` | 1. `{"timer_publish_interval": 0}` in `/dev.json`, reboot.<br>2. `mosquitto_sub -h <BROKER> -t '<PREFIX>/stats/timer_rem' -v`.<br>3. `curl -X POST http://<IP>/api/timer -d '{"duration":60,"action":"start"}'`. | `timer_rem` publishes only on state changes (start, pause, resume, reset, finish). No periodic 1-second publishes during steady-state Running. | _____ |
| `DEV-05` | [P2] `timer_finished_hold` clamps out-of-range values silently | `SETUP-BASE` | 1. Upload `{"timer_finished_hold": 1000}` to `/dev.json`. Reboot.<br>2. Set `finished=auto-clear`, start a 2 s timer, wait for expiry.<br>3. Repeat with `{"timer_finished_hold": 30}`. | (2) Hold time falls back to default (10 s) because 1000 is out of range (1–300). (3) Hold time is 30 s on expiry. | _____ |
| `DEV-06` | [P2] `timer_realert_interval` clamps; re-alert cadence reflects valid values | `SETUP-BASE` | 1. `{"timer_realert_interval": 5}` in `/dev.json`, reboot.<br>2. Set `finished=re-alert`, `buzzer=end`, start 2 s timer, wait.<br>3. Time the gap between re-alert melody plays. | (3) Re-alert melody re-fires every ~5 s until dismissed. | _____ |
| `DEV-07` | [P3] `timer_countdown_seconds` adjusts pre-expiry beep window | `SETUP-BASE` | 1. `{"timer_countdown_seconds": 5}` in `/dev.json`, reboot.<br>2. `curl -X POST http://<IP>/api/timer -d '{"duration":10,"buzzer":"countdown","action":"start"}'`.<br>3. Listen for beeps in the final seconds. | A short beep is audible for each of the final 5 seconds (10→6, 6→5, 5→4, 4→3, 3→2, 2→1) — five beeps before the end melody. | _____ |
| `DEV-08` | [P2] `timer_config_timeout` controls on-device config mode auto-exit | `SETUP-BASE` | 1. `{"timer_config_timeout": 5}` in `/dev.json`, reboot.<br>2. Navigate to Timer app, long-press middle to enter config.<br>3. Wait 6 s without touching buttons. | Config mode exits ~5 s after last input; HH:MM:SS value is persisted. | _____ |
| `DEV-09` | [P1] `timer_icon_idle` set in `dev.json` overrides NVS on every boot | `SETUP-MQTT`, `SETUP-FILES` | 1. `mosquitto_pub -h <BROKER> -t '<PREFIX>/timer' -m '{"icon_idle":"timer_b"}'` (writes NVS).<br>2. Confirm Idle state renders `timer_b`.<br>3. Upload `/dev.json` = `{"timer_icon_idle":"timer_a"}`. Reboot.<br>4. Observe icon in Idle state.<br>5. Remove key from `/dev.json`. Reboot. | (4) Icon is `timer_a` (dev.json wins over NVS at boot). (5) Icon falls back to `timer_b` (NVS retained, dev.json no longer overrides). | _____ |
| `DEV-10` | [P2] `timer_icon_running` empty string falls back to `timer_icon_idle` | `SETUP-FILES` | 1. `{"timer_icon_idle":"timer_a","timer_icon_running":""}` in `/dev.json`. Reboot.<br>2. Start a 60 s timer, observe Running state. | Running state renders `timer_a` (idle fallback) — not the built-in 8×8 pixel-art icon. | _____ |

---

### 4.2 `API-*` — REST API

| Test ID | Test Description | Pre-requisites | Test Steps | Expected Results | Status |
| --- | --- | --- | --- | --- | :---: |
| `API-01` | [P1] `action:start` from Idle starts timer with saved duration | `SETUP-TIMER-IDLE` | 1. `curl -X POST http://<IP>/api/timer -d '{"action":"start"}'`.<br>2. Observe Timer app display.<br>3. `curl http://<IP>/api/stats` and inspect `timer_state`. | (1) HTTP 200 body `OK`. (2) Timer app foregrounds (auto-switch from current app) and shows draining bar + counting time. (3) `timer_state=running`, `timer_dur=60`, `timer_rem` decreases. | _____ |
| `API-02` | [P1] `duration` sets and persists; valid range accepted | `SETUP-MQTT`, `SETUP-TIMER-IDLE` | 1. `curl -X POST http://<IP>/api/timer -d '{"duration":300}'`.<br>2. Reboot device.<br>3. `curl http://<IP>/api/stats`. | After reboot: `timer_dur=300`, `timer_state=idle`. | _____ |
| `API-03` | [P1] `duration` clamping: 0 → 1; over-max → `TIMER_MAX_DURATION` | `SETUP-MQTT`, `SETUP-TIMER-IDLE` | 1. `curl -X POST http://<IP>/api/timer -d '{"duration":0}'`.<br>2. Inspect `<PREFIX>/stats/timer_dur`.<br>3. `curl -X POST http://<IP>/api/timer -d '{"duration":99999}'` (default `TIMER_MAX_DURATION` = 86400).<br>4. Inspect again. | (2) `timer_dur=1`. (4) `timer_dur=86400`. Both calls return HTTP 200. | _____ |
| `API-04` | [P2] `duration` non-numeric / wrong type is ignored | `SETUP-TIMER-IDLE` | 1. `curl -X POST http://<IP>/api/timer -d '{"duration":"abc"}'`.<br>2. `curl -X POST http://<IP>/api/timer -d '{"duration":null}'`.<br>3. `curl http://<IP>/api/stats`. | HTTP 200 on both; `timer_dur` unchanged from prior value. Serial log shows no crash. | _____ |
| `API-05` | [P1] `buzzer` accepts `off`/`end`/`countdown` case-insensitively; persists | `SETUP-MQTT`, `SETUP-TIMER-IDLE` | 1. `curl -X POST http://<IP>/api/timer -d '{"buzzer":"OFF"}'`.<br>2. Inspect `<PREFIX>/stats/timer_buz`.<br>3. Repeat with `"End"` and `"countdown"`.<br>4. Reboot, inspect persisted value. | Each value accepted and reflected on the stats topic. Final reboot shows `timer_buz=countdown`. | _____ |
| `API-06` | [P2] `buzzer` invalid value is ignored, state unchanged | `SETUP-TIMER-IDLE` with `buzzer=end` | 1. `curl -X POST http://<IP>/api/timer -d '{"buzzer":"loud"}'`.<br>2. Inspect `<PREFIX>/stats/timer_buz`. | `timer_buz` still `end`. HTTP 200. No crash. | _____ |
| `API-07` | [P1] `finished` accepts `auto-clear`/`autoclear`/`hold`/`re-alert`/`realert` | `SETUP-MQTT`, `SETUP-TIMER-IDLE` | For each of the 5 strings: `curl -X POST http://<IP>/api/timer -d '{"finished":"<value>"}'` and inspect `<PREFIX>/stats/timer_fin`. | Each value is accepted; `autoclear` and `auto-clear` produce identical state (and the same for `realert`/`re-alert`). | _____ |
| `API-08` | [P1] `icon_*` set, clear, fallback chain | `SETUP-FILES`, `SETUP-MQTT`, `SETUP-TIMER-IDLE` | 1. `curl -X POST http://<IP>/api/timer -d '{"icon_idle":"timer_a","icon_running":"timer_b"}'`.<br>2. Observe Idle then start a timer.<br>3. `curl -X POST http://<IP>/api/timer -d '{"icon_running":""}'`. Start a new timer. | (2) Idle shows `timer_a.jpg`; Running shows `timer_b.gif` animated. (3) Running now falls back to `timer_a.jpg` (idle fallback). Retained `<PREFIX>/timer/icons` reflects the cleared slot. | _____ |
| `API-09` | [P2] `icon_*` >32 chars is truncated/rejected, never crashes | `SETUP-MQTT`, `SETUP-TIMER-IDLE` | 1. `curl -X POST http://<IP>/api/timer -d '{"icon_idle":"a-string-over-thirty-two-characters-long-xxxxxxxxxxxx"}'`.<br>2. Inspect `<PREFIX>/timer/icons`. | Stored value is ≤32 chars (truncated or rejected). HTTP 200, no crash. | _____ |
| `API-10` | [P1] `action:pause` toggles Running↔Paused | `SETUP-TIMER-RUN-30` | 1. `curl -X POST http://<IP>/api/timer -d '{"action":"pause"}'`.<br>2. Wait 3 s, observe display.<br>3. `curl -X POST http://<IP>/api/timer -d '{"action":"pause"}'`.<br>4. Observe display. | (2) Time text frozen, progress bar holds. (4) Counting resumes from the held value. | _____ |
| `API-11` | [P1] `action:reset` from any state returns to Idle | `SETUP-TIMER-FINISHED` | 1. `curl -X POST http://<IP>/api/timer -d '{"action":"reset"}'`.<br>2. Observe display + `<PREFIX>/stats/timer_state`. | End melody (if still re-alerting) stops within 1 s. State = `idle`, display shows configured duration text, no blinking `0:00`. | _____ |
| `API-12` | [P1] `POST /api/switch {"name":"Timer","fast":true}` skips transition animation | `SETUP-BASE` | 1. Ensure current app is Time (or any non-Timer app).<br>2. `curl -X POST http://<IP>/api/switch -d '{"name":"Timer","fast":true}'`.<br>3. Repeat with `"fast":false`. | (2) Timer app appears instantly with no slide / fade transition. (3) Configured transition effect plays. | _____ |
| `API-13` | [P1] `POST /api/settings {"TIMER":false}` runtime toggle without reboot | `SETUP-HA`, `SETUP-TIMER-RUN-30` | 1. `curl http://<IP>/api/settings` confirm `"TIMER":true`.<br>2. `curl -X POST http://<IP>/api/settings -d '{"TIMER":false}'`.<br>3. Observe rotation and HA entity availability.<br>4. Reboot, `curl http://<IP>/api/settings`. | (2) HTTP 200. (3) Timer app removed from rotation immediately. Active timer reset (no expiry alert later). HA entities go unavailable. (4) `TIMER` persists as `false` after reboot. | _____ |

---

### 4.3 `MQTT-*` — MQTT

| Test ID | Test Description | Pre-requisites | Test Steps | Expected Results | Status |
| --- | --- | --- | --- | --- | :---: |
| `MQTT-01` | [P1] **Parity smoke**: same payload via MQTT yields same state as `API-01` | `SETUP-MQTT`, `SETUP-TIMER-IDLE` | 1. `mosquitto_pub -h <BROKER> -t '<PREFIX>/timer' -m '{"action":"start"}'`.<br>2. Observe display + `<PREFIX>/stats/timer_state`. | Identical observable result as `API-01`: state transitions to `running`, app foregrounds, time counts down. | _____ |
| `MQTT-02` | [P1] Retained `<PREFIX>/timer/icons` published on first MQTT connect | `SETUP-MQTT` | 1. Disconnect MQTT (unplug broker or block port).<br>2. Reconnect.<br>3. `mosquitto_sub -h <BROKER> -t '<PREFIX>/timer/icons' -v -C 1` (capture next retained message). | A retained JSON `{"idle":"...","running":"...","paused":"...","finished":"..."}` is delivered immediately on subscribe, even after the device has reconnected. | _____ |
| `MQTT-03` | [P2] Changing any `icon_*` republishes retained `<PREFIX>/timer/icons` | `SETUP-MQTT`, `SETUP-FILES` | 1. `mosquitto_sub -h <BROKER> -t '<PREFIX>/timer/icons' -v` (running).<br>2. `mosquitto_pub -h <BROKER> -t '<PREFIX>/timer' -m '{"icon_running":"timer_b"}'`.<br>3. Observe retained-topic update. | Within 1 s, the subscribed topic emits the updated JSON with `"running":"timer_b"` and the message is marked retained. | _____ |
| `MQTT-04` | [P1] Invalid JSON on `<PREFIX>/timer` is a no-op (no crash) | `SETUP-MQTT`, `SETUP-TIMER-RUN-30` | 1. `mosquitto_pub -h <BROKER> -t '<PREFIX>/timer' -m 'not valid json'`.<br>2. `mosquitto_pub -h <BROKER> -t '<PREFIX>/timer' -m '{'`.<br>3. Observe serial log + state. | No crash. State unchanged (still `running`). Optional debug log entry but no reboot. | _____ |
| `MQTT-05` | [P2] Unknown JSON keys ignored, valid keys still applied in same payload | `SETUP-MQTT`, `SETUP-TIMER-IDLE` | 1. `mosquitto_pub -h <BROKER> -t '<PREFIX>/timer' -m '{"foo":"bar","duration":120,"action":"start"}'`.<br>2. Observe state and duration. | `timer_dur=120`, state transitions to `running`. Unknown `foo` key silently ignored. | _____ |
| `MQTT-06` | [P1] Publishing to a wrong-prefix topic is a no-op | `SETUP-MQTT`, `SETUP-TIMER-IDLE` | 1. `mosquitto_pub -h <BROKER> -t 'wrongprefix/timer' -m '{"action":"start"}'`.<br>2. Observe state. | State remains `idle`. No log entry. | _____ |
| `MQTT-07` | [P2] `timer_rem` publishes once per second while Running (with default `TIMER_PUBLISH_INTERVAL=1`) | `SETUP-MQTT`, `SETUP-TIMER-IDLE` | 1. `mosquitto_sub -h <BROKER> -t '<PREFIX>/stats/timer_rem' -v` (timestamps enabled).<br>2. `mosquitto_pub -h <BROKER> -t '<PREFIX>/timer' -m '{"duration":10,"action":"start"}'`.<br>3. Observe publish cadence over 10 s. | One publish per second (~10 messages) with monotonically decreasing values 10→0. | _____ |
| `MQTT-08` | [P1] `timer_state` published on every transition | `SETUP-MQTT`, `SETUP-TIMER-IDLE` | 1. `mosquitto_sub -h <BROKER> -t '<PREFIX>/stats/timer_state' -v`.<br>2. Send `start`, `pause`, `pause` (resume), wait for `finished`, `reset`. | Five state messages observed in order: `running`, `paused`, `running`, `finished`, `idle`. | _____ |

---

### 4.4 `HA-*` — Home Assistant

| Test ID | Test Description | Pre-requisites | Test Steps | Expected Results | Status |
| --- | --- | --- | --- | --- | :---: |
| `HA-01` | [P1] All 8 timer entities discovered within 30 s of boot | `SETUP-HA` | 1. Power-cycle device.<br>2. `mosquitto_sub -h <BROKER> -t '<HA>/+/+/<ID>/config' -v` for ≥30 s.<br>3. In HA UI, search the device's integration. | All 8 entities present: `{ID}_timer_dur` (number), `{ID}_timer_rem` (sensor), `{ID}_timer_state` (sensor), `{ID}_timer_buz` (select), `{ID}_timer_fin` (select), `{ID}_timer_start` (button), `{ID}_timer_pause` (button), `{ID}_timer_reset` (button). | _____ |
| `HA-02` | [P1] `timer_buz` and `timer_fin` selects show correct options | `SETUP-HA` | 1. In HA UI open `timer_buz`.<br>2. Open `timer_fin`. | (1) Options exactly `Off`, `End`, `Countdown`. (2) Options exactly `Auto-Clear`, `Hold`, `Re-Alert`. | _____ |
| `HA-03` | [P1] Writing `timer_dur` from HA UI updates device | `SETUP-HA`, `SETUP-TIMER-IDLE` | 1. In HA, set `{ID}_timer_dur` to 600.<br>2. `curl http://<IP>/api/stats`.<br>3. Inspect `<PREFIX>/stats/timer_dur`. | `timer_dur=600` reflected on the device within 2 s. | _____ |
| `HA-04` | [P1] `timer_start` button drives state to Running | `SETUP-HA`, `SETUP-TIMER-IDLE` | 1. In HA, press `{ID}_timer_start`.<br>2. Observe `{ID}_timer_state` sensor and device display. | `timer_state` flips to `running` within 2 s; Timer app foregrounds on the device. | _____ |
| `HA-05` | [P1] `timer_pause` toggles, `timer_reset` returns to Idle | `SETUP-HA`, `SETUP-TIMER-RUN-30` | 1. Press `timer_pause` in HA; observe state.<br>2. Press `timer_pause` again; observe state.<br>3. Press `timer_reset`. | (1) `paused`. (2) `running`. (3) `idle` immediately. | _____ |
| `HA-06` | [P2] `timer_rem` sensor updates every `TIMER_PUBLISH_INTERVAL` while Running | `SETUP-HA`, `SETUP-TIMER-IDLE` | 1. Start a 30 s timer.<br>2. Watch `{ID}_timer_rem` sensor history in HA. | Sensor value decreases by ~1 per second until 0. | _____ |
| `HA-07` | [P2] `dismiss` button clears `hold` mode notification and returns to Idle | `SETUP-HA`, `SETUP-TIMER-FINISHED` (finished=hold) | 1. Confirm `0:00` is blinking.<br>2. Press `{ID}_dismiss` button in HA. | Blinking stops; state → `idle`. End melody silenced. | _____ |
| `HA-08` | [P1] `SHOW_TIMER=false` empties retained discovery so HA prunes entities on next reconnect | `SETUP-HA` | 1. `mosquitto_sub -h <BROKER> -t '<HA>/+/+/<ID>/config' -v -W 10` (capture for 10 s).<br>2. `curl -X POST http://<IP>/api/settings -d '{"TIMER":false}'`.<br>3. Reboot HA's MQTT integration or restart HA.<br>4. Re-list entities. | After step 2, retained payloads on the 8 timer config topics are empty. After (3), HA no longer shows the 8 timer entities. | _____ |

---

### 4.5 `HW-*` — On-Device hardware

| Test ID | Test Description | Pre-requisites | Test Steps | Expected Results | Status |
| --- | --- | --- | --- | --- | :---: |
| `HW-01` | [P1] Middle short-press from Idle (on Timer app) starts timer | `SETUP-TIMER-IDLE` | 1. Navigate rotation to Timer app.<br>2. Middle short-press. | Timer transitions to `running`, display shows draining bar + counting time. | _____ |
| `HW-02` | [P1] Middle short-press toggles Running↔Paused; long-press resets to Idle | `SETUP-TIMER-RUN-30` | 1. Middle short-press, observe.<br>2. Middle short-press again.<br>3. Middle long-press. | (1) Paused (text frozen). (2) Running again. (3) Idle (display shows configured duration). | _____ |
| `HW-03` | [P1] Middle long-press from Idle enters config mode; HH:MM:SS cycle works | `SETUP-TIMER-IDLE` | 1. Long-press middle. Observe HH field highlighted.<br>2. Short-press middle. Observe MM highlighted.<br>3. Short-press middle twice more. | Config mode entered. Field cycles `HH → MM → SS → HH`. Underline cursor moves with the highlighted field. | _____ |
| `HW-04` | [P2] Left/Right adjust active field by `TIMER_STEP`; auto-repeat after 500 ms | `SETUP-TIMER-IDLE`, in config mode on MM field | 1. Single-press Right; observe.<br>2. Press and hold Right for ≥1 s. | (1) MM increments by 1. (2) After ~500 ms hold, value auto-increments every ~250 ms. | _____ |
| `HW-05` | [P2] Field bounds wrap: HH `99↔0`, MM/SS `59↔0` | `SETUP-TIMER-IDLE`, config mode | 1. On HH=99, press Right.<br>2. On MM=59, press Right.<br>3. On HH=0, press Left. | (1) HH wraps to 0. (2) MM wraps to 0. (3) HH wraps to 99. | _____ |
| `HW-06` | [P1] `buzzer=end` plays melody once on expiry; `buzzer=off` is silent; `buzzer=countdown` beeps + melody | `SETUP-TIMER-IDLE` | For each of `off`, `end`, `countdown`: set `buzzer`, start 5 s timer, listen. | Off: silent. End: end melody once at 0. Countdown: short beep at 3 s, 2 s, 1 s, then end melody. | _____ |
| `HW-07` | [P1] Finished state display: blinks `0:00` at 500 ms cadence with no progress bar | `SETUP-TIMER-IDLE` with `finished=hold` | 1. Start a 2 s timer.<br>2. After expiry, watch display for ≥5 s.<br>3. Time the blink intervals. | `0:00` blinks: visible 500 ms, hidden 500 ms. Icon stays visible. No progress bar. | _____ |
| `HW-08` | [P1] Cross-build sanity: Timer app works on `awtrix2_upgrade` build | `SETUP-BASE` on Wemos D1 Mini32 flashed with `awtrix2_upgrade` env | 1. Open APPS menu.<br>2. Verify exactly 5 entries (Time / Date / Temp / Humidity / Timer, no Battery, no ghost slot).<br>3. Navigate to Timer, run a 5 s timer end-to-end. | Menu has 5 entries. Timer starts, counts, expires, plays end melody. No crash or visual artefact. | _____ |

---

### 4.6 `EDGE-*` — Edge cases

| Test ID | Test Description | Pre-requisites | Test Steps | Expected Results | Status |
| --- | --- | --- | --- | --- | :---: |
| `EDGE-01` | [P1] Duration boundary: 0→1, 1, MAX, MAX+1 | `SETUP-MQTT`, `SETUP-TIMER-IDLE` | For each value `0`, `1`, `86400`, `86401`: `curl -X POST http://<IP>/api/timer -d '{"duration":<v>}'` and inspect `<PREFIX>/stats/timer_dur`. | Stored values: `1`, `1`, `86400`, `86400`. All HTTP 200. | _____ |
| `EDGE-02` | [P1] Reboot mid-Running: state returns to Idle, duration preserved | `SETUP-TIMER-IDLE` | 1. Set duration=600.<br>2. Start timer, wait until `timer_rem=400`.<br>3. Power-cycle.<br>4. After boot, `curl http://<IP>/api/stats`. | After boot: `timer_state=idle`, `timer_dur=600`, `timer_rem=600`. Documented intentional non-persistence of runtime state. | _____ |
| `EDGE-03` | [P2] MQTT broker disconnect during Running does not crash; on reconnect, status republishes | `SETUP-MQTT`, `SETUP-TIMER-RUN-30` | 1. Stop broker (`docker compose down`).<br>2. Wait 10 s.<br>3. Restart broker.<br>4. Observe `<PREFIX>/stats/timer_state` and `<PREFIX>/timer/icons` retained topic. | Timer keeps counting locally during disconnect; display unaffected. On reconnect, state and retained icons topic republish within 2 s. | _____ |
| `EDGE-04` | [P2] Icon GIF restarts at frame 0 on every state transition; old handle released | `SETUP-FILES`, `SETUP-TIMER-IDLE` | 1. Set `icon_running=timer_b` (a GIF).<br>2. Start timer; observe GIF animating.<br>3. Pause; resume.<br>4. Reset.<br>5. Inspect serial log for `LittleFS` warnings. | On resume the GIF restarts at frame 0. No LittleFS warnings about leaked file handles. | _____ |
| `EDGE-05` | [P2] Missing `/MELODIES/timer_end.txt` falls back to built-in RTTTL, no crash | `SETUP-TIMER-IDLE` | 1. Delete `/MELODIES/timer_end.txt` via web UI file manager.<br>2. Start 3 s timer (`buzzer=end`).<br>3. Wait for expiry, listen. | A short built-in RTTTL plays once. No crash, no infinite restart. Serial log shows a fallback warning. | _____ |
| `EDGE-06` | [P2] Multi-surface race: API `start` + MQTT `pause` within 50 ms — final state matches last command | `SETUP-MQTT`, `SETUP-TIMER-IDLE` | 1. In one terminal: `curl -X POST http://<IP>/api/timer -d '{"action":"start"}'`.<br>2. Within ~50 ms in another: `mosquitto_pub -h <BROKER> -t '<PREFIX>/timer' -m '{"action":"pause"}'`.<br>3. Observe `<PREFIX>/stats/timer_state` sequence. | State machine arrives at `paused` deterministically. No orphaned `Finished` notification later, no duplicated `running` publishes after `paused`. | _____ |
| `EDGE-07` | [P2] Auto-switch suppressed when a game / blocking-nav app is active | `SETUP-BASE` | 1. Launch any game (e.g. `curl -X POST http://<IP>/api/switch -d '{"name":"Snake"}'`) — or set `BLOCK_NAVIGATION=true`.<br>2. `curl -X POST http://<IP>/api/timer -d '{"duration":5,"action":"start"}'`.<br>3. Wait for expiry. | Display does NOT force-switch to Timer app on start or expiry. Timer state machine still progresses correctly (state transitions logged). | _____ |
| `EDGE-08` | [P1] MATRIX_OFF wakes display on expiry | `SETUP-TIMER-IDLE` | 1. Set duration=10, start timer, then `curl -X POST http://<IP>/api/settings -d '{"MATP":false}'` (or use device sleep).<br>2. Wait for expiry.<br>3. Observe display. | Display wakes at expiry, Timer app foregrounds, end melody plays, `0:00` blinks (if `finished=hold`). | _____ |

---

## 5. Defect Management & Severity

### Pass / Fail criteria

Each test row is **binary**: a Pass requires every expected result in the Expected Results column to be observed. Any deviation from any expected result is a **Fail**. Partial-pass cases are filed at the **lowest severity** matching the missed result; testers do not bundle multiple unrelated misses into a single defect.

### Severity tiers (timer-specific)

| Severity | Definition | Example timer-specific failures |
| --- | --- | --- |
| **Critical** | Timer feature unusable, alert never fires, firmware crashes, or device requires reboot to recover. | Timer expires but no alert fires on any surface; `POST /api/timer {"action":"start"}` reboots firmware; HA discovery never publishes; `EDGE-08` MATRIX_OFF wake fails (user misses kitchen-timer expiry). |
| **High** | A whole control surface broken while others work; state corruption; persistence broken. | `HW-*` buttons entirely non-responsive; `duration` not persisted across reboot; `TIMER_MAX_DURATION` clamp bypassed; pause/resume desyncs `timer_rem`; HA discovery missing one or more of the 8 entities. |
| **Medium** | Specific scenarios fail but a workaround exists or impact is limited. | `icon_paused` doesn't fall back to `icon_idle` (`API-08`); `timer_publish_interval` ignored but defaults still work; `SHOW_TIMER=false` leaves stale HA entities until manual purge; melody fallback file missing causes crash (should be silent). |
| **Low** | Cosmetic or polish issues with no functional impact. | Time text off-centre by 1 pixel; progress bar starts at column 30 instead of 31; HA entity icon (`mdi:`) wrong; log message typo or formatting issue. |

### Required defect metadata

Every filed defect must include:

- Severity (Critical / High / Medium / Low)
- Test ID that surfaced the defect (e.g. `MQTT-04`)
- Reproduction steps (copy-paste of the failing row's Test Steps)
- Device firmware version + branch HEAD SHA at test time
- Hardware variant (`ulanzi` / `awtrix2_upgrade`)
- Serial log excerpt (if applicable)

---

## 6. Edge Cases

Concrete cases live in section **4.6 `EDGE-*`**. The following categories are intentionally covered:

- **Duration boundaries** (`EDGE-01`): `0` clamps to `1`; `1` accepted; `TIMER_MAX_DURATION` (86400) accepted; `MAX+1` clamps to `MAX`; non-numeric ignored (also `API-04`).
- **Interrupt scenarios** (`EDGE-02`, `EDGE-03`): reboot mid-Running returns to Idle with duration preserved (runtime state intentionally not persisted); MQTT broker disconnect mid-run keeps local timer running and republishes on reconnect.
- **State collisions** (`EDGE-07`): game / blocking-nav app suppresses auto-switch on `start`; non-timer notification on screen queues the timer notification rather than dropping it.
- **Display / wake** (`EDGE-08`, `API-12`): `MATRIX_OFF` must wake on expiry; `fast` flag in `/api/switch` payload suppresses transition animation.
- **Icon fallback chain** (`API-08`, `DEV-10`, `EDGE-04`): per-state empty → idle slot; idle empty → built-in 8×8 pixel-art; configured-but-missing file → idle fallback; GIF restarts at frame 0 on every state transition; old GIF file handle released.
- **Audio fallback** (`EDGE-05`): `/MELODIES/timer_end.txt` missing → built-in RTTTL plays once; no crash, no infinite restart.
- **Config mode** (`DEV-08`, `HW-04`, `HW-05`): `TIMER_CONFIG_TIMEOUT` seconds of no input auto-applies HH:MM:SS and exits; HH wraps `99↔0`, MM/SS wrap `59↔0`.
- **Multi-surface race** (`EDGE-06`): API + MQTT commands within ~50 ms — final state matches last command; no duplicated publishes; no orphaned `Finished` notification.
- **`SHOW_TIMER` toggle** (`DEV-01`, `API-13`, `HA-08`): false at boot (dev.json), at runtime (`/api/settings`), and via on-device menu all converge on the same observable end state; running timer is reset on disable; HA discovery payloads emptied so HA prunes the 8 entities.
- **Cross-build sanity** (`HW-08`): `awtrix2_upgrade` (Wemos D1 Mini32) — Timer app loads, runs, expires; APPS menu has exactly 5 entries with no ghost slot.

---

## Related

- Feature spec: [docs/timer.md](docs/timer.md)
- `dev.json` reference: [docs/dev.md](docs/dev.md)
- Unit tests: [test/test_timer/](test/test_timer/) (run via `pio test -e native`)
- E2e harness: [tests/e2e/](tests/e2e/) and [tests/e2e/README.md](tests/e2e/README.md)
- CI workflow: [.github/workflows/test.yml](.github/workflows/test.yml)
