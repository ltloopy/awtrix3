# A per-enum codec table is the fifth descriptor-table family member

Status: accepted

## Context

The Timer's two label-bearing enums — `BuzzerMode` and `FinishedMode` — each had **four
independent spellings** of every value, scattered across four modules with no compiler help
keeping them aligned:

1. **Canonical wire string** (MQTT/HTTP/sync) — `buzzerModeString`/`finishedModeString` and
   `parseBuzzerMode`/`parseFinishedMode`, hand-written `switch`/`if` ladders in
   `src/TimerManager.cpp`.
2. **Menu label** — `kBuzzerLabels`/`kFinishedLabels` in `src/TimerMenu.cpp`, an array indexed
   by the enum value.
3. **HA select-option label** — `HAtimerBuzOptions`/`HAtimerFinOptions` in `src/TimerHa.cpp`, a
   semicolon-joined string whose token order has to match the enum.
4. **Input aliases** — `autoclear`/`realert` buried inside `parseFinishedMode`.

Each site carried a comment ("Order must match BuzzerMode { Off, End, Countdown }") — prose
load-bearing where a `static_assert` should be. ADR-0009 explicitly **deferred** this unification
(its "candidate B"): folding the menu-label / HA-options / canonical-string triplication behind one
per-enum table, noting it "touches three more modules; better landed on its own."

This is that landing — but scoped to the **foundation + the wire-string consumer only**. The
menu-label and HA-option columns are populated now; rewiring `TimerMenu`/`TimerHa` to read them is
left to two follow-up issues so each module moves under its own regression tests.

## Decision

### A per-enum codec table, indexed by the enum value

`src/TimerEnums.{h,cpp}` holds one row per enum value, in a new leaf module:

```c
struct TimerEnumCodec {
    const char *wire;     // canonical MQTT/HTTP/sync string
    const char *menu;     // on-device TIMER-menu label
    const char *ha;       // Home Assistant select-option label
    const char *aliases;  // extra accepted INPUT spelling(s), nullptr if none
};
```

`TIMER_BUZZER_CODEC[]` and `TIMER_FINISHED_CODEC[]` are **indexed by the enum's numeric value** —
the enum value *is* the row index. This is the convention `kBuzzerLabels`/`kFinishedLabels` already
used by accident; it is now the authoritative, single representation. Aliases are an **explicit
per-row column** (mostly `nullptr`; only the `FinishedMode` rows carry `autoclear`/`realert`),
rather than special-cased inside the parser.

### `COUNT` sentinels + `static_assert` make drift a build error

Both enums gain a trailing `COUNT` sentinel
(`BuzzerMode { Off, End, Countdown, COUNT }`, `FinishedMode { AutoClear, Hold, ReAlert, COUNT }`).
A `static_assert` in `TimerEnums.cpp` pins each table's row count to its enum's `COUNT`, so adding
an enum value without a row (or vice versa) **fails the build** — the prose "order must match"
comments become a compiler invariant.

### The wire-string consumers are rewired now

`parseBuzzerMode`/`parseFinishedMode` (string→enum) become a case-insensitive table scan that
matches the canonical wire spelling **or any alias** — no accepted input dropped. `buzzerModeString`
/`finishedModeString` (enum→string) become a single row read (`buzzerCodec(m).wire`). The
`GET /api/timer` snapshot and the propagated sync config snapshot emit byte-identical
`buzzer`/`finished` strings as before, because the table's `wire` column carries exactly the old
canonical spellings.

### Placement: a leaf module, no dependency cycle

The enums **move** from `TimerManager.h` into `TimerEnums.h`; `TimerManager.h` `#include`s it.
`TimerEnums.h` depends only on `Arduino.h`, so `TimerManager`, `TimerMenu` and `TimerHa` can all
reuse it without a cycle (the include arrow only ever points *into* `TimerEnums.h`). `TimerState`
and `TimerCmdResult` stay in `TimerManager.h` — they carry no labels, so they are not codec'd.

This is the **fifth member** of the Timer descriptor-table family: `TIMER_SETTINGS_DESCS`,
`TIMER_MEMBER_CONFIG_DESCS` (ADR-0009), `TIMER_HA_DESCRIPTORS`, `TIMER_MENU_SLOTS` (ADR-0008), and
now the per-enum codec table.

### The B1 boundary stays intact

This ADR consolidates only how labels/strings are **encoded**, not how values are
**applied/persisted/propagated**. `buzzer`/`finished` remain member-backed config keys on
`TimerManager`'s publish-aware setters (ADR-0007/0009); the MQTT raw-`uint8_t`-index publish path is
**unchanged** — the published index is the enum value, which is exactly this table's row index.

## Consequences

- Every spelling of an enum value now lives in **one row**; the menu/ha columns are staged for the
  two follow-up rewires (`TimerMenu`, `TimerHa`).
- A missing/extra row is a compile error (`static_assert` on `COUNT`), replacing four "order must
  match" comments.
- New host tests `test_T9` (both tables well-formed: row count == `COUNT`, non-empty
  wire/menu/ha per row) and `test_T10` (`parse(toString(v)) == v` for every value, every alias
  parses, arbitrary case parses, junk rejected). Existing `test_U35`/`test_U49`/`test_T8` continue
  to guard the HA option count, canonical spellings, and snapshot round-trip.
- `build_src_filter` for `[env:native]` gains `+<TimerEnums.cpp>`; firmware envs pick it up via the
  normal `src/` glob. No NVS format change, no new HA entities, no behaviour moved.

## Addendum — the `menu` column is now the bare leaf value (PRD #83, ADR-0016)

With the `TIMER` menu's drill-in rework (ADR-0016), the on-device `menu` column becomes
the **bare** value (`OFF`/`END`/`CDN`, `CLEAR`/`HOLD`/`RE-ALERT`) rather than the prefixed
label (`BZR END`, `FIN CLEAR`). In the drill-in list the item **name** (`BUZZER`,
`FINISH`) is shown while walking the list and the bare value only while editing the leaf,
so the prefix is redundant. The `wire`, `ha`, and `aliases` columns are **unchanged** — no
MQTT/HTTP/sync/HA string changes — so the B1 boundary holds. `test_T9` still asserts a
non-empty `menu` per row; `test_M8` pins the bare values through `timerMenuValue`.
