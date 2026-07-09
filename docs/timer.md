# Timer

The Timer app is a kitchen-style countdown timer built into the firmware. Set
a duration, start it, and the device counts down on its display. When the
timer expires the Timer app shows a finished screen, plays a melody, and
behaves according to a configurable "finished mode" (auto-clear, hold,
or re-alert).

The timer is controlled via MQTT, Home Assistant entities, or the three
physical buttons on the Ulanzi TC001.

---

## Control & configuration matrix

`POST /api/timer` and the `{prefix}/timer` MQTT topic share one command parser
([ADR-0001](adr/0001-timer-command-validation-parity.md)) and accept the **same
JSON keys** — listed once in the "API / MQTT key" column. `GET /api/timer`
returns the read-only snapshot: the run-state summary at top level **and** a
nested `config` object mirroring **every persisted config key** ([ADR-0015](adr/0015-http-observation-mirrors-full-config.md)),
so the "GET /api/timer" column below is a read path for every configurable knob —
no Home Assistant or MQTT subscription required. `dev.json` keys override NVS on
**every boot** ([dev.md](dev.md)). Legend: ✅ set · 👁 read-only · — n/a.

### Run-state (not persisted; resets to Idle on reboot)

| Item | API / MQTT key | dev.json | GET /api/timer | Home Assistant | On-device |
|------|----------------|----------|----------------|----------------|-----------|
| Start / Pause / Reset | `action`: `start`/`pause`/`reset` ✅ | — | — (`action` is not persisted) | `Timer start` / `Timer pause` / `Timer reset` buttons ✅ | mid-button (per state) ✅ |
| State | `state` 👁 (GET) | — | 👁 `state` (top-level) | `Timer state` sensor 👁 | on screen 👁 |
| Remaining | `remaining` / `remaining_str` 👁 (GET) | — | 👁 `remaining`/`remaining_str` (top-level) | `Timer remaining` sensor (`s`) 👁 | countdown text 👁 |

### Core settings (member-backed, NVS `"timer"`)

| Item | API / MQTT key | dev.json | GET /api/timer | Home Assistant | On-device |
|------|----------------|----------|----------------|----------------|-----------|
| Duration | `duration` ✅ (sec / `MM:SS` / `HH:MM:SS`) | — | 👁 `duration`/`duration_str` (top-level; **run-state, not in `config`**) | `Timer duration` text ✅ | config editor (long-press mid) ✅ |
| Buzzer mode | `buzzer`: `off`/`end`/`countdown` ✅ | — | 👁 `buzzer` (top-level + `config`) | `Timer buzzer` select ✅ | `TIMER` menu (`BZR …`) ✅ |
| Finished mode | `finished`: `auto-clear`/`hold`/`re-alert` ✅ | — | 👁 `finished` (top-level + `config`) | `Timer finished mode` select ✅ | `TIMER` menu (`FIN …`) ✅ |
| Per-state icons | `icon_idle`/`icon_running`/`icon_paused`/`icon_finished` ✅ | `timer_icon_idle` … `timer_icon_finished` ✅ | 👁 `config.icon_*` | — (mirrored to retained `{prefix}/timer/icons`) | — |

### Behavior-tuning knobs (NVS `"awtrix"`, table `TIMER_SETTINGS_DESCS`)

