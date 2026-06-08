# Timer

The Timer app is a kitchen-style countdown timer built into the firmware. Set
a duration, start it, and the device counts down on its display. When the
timer expires the Timer app shows a finished screen, plays a melody, and
behaves according to a configurable "finished mode" (auto-clear, hold,
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

> **Do not publish this topic with the MQTT retain flag.** It is a
> fire-and-forget *command* topic, not a state topic. A retained command (e.g.
> `{"action":"start"}`) would be re-delivered by the broker on every reconnect
> and **auto-start the timer on every boot** — contradicting the "a reboot
> returns the device to Idle" contract (see [Persistence](#persistence)). As a
> safeguard the device clears any retained payload on this topic on connect
> (before subscribing), so a stale retained command never replays.

| Key | Type | Values | Effect |
| --- | --- | --- | --- |
| `duration` | string or int | A clock string `"HH:MM:SS"` / `"MM:SS"`, **or** a bare integer of seconds. Must be 1 .. `TIMER_MAX_DURATION` (default 86400 = 24 h); **out-of-range is rejected, not clamped**. | Sets the timer duration. Persists to NVS. While **Paused**, also resets the timer to `Idle` with the new duration. See "Duration format" below. |
| `buzzer`   | string | `"off"`, `"end"`, `"countdown"` (case-insensitive) | Sets the buzzer mode. Persists to NVS. |
| `finished` | string | `"auto-clear"` (or `"autoclear"`), `"hold"`, `"re-alert"` (or `"realert"`) | Sets the finished-mode. Persists to NVS. |
| `icon_idle`     | string | Bare icon name resolved against `/ICONS/<name>.{jpg,gif}`; empty string clears the slot; capped at 32 chars | Icon shown in the **Idle** state and used as fallback for any other state whose slot is empty. Persists to NVS. |
| `icon_running`  | string | Same. Empty clears (then falls back to `icon_idle`). | Icon shown while counting down. Persists. |
| `icon_paused`   | string | Same. Empty clears (then falls back to `icon_idle`). | Icon shown while paused. Persists. |
| `icon_finished` | string | Same. Empty clears (then falls back to `icon_idle`). | Icon shown beneath the blinking `0:00`. Persists. |
| `finished_hold`     | integer | 1–300 (seconds) | Auto-clear delay (only meaningful when `finished = "auto-clear"`). Persists to NVS `"awtrix"`. Same value as the `CLEAR` slot of the on-device `TIMER` menu. |
| `realert_interval`  | integer | 5–300 (seconds) | Re-alert cadence (only meaningful when `finished = "re-alert"`). Persists to NVS `"awtrix"`. Same value as the `ALERT` slot of the on-device `TIMER` menu. |
| `countdown_seconds` | integer | 0–30   (seconds) | Pre-expiry beep window (only meaningful when `buzzer = "countdown"`). Persists to NVS `"awtrix"`. Same value as the `CDOWN` slot of the on-device `TIMER` menu. |
| `max_duration`               | integer | 1–604800 (seconds, 1 s .. 7 days) | Upper bound on accepted `duration` commands. Out-of-range duration is rejected, not clamped (ADR-0001). Persists to NVS `"awtrix"`. See ADR-0004. |
| `remaining_publish_interval` | integer | 1–60 (seconds) | How often `timer_rem` republishes while Running (drives the HA `{id}_timer_rem` sensor cadence). Persists to NVS `"awtrix"`. See ADR-0004. |
| `app_config_timeout`         | integer | 5–300 (seconds) | No-input idle window before the **Timer-app config mode** auto-applies and exits to `Idle`. Does **not** affect the TIMER global menu. Persists to NVS `"awtrix"`. See ADR-0004. |
| `melody_tick` | string | Bare name resolved against `/MELODIES/<name>.txt`; empty resets to default `"timer_tick"`; capped at 32 chars (alphanumeric, `_`, `-` only) | RTTTL melody played for each countdown beep when `buzzer = "countdown"`. Persists to NVS `"awtrix"`. See ADR-0004. |
| `melody_end`  | string | Same. Empty resets to default `"timer_end"`. | RTTTL melody played on timer expiry (subject to `buzzer` mode). Persists to NVS `"awtrix"`. See ADR-0004. |
| `bar_enabled` | bool | `true` / `false` | When `false`, the progress bar is hidden in Running/Paused. Persists to NVS `"awtrix"`. See ADR-0004. |
| `icon_enabled` | bool | `true` / `false` | When `false`, the timer icon (including the built-in hourglass fallback) is hidden and the time text + progress bar reflow to span the full 32px panel. Persists to NVS `"awtrix"`. See ADR-0005. |
| `bar_color`   | int or hex string | Numeric (0..0xFFFFFF) or `"#RRGGBB"` / `"RRGGBB"` | Progress-bar color. `0` follows `TEXTCOLOR_888` (the global default). Persists to NVS `"awtrix"`. See ADR-0004. |
| `action`   | string | `"start"`, `"pause"`, `"reset"` (case-insensitive) | Drives the state machine. |

### Examples

Start a 5-minute timer (numeric seconds):
```json
{"duration": 300, "action": "start"}
```

Start a 5-minute timer (clock string — equivalent to the above):
```json
{"duration": "5:00", "action": "start"}
```

### Duration format

`duration` accepts either a bare integer of seconds or a clock string. For
strings, **the number of colons decides the units**:

| Input | Interpreted as | Seconds |
| --- | --- | --- |
| `"1:30:00"` | `HH:MM:SS` | 5400 |
| `"3:00"` | `MM:SS` (one colon is minutes, never hours) | 180 |
| `"90"` or `90` | bare seconds | 90 |

Fields are **summed without a 0–59 cap**, so `"3:90"` → 3·60 + 90 = 270 s.
Surrounding whitespace is trimmed.

The duration is **published back as a trimmed clock string** (hours dropped
when zero, leading field unpadded): `300` → `5:00`, `3661` → `1:01:01`,
`45` → `0:45`.

### Invalid input is rejected on every surface

A command is **invalid** if it is malformed (bad JSON, empty/`":30"`/`"5:"`
fields, more than two colons, non-numeric like `"banana"`, or an unknown
`buzzer`/`finished`/`action`/icon value) **or out-of-range** (a well-formed
duration outside 1 .. `TIMER_MAX_DURATION`, e.g. `"30:00:00"` when the max is
24 h). All three control surfaces **reject** an invalid command **atomically —
nothing is applied** (no clamping). They differ only in how the transport can
report it:

| Surface | On invalid input |
| --- | --- |
| `{prefix}/timer` (MQTT) | Silently ignored — fire-and-forget. The retained `timer_dur` state still shows the unchanged value. |
| `POST /api/timer` (HTTP) | Returns an error code — `400` (malformed/out-of-range) or `409` (timer disabled). See [api.md](api.md). |
| `{id}_timer_dur` (HA text box) | Reverts to the previous valid time. |
| On-device config buttons | Cannot produce invalid input — the wheels are capped at the valid range. |

A partially-valid command (e.g. one good field + one bad) is rejected as a
whole; the good field is **not** applied.

See [ADR 0001](adr/0001-timer-command-validation-parity.md) for why rejection
(not clamping) was chosen.

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
    └── reset / AutoClear timeout ──────────────┘
```

`start` from `Finished` clears the finished screen and re-arms with the
configured duration. `reset` is always a hard return to `Idle`.

Updating the `duration` while the timer is **Paused** resets it to `Idle` with
the new duration (remaining is re-armed to the full new value); a subsequent
`start` then counts the full new duration. Updating the duration while `Running`
does **not** restart the in-progress countdown.

---

## Finished modes

When the timer hits zero it always: (1) sets state to `Finished`, (2) shows
the Finished screen (blinking `0:00`) on the Timer app, which pins itself to
the foreground, (3) plays `/MELODIES/timer_end.txt` (or a built-in fallback)
if `SOUND_ACTIVE` is true and buzzer mode isn't `Off`. After that, behavior
depends on `finishedMode`:

| Mode | Behavior |
| --- | --- |
| `auto-clear` (default) | The Finished screen is held for `TIMER_FINISHED_HOLD` seconds (default 10), then state returns to `Idle`. |
| `hold` | The Finished screen persists indefinitely, blinking at 500 ms. Cleared by a `start`/`reset` command (MQTT/HTTP or the HA Start/Reset buttons) or the physical middle button. |
| `re-alert` | The Finished screen persists; every `TIMER_REALERT_INTERVAL` seconds (default 15) the end-melody re-plays until cleared (`start`/`reset`). |

Also editable on-device via the `FINISH` slot of the `TIMER` top menu (see [`onscreen.md`](onscreen.md)) — each press cycles modes and publishes live (same path as MQTT/HA), but the NVS write is deferred to the menu's long-press save, in line with every other `TIMER`-menu slot (ADR-0008).

---

## Buzzer modes

| Mode | Behavior |
| --- | --- |
| `off` | Silent. No countdown beeps, no end melody. |
| `end` (default) | End melody plays once on expiry. |
| `countdown` | Short beep each of the final `TIMER_COUNTDOWN_SECONDS` seconds (default 3), plus the end melody on expiry. |

Beeps and end melody use RTTTL strings loaded from LittleFS. The filenames
are configurable per ADR-0004 (`melody_tick` and `melody_end` keys on
`{prefix}/timer`); defaults are `timer_tick` and `timer_end`, resolving to
`/MELODIES/timer_tick.txt` and `/MELODIES/timer_end.txt`. Empty resets to
defaults. If the resolved file is missing, a small built-in fallback RTTTL
is used instead.

Also editable on-device via the `BUZZER` slot of the `TIMER` top menu (see [`onscreen.md`](onscreen.md)) — each press cycles modes and publishes live (same path as MQTT/HA), but the NVS write is deferred to the menu's long-press save, in line with every other `TIMER`-menu slot (ADR-0008).

---

## Home Assistant entities

With `HA_DISCOVERY=true`, the firmware advertises eight entities:

| Entity | Type | Purpose |
| --- | --- | --- |
| `{id}_timer_dur`   | `text`        | Timer duration as a clock string `HH:MM:SS` (writable; accepts `MM:SS` and bare seconds too). Invalid input (malformed or out-of-range) reverts to the previous valid time. |
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
| Left / Right (in config) | Decrement / increment current field by 1. Hold ≥500 ms to auto-repeat every 250 ms. |
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
| `TIMER_FINISHED_HOLD`, `TIMER_REALERT_INTERVAL`, `TIMER_COUNTDOWN_SECONDS` | Yes (NVS namespace `"awtrix"`, keys `TFHOLD` / `TRALERT` / `TCDOWN`), written when the `TIMER` top menu's long-press save fires. Same dev.json-overrides-NVS rule applies. |
| `TIMER_MAX_DURATION`, `TIMER_PUBLISH_INTERVAL`, `TIMER_CONFIG_TIMEOUT` (the three ADR-0004 behavior parameters) | Yes (NVS namespace `"awtrix"`, keys `TMAXD` / `TPUBI` / `TCFGT`), written by `parseCommand` whenever any of these keys is supplied on `{prefix}/timer`. Same dev.json-overrides-NVS rule applies. |
| `TIMER_MELODY_TICK`, `TIMER_MELODY_END`, `TIMER_BAR_ENABLED`, `TIMER_BAR_COLOR` (the four ADR-0004 new options) | Yes (NVS namespace `"awtrix"`, keys `TMTICK` / `TMEND` / `TBAREN` / `TBARC`). Same dev.json-overrides-NVS rule applies. |
| `TIMER_ICON_ENABLED` (ADR-0005) | Yes (NVS namespace `"awtrix"`, key `TICONEN`), written by `parseCommand` and by the on-device `TIMER` menu's `ICON` slot long-press save. Same dev.json-overrides-NVS rule applies. |
| `TIMER_SYNC_FOLLOW`, `TIMER_SYNC_TARGETS` (ADR-0006, multi-device sync) | Yes (NVS namespace `"awtrix"`, keys `TSYNF` / `TSYNT`), written by `parseCommand`. Same dev.json-overrides-NVS rule applies. These are **local identity** and are never propagated to peers. |
| Runtime state (Running / Paused / Finished, remaining seconds, elapsed time) | **No.** A reboot mid-run returns the device to `Idle` with the saved duration. This is intentional — the device has no RTC backup and resuming a timer with a wrong elapsed-time estimate would be worse than restarting. |

---

## Settings (globals, defaults)

These tune timer behavior. With the exception of `SHOW_TIMER`, they are
**not exposed via the `/settings` MQTT topic** — they reach the timer via
[`dev.json`](dev.md), the `{prefix}/timer` MQTT topic, and `POST /api/timer`
(same atomic-reject validation as the rest of `parseCommand` per ADR-0001).

`TIMER_FINISHED_HOLD`, `TIMER_REALERT_INTERVAL`, and `TIMER_COUNTDOWN_SECONDS`
are additionally user-editable via the on-device `TIMER` top menu (see
[`onscreen.md`](onscreen.md)) — see ADR-0003.

The remaining ADR-0004 behavior parameters (`TIMER_MAX_DURATION`,
`TIMER_PUBLISH_INTERVAL`, `TIMER_CONFIG_TIMEOUT`) and the
melody/color options (`TIMER_MELODY_TICK`, `TIMER_MELODY_END`,
`TIMER_BAR_COLOR`) reach the timer only via the `{prefix}/timer` /
`POST /api/timer` / dev.json surfaces — no on-device menu, no HA entities.
The two display toggles `TIMER_ICON_ENABLED` (ADR-0005) and
`TIMER_BAR_ENABLED` (ADR-0004) reach the timer via those same three surfaces
**and** the on-device `TIMER` menu's `ICON` / `BAR` slots — still no HA
entities. All persist to NVS namespace `"awtrix"`; any matching `dev.json`
key still overrides NVS on every boot.

| Global | Default | Effect |
| --- | --- | --- |
| `SHOW_TIMER` | `true` | Master enable. When `false`, the Timer app is hidden from rotation, the 8 Home Assistant entities are not published, `POST /api/timer` and the MQTT `{prefix}/timer` topic are ignored, and any running timer is reset. On the `true → false` transition the firmware publishes empty retained discovery payloads so HA prunes the stale entities on next reconnect. Toggle from `/api/settings` (`TIMER` key), `dev.json` (`show_timer`), or the on-device **APPS** menu (last entry). |
| `TIMER_MAX_DURATION` | `86400` (24 h) | Upper bound on accepted `duration` (range: 1..604800). |
| `TIMER_PUBLISH_INTERVAL` | `1` (s) | How often `timer_rem` re-publishes while running (range: 1..60). |
| `TIMER_FINISHED_HOLD` | `10` (s) | AutoClear notification hold time. |
| `TIMER_REALERT_INTERVAL` | `15` (s) | Re-alert cadence in re-alert mode. |
| `TIMER_COUNTDOWN_SECONDS` | `3` | Number of pre-expiry beep seconds in countdown buzzer mode. |
| `TIMER_CONFIG_TIMEOUT` | `30` (s) | Idle timeout before the Timer-app config mode auto-exits (range: 5..300). |
| `TIMER_MELODY_TICK` | `"timer_tick"` | Bare name resolved against `/MELODIES/<name>.txt` for countdown beeps. |
| `TIMER_MELODY_END` | `"timer_end"` | Bare name resolved against `/MELODIES/<name>.txt` for the end melody. |
| `TIMER_BAR_ENABLED` | `true` | When `false`, the Running/Paused progress bar is hidden. |
| `TIMER_ICON_ENABLED` | `true` | When `false`, the timer icon is hidden and the time text + bar reflow to the full panel (ADR-0005). |
| `TIMER_BAR_COLOR` | `0` (= `TEXTCOLOR_888`) | Hex color for the progress bar. `0` follows the global text color. |
| `TIMER_SYNC_FOLLOW` | `false` | When `true`, this clock applies inbound timer-sync packets it is targeted by (the follow consent gate). See **Multi-device sync** below. |
| `TIMER_SYNC_TARGETS` | `""` | Whom this clock commands when *it* acts: `""` (sync off), `all`, or a comma list of peer device IDs (e.g. `awtrix_ab12,awtrix_cd34`). See **Multi-device sync** below. |

---

## Multi-device sync (propagation surface)

Two or more clocks on the same LAN can mirror each other's timer — when one
starts/pauses/resets, or its configuration is edited, the change propagates to a
chosen set of peers (or `all`). This is the **propagation surface** (see
[`CONTEXT.md`](../CONTEXT.md)); it is **broker-free** (no MQTT broker required) and
rides a dedicated **UDP broadcast** on **port 4212**. Full rationale in
[ADR-0006](adr/0006-timer-multi-device-sync.md).

### Roles (two independent axes)

| Setting | Axis | Meaning |
| --- | --- | --- |
| `sync_targets` | **send** | Whom this clock commands on a *local* action. `""` = send nothing; `all`; or a comma list of peer `uniqueID`s. |
| `sync_follow` | **receive** | Whether this clock *obeys* inbound sync it is targeted by (consent gate; default off). |

Compose them: a **leader** (targets set, follow off) commands but never obeys; a
**follower** (follow on, no targets) obeys but never commands; a **peer/mirror** (both)
does both; **standalone** (neither) is sync off. For "everyone mirrors everyone," set
every clock to `sync_targets=all` and `sync_follow=true`.

### What propagates, and when

- **Run-state** — a `start` / `pause` / `reset` (and on-device duration edits) propagate
  the action and, for a start, the `duration`. `duration` is run-state: a bare start
  never carries config, so it cannot clobber a peer's settings.
- **Config** — a deliberate config edit (buzzer, finished, the timing knobs, behavior
  parameters, bar/icon toggles, icon images, melodies) propagates a **full config
  snapshot**; the group ends up configured identically (last-config-writer-wins).
- **Not propagated:** `sync_follow` / `sync_targets` (each clock's own identity), and the
  live `remaining` (receivers snapshot their own; a missed packet self-heals on the next
  start/reset).

### Behavior notes

- **Targeting** is by stable `uniqueID` (the same id shown in the web UI / mDNS), not
  hostname. The `all` keyword spares you from listing ids.
- **One-hop:** an applied inbound command is never re-broadcast. `all` reaches everyone
  directly; partial target lists do **not** chain.
- **Delivery** is best-effort: each packet is sent 3× and receivers de-duplicate by
  `(src, seq)`, so a single dropped frame rarely desyncs a clock.
- A clock with `SHOW_TIMER = false` ignores inbound sync (its `parseCommand` is disabled).
- Reachable as `sync_follow` / `sync_targets` on `POST /api/timer` and `{prefix}/timer`
  MQTT, and as `timer_sync_follow` / `timer_sync_targets` in `dev.json`. No on-device
  menu, no HA entity (consistent with the other ADR-0004/0005 config flags).

---

## Clearing the Finished alert

The finished `0:00` is a pinned native Timer-app screen, **not** a
notification ([ADR-0002](adr/0002-timer-finished-is-native-app-screen.md)).
It does **not** participate in the notification queue, so
`{MQTT_PREFIX}/notify/dismiss` (and HA's `{id}_dismiss` button) clear only
generic notifications — they have **no** effect on the timer.

Clearing the finished alert is always an explicit Timer command:

```
{MQTT_PREFIX}/timer  →  {"action": "reset"}   # hard return to Idle
{MQTT_PREFIX}/timer  →  {"action": "start"}   # clear + re-arm from configured duration
```

…or the equivalent `POST /api/timer`, HA's Reset/Start button entities, or
the physical buttons while state is Finished: middle short-press → Idle,
middle long-press → re-arm straight to Running.

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

The manual test plan lives at [TIMER_TEST_PLAN.md](../TIMER_TEST_PLAN.md) — run
it against real hardware for any timer-touching PR.
