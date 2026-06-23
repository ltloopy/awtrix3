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
| Auto-clear delay | `TIMER_FINISHED_HOLD` | `finished mode = auto-clear` | `dev.json` (`timer_finished_hold`), MQTT/HTTP `{prefix}/timer` (`finished_hold`), `TIMER` menu (`AUTOCLEAR` item) |
| Re-alert interval | `TIMER_REALERT_INTERVAL` | `finished mode = re-alert` | `dev.json` (`timer_realert_interval`), MQTT/HTTP `{prefix}/timer` (`realert_interval`), `TIMER` menu (`REALERT` item) |
| Countdown beep window | `TIMER_COUNTDOWN_SECONDS` | `buzzer mode = countdown` | `dev.json` (`timer_countdown_seconds`), MQTT/HTTP `{prefix}/timer` (`countdown_seconds`), `TIMER` menu (`COUNTDOWN` item) |

Persisted in NVS namespace `"awtrix"` (keys `TFHOLD` / `TRALERT` / `TCDOWN`). `dev.json` overrides NVS on every boot. The accepted ranges and persistence for these — and for every value-config key below — are defined once in the persisted-settings table `TIMER_SETTINGS_DESCS` ([src/TimerSettings.h](src/TimerSettings.h)); each surface (`POST /api/timer` / `{prefix}/timer`, `dev.json`, NVS, the `TIMER` menu's clamp, the sync snapshot) is driven from that one table (ADR-0007).

> **"Tuning knobs" — two senses, kept distinct.** The glossary reserves **"tuning knobs"** for *these three* per-mode timing knobs (the ADR-0003 sense) — that narrow reservation is **preserved**. timer.md's **"behavior-tuning knobs"** table is a broader *umbrella* over every persisted-settings row (`TIMER_SETTINGS_DESCS`), not the reserved three. As of PRD #57 / ADR-0014 **every persisted-settings row is additionally projected as a read-only HA attribute on its semantic carrier** (see **Timer HA presence** below); this changes only HA *visibility*, not the glossary's narrow reservation. _Avoid_: reading the umbrella table's name as the reserved three, or the reverse.

### Timer behavior parameters

Three user-editable parameters that shape Timer behavior outside of the **per-mode timing knobs** above. **Each is a distinct category** — they are *not* "tuning knobs" in the ADR-0003 sense. Listed individually so future readers don't lump them. See ADR-0004.

| Parameter | Category | Variable | Default | Controls |
|---|---|---|---|---|
| **Max duration** | Input bound | `TIMER_MAX_DURATION` | 86400 (24 h) | Upper bound on accepted `duration` commands. Out-of-range is rejected, not clamped (ADR-0001). |
| **Remaining publish interval** | Output cadence | `TIMER_PUBLISH_INTERVAL` | 1 s | How often `timer_rem` republishes while Running (also drives the HA `{id}_timer_rem` sensor). |
| **App config timeout** | UI timing | `TIMER_CONFIG_TIMEOUT` | 30 s | No-input idle window before **Timer-app config mode** auto-applies and exits to `Idle`. Does **not** apply to the **TIMER global menu**. |

All three reach: `dev.json` (`timer_max_duration` / `timer_remaining_publish_interval` / `timer_app_config_timeout`), MQTT/HTTP `{prefix}/timer` (`max_duration` / `remaining_publish_interval` / `app_config_timeout`), NVS namespace `"awtrix"` (keys `TMAXD` / `TPUBI` / `TCFGT`). `dev.json` overrides NVS on every boot. No on-device menu; no HA *entity* — but all three are **observable** as read-only HA attributes (`max_duration` on the state sensor as raw seconds **and** on the `{id}_timer_dur` Duration entity in `H:MM:SS` clock form (#68); `app_config_timeout` on the state sensor; `remaining_publish_interval` on the remaining + state sensors) per **Timer HA presence** below.

_Avoid_: "tuning knobs" (reserved for the three per-mode knobs above); "compile-time globals" (they aren't — they're runtime-mutable as of ADR-0004).

### Display-element toggles vs. icon image selection

Two different `icon`/`bar`-named families on the Timer surface; do not conflate them.

- **Display-element toggles** — the `_enabled`-suffixed booleans `icon_enabled` (`TIMER_ICON_ENABLED`, ADR-0005) and `bar_enabled` (`TIMER_BAR_ENABLED`, ADR-0004). Each shows/hides a *drawn element*. `icon_enabled = false` suppresses the icon region entirely — including the built-in hourglass fallback — and reflows **both** the time text and the progress bar to span the full 32px panel (ADR-0005); `bar_enabled = false` only hides the bar. Reachable via `dev.json` / `POST /api/timer` / `{prefix}/timer` MQTT **and** the on-device `TIMER` menu's `ICON` / `BAR` slots. No dedicated HA control entity (surfaced read-only as state-sensor attributes); readable over HTTP in `GET /api/timer`'s `config` mirror (`config.icon_enabled` / `config.bar_enabled`, ADR-0015). Persisted in NVS `"awtrix"` (`TICONEN` / `TBAREN`).
- **Icon image selection** — the `icon_<state>` family `icon_idle` / `icon_running` / `icon_paused` / `icon_finished` (`TIMER_ICON_*`). Each selects *which image* is drawn in a given state; empty falls back per [timer.md](docs/timer.md). These pick the picture; `icon_enabled` decides whether *any* icon (picture or hourglass) is drawn at all.

_Avoid_: reading `icon_enabled` as "enable the idle icon" or as a member of the `icon_<state>` family — it is the master on/off for the whole icon region.

### On-device timer-config surface

The **TIMER global menu** is the on-device place to configure the Timer — including its
**duration** (the `DURATION` leaf, #86). The display-free duration **edit engine** is reused
behind that leaf; the only remaining standalone entry is the Timer-app idle long-press, which
still opens the bare HH:MM:SS wheel for now and is rerouted to open this menu in #87.

- **TIMER global menu** — long-press middle from any app to open the global menu, navigate to the `TIMER` top entry (which now sits **before** `APPS`). A **drill-in navigable list** (ADR-0016, consistent with every other on-device menu): left/right walks the named items and a short press drills into the highlighted item's editor. It edits **duration** (the `DURATION` leaf — the HH:MM:SS wheel, read-only while the timer is Running/Paused), **buzzer mode, finished mode, the three per-mode timing knobs, and the two display-element toggles** (`ICON` / `BAR`, see below), plus a `MAIN` item back to the main menu. Lives in `MenuManager` ([MenuManager.cpp](src/MenuManager.cpp)) over the `TimerMenuNav` state machine ([TimerMenuNav.cpp](src/TimerMenuNav.cpp)).
- **Duration edit engine** — the display-free `TimerConfigEditor` ([TimerConfigEditor.cpp](src/TimerConfigEditor.cpp)): field cursor, the three edit buffers, the cap-aware adjust math, and the button hold-to-repeat (via `tick(nowMs, buttonState)` with injected time + button state). It powers the `DURATION` leaf (a `MenuManager`-owned instance, timeout-free) and — until #87 — the Timer-app idle long-press wheel (`TimerManager`'s `enterConfigMode`/`exitConfigMode`/`configCycleField`/`configAdjust` forwarders). Either path commits the edited duration through `setDuration`. See [ADR-0011](docs/adr/0011-timer-config-editor-extraction.md) (extraction) and [ADR-0012](docs/adr/0012-timer-config-timing-in-editor.md) (timing).

ADR-0001 originally named "the on-device config buttons" as the timer's single on-device control surface — that referred to the Timer-app duration wheel. The global `TIMER` menu (ADR-0003) now carries on-device timer configuration, the `DURATION` leaf folds the duration wheel into it (#86), and #87 reroutes the last standalone entry so the menu is the single surface.

### TIMER menu slot table

The data model behind the **TIMER global menu**'s drill-in list: the table `TIMER_MENU_SLOTS`
([src/TimerMenu.h](src/TimerMenu.h)), a member of the Timer descriptor-table family
alongside `TIMER_SETTINGS_DESCS`, `TIMER_MEMBER_CONFIG_DESCS` and `TIMER_HA_DESCRIPTORS`.
The nine rows are `DURATION` (first), the seven value settings, and the `MAIN` (back-to-main)
row, last. Two accessors split the old label: `timerMenuName(slot)` returns the **list
label** (the item name, e.g. `BUZZER`, shown while walking the list) and `timerMenuValue(slot)`
returns the **bare leaf value** (e.g. `END`, `30`, `ON`, or the `HH:MM:SS` clock for
`DURATION`, shown while editing). `MenuManager` keeps only the drawing + commit; the module
is display-free, so the name/value/clamp/wrap/cycle logic is host-tested. Five slot kinds:

- **Duration slot** (`DURATION`, first) carries no storage row; it delegates to the existing
  display-free `TimerConfigEditor` edit engine (the HH:MM:SS wheel, ADR-0011). `timerMenuLeafKind(slot, state)`
  (host-tested) gates it: an editable wheel only while the timer is **Idle**, read-only
  otherwise. Its value commits via `setDuration` on leaf back-out (run-state, separate from
  the list → main config batch).
- **Table-backed slots** (`SteppedRange`, `BoolToggle`) carry only a `cmdKey`; the dispatch
  reads both *storage* and *range* (`lo`/`hi`) from the matching `TIMER_SETTINGS_DESCS` row.
  The menu reuses the settings table's pointer and bounds, so it cannot drift from it.
- **Enum slots** (buzzer, finished) are member-backed (the B1 boundary, ADR-0007): they
  carry bespoke `getEnum`/`setEnum` hooks, like the settings table's `bespoke` validators.
  Their bare value comes from the per-enum codec's `menu` column (now bare, ADR-0010/0016).
- **Navigation slot** (`MAIN`) carries no storage or value; selecting it returns to the
  main menu.

The interaction model lives in a separate display-free state machine, `TimerMenuNav`
([src/TimerMenuNav.h](src/TimerMenuNav.h), ADR-0016): `{focus ∈ List|Editing,
selectedIndex, origin ∈ Menu|App}`, mapping button inputs to outcomes the device acts on
(navigate-with-wrap, drill-in `EnterLeaf`, `ConfirmBackToList`/`BackToList`, `GoToMainMenu`,
`ExitMenu`). Host-tested `test_N1`..`test_N8`.

All value slots share one commit model (ADR-0008/0016, superseding ADR-0003's split): each
slot **applies to RAM live** while editing — and enum slots also **publish** live, so HA
reflects them — but the **NVS write and the peer broadcast happen only on the list →
main-menu transition** (a long-press out of the list, or selecting `MAIN`): one
`PersistBatch` window (the same commit seam `parseCommand` uses, PRD #29) whose scope exit
flushes deferred enum edits to the `"timer"` namespace and the table half to `"awtrix"`
(`saveSettings()`), then `broadcastConfig()` + the HA attribute republish (#60). A
long-press *inside a leaf* just steps back to the list (value already live in RAM); it does
not commit. Enum slots defer their NVS write via the `persist=false` argument on
`setBuzzerMode`/`setFinishedMode` (sibling to `setIcon*`'s `publish` flag).

_Avoid_: implying enum edits persist per-press (they no longer do, per ADR-0008), or
conflating the `DURATION` leaf's run-state commit (on leaf back-out, via `setDuration`) with
the value slots' config commit (once, on the list → main transition).

### Control surface vs. observation surface

Two distinct kinds of Timer interface; do not conflate them.

- **Control surface** — *writes* timer state/config. The three that must stay in parity (ADR-0001): on-device config buttons, `POST /api/timer`, and the `{prefix}/timer` MQTT topic (plus the HA `timer_dur` text entity as the discovery face of the MQTT one). Their shared obligation is the **atomic-reject validation contract**: an invalid command is rejected whole, nothing applied.
- **Observation surface** — *reads* timer state **and persisted configuration** without mutating it. Today: the Home Assistant MQTT discovery sensors (`{id}_timer_state` / `{id}_timer_rem`, etc., plus their read-only attribute groups) and `GET /api/timer`. Their obligation is **parity of reported values**: every observation surface reports the same live values (same `state` vocabulary, same remaining-seconds basis) **and the same persisted configuration** the others do. They carry *none* of the validation contract — there is no input to validate. `GET /api/timer` reaches config-read parity by returning a nested `config` object mirroring the full persisted configuration (read-only, ADR-0015) — it does not gain a per-entity attribute structure; on HTTP, config-read genuinely *is* just `GET`.

_Avoid_: calling `GET /api/timer` a "control surface" or implying it participates in atomic-reject. It observes; it never writes — even though it now reports the full config, it remains pure read (always `200`, no `409`).

**Carrier-native representation** (a general property of *reads*, on any carrier — not only HA attribute groups). A read surface renders each value in the representation that carrier already uses for its own state, so a multi-carrier key can read differently on each carrier while the underlying value cannot drift (every carrier reads the same persisted storage). On HA attribute groups: `bar_color` as `"default"`/`"#RRGGBB"` and `max_duration` as raw seconds on the state sensor vs. the `H:MM:SS` clock string on the Duration entity. On HTTP `GET /api/timer`: the same `bar_color` rendering, and `max_duration` reported **both** raw and as `max_duration_str` — following the endpoint's own raw+`_str` duration precedent rather than the HA form. The raw-vs-rendered choice thus has a documented home on every carrier; the HTTP `config` mirror deliberately diverges from the raw propagation snapshot, yet cannot disagree in value because both read the same storage.

**Icons are the one deliberate HA observation non-gap.** The four `icon_<state>` names are readable over HTTP (`config.icon_*`, ADR-0015) and MQTT (retained `{prefix}/timer/icons`), but have **no HA read path** by design: they already have two read paths, HA cannot usefully render an AWTRIX icon file, and they sit outside the settings/attribute-group tables. Documented, not an oversight.

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

Carrier entities also carry read-only **JSON attribute groups** (PRD #57 / ADR-0014,
generalizing the bespoke `realert_interval` of PRD #17): the vendored ArduinoHA
`HASelect` (#58), `HASensor` (#59) and `HAText` (#67) each have an opt-in
`setJsonAttributes(bool)` capability, so a carrier's discovery config advertises a
`json_attr_t` topic. Which
settings key rides which carrier is a one-row fact in the `TIMER_ATTR_GROUP_DESCS` table; one
generic builder (`timerBuildAttributeGroup`) turns a carrier's rows into a JSON
object, reusing the same `timerSettingEmitValue` the config snapshot uses so the
two can never disagree about a value. The retained object rides the wire seam to
the carrier's `json_attr_t` topic via `TimerManager.publishAttributeGroup(carrier)`
— sourced through `timerWireAttrTopic(slot)` → `formatTimerHaAttrTopic` (the
json_attr_t sibling of `formatTimerHaDataTopic`, same byte-identity obligation).
The refresh sites call `publishAllAttributeGroups()` right after `publishAllWire()`,
so every group is live the moment the entities come online and (being retained)
after an HA/broker restart; `parseCommand` republishes a carrier's group whenever
one of its mapped keys is edited — local or peer-propagated — so HA always tracks
the live value (catalogued under **Timer HA presence** below).

_Avoid_: publishing Timer MQTT output around the seam, or computing a wire topic
anywhere but the TimerHa builders.

### Timer HA presence

With `HA_DISCOVERY = true` and `SHOW_TIMER = true`, the Timer advertises **eight
MQTT-discovery entities** — the HA face of its **control** and **observation
surfaces**: the `{id}_timer_dur` text (discovery face of the `{prefix}/timer`
control surface), the `{id}_timer_rem` / `{id}_timer_state` sensors (observation),
the `{id}_timer_buz` / `{id}_timer_fin` selects, and the `start` / `pause` / `reset`
buttons. Their ids, names, icons and option strings come from the
`TIMER_HA_DESCRIPTORS` descriptor table — a member of the Timer descriptor-table
family alongside `TIMER_SETTINGS_DESCS`, `TIMER_MEMBER_CONFIG_DESCS`,
`TIMER_MENU_SLOTS` and `TIMER_ATTR_GROUP_DESCS` — and the full list lives in
[timer.md](docs/timer.md). On the `SHOW_TIMER true → false` transition the firmware
publishes empty retained discovery payloads so HA prunes the stale entities, and
clears each attribute carrier's retained `json_attr_t` object
(`clearAllAttributeGroups()`) so the broker is left holding no orphaned attribute
payload (issue #60).

**Read-only attribute groups.** Beyond its own state, a carrier entity can carry a
**read-only JSON attribute object** projecting persisted settings, so a user can read
the device's live configuration from inside HA (PRD #57, ADR-0014). Which key rides
which carrier is the `TIMER_ATTR_GROUP_DESCS` table (the descriptor-table family's
fifth member), each row a `{carrier, settings-key, optional formatter}`; a key may map
to multiple carriers (one row each). **All five carriers** are lit up today — one
`HAText`, two `HASelect`s and two `HASensor`s (#58 lit the selects, #59 the sensors,
#68 the Duration text): the `{id}_timer_dur` Duration text entity carries
`{max_duration}` in clock form (see carrier-native representation below), the
`{id}_timer_fin` finished select carries `{realert_interval, finished_hold}` ("what
happens at zero"), the `{id}_timer_buz` buzzer select carries `{countdown_seconds,
melody_tick, melody_end}`, the `{id}_timer_rem` remaining sensor carries
`{remaining_publish_interval}` (its own cadence), and the `{id}_timer_state` state
sensor carries the full config bag `{max_duration, remaining_publish_interval,
app_config_timeout, icon_enabled, bar_enabled, bar_color, sync_follow, sync_targets}`.
`remaining_publish_interval` and `max_duration` each ride **two** carriers (one settings
row, two table rows). A per-row formatter renders a key in its **carrier-native
representation** (see the general principle below): a multi-carrier key may therefore
read differently on each carrier, while the underlying value cannot drift (every carrier
reads the same persisted storage). `bar_color` first showed an attribute can differ from
the **config snapshot** (`"default"`/`"#RRGGBB"` rather than its raw-int form);
`max_duration` (#68) sharpens that to differ **per carrier** — raw seconds (`86400`) on
the state sensor, the trimmed `H:MM:SS` clock string (`"24:00:00"`, via the same
`formatHMS` the Duration state speaks) on the Duration entity. Values are read-only from
HA; they change only through their config keys, never the attribute. Each carrier's retained object rides the
**Timer wire seam** to
its `json_attr_t` topic via `publishAttributeGroup(carrier)` and is (re)published so HA
never drifts from the device: at **discovery-enable** and every **MQTT (re)connect**
(`publishAllAttributeGroups()`, right after the wire refresh), and on any **edit of a
mapped key** — `parseCommand` republishes exactly the affected carrier(s), on **local
and peer-propagated** edits alike (the edit rides the **propagation surface** into each
peer's `parseCommand`, which fires the same republish). The **on-device `TIMER`-menu long-press commit**
also refreshes every carrier (`publishAllAttributeGroups()`, after its peer
broadcast), since a menu commit does not track which knob changed (issue #60).
Being retained, the last value
survives an HA or broker restart with no extra publish.

**Reusable opt-in capability.** JSON attributes are a generic, opt-in capability on the
vendored ArduinoHA device types, not a Timer-specific hack: `setJsonAttributes(true)`
adds the `json_attr_t` topic to that entity's discovery config and
`publishJsonAttributes(json)` sends a retained object. It defaults **off**, so an entity
that has not opted in keeps its discovery payload byte-for-byte unchanged. The capability
is **per-type, not base-lifted**: it lives on `HASelect` (#58), `HASensor` (#59) and
`HAText` (#67) — `HASensorNumber` inherits it, so the remaining sensor came for free —
because the `json_attr_t` discovery-topic line is irreducibly per-subclass (it must be
emitted where each type's serializer is built and sized), so a base-class lift could not
remove it and would only touch more code (ADR-0014). All five carriers (the Duration
text, the buzzer/finished selects and the remaining/state sensors) are today's
consumers; another entity type can adopt attributes by opting in, with no shared-base
change — as `HAText` did for the Duration entity (#67), the third device type to adopt.

_Avoid_: calling an attribute key an HA *entity* (it is an attribute of its carrier
entity) or *writable from HA* (read-only — the config key is the only write path);
implying the attributes capability is Timer-specific (it is a general per-type opt-in on
`HASelect`/`HASensor`/`HAText`) or that it should be lifted into the base device type;
assuming a key reads the same on every carrier (a multi-carrier key renders in each
carrier's native representation — e.g. `max_duration` as raw seconds on the state sensor
but a clock string on the Duration entity).

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
