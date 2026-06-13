# TIMER menu as a slot table; unify its commit model

Status: accepted

## Context

The on-device `TIMER` global menu (ADR-0003) edits seven slots: buzzer mode, finished
mode, the three per-mode timing knobs, and the two display-element toggles. It was
implemented as **three near-identical `switch (timerConfigIndex)` blocks** in
[MenuManager.cpp](../../src/MenuManager.cpp) — one in `menutext()` (label), one in
`rightButton()` (increment), one in `leftButton()` (decrement). Each slot's three
behaviours lived in three different functions; labels and step sizes were inline literals.
Adding or reordering a slot meant editing three switches in lockstep.

This was the one Timer surface the descriptor-table discipline never reached. ADR-0007
built `TIMER_SETTINGS_DESCS` and explicitly held the menu **out of scope** ("the `TIMER`
menu keeps its own step sizes; it now reads each knob's range from the row"). Two
consequences had accumulated since:

1. Because `MenuManager.cpp` is **not** in the `[env:native]` `build_src_filter`, the
   menu's clamp/wrap/cycle arithmetic was the **only** Timer logic with zero host-test
   coverage.
2. ADR-0003 decision 4 deliberately made the two **enum** slots persist to NVS *and*
   publish on every left/right press, while the five **table-backed** slots defer to the
   long-press commit. ADR-0003 justified the split by noting that deferring the enum
   setters "would require expanding `TimerManager`'s deliberately-minimal public surface."
   An architecture review + `/grill-with-docs` session revisited this. There is **no menu
   timeout or cancel path** (`inMenu=false` happens only on a `MainMenu`/`GAME_ACTIVE`
   long-press), so every exit from the submenu already commits — which means the asymmetry
   is observable only on a **reboot mid-edit** (enum edits survive, knob edits don't) plus
   minor MQTT chatter while scrolling an enum. A small, real wart created by decision 4.

## Decision

### A descriptor table for the menu, the third of the family

[src/TimerMenu.h](../../src/TimerMenu.h) / [TimerMenu.cpp](../../src/TimerMenu.cpp) hold
`TIMER_MENU_SLOTS` — one row per slot — alongside `timerMenuLabel(slot)` and
`timerMenuAdjust(slot, dir)`. MenuManager's three switches collapse to three one-line table
walks; it keeps only the drawing (`drawMenuIndicator`) and the commit. The module is
display-free (no `DisplayManager`), joins the native `build_src_filter`, and is covered by
host tests `test_M1`..`test_M7`. It is the third member of the Timer descriptor-table family
(`TIMER_SETTINGS_DESCS`, `TIMER_HA_DESCRIPTORS`, `TIMER_MENU_SLOTS`).

Three slot kinds (`EnumCycle` / `SteppedRange` / `BoolToggle`) cover all seven slots. The
five non-enum slots are **table-backed**: a row carries only its `cmdKey`, and the dispatch
reads `storage` **and** `lo/hi` from the matching `TIMER_SETTINGS_DESCS` row via
`timerSettingByCmdKey`. The menu therefore physically reuses the settings table's pointer
and bounds — it *cannot* drift from it (`test_M5`). The two enum slots stay member-backed
(the B1 boundary, ADR-0007) and carry `getEnum`/`setEnum` hooks, exactly like the settings
table's `bespoke` fn pointers.

- **vs. UI columns on `TimerSettingDesc`** — rejected: pollutes the persistence/validation
  table with display concerns, and the enum slots aren't in that table at all.

### Unify the commit model: all slots defer NVS to the commit

Decision 4 of ADR-0003 is **superseded**. All seven slots now behave uniformly: **apply to
RAM (and, for enums, publish to MQTT) live during scroll; persist to NVS and broadcast on
the long-press commit.** The reboot-mid-edit asymmetry is gone, and the slot table no longer
*looks* uniform while *behaving* two ways.

### Mechanism: a per-call `persist` flag, not a stateful bracket

`setBuzzerMode` / `setFinishedMode` gain a `bool persist = true` parameter, mirroring the
existing `setIcon*(name, bool publish = true)` convention. With `persist=false` the value
still applies and publishes, but the NVS write is skipped. The `EnumCycle` `setEnum` hooks
pass `persist=false`; the menu commit calls a new public `TimerManager::persistConfig()`
(thin wrapper over the private `persist()`) before `saveSettings()` + `broadcastConfig()`.

This is the minimal expansion of `TimerManager`'s public surface that ADR-0003 decision 4
was avoiding — one default parameter and one wrapper — and it is preferable to the
alternatives:

- **vs. a stateful `_suspendPersist` bracket spanning the menu session** — rejected:
  `_suspendPersist` is also driven by `parseCommand`, which resets it and flushes `_dirty`
  at the end of every call. An inbound MQTT/HTTP/sync command mid-edit would silently tear
  the bracket down. A per-call flag is stateless and immune to that.
- **vs. preserve-as-is** — rejected: the asymmetry is a genuine (if narrow) wart, and a
  uniform-looking table that behaves two ways invites a future maintainer to "fix" it
  wrongly.
- **vs. persist-all-live** (make knob/bool slots also persist+broadcast each press) —
  rejected: NVS flash wear during a hold-to-repeat scroll, plus MQTT/UDP chatter; and the
  toggles have no live publish path today.

## Consequences

- Adding/reordering a menu slot is now one table row, not three switch edits. The
  label/clamp/wrap/cycle logic is host-testable for the first time (`test_M1`..`test_M7`).
- The menu can no longer drift from `TIMER_SETTINGS_DESCS`: table-backed slots reuse the
  settings row's storage pointer and range; a slot whose `cmdKey` doesn't resolve fails
  `test_M1`.
- `setBuzzerMode`/`setFinishedMode` signatures gain a trailing defaulted `persist` arg; all
  existing callers (`parseCommand`, MQTT `onSelectCommand`, HA) are source-compatible and
  unchanged. `TimerManager::persistConfig()` is new public API.
- ADR-0003's "mode changes apply immediately on each press" (decision 4) no longer holds on
  the menu: enum edits now reach NVS only on the long-press commit, like every other slot.
  Enum edits still **publish** live, so HA reflects them while scrolling. A reboot before
  the commit now discards enum edits too (uniform with the knobs).
- No NVS format change; no new HA entities; the per-slot step sizes from ADR-0003 decision 5
  are preserved (now data in the table).

**Addendum — the commit endpoint is now a `PersistBatch` window (PRD #29, #45).**
`TimerManager::persistConfig()` is retired. The long-press commit opens the (now public)
`TimerManager_::PersistBatch` RAII guard — the same commit seam `parseCommand`'s apply block
uses — whose scope exit flushes the deferred enum edits (`"timer"` namespace, iff a
`persist=false` edit marked the member state dirty during the scroll session) and then the
table-backed half (`"awtrix"` namespace via `markTableDirty()` → `saveSettings()`), each at
most once. The per-call `persist=false` flag and the deferred-to-commit behaviour above are
unchanged; only the commit endpoint moved. The "stateful bracket spanning the menu session"
rejected above stays rejected: the guard's window is the commit itself, not the session —
scroll edits still ride the stateless per-call flag, with the dirty bit recording that a
flush is owed.
