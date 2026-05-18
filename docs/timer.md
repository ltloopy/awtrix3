# Timer

The Timer app is a kitchen-style countdown timer built into the firmware. Set
a duration, start it, and the device counts down on its display. When the
timer expires it pushes a notification (channel `"timer"`), plays a melody,
and behaves according to a configurable "finished mode" (auto-clear, hold,
or re-alert).

The timer is controlled via MQTT, Home Assistant entities, or the three
physical buttons on the Ulanzi TC001.

---

## MQTT command topic

```
{MQTT_PREFIX}/timer
```

Accepts a JSON payload. **All keys are optional**; only the supplied ones
take effect. Keys are processed in this order: `duration` → `buzzer` →
`finished` → `icon_*` → `action`, so you can configure icons and start a
timer in one publish.

| Key | Type | Values | Effect |
| --- | --- | --- | --- |
| `duration` | int | 1 to `TIMER_MAX_DURATION` (default 86400 = 24 h); clamped on out-of-range | Sets the timer duration in seconds. Persists to NVS. |
| `buzzer`   | string | `"off"`, `"end"`, `"countdown"` (case-insensitive) | Sets the buzzer mode. Persists to NVS. |
| `finished` | string | `"auto-clear"` (or `"autoclear"`), `"hold"`, `"re-alert"` (or `"realert"`) | Sets the finished-mode. Persists to NVS. |
| `icon_idle`     | string | Bare icon name resolved against `/ICONS/<name>.{jpg,gif}`; empty string clears the slot; capped at 32 chars | Icon shown in the **Idle** state and used as fallback for any other state whose slot is empty. Persists to NVS. |
| `icon_running`  | string | Same. Empty clears (then falls back to `icon_idle`). | Icon shown while counting down. Persists. |
| `icon_paused`   | string | Same. Empty clears (then falls back to `icon_idle`). | Icon shown while paused. Persists. |
| `icon_finished` | string | Same. Empty clears (then falls back to `icon_idle`). | Icon shown beneath the blinking `0:00`. Persists. |
| `action`   | string | `"start"`, `"pause"`, `"reset"` (case-insensitive) | Drives the state machine. |

### Examples

Start a 5-minute timer:
```json
{"duration": 300, "action": "start"}
```

Change buzzer mode without affecting the timer:
```json
{"buzzer": "countdown"}
```

Configure a 1-hour Hold-mode timer with no audio and start it:
```json
{"duration": 3600, "buzzer": "off", "finished": "hold", "action": "start"}
```

Pause / resume:
```json
{"action": "pause"}
```
(Pausing while paused resumes; pausing while idle is a no-op.)

Reset to idle (also clears any active timer notification):
```json
{"action": "reset"}
```

Set per-state icons in one publish (Idle stays as fallback; Running uses
an animated GIF; clear the Paused slot):
```json
{"icon_idle": "64936", "icon_running": "74706", "icon_paused": ""}
```

### Published icon state

The current icon configuration is mirrored to a retained JSON topic so
subscribers (including the device itself on reconnect) see the latest
values without reading NVS:

```
{MQTT_PREFIX}/timer/icons    (retained)
```

Payload shape:

```json
{"idle": "64936", "running": "74706", "paused": "", "finished": ""}
```

Published on MQTT connect and on every change made through the
`{MQTT_PREFIX}/timer` command topic, `POST /api/timer`, or a boot-time
`dev.json` reload.

### State machine

```
                ┌─ start ─┐
                │         ▼
   Idle ──── start ──► Running ── tick crosses 0 ──► Finished
    ▲           ▲          │                            │
    │           │          │                            │
    │           └── pause ─┴── pause ───► Paused ───────┘
    │                       (toggle)            │
    │                                           │
    └── reset / AutoClear timeout / dismiss ────┘
```

`start` from `Finished` dismisses the timer notification and re-arms with the
configured duration. `reset` is always a hard return to `Idle`.

---

## Finished modes

When the timer hits zero it always: (1) sets state to `Finished`, (2) pushes
a notification on the `"timer"` channel, (3) plays
`/MELODIES/timer_end.txt` (or a built-in fallback) if `SOUND_ACTIVE` is true
and buzzer mode isn't `Off`. After that, behavior depends on
`finishedMode`:

