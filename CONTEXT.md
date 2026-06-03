# Project glossary

Canonical terms used across the codebase and documentation. When a term appears in code review, docs, or commits, use the meaning defined here.

## Timer

### Finished mode

The categorical setting selecting *what happens when the timer reaches zero*. Three values: `auto-clear` / `hold` / `re-alert`. Stored in `TimerManager::finishedMode` (enum `FinishedMode`), persisted in NVS namespace `"timer"` key `FIN`. Editable via:

- MQTT `{prefix}/timer` (`"finished"` key)
- Home Assistant `{id}_timer_fin` select
- On-device: the `FINISH` slot of the `TIMER` top menu

Not to be confused with the per-mode *timing knobs* below.

### Per-mode timing knobs

Three numeric tunings, each only meaningful in one specific mode:

| Knob | Variable | Relevant when | Editable via |
|---|---|---|---|
| Auto-clear delay | `TIMER_FINISHED_HOLD` | `finished mode = auto-clear` | `dev.json` (`timer_finished_hold`), MQTT/HTTP `{prefix}/timer` (`finished_hold`), `TIMER` top menu (`CLEAR` slot) |
| Re-alert interval | `TIMER_REALERT_INTERVAL` | `finished mode = re-alert` | `dev.json` (`timer_realert_interval`), MQTT/HTTP `{prefix}/timer` (`realert_interval`), `TIMER` top menu (`ALERT` slot) |
| Countdown beep window | `TIMER_COUNTDOWN_SECONDS` | `buzzer mode = countdown` | `dev.json` (`timer_countdown_seconds`), MQTT/HTTP `{prefix}/timer` (`countdown_seconds`), `TIMER` top menu (`CDOWN` slot) |

Persisted in NVS namespace `"awtrix"` (keys `TFHOLD` / `TRALERT` / `TCDOWN`). `dev.json` overrides NVS on every boot.

### Timer behavior parameters

Four user-editable parameters that shape Timer behavior outside of the **per-mode timing knobs** above. **Each is a distinct category** — they are *not* "tuning knobs" in the ADR-0003 sense. Listed individually so future readers don't lump them. See ADR-0004.

| Parameter | Category | Variable | Default | Controls |
|---|---|---|---|---|
| **Max duration** | Input bound | `TIMER_MAX_DURATION` | 86400 (24 h) | Upper bound on accepted `duration` commands. Out-of-range is rejected, not clamped (ADR-0001). |
| **Button step** | Input granularity | `TIMER_STEP` | 1 | How much each left/right press changes the current field in the **Timer-app config mode**. |
| **Remaining publish interval** | Output cadence | `TIMER_PUBLISH_INTERVAL` | 1 s | How often `timer_rem` republishes while Running (also drives the HA `{id}_timer_rem` sensor). |
| **App config timeout** | UI timing | `TIMER_CONFIG_TIMEOUT` | 30 s | No-input idle window before **Timer-app config mode** auto-applies and exits to `Idle`. Does **not** apply to the **TIMER global menu**. |

All four reach: `dev.json` (`timer_max_duration` / `timer_button_step` / `timer_remaining_publish_interval` / `timer_app_config_timeout`), MQTT/HTTP `{prefix}/timer` (`max_duration` / `button_step` / `remaining_publish_interval` / `app_config_timeout`), NVS namespace `"awtrix"` (keys `TMAXD` / `TSTEP` / `TPUBI` / `TCFGT`). `dev.json` overrides NVS on every boot. No on-device menu; no HA entities.

_Avoid_: "tuning knobs" (reserved for the three per-mode knobs above); "compile-time globals" (they aren't — they're runtime-mutable as of ADR-0004).

### Display-element toggles vs. icon image selection

Two different `icon`/`bar`-named families on the Timer surface; do not conflate them.

- **Display-element toggles** — the `_enabled`-suffixed booleans `icon_enabled` (`TIMER_ICON_ENABLED`, ADR-0005) and `bar_enabled` (`TIMER_BAR_ENABLED`, ADR-0004). Each shows/hides a *drawn element*. `icon_enabled = false` suppresses the icon region entirely — including the built-in hourglass fallback — and reflows **both** the time text and the progress bar to span the full 32px panel (ADR-0005); `bar_enabled = false` only hides the bar. Reachable via `dev.json` / `POST /api/timer` / `{prefix}/timer` MQTT **and** the on-device `TIMER` menu's `ICON` / `BAR` slots. No HA entity; not echoed in `GET /api/timer`. Persisted in NVS `"awtrix"` (`TICONEN` / `TBAREN`).
- **Icon image selection** — the `icon_<state>` family `icon_idle` / `icon_running` / `icon_paused` / `icon_finished` (`TIMER_ICON_*`). Each selects *which image* is drawn in a given state; empty falls back per [timer.md](docs/timer.md). These pick the picture; `icon_enabled` decides whether *any* icon (picture or hourglass) is drawn at all.