| Item | API / MQTT key | dev.json | GET /api/timer | Home Assistant | On-device |
|------|----------------|----------|----------------|----------------|-----------|
| Finished hold (1–300 s, dflt 10) | `finished_hold` ✅ | `timer_finished_hold` ✅ | 👁 `config.finished_hold` | 👁 attr on finished select | `TIMER` menu (`CLEAR`) ✅ |
| Re-alert interval (5–300 s, dflt 15) | `realert_interval` ✅ | `timer_realert_interval` ✅ | 👁 `config.realert_interval` | 👁 attr on finished select | `TIMER` menu (`ALERT`) ✅ |
| Countdown window (0–30 s, dflt 3) | `countdown_seconds` ✅ | `timer_countdown_seconds` ✅ | 👁 `config.countdown_seconds` | 👁 attr on buzzer select | `TIMER` menu (`CDOWN`) ✅ |
| Max duration (1–604800 s, dflt 86400) | `max_duration` ✅ | `timer_max_duration` ✅ | 👁 `config.max_duration` (raw) + `config.max_duration_str` (clock form) | 👁 attr on state sensor (raw seconds) + Duration entity (clock form) | — (caps config editor) |
| Remaining publish interval (1–60 s, dflt 1) | `remaining_publish_interval` ✅ | `timer_remaining_publish_interval` ✅ | 👁 `config.remaining_publish_interval` | 👁 attr on remaining + state sensors (also governs sensor cadence) | — |
| Icon enabled (dflt true) | `icon_enabled` ✅ | `timer_icon_enabled` ✅ | 👁 `config.icon_enabled` | 👁 attr on state sensor | `TIMER` menu (`ICON`) ✅ |
| Bar enabled (dflt true) | `bar_enabled` ✅ | `timer_bar_enabled` ✅ | 👁 `config.bar_enabled` | 👁 attr on state sensor | `TIMER` menu (`PROGRESS BAR`) ✅ |
| Bar color (hex / `#RRGGBB`, dflt 0 = text color) | `bar_color` ✅ | `timer_bar_color` ✅ | 👁 `config.bar_color` (as `"default"`/`"#RRGGBB"`) | 👁 attr on state sensor (as `"default"`/`"#RRGGBB"`) | — |
| Bar background color (hex / `#RRGGBB`, dflt 0 = black/no track) | `bar_bg_color` ✅ | `timer_bar_bg_color` ✅ | 👁 `config.bar_bg_color` (as `"none"`/`"#RRGGBB"`) | 👁 attr on state sensor (as `"none"`/`"#RRGGBB"`) | — |
| Tick melody (dflt `timer_tick`) | `melody_tick` ✅ | `timer_melody_tick` ✅ | 👁 `config.melody_tick` | 👁 attr on buzzer select | — |
| End melody (dflt `timer_end`) | `melody_end` ✅ | `timer_melody_end` ✅ | 👁 `config.melody_end` | 👁 attr on buzzer select | — |

### Multi-device sync (local identity; never propagated to peers)

| Item | API / MQTT key | dev.json | GET /api/timer | Home Assistant | On-device |
|------|----------------|----------|----------------|----------------|-----------|
| Sync follow (dflt on) | `sync_follow` ✅ | `timer_sync_follow` ✅ | 👁 `config.sync_follow` | ✅ Follow `switch`; 👁 attr on state sensor | — |
| Sync targets (`all` / CSV, dflt empty) | `sync_targets` ✅ | `timer_sync_targets` ✅ | 👁 `config.sync_targets` | ✅ Targets `select` (static `Off`/`All`; unknown for a specific-ID CSV); 👁 attr on state sensor | — |

### Master enable