| Mode | Behavior |
| --- | --- |
| `auto-clear` (default) | Notification auto-dismisses after `TIMER_FINISHED_HOLD` seconds (default 10); state returns to `Idle`. |
| `hold` | Notification persists indefinitely with a 500 ms blink. Dismiss via `{prefix}/notify/dismiss`, the HA `dismiss` button, or physical middle long-press. |
| `re-alert` | Notification persists; every `TIMER_REALERT_INTERVAL` seconds (default 15) the end-melody re-plays until dismissed. |

---

## Buzzer modes

| Mode | Behavior |
| --- | --- |
| `off` | Silent. No countdown beeps, no end melody. |
| `end` (default) | End melody plays once on expiry. |
| `countdown` | Short beep each of the final `TIMER_COUNTDOWN_SECONDS` seconds (default 3), plus the end melody on expiry. |

Beeps and end melody use RTTTL strings loaded from LittleFS
(`/MELODIES/timer_tick.txt` for countdown beeps, `/MELODIES/timer_end.txt`
for end melody). If the file is missing, a small built-in fallback RTTTL is
used instead.

---

## Home Assistant entities

With `HA_DISCOVERY=true`, the firmware advertises eight entities:

| Entity | Type | Purpose |
| --- | --- | --- |
| `{id}_timer_dur`   | `number`      | Timer duration in seconds (writable). |
| `{id}_timer_rem`   | `sensor`      | Seconds remaining (read-only, updates every `TIMER_PUBLISH_INTERVAL` s while running). |
| `{id}_timer_state` | `sensor`      | One of `idle` / `running` / `paused` / `finished`. |
| `{id}_timer_buz`   | `select`      | Buzzer mode. |
| `{id}_timer_fin`   | `select`      | Finished mode. |
| `{id}_timer_start` | `button`      | Equivalent to `{"action":"start"}`. |
| `{id}_timer_pause` | `button`      | Equivalent to `{"action":"pause"}`. |
| `{id}_timer_reset` | `button`      | Equivalent to `{"action":"reset"}`. |

When the timer is started from `Idle` (and no game is active, no
blocking-nav app is on screen), the display auto-switches to the Timer app.

---

## Physical buttons (Timer app)

| Input | Effect |
| --- | --- |
| Middle long-press (from Idle, Timer app) | Enter config mode. `HH` field highlighted. |
| Middle short-press (in config) | Cycle field `HH → MM → SS → HH`. |
| Left / Right (in config) | Decrement / increment current field by `TIMER_STEP`. Hold ≥500 ms to auto-repeat every 250 ms. |
| 30 s of no input (in config) | Auto-applies HH:MM:SS to duration, exits config. |
| Middle short-press (Idle, Timer app) | Start the timer with the saved duration. |
| Middle short-press (Running) | Pause. |
| Middle short-press (Paused) | Resume. |
| Middle short-press (Finished) | Dismiss alert: stops the end melody and returns to Idle. A second short-press from Idle re-arms the timer with the saved duration. |
| Middle long-press (Running / Paused, Timer app) | Reset to Idle. |
| Middle long-press (Finished) | Stops the end melody and immediately re-arms the timer (jumps straight to Running with the saved duration). |

Field bounds: `HH` wraps `99 ↔ 0`; `MM`/`SS` wrap `59 ↔ 0`.

---

## Display

The Timer app renders the timer icon, the time text, and (while
Running/Paused) a progress bar that drains from the left — the right edge
is anchored at column 31; the left edge sweeps rightward as time elapses.

- **Idle:** icon + configured duration text (`MM:SS` / `HH:MM`), no bar.
- **Running / Paused:** icon + remaining time text + draining bar.
- **Finished:** icon + blinking `0:00` (500 ms cadence), no bar. The
  Timer app pulls itself to the foreground and wakes the display when
  the timer reaches zero. AutoClear returns to Idle after
  `TIMER_FINISHED_HOLD` seconds; Hold / ReAlert stay until reset.

The Timer app is always present in the rotation regardless of state.

### Icon resolution

