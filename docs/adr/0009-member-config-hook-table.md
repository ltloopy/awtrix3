# Member-backed config gets a hook table; the config block becomes two tables

Status: accepted

## Context

ADR-0007 drew the **B1 boundary**: the config-block keys whose values carry real behaviour —
`buzzer`, `finished`, and the four `icon_<state>` keys — stay on `TimerManager`'s publish-aware
setters (equality-skip, `_suspendPersist` batching, MQTT publish) instead of folding into the
declarative `TIMER_SETTINGS_DESCS`. That boundary is correct and stands.

But the *six key names* ended up enumerated across **four sites** in `TimerManager.cpp` that had
to agree, with no compiler help if one drifted:

1. `kMemberConfigKeys[]` — a membership list, added solely to stop the snapshot and the
   `parseCommand` broadcast trigger from drifting apart.
2. `parseCommand` validation — `buzzer`/`finished`/4×`icon_*` each hand-validated.
3. `parseCommand` apply — each setter called by hand inside the `_suspendPersist` bracket.
4. `addMemberConfigToSnapshot` — an `if/else strcmp` chain mapping each key to its live value.

The existence of `kMemberConfigKeys` was the tell: it was a *partial* fix that unified membership
only, while validation, apply, and snapshot-emit stayed scattered. Adding a seventh member-config
key was a 4-5 site edit. An architecture review (`/improve-codebase-architecture`) flagged this as
the branch's strongest remaining shallow seam — the **deletion test** says removing the
validate/apply/snapshot blocks doesn't make the complexity vanish, it re-concentrates in every
surface that round-trips these six keys.

## Decision

### A hook table for the member-backed half — the config block's second table

`TIMER_MEMBER_CONFIG_DESCS` ([src/TimerSettings.h](../../src/TimerSettings.h) /
[TimerSettings.cpp](../../src/TimerSettings.cpp)) holds one row per member-backed config key:

```c
struct TimerMemberConfigDesc {
    const char *cmdKey;
    bool (*validate)(JsonVariantConst, TcValue &out);   // pure: validate + coerce, no mutation
    void (*apply)(const TcValue &);                      // routes via TimerManager's deep setter
    void (*emit)(JsonDocument &doc);                     // writes the live value into the snapshot
};
```

Unlike `TIMER_SETTINGS_DESCS`' **declarative** rows, these carry **function-pointer hooks** —
exactly like `TIMER_MENU_SLOTS`' enum slots (ADR-0008) — because their values are setter-owned, not
reachable through a typed storage pointer. The hooks reuse `TimerManager`'s existing public
statics/getters/setters (`parseBuzzerMode`/`setBuzzerMode`/`buzzerModeString`, `isValidIconName`/
`setIcon*`/`getIcon*`); the deep setters are **untouched**. Staging uses the same `TcValue` and the
same `validate(json, TcValue&)` / `apply(const TcValue&)` shape as `timerSettingParse` /
`timerSettingStore`, so ADR-0001 atomic-reject (validate-all, then apply) is preserved.

`parseCommand`'s member validate/apply blocks, `addMemberConfigToSnapshot`, and
`docTouchesMemberConfig` all collapse to a single loop over the table; `kMemberConfigKeys` is
deleted. The config block is now literally **two tables**: `TIMER_SETTINGS_DESCS`' `inSnapshot` rows
plus `TIMER_MEMBER_CONFIG_DESCS`, each feeding both `buildConfigSnapshot` and the broadcast trigger.
It is the fourth member of the Timer descriptor-table family (`TIMER_SETTINGS_DESCS`,
`TIMER_MEMBER_CONFIG_DESCS`, `TIMER_HA_DESCRIPTORS`, `TIMER_MENU_SLOTS`).

### This is not the rejected "B2"

ADR-0007 rejected **B2** — folding the publish-aware keys into `TIMER_SETTINGS_DESCS` — because
their behaviour does not fit a *declarative* row. This decision does **not** reopen B2: the keys do
**not** join the declarative table. They get a **separate hook table** whose rows are deliberately
function-pointer-based. The B1 boundary is unchanged; only the *enumeration* of the B1 keys moved
from four hand-kept sites into one row per key.

### `duration` stays a hand-coded one-off, not a row

`duration` is member-backed too but is **excluded** from the table: it is **run-state, not config**
(CONTEXT.md — it rides with run-state broadcasts, never the config snapshot), and its validation
depends on the cross-field `effectiveMaxDuration` staged from the table half (ADR-0001 addendum).
Folding it in would drag run-state semantics and a cross-field dependency into a config table.

### Placement: co-located in TimerSettings, not inside TimerManager

The table lives in `TimerSettings.{h,cpp}` — "the config block is two tables in one file" — rather
than as a file-static inside the 962-line `TimerManager.cpp`.

- **vs. a file-static in TimerManager.cpp** — rejected: the other family tables each live in their
  own findable file; burying the second config-block table in the giant works against the
  navigability the family exists to create. `TimerSettings.cpp` already `#include`s `TimerManager.h`.
- **vs. composing in the enum-codec unification** (a separate review candidate, "B") — deferred: it
  would also fold the menu-label / HA-options / canonical-string triplication behind one per-enum
  table, but that touches three more modules; better landed on its own.

Cost: `buzzerModeString()` / `finishedModeString()` move from private to public so the emit hooks
can read the canonical spellings — siblings of the already-public `getStateString()`.

## Consequences

- Adding/removing a member-backed config key is now **one table row**, not a 4-5 site edit; the
  snapshot/broadcast-trigger drift class is structurally impossible for the B1 half (it was the
  whole reason `kMemberConfigKeys` existed).
- `parseCommand` keeps the `_suspendPersist` bracket and `setDuration` call; only the six
  hand-written member setters become a table loop inside that bracket. Behaviour is preserved —
  proven by the unchanged `test_U21..U31` (atomic-reject), `S2`/`S3` (config vs run-state broadcast)
  regression tests staying green.
- New host tests `test_T7`/`test_T8`: the member table is well-formed (exactly the six B1 keys,
  every hook present, **disjoint** from `TIMER_SETTINGS_DESCS`), validates accept/reject per row, and
  round-trips through `timerMemberConfigBuildSnapshot`.
- Public surface grows by two accessors (`buzzerModeString`/`finishedModeString`); no NVS format
  change, no new HA entities, no `build_src_filter` change (`TimerSettings.cpp` was already in it).
- Supersedes the `kMemberConfigKeys` mechanism of [ADR-0007](0007-timer-settings-descriptor-table.md);
  the B1 boundary that ADR drew is otherwise unchanged.
