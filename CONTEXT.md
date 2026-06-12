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

Persisted in NVS namespace `"awtrix"` (keys `TFHOLD` / `TRALERT` / `TCDOWN`). `dev.json` overrides NVS on every boot. The accepted ranges and persistence for these — and for every value-config key below — are defined once in the persisted-settings table `TIMER_SETTINGS_DESCS` ([src/TimerSettings.h](src/TimerSettings.h)); each surface (`POST /api/timer` / `{prefix}/timer`, `dev.json`, NVS, the `TIMER` menu's clamp, the sync snapshot) is driven from that one table (ADR-0007).

### Timer behavior parameters

Three user-editable parameters that shape Timer behavior outside of the **per-mode timing knobs** above. **Each is a distinct category** — they are *not* "tuning knobs" in the ADR-0003 sense. Listed individually so future readers don't lump them. See ADR-0004.

| Parameter | Category | Variable | Default | Controls |
|---|---|---|---|---|
| **Max duration** | Input bound | `TIMER_MAX_DURATION` | 86400 (24 h) | Upper bound on accepted `duration` commands. Out-of-range is rejected, not clamped (ADR-0001). |
| **Remaining publish interval** | Output cadence | `TIMER_PUBLISH_INTERVAL` | 1 s | How often `timer_rem` republishes while Running (also drives the HA `{id}_timer_rem` sensor). |
| **App config timeout** | UI timing | `TIMER_CONFIG_TIMEOUT` | 30 s | No-input idle window before **Timer-app config mode** auto-applies and exits to `Idle`. Does **not** apply to the **TIMER global menu**. |

All three reach: `dev.json` (`timer_max_duration` / `timer_remaining_publish_interval` / `timer_app_config_timeout`), MQTT/HTTP `{prefix}/timer` (`max_duration` / `remaining_publish_interval` / `app_config_timeout`), NVS namespace `"awtrix"` (keys `TMAXD` / `TPUBI` / `TCFGT`). `dev.json` overrides NVS on every boot. No on-device menu; no HA entities.

_Avoid_: "tuning knobs" (reserved for the three per-mode knobs above); "compile-time globals" (they aren't — they're runtime-mutable as of ADR-0004).

### Display-element toggles vs. icon image selection

Two different `icon`/`bar`-named families on the Timer surface; do not conflate them.

- **Display-element toggles** — the `_enabled`-suffixed booleans `icon_enabled` (`TIMER_ICON_ENABLED`, ADR-0005) and `bar_enabled` (`TIMER_BAR_ENABLED`, ADR-0004). Each shows/hides a *drawn element*. `icon_enabled = false` suppresses the icon region entirely — including the built-in hourglass fallback — and reflows **both** the time text and the progress bar to span the full 32px panel (ADR-0005); `bar_enabled = false` only hides the bar. Reachable via `dev.json` / `POST /api/timer` / `{prefix}/timer` MQTT **and** the on-device `TIMER` menu's `ICON` / `BAR` slots. No HA entity; not echoed in `GET /api/timer`. Persisted in NVS `"awtrix"` (`TICONEN` / `TBAREN`).
- **Icon image selection** — the `icon_<state>` family `icon_idle` / `icon_running` / `icon_paused` / `icon_finished` (`TIMER_ICON_*`). Each selects *which image* is drawn in a given state; empty falls back per [timer.md](docs/timer.md). These pick the picture; `icon_enabled` decides whether *any* icon (picture or hourglass) is drawn at all.

_Avoid_: reading `icon_enabled` as "enable the idle icon" or as a member of the `icon_<state>` family — it is the master on/off for the whole icon region.

### Two on-device timer-config surfaces

There are two physically distinct on-device places to configure the Timer; use the right name for the right one.

- **Timer-app config mode** — long-press middle from `Idle` while the Timer app is on screen. Edits **duration only** (HH/MM/SS wheels, auto-repeat on hold, 30 s no-input auto-applies). The display-free editor — field cursor, the three edit buffers, the cap-aware adjust math, and the config-mode timing (the 30 s auto-apply timeout and the button hold-to-repeat, via `tick(nowMs, buttonState)` with injected time + button state) — lives in `TimerConfigEditor` ([TimerConfigEditor.cpp](src/TimerConfigEditor.cpp)); `TimerManager` keeps thin forwarders (`enterConfigMode`/`exitConfigMode`/`configCycleField`/`configAdjust`), feeds the editor button presses from its `tick()`, and commits the edited duration through `setDuration`. See [ADR-0011](docs/adr/0011-timer-config-editor-extraction.md) (extraction) and [ADR-0012](docs/adr/0012-timer-config-timing-in-editor.md) (timing).
- **TIMER global menu** — long-press middle from any app to open the global menu, navigate to the `TIMER` top entry. A seven-slot field walker that edits **buzzer mode, finished mode, the three per-mode timing knobs, and the two display-element toggles** (`ICON` / `BAR`, see below). Lives in `MenuManager` ([MenuManager.cpp](src/MenuManager.cpp)).

ADR-0001 originally named "the on-device config buttons" as the timer's single on-device control surface — that referred to the Timer-app config mode. With the global `TIMER` menu added, on-device timer configuration now spans both surfaces; ADR-0003 documents the addition.

### TIMER menu slot table

The data model behind the **TIMER global menu**'s seven slots: the table `TIMER_MENU_SLOTS`
([src/TimerMenu.h](src/TimerMenu.h)), a member of the Timer descriptor-table family
alongside `TIMER_SETTINGS_DESCS`, `TIMER_MEMBER_CONFIG_DESCS` and `TIMER_HA_DESCRIPTORS`.
`MenuManager` walks it for
label and adjust (`timerMenuLabel` / `timerMenuAdjust`) and keeps only the drawing; the
module is display-free, so the label/clamp/wrap/cycle logic is host-tested. Three slot
kinds:

- **Table-backed slots** (`SteppedRange`, `BoolToggle`) carry only a `cmdKey`; the dispatch
  reads both *storage* and *range* (`lo`/`hi`) from the matching `TIMER_SETTINGS_DESCS` row.
  The menu reuses the settings table's pointer and bounds, so it cannot drift from it.
- **Enum slots** (buzzer, finished) are member-backed (the B1 boundary, ADR-0007): they
  carry bespoke `getEnum`/`setEnum` hooks, like the settings table's `bespoke` validators.

All seven slots share one commit model (ADR-0008, superseding ADR-0003's split): each slot
**applies to RAM live** while scrolling — and enum slots also **publish** live, so HA
reflects them — but the **NVS write and the peer broadcast happen only on the long-press
commit** (`TimerManager::persistConfig()` for the enum half, `saveSettings()` for the table
half, then `broadcastConfig()`). Enum slots defer their NVS write via the `persist=false`
argument on `setBuzzerMode`/`setFinishedMode` (sibling to `setIcon*`'s `publish` flag).

_Avoid_: calling this the "Timer-app config mode" (that is the separate HH/MM/SS duration
editor) or implying enum edits persist per-press (they no longer do, per ADR-0008).

### Control surface vs. observation surface

Two distinct kinds of Timer interface; do not conflate them.

- **Control surface** — *writes* timer state/config. The three that must stay in parity (ADR-0001): on-device config buttons, `POST /api/timer`, and the `{prefix}/timer` MQTT topic (plus the HA `timer_dur` text entity as the discovery face of the MQTT one). Their shared obligation is the **atomic-reject validation contract**: an invalid command is rejected whole, nothing applied.
- **Observation surface** — *reads* timer state without mutating it. Today: the Home Assistant MQTT discovery sensors (`{id}_timer_state` / `{id}_timer_rem`, etc.) and `GET /api/timer`. Their obligation is **parity of reported values**: every observation surface reports the same live values (same `state` vocabulary, same remaining-seconds basis) the others do. They carry *none* of the validation contract — there is no input to validate.

_Avoid_: calling `GET /api/timer` a "control surface" or implying it participates in atomic-reject. It observes; it never writes.

### Timer wire seam

The single chokepoint through which Timer MQTT output flows as `(topic, payload)`
strings: `MQTTManager.publishTimerWire(topic, payload)`. On device it reaches the
broker (retained, exactly like the ArduinoHA `setValue` path it replaces); in host
tests the stub records the pairs, so tests assert the **real wire contract** — the
exact topic and payload the broker would see — not the internal dispatch path.

An entity's canonical topic is sourced through `timerWireTopic(slot)` →
`formatTimerHaDataTopic` in TimerHa (host-compiled), which **must stay
byte-identical** to what ArduinoHA's `HASerializer::generateDataTopic` emits
(`{dataPrefix}/{deviceUniqueId}/{entityId}/stat_t`). Re-routing a key through the
seam is a structural change only; any topic or payload difference it introduces is
a bug. **All** Timer publishes flow through the seam: the run-state keys (`state`,
`remaining`, `duration`) directly, the member-config keys via their
`TIMER_MEMBER_CONFIG_DESCS` rows' publish hooks. Re-routing changes how a
publish is expressed, never when it fires — the periodic `remaining` republish
keeps its `TIMER_PUBLISH_INTERVAL` throttle in `tick()`.

The **full wire refresh** — every wire artifact republished once, on MQTT
(re)connect or discovery enable — is `TimerManager.publishAllWire()`: the
run-state trio plus the member table's publish hooks (deduped, so the shared
icons hook fires once). Because the config half is derived from the table, a new
published row cannot be silently skipped by the refresh.

_Avoid_: publishing Timer MQTT output around the seam, or computing a wire topic
anywhere but the TimerHa builders.

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
  Concretely, the config block is **two tables**: the `inSnapshot == true` rows of
  `TIMER_SETTINGS_DESCS` (the declarative half) and `TIMER_MEMBER_CONFIG_DESCS` (the
  member-backed half — `buzzer`/`finished`/icons, B1). Each table feeds both the snapshot
  build and the `parseCommand` broadcast trigger, so the snapshot can't drift from the
  trigger (ADR-0007, ADR-0009).

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

Mechanically, `TIMER_SYNC_FOLLOW` / `TIMER_SYNC_TARGETS` are the `inSnapshot == false`
rows of `TIMER_SETTINGS_DESCS` — persisted and validated like every other table key, but
deliberately excluded from the config snapshot so peers can't hijack each other's
targeting (ADR-0006, ADR-0007).