Per state, the renderer looks up the configured slot (`icon_idle` /
`icon_running` / `icon_paused` / `icon_finished`). If that slot is empty
it falls back to `icon_idle`. If `icon_idle` is also empty (or the
configured file isn't on the filesystem), the original 8 × 8 icon is drawn.

The configured value is a **bare name** (no extension). The loader
checks `/ICONS/<name>.jpg` first, then `/ICONS/<name>.gif`, and uses
whichever exists. Animated GIFs Just Work: whenever the resolved icon
changes (state transition or setter mid-run) the GIF restarts at frame 0
and the previous file handle is released. Icon files are uploaded via
the existing web UI — nothing about icons is bundled in firmware.

---

## Persistence

| Field | Persisted? |
| --- | --- |
| `duration`, `buzzer mode`, `finished mode`, per-state icons | Yes (NVS namespace `"timer"`, keys `DUR` / `BUZ` / `FIN` / `ICON_IDLE` / `ICON_RUN` / `ICON_PAUSE` / `ICON_FIN`). Survives reboot, **but** any matching key in `dev.json` overrides NVS on every boot — see [`dev.md`](dev.md). |
| Runtime state (Running / Paused / Finished, remaining seconds, elapsed time) | **No.** A reboot mid-run returns the device to `Idle` with the saved duration. This is intentional — the device has no RTC backup and resuming a timer with a wrong elapsed-time estimate would be worse than restarting. |

---

## Settings (globals, defaults)

These tune timer behavior. With the exception of `SHOW_TIMER`, they are
**not currently exposed via the `/settings` MQTT topic** — they're
compile-time defaults overridable via [`dev.json`](dev.md).

| Global | Default | Effect |
| --- | --- | --- |
| `SHOW_TIMER` | `true` | Master enable. When `false`, the Timer app is hidden from rotation, the 8 Home Assistant entities are not published, `POST /api/timer` and the MQTT `{prefix}/timer` topic are ignored, and any running timer is reset. On the `true → false` transition the firmware publishes empty retained discovery payloads so HA prunes the stale entities on next reconnect. Toggle from `/api/settings` (`TIMER` key), `dev.json` (`show_timer`), or the on-device **APPS** menu (last entry). |
| `TIMER_MAX_DURATION` | `86400` (24 h) | Upper clamp for `setDuration`. |
| `TIMER_STEP` | `1` | Increment step for left/right adjusts in config mode. |
| `TIMER_PUBLISH_INTERVAL` | `1` (s) | How often `timer_rem` re-publishes while running. |
| `TIMER_FINISHED_HOLD` | `10` (s) | AutoClear notification hold time. |
| `TIMER_REALERT_INTERVAL` | `15` (s) | Re-alert cadence in re-alert mode. |
| `TIMER_COUNTDOWN_SECONDS` | `3` | Number of pre-expiry beep seconds in countdown buzzer mode. |
| `TIMER_CONFIG_TIMEOUT` | `30` (s) | Idle timeout before config mode auto-exits. |

---

## Dismiss + reset

Two ways to clear an active timer notification:

```
{MQTT_PREFIX}/notify/dismiss          # also dismisses any other notification
```

…or HA's `{id}_dismiss` button, or the physical middle short-press
(dismiss to Idle) or middle long-press (dismiss + re-arm to Running)
while state is Finished. Dismissing the notification while in `Hold`
mode (or between `re-alert` cycles) returns the timer to `Idle`.

To hard-reset state without dismissing other notifications, publish:

```
{MQTT_PREFIX}/timer  →  {"action": "reset"}
```

To restart the whole device:

```
GET http://<device-ip>/api/reboot
```

---

## Testing

Automated tests for the timer state machine live in:

- `test/test_timer/` — native unit tests, run via `pio test -e native`.
  Run on every PR via [.github/workflows/test.yml](../.github/workflows/test.yml).
  **Branch protection should require the `test` job to pass.**
- `tests/e2e/` — end-to-end MQTT harness for maintainer-only pre-merge
  validation. See [tests/e2e/README.md](../tests/e2e/README.md).

The manual smoke checklist lives in
[docs/test-plans/timer.md](test-plans/timer.md) — run it against real
hardware for any timer-touching PR.
