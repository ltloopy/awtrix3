# Timer settings as a descriptor table; member-backed fields stay bespoke

Status: accepted

## Context

The Timer's value-config keys (the per-mode timing knobs, behavior parameters,
display-element toggles, bar color, melodies, and the two sync-role keys) were ~16 free
`TIMER_*` globals. Adding or changing one meant editing **seven** sites that each
enumerated the key by hand:

1. `loadDevSettings` (dev.json parse) — [src/Globals.cpp](../../src/Globals.cpp)
2. `loadSettings` (NVS read) — Globals.cpp
3. `saveSettings` (NVS write) — Globals.cpp
4. `parseCommand` validation pass — [src/TimerManager.cpp](../../src/TimerManager.cpp)
5. `parseCommand` apply pass — TimerManager.cpp
6. `buildConfigSnapshot` (propagation) — TimerManager.cpp
7. the on-device `TIMER` menu — [src/MenuManager.cpp](../../src/MenuManager.cpp)

The numeric ranges (`1..300` hold, `5..300` realert, `≤30` countdown, `1..604800` max)
were duplicated independently across sites 1, 4 and 7 and could silently drift, and
`parseCommand`'s validate-pass / apply-pass were a manual two-list duality (a key
validated but not applied, or vice-versa, still compiled).

The repo already had the cure in [TimerHa.h](../../src/TimerHa.h)
(`TIMER_HA_DESCRIPTORS`, "one table, no drift"). An architecture review + a
`/grill-with-docs` session walked the design tree; this ADR records the load-bearing
boundary choice. Terminology lives in [CONTEXT.md](../../CONTEXT.md).

## Decision

### A single descriptor table over the existing globals

`TIMER_SETTINGS_DESCS` ([src/TimerSettings.h](../../src/TimerSettings.h)) holds one row
per value-config key: command / dev.json / NVS key names, type, validator (declarative
`UIntRange` / `Bool` / `Name`, or a bespoke fn pointer for `bar_color` and
`sync_targets`), an `inSnapshot` flag, and a typed `storage` pointer at the existing
global. Validation, apply, NVS save/load, dev.json and the propagated config snapshot all
loop the one table. The rows **describe** the globals rather than owning them, so the
`TIMER_*` symbols that `MenuManager` / `Apps` / the host tests read are undisturbed
(smallest blast radius). `parseCommand`'s ~150 lines of per-field blocks collapse to two
table walks over a staging array, and `timerSettingParse` is pure (validate + coerce, no
write) so atomic-reject (ADR-0001) is preserved.

### The table is a superset, not "the config block"

The `inSnapshot` column **is** the boundary CONTEXT.md draws in prose:

- **config block** = `inSnapshot == true` rows (the propagated snapshot).
- **sync roles / local identity** = `inSnapshot == false` rows (`sync_follow` /
  `sync_targets`) — excluded from the snapshot exactly as ADR-0006 requires.

`inSnapshot` is a single flag doing double duty: snapshot membership *is* the
broadcast-trigger, because `broadcastConfig()` sends the whole snapshot as one unit
(ADR-0006) — there is no per-key broadcast granularity.

### B1 boundary: `duration` / `buzzer` / `finished` / the 4 icons stay bespoke

These keys are config block too, but they are **not** in the table. They keep their
publish-aware `TimerManager` setters (equality-skip, `_suspendPersist` batching, MQTT
publish, and — for `duration` — the `max_duration`-gated validation and clamp). Folding
them into the table would require per-row apply/publish callbacks, re-introducing
machinery the table exists to remove and risking the already-deep setters.

- **vs. folding everything in (B2)** — rejected: more table machinery, and the member
  setters carry real behavior (publish + skip + batched persist) that does not fit a
  declarative row.

So the config block now has **two halves**: the table's `inSnapshot` rows and the
member-backed fields. To keep them from drifting across the snapshot and the broadcast
trigger, the member-backed half is governed by a single shared membership list
(`kMemberConfigKeys` in TimerManager.cpp), used by both `addMemberConfigToSnapshot` and
the `parseCommand` broadcast trigger.

### Validators shared across surfaces; atomicity is per-surface

`parseCommand` (control surface) stays **atomic-reject** (one bad field rejects all).
`loadDevSettings` (dev.json) reuses the same validators but stays **per-key best-effort**
(validate each key, apply iff valid, skip-and-continue) — dev.json is a boot override
layer, and one stale key must not discard the rest. Net effect on dev.json: the ranges
are now defined once, and the previously-unvalidated keys (`bar_color`, melodies,
`sync_*`) gain the same ignore-if-invalid treatment the numerics already had.

## Out of scope

- **No on-device menu / HA entity changes.** The `TIMER` menu keeps its own step sizes;
  it now reads each knob's range from the row instead of an inline literal.
- **No NVS format change.** Keys, types and defaults are unchanged; only their
  enumeration moved into the table.

## Consequences

- Adding a new **plain value-config key** is now a single table row (validation, apply,
  NVS, dev.json and snapshot follow automatically). A new **publish-aware** key still
  needs its setter plus an entry in `kMemberConfigKeys`.
- The numeric ranges and the validate/apply duality are eliminated; CONTEXT.md's
  config-block / sync-roles split is codified as the `inSnapshot` column.
- The Timer NVS round-trip became host-testable for the first time (it had lived in the
  uncompiled `Globals.cpp`). Host tests `test_T1`..`test_T6` cover table validation
  boundaries, strict types, the bespoke validators, the NVS round-trip, dev.json
  best-effort, and snapshot membership.
- `src/TimerSettings.cpp` joins the `[env:native]` `build_src_filter`.
