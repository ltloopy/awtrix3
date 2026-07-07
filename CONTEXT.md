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
| Auto-clear delay | `TIMER_FINISHED_HOLD` | `finished mode = auto-clear` | `dev.json` (`timer_finished_hold`), MQTT/HTTP `{prefix}/timer` (`finished_hold`), `TIMER` menu (`CLEAR DELAY` item) |
| Re-alert interval | `TIMER_REALERT_INTERVAL` | `finished mode = re-alert` | `dev.json` (`timer_realert_interval`), MQTT/HTTP `{prefix}/timer` (`realert_interval`), `TIMER` menu (`RE-ALERT INTERVAL` item) |
| Countdown beep window | `TIMER_COUNTDOWN_SECONDS` | `buzzer mode = countdown` | `dev.json` (`timer_countdown_seconds`), MQTT/HTTP `{prefix}/timer` (`countdown_seconds`), `TIMER` menu (`COUNTDOWN` item) |

Persisted in NVS namespace `"awtrix"` (keys `TFHOLD` / `TRALERT` / `TCDOWN`). `dev.json` overrides NVS on every boot. The accepted ranges and persistence for these — and for every value-config key below — are defined once in the persisted-settings table `TIMER_SETTINGS_DESCS` ([src/TimerSettings.h](src/TimerSettings.h)); each surface (`POST /api/timer` / `{prefix}/timer`, `dev.json`, NVS, the `TIMER` menu's clamp, the sync snapshot) is driven from that one table (ADR-0007).

> **"Tuning knobs" — two senses, kept distinct.** The glossary reserves **"tuning knobs"** for *these three* per-mode timing knobs (the ADR-0003 sense) — that narrow reservation is **preserved**. timer.md's **"behavior-tuning knobs"** table is a broader *umbrella* over every persisted-settings row (`TIMER_SETTINGS_DESCS`), not the reserved three. As of PRD #57 / ADR-0014 **every persisted-settings row is additionally projected as a read-only HA attribute on its semantic carrier** (see **Timer HA presence** below); this changes only HA *visibility*, not the glossary's narrow reservation. _Avoid_: reading the umbrella table's name as the reserved three, or the reverse.

### Timer behavior parameters

Two user-editable parameters that shape Timer behavior outside of the **per-mode timing knobs** above. **Each is a distinct category** — they are *not* "tuning knobs" in the ADR-0003 sense. Listed individually so future readers don't lump them. See ADR-0004. (A third, `app_config_timeout`, was **removed** in PRD #83 / #88 — the on-device TIMER menu is timeout-free, so the no-input auto-apply window no longer has a place. Inbound commands still carrying the key are silently ignored; the stored NVS `TCFGT` key is left as dead bytes, no migration.)

| Parameter | Category | Variable | Default | Controls |
|---|---|---|---|---|
| **Max duration** | Input bound | `TIMER_MAX_DURATION` | 86400 (24 h) | Upper bound on accepted `duration` commands. Out-of-range is rejected, not clamped (ADR-0001). |
| **Remaining publish interval** | Output cadence | `TIMER_PUBLISH_INTERVAL` | 1 s | How often `timer_rem` republishes while Running (also drives the HA `{id}_timer_rem` sensor). |

Both reach: `dev.json` (`timer_max_duration` / `timer_remaining_publish_interval`), MQTT/HTTP `{prefix}/timer` (`max_duration` / `remaining_publish_interval`), NVS namespace `"awtrix"` (keys `TMAXD` / `TPUBI`). `dev.json` overrides NVS on every boot. No on-device menu; no HA *entity* — but both are **observable** as read-only HA attributes (`max_duration` on the state sensor as raw seconds **and** on the `{id}_timer_dur` Duration entity in `H:MM:SS` clock form (#68); `remaining_publish_interval` on the remaining + state sensors) per **Timer HA presence** below.

_Avoid_: "tuning knobs" (reserved for the three per-mode knobs above); "compile-time globals" (they aren't — they're runtime-mutable as of ADR-0004).

### Display-element toggles vs. icon image selection

Two different `icon`/`bar`-named families on the Timer surface; do not conflate them.

- **Display-element toggles** — the `_enabled`-suffixed booleans `icon_enabled` (`TIMER_ICON_ENABLED`, ADR-0005) and `bar_enabled` (`TIMER_BAR_ENABLED`, ADR-0004). Each shows/hides a *drawn element*. `icon_enabled = false` suppresses the icon region entirely — including the built-in hourglass fallback — and reflows **both** the time text and the progress bar to span the full 32px panel (ADR-0005); `bar_enabled = false` only hides the bar. Reachable via `dev.json` / `POST /api/timer` / `{prefix}/timer` MQTT **and** the on-device `TIMER` menu's `ICON` / `PROGRESS BAR` slots. No dedicated HA control entity (surfaced read-only as state-sensor attributes); readable over HTTP in `GET /api/timer`'s `config` mirror (`config.icon_enabled` / `config.bar_enabled`, ADR-0015). Persisted in NVS `"awtrix"` (`TICONEN` / `TBAREN`).
- **Icon image selection** — the `icon_<state>` family `icon_idle` / `icon_running` / `icon_paused` / `icon_finished` (`TIMER_ICON_*`). Each selects *which image* is drawn in a given state; empty falls back per [timer.md](docs/timer.md). These pick the picture; `icon_enabled` decides whether *any* icon (picture or hourglass) is drawn at all.

_Avoid_: reading `icon_enabled` as "enable the idle icon" or as a member of the `icon_<state>` family — it is the master on/off for the whole icon region.

### Bar foreground vs. background color

The Timer progress bar has **two** color knobs, mirroring the custom-app `progressC` / `progressBC` pair:

- **`bar_color`** (`TIMER_BAR_COLOR`) — the **foreground**, the draining "remaining" segment. `0` is a **sentinel**: *follow the text color* (`TEXTCOLOR_888`). See ADR-0004.
- **`bar_bg_color`** (`TIMER_BAR_BG_COLOR`) — the **background track** drawn behind the bar (the trough). `0` is a **literal** black = LEDs off = *no track* — the default, so the bar looks exactly as it did before the background existed. See ADR-0020.

The meaning of `0` is therefore deliberately **asymmetric** between the two, and surfaces on read carriers as `"default"` (foreground) vs `"none"` (background). The track **persists for the whole Running/Paused window** — it stays visible after the foreground has drained below one cell — and both are hidden together when `bar_enabled = false`.

_Avoid_: reading `bar_bg_color = 0` as "follow text color" (that is the *foreground's* sentinel; the background's `0` is plain black / off); expecting the background to vanish when the foreground drains (the trough persists, unlike the foreground bar).

### On-device timer-config surface

The **TIMER global menu** is the **single** on-device place to configure the Timer —
including its **duration** (the `DURATION` leaf, #86). Both ways in lead to the same menu:
the main-menu `TIMER` entry (origin = Menu) and the **Timer-app idle long-press** (origin =
App, #87). The display-free duration **edit engine** is reused behind the `DURATION` leaf;
the bare HH:MM:SS wheel has no standalone entry point anymore.

- **TIMER global menu** — open it via the main menu's `TIMER` entry (now **before** `APPS`) or by long-pressing middle from the Timer app while **Idle**. A **drill-in navigable list** (ADR-0016, consistent with every other on-device menu): left/right walks the named items and a short press drills into the highlighted item's editor. It edits **duration** (the `DURATION` leaf — the HH:MM:SS wheel, read-only while the timer is Running/Paused), **buzzer mode, finished mode, the three per-mode timing knobs, and the two display-element toggles** (`ICON` / `PROGRESS BAR`, see below), plus a `MAIN` item back to the main menu. Lives in `MenuManager` ([MenuManager.cpp](src/MenuManager.cpp)) over the `TimerMenuNav` state machine ([TimerMenuNav.cpp](src/TimerMenuNav.cpp)). **Context-aware exit:** a long-press out of the list returns to the Timer app when entered from the app, or to the main menu when entered from the main menu; `MAIN` always commits and goes to the main menu.
- **Duration edit engine** — the display-free `TimerConfigEditor` ([TimerConfigEditor.cpp](src/TimerConfigEditor.cpp)): field cursor, the three edit buffers, the cap-aware adjust math, and the button hold-to-repeat (via `tick(nowMs, buttonState)` with injected time + button state). It powers the `DURATION` leaf (a `MenuManager`-owned instance, timeout-free); the edited duration commits through `setDuration` on leaf back-out. See [ADR-0011](docs/adr/0011-timer-config-editor-extraction.md) (extraction) and [ADR-0012](docs/adr/0012-timer-config-timing-in-editor.md) (timing).

ADR-0001 originally named "the on-device config buttons" as the timer's single on-device control surface — that referred to the Timer-app duration wheel. The global `TIMER` menu (ADR-0003) now carries on-device timer configuration, the `DURATION` leaf folds the duration wheel into it (#86), and the Timer-app idle long-press now opens this menu (#87) — so it is once again a single surface, just the richer menu.

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
reflects them — but the **NVS write and the HA attribute republish happen only on the list →
main-menu transition** (a long-press out of the list, or selecting `MAIN`): one
`PersistBatch` window (the same commit seam `parseCommand` uses, PRD #29) whose scope exit
flushes deferred enum edits to the `"timer"` namespace and the table half to `"awtrix"`
(`saveSettings()`), then the HA attribute republish (#60). A
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

**Carrier-native representation** (a general property of *reads*, on any carrier — not only HA attribute groups). A read surface renders each value in the representation that carrier already uses for its own state, so a multi-carrier key can read differently on each carrier while the underlying value cannot drift (every carrier reads the same persisted storage). On HA attribute groups: `bar_color` as `"default"`/`"#RRGGBB"`, `bar_bg_color` as `"none"`/`"#RRGGBB"`, and `max_duration` as raw seconds on the state sensor vs. the `H:MM:SS` clock string on the Duration entity. On HTTP `GET /api/timer`: the same `bar_color` rendering, and `max_duration` reported **both** raw and as `max_duration_str` — following the endpoint's own raw+`_str` duration precedent rather than the HA form. The raw-vs-rendered choice thus has a documented home on every carrier; the HTTP `config` mirror deliberately diverges from the raw propagation snapshot, yet cannot disagree in value because both read the same storage.

**Icons are the one deliberate HA observation non-gap.** The four `icon_<state>` names are readable over HTTP (`config.icon_*`, ADR-0015) and MQTT (retained `{prefix}/timer/icons`), but have **no HA read path** by design: they already have two read paths, HA cannot usefully render an AWTRIX icon file, and they sit outside the settings/attribute-group tables. Documented, not an oversight.

### The command plan

The validated, staged form of one control-surface command, produced by the **pure**
classifier `TimerCommand::classify(packet, Context) -> Plan`
([src/TimerCommand.h](src/TimerCommand.h)) — a deliberate **fourth sibling** of
`PeerRegistry`/`SyncSeenCache`/`SyncEnvelope`, and like `SyncEnvelope` **stateless** (free
functions over value structs, no set to own). `classify` runs the whole **atomic-reject
validation contract** (ADR-0001) in one mutate-nothing pass — the table rows
(`TIMER_SETTINGS_DESCS`), the `duration` cross-field check, the member-backed half
(`TIMER_MEMBER_CONFIG_DESCS`), the `action`, the payload-level `save` flag and inline-melody
classification — and on the first invalid field returns `{ok=false, reject}` (the coarse
`TimerCmdResult::BadField`, wire parity) having staged nothing. On success the `Plan` carries
**everything apply needs**: the staged table/member values, `durationSec`, `oneShot`, the
parsed `action`, the dirty HA-attr-carrier set and `configInCommand` — so apply never re-reads
the packet. Its `Context {savedMaxDuration, remoteApply}` injects the only two non-packet
inputs (the saved `TIMER_MAX_DURATION` ceiling and the receiver-forced one-shot flag, mirroring
`SyncEnvelope`'s `{ownId, follow}` discipline), so `classify` reads **no globals** and is
host-tested in its own `[env:native_validate]` linking the descriptor-table family
(`TimerSettings.cpp` + the codecs + `parseHMS`/`isValidAction`, relocated out of the singleton)
and ArduinoJson, but **no singleton**. That isolation claim is deliberately weaker than the
three siblings' "links nothing" — the validator *is* the descriptor tables, the contract it
enforces. `TimerManager::parseCommand` keeps the **impure shell**: fill `Context`, `classify`,
then on `ok` apply the `Plan` (one `PersistBatch`; the override capture/rebaseline that **stays
in `TimerManager`** per ADR-0024; the HA-attr republish; the run-state broadcast). Only the
validation **decision** leaves — the override **store** does not. See ADR-0025.

**Apply keeps one ordering invariant.** `setDuration` re-clamps against the **global**
`TIMER_MAX_DURATION`, not the staged ceiling, so applying a `Plan` **must** write the table
rows (landing a raised `max_duration`) **before** `setDuration`, or a duration `classify`
accepted against the new ceiling is silently clamped to the old one (the ADR-0001 addendum).
The `Plan` is order-free data; the one constraint lives in apply.

_Avoid_: calling the command plan a control surface (it is the validated *form* of one
command, not a write path); putting any of the receiver's own state into `Context` beyond
`{savedMaxDuration, remoteApply}` (the `SyncEnvelope` lesson — an unused gate field gets
"wired up" wrong later); implying the override snapshot moved out (ADR-0024 keeps it in
`TimerManager`); expecting `classify` to write anything (it stages only — the shell applies).

### One-shot command (`save:false`)

A Timer command carrying the payload-level boolean **`save:false`** (PRD #99 / ADR-0017). Its config changes apply to the **current run only** and revert to the saved settings the moment the timer returns to Idle (via `reset()` or auto-clear, through the shared `returnToIdle()` seam), **writing nothing to flash**. `save` defaults to `true` (the legacy persist-everything behaviour); it is one **payload-level** flag covering *all* config keys in the payload, validated in the atomic-reject pass (a non-boolean `save` is `BadField`/400). A normal (`save:true`) config command arriving *during* an active one-shot run **rebaselines** — it commits the live config, including the prior one-shot values, as the new saved baseline and ends the override. The on-device **TIMER menu always persists** (it never emits `save:false`) — a documented parity note, not an ADR-0001 control-surface divergence (`save` is a wire-payload concept the menu has no payload for).

_Avoid_: reading `save` as the MQTT *retain* flag or as the custom-apps `save` key (a different feature); calling it per-key (it is payload-level).

### Saved config vs effective run config

The distinction the one-shot model rests on. **Saved config** is the flash (NVS) truth that *every* observation/sync carrier reports; **effective run config** is what the active run actually uses. A `save:false` command writes only the **effective** layer: it captures a snapshot of the saved config (generically over the two descriptor tables — `TIMER_SETTINGS_DESCS`' `inSnapshot` rows + `TIMER_MEMBER_CONFIG_DESCS` — plus `durationSec` and the resolved melody RAM), applies live, and suppresses the persist and HA-attribute side effects. While an override is active the **read** carriers stay **honest** — the `GET /api/timer` `config` mirror and the HA attribute bags serialize from the **saved** snapshot — while the **top-level run-state** (`duration`, `buzzer`, `finished` at the top level of `GET /api/timer`) stays **effective**. The split is: *top level = effective (what is running now), `config` = saved (what is persisted)*. The one deliberate exception is the **device-to-device sync carrier**: the config snapshot bundled with a `start` reports **effective** config (ADR-0018 §4), precisely so a leader's own one-shot run mirrors to followers — the follower then applies it one-shot and reverts on its own return to Idle. `sync_*` (local identity) and `duration` (run-state) are excluded from the snapshot by construction. See ADR-0017 and ADR-0018. The snapshot store itself **stays inside `TimerManager`** (it is not extracted into a `ConfigSnapshot` module) — the config it captures is two families (the generic settings table + the singleton's own member-backed half), so a standalone store would be shallow and untestable in isolation; the grilling that declined the extraction is [ADR-0024](docs/adr/0024-one-shot-override-store-stays-in-timermanager.md).

_Avoid_: implying the `config` mirror or the HA attribute bags report the one-off values during an override (they report **saved**); implying the **sync** snapshot reports saved during an override (it reports **effective** — the run-scoped config mirror, ADR-0018); implying top-level `duration` reverts to saved during a run (it shows the **live** one-shot value).

### Inline melody

An **inline RTTTL tune** supplied directly on `melody_end`/`melody_tick` (a literal melody string), as opposed to a **bare name** that resolves to an uploaded `/MELODIES/<name>.txt` file. The two are distinguished by content: a bare name is `[A-Za-z0-9_-]*` (no colon); an inline tune carries RTTTL structure (`name:control:notes` with a `d=`/`o=`/`b=` control section), validated by a pure classifier/validator (a malformed inline tune is `BadField`/400). An inline tune is **always one-shot regardless of `save`** — it has no persistable file form, so it is staged directly into the resolved melody RAM, the saved melody-name global is never touched, it **never appears** in any carrier (audible-only run-state; the `config` mirror shows the **saved bare name** throughout), and it reverts on return to Idle. A bare name persists and obeys `save` exactly as before. See ADR-0017 / issue #102.

_Avoid_: expecting an inline tune in the `GET /api/timer` `config` mirror or HA bags (it is never there); treating a bare name as one-shot (only inline tunes are inherently one-shot).

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

With `HA_DISCOVERY = true` and `SHOW_TIMER = true`, the Timer advertises **ten
MQTT-discovery entities** — the HA face of its **control** and **observation
surfaces**: the `{id}_timer_dur` text (discovery face of the `{prefix}/timer`
control surface), the `{id}_timer_rem` / `{id}_timer_state` sensors (observation),
the `{id}_timer_buz` / `{id}_timer_fin` selects, the `start` / `pause` / `reset`
buttons, and the two **sync-control** entities — a `{id}_timer_sync_follow` `HASwitch`
and a static `Off`/`All` `{id}_timer_sync_targets` `HASelect` (issue #110) that make
the two sync settings writable from HA (routed through `parseCommand`, so they inherit
atomic-reject validation and NVS persistence; both stay `inSnapshot=false` local
identity and never propagate to peers — ADR-0006/0014). The Targets select reflects
`Off`/`All`, or **unknown** when `sync_targets` holds a specific-ID CSV set out-of-band
(the read-only state-sensor attribute stays authoritative for the exact value). Their
ids, names, icons and option strings come from the
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
icon_enabled, bar_enabled, bar_color, bar_bg_color, sync_follow, sync_targets}`.
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
entity) or *writable from HA* (the attribute projection is read-only — the config key
is the write path; note `sync_follow` / `sync_targets` ALSO have their own dedicated
writable control entities since issue #110, but that is a separate switch/select, not
the read-only attribute on the state sensor);
implying the attributes capability is Timer-specific (it is a general per-type opt-in on
`HASelect`/`HASensor`/`HAText`) or that it should be lifted into the base device type;
assuming a key reads the same on every carrier (a multi-carrier key renders in each
carrier's native representation — e.g. `max_duration` as raw seconds on the state sensor
but a clock string on the Duration entity).

### Timer HA host

The **impure, device-bound half** of the Timer's Home Assistant surface, paired with the
pure **Timer HA presence** builders (`TimerHa`: topics, descriptors, options,
`haRegistrationAtCap`). `TimerHaHost` (`src/TimerHaHost.{h,cpp}`) owns the ten HA
**carrier** entities' *lifecycle* — the carrier pointers and their resolved discovery ids
(private state), constructing/registering them (idempotently, at the same HA-setup
sequence point so the ArduinoHA registration/entity-cap drop order is unchanged), the
runtime `enable()`/`remove()` on the `SHOW_TIMER` toggle, `onConnected()` republish, the
boot-time `reconcile()` (SHOW_TIMER-vs-persisted-`SHOW_TIMER_HA_PREV`, latching a
**private** one-shot discovery cleanup that `onConnected()` flushes when the timer was
toggled off across a reboot — issue #195), the dedicated Timer **duration text callback**,
and the Timer branches of the shared ArduinoHA select/switch/button callbacks. Those shared callbacks still live in `MQTTManager` (they
keep their non-Timer branches) and delegate the Timer sender via a leading
`if (TimerHaHost.tryHandle…(…)) return;`; each `tryHandle*` returns whether the sender was
a Timer carrier and, when so, performs the same route-through-`timerHaApply` +
snap-back-echo it always did (ADR-0001 / ADR-0014). The dynamic Targets select's
**republish debounce decision** lives in the host-tested `SyncTargetsDebounce` module
(`src/SyncTargetsDebounce.{h,cpp}`, ADR-0027): `refreshTargets()` keeps the shell —
carrier/connected guards **before** `step()`, the options build, then the effects
(rebuild options, republish discovery, re-apply state) only on `Republish`; carrier
creation `seed()`s the baseline. The host reaches the `HADevice`/`HAMqtt`
client via the `extern` globals still owned by `MQTTManager` — **no injection** (one
implementation forever; ADR-0026). `reconcile()` likewise reaches `SHOW_TIMER_HA_PREV`
via `extern`: it stays a **Globals-owned persisted (NVS-backed) flag**, not host-private,
because the settings-apply path writes it independently of reconcile — just like
`SHOW_TIMER` itself. After issue #195 `MQTTManager` holds **zero** Timer-HA-specific state. It is a **relocation of responsibility, not a redesign**:
no discovery payload, wire topic, retained value, attribute group, callback outcome, or
snap-back echo changes; delete it and the ten-carrier lifecycle scatters straight back
across the general MQTT module (today's pre-extraction state). Being device-bound (it
`new HAText(...)`s the real client), it is excluded from every `native*` build exactly like
`MQTTManager.cpp` and ships **no** host-test env — its value is locality and a shrunk
`MQTTManager`, and it is the seam the Pomodoro host (#119) will reuse. The **Timer wire seam**
stays in `MQTTManager` and reaches the host's private carrier state through two small
accessors (`carriersReady()`, `entityId(slot)`).

_Avoid_: reading "the Timer's HA carrier lifecycle" out of `MQTTManager` (it lives in
`TimerHaHost` now); adding a Timer carrier or callback branch to the general MQTT module
(it belongs on the host); introducing client injection for the ArduinoHA globals (the host
reads them via `extern` by design — ADR-0026); expecting a host-test env for `TimerHaHost`
(there is none — it is device-bound; the regression surface is the existing green suites +
the `ulanzi`/`native` builds compiling).

### Propagation surface

A third kind of Timer interface, distinct from both control and observation. The
**propagation surface** is the device-to-device sync channel: when a clock takes a
local control-surface action, it relays that action to other clocks over the network,
and a receiving clock re-applies it locally. On the send side there is one seam: every
local run-state actor (MQTT/HTTP/HA via `parseCommand`, the physical buttons) calls
`TimerManager.runStateAction()`, which pairs the verb with its peer mirror itself —
`broadcastRunState` is private, so the pairing is no longer a caller obligation (#213).

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

- **Run-state propagation** — carries `action` (start / pause / reset). Fired by a
  start, pause, or reset. Carries the `action` only — never the live `remaining`:
  receivers snapshot their own remaining, so a propagated pause aligns to within network
  latency, and a *missed* run-state packet self-corrects on the next `start` (which
  re-establishes a shared duration). `duration` is **run-state, not config**: it defines
  "the same countdown," so it rides **only inside a `start`**, never inside the config
  block and never on its own. A **bare duration edit propagates nothing** (#126): a
  leader's length change no longer moves a follower's displayed time — the follower
  adopts the leader's duration on the next `start` and reverts to its own on Idle. A bare
  start never clobbers a peer's config.
- **Run-scoped config mirror** — config no longer propagates on a config *edit*; it
  travels **only bundled with a `start`** (ADR-0018, superseding ADR-0006's config-edit
  snapshot). A `start` broadcasts **one combined packet** = the leader's **effective**
  config snapshot + `duration` + `action:"start"`, scoped to that run — the `start` is
  the **sole duration-bearing packet**; `pause`/`reset` stay run-state-only (action
  alone) and a bare duration edit propagates **nothing** (#126). A follower applies a
  received command **one-shot** (receiver-forced via the `_remoteApply`
  guard, reusing the ADR-0017 override core): it mirrors the leader for the run, persists
  **nothing**, and reverts to its **own** saved config/duration on return to Idle —
  regardless of the leader's `save` flag. So a config edit no longer rewrites peers' saved
  settings (each clock keeps its own identity), yet a run still mirrors. Concretely, the
  config block is **two tables**: the `inSnapshot == true` rows of `TIMER_SETTINGS_DESCS`
  (the declarative half) and `TIMER_MEMBER_CONFIG_DESCS` (the member-backed half —
  `buzzer`/`finished`/icons, B1), built into the combined start packet (ADR-0007, ADR-0009).
  The bundled snapshot reports **effective** config (so a leader's own one-shot run mirrors
  to followers) — deliberately unlike the `GET /api/timer` `config` mirror and the HA
  attribute bags, which stay **saved** (ADR-0017 §3, ADR-0018 §4). Inline melodies never
  travel (the snapshot reads the saved bare name).

**Receive dedup** — a clock broadcasts each command as a small burst of redundant copies
(loss tolerance), so the receiver must apply each `(src, seq)` **exactly once**. That
bounded recently-seen set lives in its own host-testable module, `SyncSeenCache`
([src/SyncSeenCache.h](src/SyncSeenCache.h)) — a deliberate **sibling of `PeerRegistry`**
(same bounded, TTL-aged, src-keyed RAM-set shape), kept separate rather than merged under a
shared generic. Its single `seen(src, seq, nowMs)` op test-and-records in one atomic call:
the first copy records and returns `false` (apply it), redundant copies within the TTL
return `true` (drop them) **without** refreshing the entry, so first-seen ages out (the
dedup semantic — opposite `PeerRegistry`'s keep-alive refresh). A FIFO ring evicts at the
bound and entries age out past the TTL, so a sender reboot (seq restart) self-clears.
`TimerManager` owns a `SyncSeenCache` and keeps the UDP transport + the `parseCommand`
re-entry; only the set/algorithm moved out (the cut-line mirrors ADR-0021). The
extraction rationale — and why the sibling is **not** merged with `PeerRegistry` under a
generic — is recorded in [ADR-0022](docs/adr/0022-sync-seen-cache-extraction.md).

**The sync gate** — the inbound **decision** ("what should this clock do with this
packet?") lives in its own host-testable module, `SyncEnvelope`
([src/SyncEnvelope.h](src/SyncEnvelope.h)) — a deliberate **third sibling** of
`PeerRegistry`/`SyncSeenCache`, but **stateless** (free functions over a value struct, no
set to own). Its `classify(packet, {ownId, follow}) -> Decision {Ignore | HarvestPresence |
Apply}` is a **pure** function deciding the **four** gates — echo-drop, ungated presence,
follow consent, target match (`targetsMe`) — reading no globals, no clock, no cache. Its
context is `{ownId, follow}` **only**: the receive decision turns on the **sender's** `tgt`
plus this clock's **own consent**, never this clock's own target list (the two independent
**Sync roles** axes — putting the local target list here would break the follower role).
The **dedup** is deliberately *not* in `classify` — `SyncSeenCache.seen()` is stateful
(test-and-record), so it stays the **shell's** single guard between the `Apply` verdict and
the re-entry. `SyncEnvelope::build` is the symmetric send half (the `{src,seq,tgt}` envelope
+ the target-CSV split, `seq` injected from `TimerManager`'s `_syncSeq`). `TimerManager`
keeps the impure shell: deserialize, then act on the `Decision` (record presence /
dedup-then-`parseCommand` under `_remoteApply` / send). See
[ADR-0023](docs/adr/0023-sync-gate-extraction.md).

_Avoid_: putting `duration` in the config snapshot; expecting a config *edit* to propagate
(it no longer does — config rides only with a `start`, ADR-0018); or expecting a follower
to persist a synced run (it is always one-shot on receive and reverts on Idle); confusing
the **wire-gate** `SyncEnvelope::targetsMe` (does the sender's `tgt` cover me?) with
`TimerHa`'s **HA-select** `timerSyncTargets*` cluster (mapping the Targets *select*'s
`Off`/`All`/peer-id ↔ option index) — same words, opposite surfaces, separate files.

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

The **factory default is follow-on with no targets**, so a fresh clock's default role is
**follower**: it obeys sync it is targeted by but commands nobody (safe, because an empty
target list means it can hijack no one). **Standalone is an explicit opt-out**
(`follow=false`). The default lives in two places kept in step — the RAM initializer
(`Globals.cpp`) and the `sync_follow` descriptor's persisted-settings fallback — but **NVS
wins on migration**: a clock that already stored `follow=false` stays standalone across the
update (#124).

Mechanically, `TIMER_SYNC_FOLLOW` / `TIMER_SYNC_TARGETS` are the `inSnapshot == false`
rows of `TIMER_SETTINGS_DESCS` — persisted and validated like every other table key, but
deliberately excluded from the config snapshot so peers can't hijack each other's
targeting (ADR-0006, ADR-0007).

### Peer presence / peer registry

The set of *other clocks currently on the LAN*, learned passively over the same
propagation-surface UDP channel (port 4212). It exists so a clock can offer a list of
real, reachable peer ids — the **stable `uniqueID`**, the targeting key — without the user
hand-typing them. `FIND_AWTRIX` is unsuitable: it returns the user-mutable **hostname**,
not the `uniqueID`. See [ADR-0019](docs/adr/0019-peer-presence-registry.md).

- **Presence beacon** — a small `{_sync:{src,seq}, presence:true}` packet each clock
  broadcasts periodically (~30s, `kPresenceIntervalMs`) **unconditionally** when on a real
  network — *not* in AP mode (so a standalone clock with no real LAN does not beacon, but
  one on a network is discoverable even if it commands nobody). It carries **no**
  action/duration/config and is *not* a command.
- **Harvest (ungated)** — on receive, `applySyncCommand` recognises the `presence` marker
  and records the sender's `uniqueID` into the registry **bypassing the follow/target
  gate** (presence is informational, not a command), applying **no** timer state and
  changing nothing else. It short-circuits before the command path entirely. This is the
  one inbound path on the surface that is deliberately ungated — contrast the run-state
  command path, which always passes follow + targeting.
- **Peer registry** — a bounded (`kPeerMax` ~16) RAM set of `{uniqueID, lastSeen}`. The
  clock's **own id is excluded**; entries **age out** after the TTL (`kPeerTtlMs` ~100s,
  ~3 missed beacons), pruned on each `tickPresence()`. Pure LAN-derived state: cleared on
  boot, never persisted. `peerIds()` returns the current ids **sorted** for consumers. The
  registry itself lives in its own host-testable module, `PeerRegistry`
  ([src/PeerRegistry.h](src/PeerRegistry.h)) — `record`/`prune`/`has`/`ids`/`count`/`clear`
  over an injected `nowMs` and an injected own-id, with **no** globals; `TimerManager` owns a
  `PeerRegistry` and keeps only the beacon cadence + UDP send, exposing `peerCount`/`hasPeer`/
  `peerIds` as thin forwarders so consumers (the dynamic HA Targets select) are unchanged. See
  [ADR-0021](docs/adr/0021-peer-registry-extraction.md).
- **Dynamic HA Targets select** — the Home Assistant **Timer sync targets** select
  (writable since #110) consumes the registry: its options are built at runtime as
  `Off`, `All`, then each discovered peer id (sorted), and the entity's discovery is
  re-published (debounced, on change) as membership shifts — the settle-window decision
  is the host-tested `SyncTargetsDebounce` module
  ([src/SyncTargetsDebounce.h](src/SyncTargetsDebounce.h), ADR-0027), stepped by
  `TimerHaHost::refreshTargets`. This is **HA-presentation** logic *consuming* the peer
  registry, not propagation-surface logic. It is **single-target** by
  the platform's nature — picking a peer sets `sync_targets` to that one id; a multi-id
  CSV list (set out-of-band) cannot be shown and reflects as **unknown**, while the
  read-only `sync_targets` attribute stays authoritative for the exact value.

_Avoid_: calling presence a fourth control/propagation command (it changes no state);
keying peers by hostname (use `uniqueID`); gating the harvest behind follow/targets;
expecting the HA select to express a multi-id CSV target list (single-target only).
