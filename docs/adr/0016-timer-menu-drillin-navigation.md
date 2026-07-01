# TIMER menu drill-in navigation; commit on the list → main transition

Status: accepted

## Context

The on-device `TIMER` global menu (ADR-0003, ADR-0008) was a **flat field-walker**:
a short press on the middle button cycled between the seven settings and left/right
changed the *current* value. Every **other** on-device menu works the opposite way —
left/right moves between items and a short press selects/drills into the highlighted
thing. The `TIMER` menu was the one surface with inverted controls, so a user had to
relearn the buttons just for it (PRD #83, originating request #81).

The menu's data model already lived in a display-free, host-tested slot table
(`TIMER_MENU_SLOTS`, ADR-0008), but the *interaction model* — which button does what,
in which focus — was scattered inline across `MenuManager`'s `menutext()` /
`rightButton()` / `leftButton()` / `selectButton()` / `selectButtonLong()`, none of
which is host-built (`MenuManager.cpp` is not in the native `build_src_filter`). So the
interaction model had **zero** host-test coverage, exactly the gap ADR-0008 closed for
the label/clamp arithmetic.

## Decision

### A display-free navigation state machine — a new deep module

`TimerMenuNav` ([src/TimerMenuNav.h](../../src/TimerMenuNav.h) /
[TimerMenuNav.cpp](../../src/TimerMenuNav.cpp)) owns the whole interaction model behind
a small interface. State is `{focus ∈ List|Editing, selectedIndex, origin ∈ Menu|App}`.
Button inputs map to **outcomes** the device layer acts on:

- **List focus:** `navigate(±1)` moves the cursor and **wraps**; `select()` on a value
  row → `EnterLeaf`; `select()` on the `MAIN` row → `GoToMainMenu`; `back()` (long press)
  → context-aware exit (`GoToMainMenu` if origin = Menu, `ExitMenu` if origin = App).
- **Editing focus (value leaf):** `navigate(±1)` → `AdjustValue` (the caller mutates the
  value via the existing `timerMenuAdjust` dispatch — enum edits still publish live);
  `select()` → `ConfirmBackToList`; `back()` → `BackToList` (value already live in RAM).

The module depends on nothing but `Arduino.h`, joins the native `build_src_filter`, and
is covered by host tests `test_N1`..`test_N8`. `MenuManager` keeps only the drawing (the
list indicator / the bare leaf value) and the single commit — the same thin-device split
ADR-0008 established for the data model. It is the interaction-model sibling of the
descriptor-table family.

- **vs. leaving the interaction model inline in `MenuManager`** — rejected: it would stay
  the only untestable Timer logic, and the inverted-vs-drill-in rework is precisely the
  kind of behaviour that needs host tests to pin.
- **vs. the state machine reaching into the slot table / display** — rejected: it stays
  display-free and table-free; `MAIN`'s row index is *passed in* by the device (computed
  from the slot table's lone `Navigation` row), so the machine never hard-codes which row
  is `MAIN`.

### Slot table gains a `name` column and a `Navigation` kind; accessors split

`TimerMenuSlot` drops the value `prefix` and gains a `name` (the list label). The two
accessors split: `timerMenuName(slot)` returns the list label (the item **name**) and
`timerMenuValue(slot)` returns the **bare** leaf value (bare enum label / bare number /
`ON`|`OFF`). A fourth slot kind, `Navigation`, models the `MAIN` row (no storage, no
value); it sits **last** in the table — the back-to-main invariant the device relies on,
pinned by `test_M1`.

### Codec on-device label becomes the bare value (ADR-0010 annotated)

The per-enum codec's `menu` column becomes the **bare** value (`END`, not `BZR END`),
because the item name already gives the context (PRD user story 18). The `wire` and `ha`
columns are untouched, so **no carrier-facing string changes** (ADR-0010's B1 boundary
holds). See the addendum on ADR-0010.

### The persist commit moves to the list → main transition (ADR-0008 annotated)

The single persist + broadcast + attribute-republish commit (ADR-0008's `PersistBatch`
window + the #60 attribute refresh) fires **once**, on the **Timer-list → main-menu
transition**: a long-press out of the list, or selecting `MAIN`. It no longer fires on a
long-press *inside a leaf* — that is now just "step back up to the list", with the value
already live in RAM. Leaf edits still apply to RAM live and enum leaves still publish
live, exactly as before. See the addendum on ADR-0008.

## Consequences

- The `TIMER` menu now matches every other on-device menu: left/right navigates, a short
  press selects/drills in. The interaction model is host-testable for the first time
  (`test_N1`..`test_N8`).
- `MenuManager`'s TIMER cases became thin nav-driven dispatch; the `timerConfigIndex`
  global is gone (the state machine owns the cursor).
- `timerMenuLabel` is replaced by `timerMenuName` + `timerMenuValue`; the existing slot
  tests are re-pointed (`test_M1`/`M6`/`M8`) and the slot count is now 8 (7 value rows +
  `MAIN`).
- No carrier-facing change: the codec `wire`/`ha` columns and every MQTT/HTTP/HA surface
  are untouched; only the on-device strings and button semantics change.
- `origin = App` and its `ExitMenu` outcome are **groundwork** here (the menu is only
  entered from the main menu in this slice); the Timer-app idle long-press entry that
  produces `origin = App` is wired by issue #87. `DURATION` as the first leaf arrives with
  issue #86; `MAIN` already anchors the list end so the insert is purely a prepend.

## Relationship to prior ADRs

- **ADR-0008 (menu slot table + commit model)** — extended: the slot descriptor gains a
  `name` column and a `Navigation` kind; the accessor splits into name/value; the commit
  endpoint moves from "any long-press exit of the submenu" to the **list → main-menu
  transition** specifically.
- **ADR-0010 (per-enum codec table)** — annotated: the `menu` column is now the bare leaf
  value. `wire`/`ha`/`aliases` are unchanged.
- **ADR-0003 (on-device tuning knobs)** — its control scheme (short-press cycles fields,
  left/right changes value) is superseded by the drill-in model for the `TIMER` menu.

## Addendum — the DURATION leaf (issue #86)

`DURATION` is added as the **first** list row, folding the HH:MM:SS duration editing into the
menu so there is one on-device place to set every timer value.

- **A new `Duration` slot kind** (no settings-storage row) delegates to the existing
  display-free `TimerConfigEditor` edit engine (ADR-0011) — the wheel math is **reused, not
  reimplemented**. The state machine grows a `TimerNavLeaf ∈ {Value, DurationEditable,
  DurationReadOnly}`: a value leaf confirms back on a short press, while the duration leaf
  **cycles the H/M/S field** on a short press and **steps the field** on left/right. The
  device hands the leaf kind to the nav via `timerMenuLeafKind(slot, state)` (host-tested),
  so the gating decision lives in testable code, not in `MenuManager`.
- **Idle-only.** The duration leaf is editable only when the timer is **Idle**; while
  Running/Paused it is read-only (no underline, left/right no-op, any press returns to the
  list) — you cannot disturb a running timer's length (PRD user story 27).
- **Run-state commit, separate from the config batch.** The duration is run-state, not
  config: it commits immediately via the normal `setDuration` path on **leaf back-out**, not
  on the list → main transition. The single config `PersistBatch` commit is unchanged.
- **Timeout-free.** The menu owns its own editor instance and drives the engine's
  hold-to-repeat each frame while ignoring the engine's no-input `TimedOut` (the menu has no
  auto-apply timeout, unlike the legacy Timer-app config mode). Host tests: `test_N9`
  (Idle-only gating), `test_N10` (editable leaf inputs), `test_N11` (read-only leaf).

The Timer-app idle long-press still opens the legacy standalone wheel here; issue #87
reroutes it to open this menu, after which the `DURATION` leaf is the only path to the wheel.

## Addendum — Timer-app entry + context-aware exit (issue #87)

The context-aware exit designed above is now fully wired. `MenuManager` gains
`openTimerMenuFromApp()`, which opens the menu at the top of the list with `origin = App`;
the Timer app's **idle long-press** calls it instead of `TimerManager.enterConfigMode()`, so
the bare duration wheel has **no entry point other than the `DURATION` leaf**. A long-press
out of the list maps the nav's `ExitMenu` outcome to closing the menu (the Timer app
reappears) when `origin = App`, and `GoToMainMenu` to the main menu when `origin = Menu`;
`MAIN` always commits and goes to the main menu regardless of entry (`test_N6`/`test_N7`).
The Timer app's other long-press actions are unchanged (Finished → start, Running → reset).
`TimerManager`'s now-unreachable config-mode forwarders (`enterConfigMode` etc.) are left as
dead code for a follow-up cleanup; the editor *class* remains in use behind the leaf.

> **Superseded by [#154](https://github.com/ltloopy/awtrix3/issues/154).** That follow-up cleanup
> is done: the dead config-mode forwarders (and the `isInConfig`/`getConfigField`/`getConfigHH/MM/SS`
> getters) were removed from `TimerManager` by [#168](https://github.com/ltloopy/awtrix3/issues/168),
> after [#167](https://github.com/ltloopy/awtrix3/issues/167) removed the config-screen render path
> and [#166](https://github.com/ltloopy/awtrix3/issues/166) the config-mode button guards. The
> `TimerConfigEditor` **class** remains live behind the `DURATION` leaf, as noted above.