_Avoid_: reading `icon_enabled` as "enable the idle icon" or as a member of the `icon_<state>` family — it is the master on/off for the whole icon region.

### Two on-device timer-config surfaces

There are two physically distinct on-device places to configure the Timer; use the right name for the right one.

- **Timer-app config mode** — long-press middle from `Idle` while the Timer app is on screen. Edits **duration only** (HH/MM/SS wheels, auto-repeat on hold, 30 s no-input auto-applies). Lives in `TimerManager` ([TimerManager.cpp:483-499](src/TimerManager.cpp#L483)).
- **TIMER global menu** — long-press middle from any app to open the global menu, navigate to the `TIMER` top entry. A seven-slot field walker that edits **buzzer mode, finished mode, the three per-mode timing knobs, and the two display-element toggles** (`ICON` / `BAR`, see below). Lives in `MenuManager` ([MenuManager.cpp](src/MenuManager.cpp)).

ADR-0001 originally named "the on-device config buttons" as the timer's single on-device control surface — that referred to the Timer-app config mode. With the global `TIMER` menu added, on-device timer configuration now spans both surfaces; ADR-0003 documents the addition.

### Control surface vs. observation surface

Two distinct kinds of Timer interface; do not conflate them.

- **Control surface** — *writes* timer state/config. The three that must stay in parity (ADR-0001): on-device config buttons, `POST /api/timer`, and the `{prefix}/timer` MQTT topic (plus the HA `timer_dur` text entity as the discovery face of the MQTT one). Their shared obligation is the **atomic-reject validation contract**: an invalid command is rejected whole, nothing applied.
- **Observation surface** — *reads* timer state without mutating it. Today: the Home Assistant MQTT discovery sensors (`{id}_timer_state` / `{id}_timer_rem`, etc.) and `GET /api/timer`. Their obligation is **parity of reported values**: every observation surface reports the same live values (same `state` vocabulary, same remaining-seconds basis) the others do. They carry *none* of the validation contract — there is no input to validate.

_Avoid_: calling `GET /api/timer` a "control surface" or implying it participates in atomic-reject. It observes; it never writes.

### Propagation surface

A third kind of Timer interface, distinct from both control and observation. The
**propagation surface** is the device-to-device sync channel: when a clock takes a
local control-surface action, it relays that action to other clocks over the network,
and a receiving clock re-applies it locally.

It is **not** a fourth control surface. The "three control surfaces that must stay in
parity" (ADR-0001) are the *user-facing* write paths. The propagation surface carries a
clock's already-formed command to a peer, where it **re-enters the local control
surface** (via the same `parseCommand` path) and is subject to the identical
atomic-reject contract. So a propagated command is validated exactly as a local one;
the propagation surface adds no new validation contract of its own — it is the *output*
of one clock's control surface becoming the *input* to another's.

_Avoid_: counting the propagation surface among "the three" parity surfaces, or
implying it bypasses atomic-reject. It rides on top of the control surface; it does not
join or weaken it.

What the propagation surface carries splits into two classes that move on different
triggers and must not be conflated:

- **Run-state propagation** — carries `action` (start / pause / reset) and/or
  `duration`. Fired by a start, pause, reset, or duration edit. Carries the `action`
  only — never the live `remaining`: receivers snapshot their own remaining, so a
  propagated pause aligns to within network latency, and a *missed* run-state packet
  self-corrects on the next start or reset (which re-establishes a shared duration).
  `duration` is **run-state, not config**: it defines "the same countdown," so it
  travels with the run-state, never inside the config block. A bare start never clobbers
  a peer's config.
- **Config propagation** — carries a **full snapshot** of the Timer config block
  (buzzer mode, finished mode, the per-mode timing knobs, the behavior parameters, the
  display-element toggles, icon images, melodies, bar color) with **no** `action` and
  **no** `duration`. Fired only by a deliberate config edit. Last-config-writer-wins for
  the whole block: after a config edit propagates, the group is configured identically.

_Avoid_: putting `duration` in the config snapshot, or letting a start/reset re-push
config — those reintroduce the "starting a timer rewrote my settings" surprise this
split exists to prevent.

### Sync roles

A clock's participation on the propagation surface is set by two independent axes, not a
single on/off. Use these role names:

- **Target list** — whom this clock *commands* when it takes a local action (its send
  axis). A peer-id list, or the literal `all`. Empty = this clock sends nothing.
- **Follow** — whether this clock *obeys* inbound propagation it is targeted by (its
  receive-consent axis). A clock never acts on sync it did not opt into via follow.

The axes compose into roles: **leader** (target list set, follow off — commands, never
obeys), **follower** (follow on, no targets — obeys, never commands), **peer/mirror**
(both — commands and obeys), **standalone** (neither — sync off). There is no symmetric
"group" primitive; membership is always expressed as one side's target list plus the
other side's consent.