| Item | API / MQTT | dev.json | GET /api/timer | Home Assistant | On-device |
|------|------------|----------|----------------|----------------|-----------|
| Show timer (dflt true) | `enabled` 👁 (GET); POST→409 & topic ignored when off | `show_timer` ✅ | 👁 `enabled` (top-level; not in `config`) | — (10 entities pruned when off) | web settings `TIMER` toggle (NVS) |

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
| `melody_tick` | string | **Either** a bare name resolved against `/MELODIES/<name>.txt` (≤32 chars, alphanumeric/`_`/`-`) **or** an inline RTTTL tune | RTTTL melody played for each countdown beep when `buzzer = "countdown"`. A bare name persists to NVS `"awtrix"` and obeys `save`; an **inline tune is always one-shot** (never saved — see [One-shot commands & inline melodies](#one-shot-commands--inline-melodies)). See ADR-0004 / ADR-0017. |
| `melody_end`  | string | Same. A bare empty string resets to default `"timer_end"`. | RTTTL melody played on timer expiry (subject to `buzzer` mode). Bare name persists & obeys `save`; inline tune always one-shot. See ADR-0004 / ADR-0017. |
| `bar_enabled` | bool | `true` / `false` | When `false`, the progress bar is hidden in Running/Paused. Persists to NVS `"awtrix"`. See ADR-0004. |
| `icon_enabled` | bool | `true` / `false` | When `false`, the timer icon (including the built-in hourglass fallback) is hidden and the time text + progress bar reflow to span the full 32px panel. Persists to NVS `"awtrix"`. See ADR-0005. |
| `bar_color`   | int or hex string | Numeric (0..0xFFFFFF) or `"#RRGGBB"` / `"RRGGBB"` | Progress-bar (foreground) color. `0` follows `TEXTCOLOR_888` (the global default). Persists to NVS `"awtrix"`. See ADR-0004. |
| `bar_bg_color` | int or hex string | Numeric (0..0xFFFFFF) or `"#RRGGBB"` / `"RRGGBB"` | Progress-bar **background track** color, drawn behind the bar (mirrors the custom-app `progressBC`). `0` = black = **no track** (LEDs off — the default, so the bar looks unchanged from before this feature). The track persists for the whole Running/Paused window, even after the foreground has drained. Note the asymmetry: for `bar_color` `0` is a *sentinel* (follow text color); for `bar_bg_color` `0` is a *literal* black. Persists to NVS `"awtrix"`. See ADR-0020. |
| `save`     | bool | `true` (default) / `false` | `save:false` makes the **whole command one-shot**: its config applies to the current run only and reverts to the saved settings when the timer next returns to Idle, writing nothing to flash. A non-boolean is rejected (atomic-reject). See [One-shot commands & inline melodies](#one-shot-commands--inline-melodies) / ADR-0017. |
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

### One-shot commands & inline melodies

By default every config key you send **persists to flash and becomes your new
default**. Add **`save:false`** to run a timer **once** with custom parameters
without overwriting the settings you keep for next time. See
[ADR-0017](adr/0017-one-shot-timer-commands.md).

- **`save` is one payload-level flag** (default `true`). `save:false` makes the
  *whole* command one-shot: every config key in it — duration, buzzer, finished
  mode, the timing knobs, bar/icon toggles, colours, melodies — applies to the
  **current run only**.
- **Revert on return to Idle.** The moment the timer returns to Idle (a `reset`,
  or auto-clear after a Finished run) the one-off values revert to your saved
  settings and **nothing is written to flash**. The *next* run uses your saved
  defaults again.
- **Observation stays honest.** While a one-shot run is active, `GET /api/timer`'s
  `config` mirror, the Home Assistant attribute entities, and device-to-device
  sync all keep reporting your **saved** configuration, never the one-off values.
  The top-level `duration` (and `buzzer`/`finished`) of `GET /api/timer` still
  show the **live** one-shot values so you can observe what is counting down.
- **Run-state still syncs.** Start/pause/reset propagate to your sync followers (so
  synced timers start together), and a `start` carries its one-off `duration` and
  effective config so followers mirror the run one-shot. A *bare* duration or config
  edit does **not** propagate (#126): followers adopt the leader's values on the next
  `start` and revert to their own on Idle.
- **Commit mid-run if you choose.** A normal (`save:true`) config command sent
  *during* an active one-shot run becomes your new saved baseline.
- **Inline melodies.** `melody_end`/`melody_tick` accept **either** a bare saved
  file name (as before) **or** an inline RTTTL tune (a literal melody string, e.g.
  `"alarm:d=4,o=5,b=120:c,e,g"`). An inline tune is detected by content (it
  contains `:` separators and a `d=`/`o=`/`b=` control section); a malformed inline
  tune rejects the whole command (`400`). An **inline tune is always one-shot
  regardless of `save`** — it is played for that run only, never written to flash,
  and never overwrites your saved melody name (the `config` mirror keeps showing
  the saved bare name throughout). A bare name persists and obeys `save` exactly
  as before.
- **The on-device TIMER menu always persists** — physical edits never go through
  `save:false`, so they behave predictably and commit to NVS.

Run a 5-minute timer **once** with a custom inline alarm — the next bare
`{"action":"start"}` uses your saved duration and saved melody again:
```json
{"duration": 300, "melody_end": "alarm:d=4,o=5,b=120:c,e,g", "action": "start", "save": false}
```

Run one timer with a one-off finished mode and hold delay, leaving your defaults:
```json
{"finished": "re-alert", "realert_interval": 30, "action": "start", "save": false}
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

With `HA_DISCOVERY=true`, the firmware advertises ten entities:

| Entity | Type | Purpose |
| --- | --- | --- |
| `{id}_timer_dur`   | `text`        | Timer duration as a clock string `HH:MM:SS` (writable; accepts `MM:SS` and bare seconds too). Invalid input (malformed or out-of-range) reverts to the previous valid time. Carries a read-only JSON attribute object `{max_duration}` in the same `H:MM:SS` clock form (e.g. `"24:00:00"`), so the largest duration the timer will accept is visible at the point of entry. The cap is **read-only**; only the duration *state* is writable. |
| `{id}_timer_rem`   | `sensor`      | Seconds remaining (read-only, updates every `TIMER_PUBLISH_INTERVAL` s while running). Carries a read-only JSON attribute object `{remaining_publish_interval}` — this sensor's own update cadence. |
| `{id}_timer_state` | `sensor`      | One of `idle` / `running` / `paused` / `finished`. Carries the full-config read-only JSON attribute bag `{max_duration, remaining_publish_interval, icon_enabled, bar_enabled, bar_color, bar_bg_color, sync_follow, sync_targets}` so the whole configuration is readable from one entity. |
| `{id}_timer_buz`   | `select`      | Buzzer mode. Carries a read-only JSON attribute object `{countdown_seconds, melody_tick, melody_end}` so the beep window and both melodies are visible in HA without leaving the entity. |
| `{id}_timer_fin`   | `select`      | Finished mode. Carries a read-only JSON attribute object `{realert_interval, finished_hold}` so the re-alert cadence and auto-clear hold are visible in HA without leaving the entity. |
| `{id}_timer_start` | `button`      | Equivalent to `{"action":"start"}`. |
| `{id}_timer_pause` | `button`      | Equivalent to `{"action":"pause"}`. |
| `{id}_timer_reset` | `button`      | Equivalent to `{"action":"reset"}`. |
| `{id}_timer_sync_follow`  | `switch` | The receive-consent toggle (`sync_follow`), writable. Toggling emits `{"sync_follow":<bool>}` through `parseCommand` (atomic-reject + NVS persist). Reflects the current `sync_follow`. Local identity — never propagated to peers. |
| `{id}_timer_sync_targets` | `select` | The send-targeting setting (`sync_targets`), writable with **static** options `Off`/`All`. Selecting emits `{"sync_targets":""\|"all"}` through `parseCommand`. Reflects `Off`/`All`, or **unknown** (blank) when `sync_targets` holds a specific-ID CSV set out-of-band (the read-only `sync_targets` attribute on the state sensor stays authoritative for the exact value). Becomes a dynamic peer list in a later slice. Local identity — never propagated. |

When the timer is started from `Idle` (and no game is active, no
blocking-nav app is on screen), the display auto-switches to the Timer app.

### Read-only attribute groups

Carrier entities surface persisted settings as **read-only JSON attribute
objects**, so a user can read the device's live configuration from inside HA
without an MQTT/HTTP query (see [PRD #57](https://github.com/ltloopy/awtrix3/issues/57)
and [ADR-0014](adr/0014-timer-ha-attribute-projection.md), generalizing the single
`realert_interval` attribute of
[PRD #17](https://github.com/ltloopy/awtrix3/issues/17)). Which settings key
rides which carrier is one table (`TIMER_ATTR_GROUP_DESCS`). **All five carriers**
are lit up today — one text, two selects and two sensors — each opting into a
`json_attributes_topic` (`{prefix}/{deviceId}/{id}/json_attr_t`) via the vendored
ArduinoHA per-type opt-in: the `HASelect` capability (issue #58) lit the selects,
extending the same opt-in to `HASensor` (issue #59; `HASensorNumber` inherits it) lit
the sensors, and extending it to `HAText` (issue #67) lit the Duration entity. Every
persisted-settings knob is projected onto the entity it is semantically about:

| Carrier | Attribute object |
| --- | --- |
| `{id}_timer_dur` (Duration text)   | `{"max_duration":"24:00:00"}` — clock form |
| `{id}_timer_fin` (finished select) | `{"realert_interval":N, "finished_hold":N}` |
| `{id}_timer_buz` (buzzer select)   | `{"countdown_seconds":N, "melody_tick":"…", "melody_end":"…"}` |
| `{id}_timer_rem` (remaining sensor) | `{"remaining_publish_interval":N}` |
| `{id}_timer_state` (state sensor)  | `{"max_duration":N, "remaining_publish_interval":N, "icon_enabled":bool, "bar_enabled":bool, "bar_color":"default"\|"#RRGGBB", "bar_bg_color":"none"\|"#RRGGBB", "sync_follow":bool, "sync_targets":"…"}` |

`remaining_publish_interval` and `max_duration` each deliberately ride **two** carriers.
`remaining_publish_interval` rides the remaining sensor (its own cadence) and the state
sensor (a complete single-entity config view). `max_duration` rides the state sensor
**and** the Duration entity — and is the first key whose representation differs **per
carrier**: the state sensor keeps **raw seconds** (`86400`), while the Duration entity
renders the **same value** in the `H:MM:SS` **clock form** its own state speaks
(`"24:00:00"`, via the same `formatHMS` trimming — `"24:00:00"`, `"1:00:00"`, `"0:45"`).
The two never contradict: both read the same persisted `max_duration`, so they always
describe the same underlying value. `bar_color` is likewise rendered as the human string
`"default"` (when 0 = follow the text color) or `"#RRGGBB"`, not its raw integer — an
attribute renders in its carrier's native representation. `bar_bg_color` follows the same
shape but its 0 renders as `"none"` (no track) rather than `"default"`, mirroring the
deliberate sentinel asymmetry between the foreground and background colors (ADR-0020).

Each carrier's retained object goes out at discovery-enable and on every MQTT
(re)connect, right after the wire refresh, on the on-device `TIMER`-menu
long-press commit (after its peer broadcast — it refreshes every carrier, not
just the changed one, since the commit does not track which knob moved), and is
republished promptly whenever any of its mapped keys is applied — locally or via a synced peer's propagated
config snapshot — so HA always reflects the device's real configuration; being
retained, it also survives an HA or broker restart with no extra publish.
Disabling the Timer (`SHOW_TIMER` true to false) clears each carrier's retained
`json_attr_t` object alongside pruning the discovery entities, so the broker is
left holding no orphaned attribute payload (issue #60). The
values are **read-only from HA** — change them via their command keys, `dev.json`,
or (where applicable) the on-device `TIMER` menu; the attribute is observation
only. `realert_interval` is only meaningful while finished mode is `re-alert`. The
sync rows (`sync_follow` / `sync_targets`) ride this read-only bag **and** have
their own writable control entities (the Follow `switch` / Targets `select`, issue
#110) — but they remain **local identity**: never propagated to peers (ADR-0006 is
unchanged; see **Multi-device sync** below). The read-only `sync_targets` attribute
stays authoritative for the exact value, since the static select only renders
`Off`/`All` (a specific-ID CSV shows as unknown there).

### HTTP config readback (`GET /api/timer`)

`GET /api/timer` returns the read-only run-state summary at top level **and** a
nested **`config`** object mirroring **every persisted config key** — the same
keys the carrier attribute groups expose, but on the HTTP carrier and without an
HA install or MQTT subscription ([ADR-0015](adr/0015-http-observation-mirrors-full-config.md);
full key list in [api.md](api.md#state-observation)). The mirror is projected from
the very tables that drive the control surface (`TIMER_SETTINGS_DESCS`,
`TIMER_MEMBER_CONFIG_DESCS`), so the read surface cannot drift from what is
writable. Values are raw except the carrier-native renderings that follow this
endpoint's raw+`_str` duration precedent: `bar_color` as `"default"`/`"#RRGGBB"`,
`bar_bg_color` as `"none"`/`"#RRGGBB"`, and `max_duration` reported both raw and as
`max_duration_str` (clock form).
`duration` stays top-level only (run-state, not config); `buzzer`/`finished` appear
both top-level (legacy back-compat) and inside `config` (a self-contained mirror).

This makes the four per-state icons readable over HTTP (`config.icon_*`) in
addition to the retained `{prefix}/timer/icons` topic. Icons remain the one
capability with **no HA read path** — a deliberate non-gap: they already have two
read paths, and HA cannot usefully render an AWTRIX icon file.

---

## Physical buttons (Timer app)

| Input | Effect |
| --- | --- |
| Middle long-press (from Idle, Timer app) | Open the **TIMER menu** at the top of its list (origin = App). Duration is the first item (`DURATION` leaf — the same HH:MM:SS wheel); see [`onscreen.md`](onscreen.md). The bare wheel has no other entry point (PRD #83 / #87). |
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
configured file isn't on the filesystem), the built-in `icon_timer`
hourglass bitmap (`src/icons.h`) is drawn — the same glyph the on-device
menu uses for the Timer entry.

The configured value is a **bare name** (no extension). The loader
checks `/ICONS/<name>.jpg` first, then `/ICONS/<name>.gif`, and uses
whichever exists. Animated GIFs Just Work: whenever the resolved icon
changes (state transition or setter mid-run) the GIF restarts at frame 0
and the previous file handle is released. Icon files are uploaded via
the existing web UI — no *user-named* icons are bundled in firmware. The
sole exception is the built-in `icon_timer` hourglass used as the default
fallback (see above).

---

## Persistence

| Field | Persisted? |
| --- | --- |
| `duration`, `buzzer mode`, `finished mode`, per-state icons | Yes (NVS namespace `"timer"`, keys `DUR` / `BUZ` / `FIN` / `ICON_IDLE` / `ICON_RUN` / `ICON_PAUSE` / `ICON_FIN`). Survives reboot, **but** any matching key in `dev.json` overrides NVS on every boot — see [`dev.md`](dev.md). |
| `TIMER_FINISHED_HOLD`, `TIMER_REALERT_INTERVAL`, `TIMER_COUNTDOWN_SECONDS` | Yes (NVS namespace `"awtrix"`, keys `TFHOLD` / `TRALERT` / `TCDOWN`), written when the `TIMER` top menu's long-press save fires. Same dev.json-overrides-NVS rule applies. |
| `TIMER_MAX_DURATION`, `TIMER_PUBLISH_INTERVAL` (the ADR-0004 behavior parameters; `app_config_timeout` removed in PRD #83 / #88) | Yes (NVS namespace `"awtrix"`, keys `TMAXD` / `TPUBI`), written by `parseCommand` whenever any of these keys is supplied on `{prefix}/timer`. Same dev.json-overrides-NVS rule applies. The old `TCFGT` key is left as dead bytes; inbound `app_config_timeout` is silently ignored. |
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
`TIMER_PUBLISH_INTERVAL`) and the
melody/color options (`TIMER_MELODY_TICK`, `TIMER_MELODY_END`,
`TIMER_BAR_COLOR`) reach the timer only via the `{prefix}/timer` /
`POST /api/timer` / dev.json surfaces — no on-device menu, no HA *control*
entity. The two display toggles `TIMER_ICON_ENABLED` (ADR-0005) and
`TIMER_BAR_ENABLED` (ADR-0004) reach the timer via those same three surfaces
**and** the on-device `TIMER` menu's `ICON` / `PROGRESS BAR` slots — still no HA
control entity. All persist to NVS namespace `"awtrix"`; any matching `dev.json`
key still overrides NVS on every boot. Most of these are, however, **observable**
in HA as read-only JSON attributes on a carrier entity (PRD #57 / ADR-0014) — see
[Read-only attribute groups](#read-only-attribute-groups); the attribute is
observation only and never a write path.

| Global | Default | Effect |
| --- | --- | --- |
| `SHOW_TIMER` | `true` | Master enable. When `false`, the Timer app is hidden from rotation, the 8 Home Assistant entities are not published, `POST /api/timer` and the MQTT `{prefix}/timer` topic are ignored, and any running timer is reset. On the `true → false` transition the firmware publishes empty retained discovery payloads so HA prunes the stale entities on next reconnect. Toggle from `/api/settings` (`TIMER` key), `dev.json` (`show_timer`), or the on-device **APPS** menu (last entry). |
| `TIMER_MAX_DURATION` | `86400` (24 h) | Upper bound on accepted `duration` (range: 1..604800). |
| `TIMER_PUBLISH_INTERVAL` | `1` (s) | How often `timer_rem` re-publishes while running (range: 1..60). |
| `TIMER_FINISHED_HOLD` | `10` (s) | AutoClear notification hold time. |
| `TIMER_REALERT_INTERVAL` | `15` (s) | Re-alert cadence in re-alert mode. |
| `TIMER_COUNTDOWN_SECONDS` | `3` | Number of pre-expiry beep seconds in countdown buzzer mode. |
| `TIMER_MELODY_TICK` | `"timer_tick"` | Bare name resolved against `/MELODIES/<name>.txt` for countdown beeps. |
| `TIMER_MELODY_END` | `"timer_end"` | Bare name resolved against `/MELODIES/<name>.txt` for the end melody. |
| `TIMER_BAR_ENABLED` | `true` | When `false`, the Running/Paused progress bar is hidden. |
| `TIMER_ICON_ENABLED` | `true` | When `false`, the timer icon is hidden and the time text + bar reflow to the full panel (ADR-0005). |
| `TIMER_BAR_COLOR` | `0` (= `TEXTCOLOR_888`) | Hex color for the progress bar. `0` follows the global text color. |
| `TIMER_SYNC_FOLLOW` | `true` | The factory default: a fresh clock is a **follower** — it applies inbound timer-sync packets it is targeted by (the follow consent gate). Set `false` for **standalone** (an explicit opt-out). NVS wins on migration: a device that already stored `false` stays standalone. See **Multi-device sync** below. |
| `TIMER_SYNC_TARGETS` | `""` | Whom this clock commands when *it* acts: `""` (sync off), `all`, or a comma list of peer device IDs (e.g. `awtrix_ab12,awtrix_cd34`). See **Multi-device sync** below. |

---

## Multi-device sync (propagation surface)

Two or more clocks on the same LAN can mirror each other's timer — when one
starts/pauses/resets, the action propagates to a chosen set of peers (or `all`).
This is the **propagation surface** (see [`CONTEXT.md`](../CONTEXT.md)); it is
**broker-free** (no MQTT broker required) and rides a dedicated **UDP broadcast**
on **port 4212**. Full rationale in
[ADR-0006](adr/0006-timer-multi-device-sync.md); the config-propagation half of
that design is superseded by the run-scoped config mirror,
[ADR-0018](adr/0018-run-scoped-config-mirror.md).

### Roles (two independent axes)

| Setting | Axis | Meaning |
| --- | --- | --- |
| `sync_targets` | **send** | Whom this clock commands on a *local* action. `""` = send nothing; `all`; or a comma list of peer `uniqueID`s. |
| `sync_follow` | **receive** | Whether this clock *obeys* inbound sync it is targeted by (consent gate; default **on** — a fresh clock is a follower, standalone is an explicit opt-out). |

Compose them: a **leader** (targets set, follow off) commands but never obeys; a
**follower** (follow on, no targets) obeys but never commands; a **peer/mirror** (both)
does both; **standalone** (neither) is sync off. For "everyone mirrors everyone," set
every clock to `sync_targets=all` and `sync_follow=true`.

### What propagates, and when

- **Run-state** — a `start` / `pause` / `reset` propagate the action; a `start` also
  carries the `duration` (it is the **sole duration-bearing packet**). A bare duration
  edit propagates **nothing** (#126): `duration` is run-state that rides only with a
  start, so a length edit never moves a follower's countdown.
- **Config** — a config edit propagates **nothing**. Config travels **only** bundled
  inside a `start`, as the leader's **effective** config snapshot (the run-scoped
  config mirror, [ADR-0018](adr/0018-run-scoped-config-mirror.md)); the follower
  applies it **one-shot** for that run and reverts to its own saved settings on its
  own return to Idle. A start therefore cannot clobber a peer's saved settings —
  the follower never persists the leader's values.
- **Not propagated:** `sync_follow` / `sync_targets` (each clock's own identity), and the
  live `remaining` (receivers snapshot their own; a missed packet self-heals on the next
  `start`, which re-establishes a shared duration).

### Behavior notes

- **Targeting** is by stable `uniqueID` (the same id shown in the web UI / mDNS), not
  hostname. The `all` keyword spares you from listing ids.
- **One-hop:** an applied inbound command is never re-broadcast. `all` reaches everyone
  directly; partial target lists do **not** chain.
- **Delivery** is best-effort: each packet is sent 3× and receivers de-duplicate by
  `(src, seq)`, so a single dropped frame rarely desyncs a clock. The bounded
  recently-seen `(src, seq)` set is a host-testable module, `SyncSeenCache`, extracted
  from `TimerManager` as a deliberate sibling of the peer registry — same bounded,
  TTL-aged RAM-set shape, opposite expiry semantic (first-seen ages out, no keep-alive
  refresh). See [ADR-0022](adr/0022-sync-seen-cache-extraction.md).
- A clock with `SHOW_TIMER = false` ignores inbound sync (its `parseCommand` is disabled).
- Reachable as `sync_follow` / `sync_targets` on `POST /api/timer` and `{prefix}/timer`
  MQTT, and as `timer_sync_follow` / `timer_sync_targets` in `dev.json`. No on-device
  menu. Both are surfaced as **read-only HA attributes** on the `{id}_timer_state`
  sensor (PRD #57 / ADR-0014) **and** as dedicated **writable HA control entities** —
  the Follow `switch` and the static `Off`/`All` Targets `select` (issue #110, routed
  through `parseCommand`). They are **still never propagated** to peers: being
  HA-writable does not put them in the config snapshot (they remain `inSnapshot=false`
  local identity; ADR-0006 unchanged).

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

## Build-time opt-out

The Timer feature is included by default. To build a firmware without it —
reclaiming roughly 29 kB of flash and 3 kB of RAM on space-constrained
devices — add `-DAWTRIX_DISABLE_TIMER` to `build_flags` in `platformio.ini`,
or set `PLATFORMIO_BUILD_FLAGS=-DAWTRIX_DISABLE_TIMER` when invoking
`pio run`. A disabled build has no Timer app, no `TIMER` on-device menu
entry, no `/api/timer` HTTP endpoint, no `{prefix}/timer` MQTT topic, no
Home Assistant timer entities, and no multi-device sync. The runtime
`SHOW_TIMER` setting is only meaningful in default builds.

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
