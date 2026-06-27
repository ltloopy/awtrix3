# The one-shot override store stays in TimerManager (no ConfigSnapshot extraction)

Status: accepted

This ADR records a **rejected** extraction. It is the fourth and final cut considered in
the #131 deepening trajectory, after [ADR-0021](0021-peer-registry-extraction.md)
(PeerRegistry), [ADR-0022](0022-sync-seen-cache-extraction.md) (SyncSeenCache) and
[ADR-0023](0023-sync-gate-extraction.md) (SyncEnvelope). Unlike those three, the candidate
here — pulling the one-shot override snapshot (`captureSnapshot`/`restoreSnapshot` +
`SavedConfigScope` + the `_snap*` fields, [ADR-0017](0017-one-shot-timer-commands.md)) out of
`TimerManager` into a `ConfigSnapshot` store — was **grilled and declined**. It is documented
so it is not re-suggested by a future architecture pass.

## Context

The #131 review flagged the one-shot override as a fifth separable responsibility, by analogy
to the three sibling cuts. The siblings were **deep** for one shared reason: each lifted a
self-contained piece of state with an *injected* dependency surface (`nowMs`, `ownId`,
`{ownId, follow}`), so each landed its own `native_*` test env that links **no Timer code** —
the suite linking against nothing *is* the architectural claim.

The override store has the opposite shape. Its data is the singleton's own live config, which
exists as **two deliberately distinct families** (the B1 boundary, ADR-0007/0009):

- **Family A — the table half.** The timing knobs, colours, display toggles, melody *names*
  and sync settings live as **globals**, snapshotted generically by walking the `inSnapshot`
  rows of `TIMER_SETTINGS_DESCS` through each row's `storage` pointer
  (`timerSettingsCaptureSnapshot`/`timerSettingsRestoreSnapshot` in `TimerSettings.cpp`). This
  half is **already deep**: descriptor-driven, no per-key code, already out of `TimerManager`,
  already host-testable on its own (`test_T6`/`test_T8`).
- **Family B — the member half.** `buzzerMode`, `finishedMode`, the four `icon<State>`
  strings, `durationSec` (run-state) and the resolved RTTTL RAM (`endRtttl`/`tickRtttl`) are
  `TimerManager`'s own private members, copied field-by-field into `_snap*` siblings.

So the override store **is** the singleton's live member layout. A `ConfigSnapshot` extracted
as-is would need to reach into ~9 private members to capture and ~9 to restore — an interface
as wide as its body (a shallow cut). The grilling's task was to find a value-struct seam that
makes it deep, or to decline.

## Decision

**Keep the one-shot override store in `TimerManager`.** No `ConfigSnapshot` module is created.
The grilling found no seam that makes the extraction deep, and the extraction's stated payoff
(host-testability without the full fixture) is unreachable. Three alternatives were considered
and each is rejected for a specific reason:

### Rejected: extract `ConfigSnapshot` as-is

A store that captures/restores by reaching into Family B's members has an interface nearly as
complex as its body — the textbook shallow module. Worse, it would **not** be host-testable in
isolation in any meaningful sense: the override's *behaviour* is its interaction with
persistence-suppression, carrier honesty, the run-state revert and the one-shot sync-receive
path — all of which require the singleton. A `ConfigSnapshot`-in-isolation unit could only
exercise field-copy plumbing (`buzzerMode → _snapBuzzer`), which carries no logic and no bug
to catch. So the cut delivers none of the testability payoff that justified
PeerRegistry/SyncSeenCache/SyncEnvelope, whose isolated suites caught real edge logic. The
override's behaviour is already covered where it lives — `test_OS1..OS9`, `test_HC1..HC3`,
`test_IM2`, `test_S7`, `test_SR3`/`SR4` — at the integration level its meaning demands.

### Rejected: unify config into a `TimerConfig` value struct first (the only "deep" path)

The sole way to make the store deep is to first collapse Family B's scattered members into one
`TimerConfig` value struct that `TimerManager` holds as a single member; then snapshot becomes
a struct copy and the three hand-rolled copies (capture, restore, and `SavedConfigScope`'s
effective-stash) become value semantics. This is rejected on cost-vs-benefit:

- It is a **~100-site refactor of the singleton's primary storage** (every internal
  `buzzerMode`/`durationSec`/icon access, plus `TimerConfigEditor`), far larger and riskier
  than the three sibling cuts, which touched only peripheral state.
- It deepens **only half the snapshot.** Family A is global-backed by ADR-0007 design and
  would *not* join the struct — pulling it in would fight the descriptor-table model that gives
  every config key one declarative row. So even after the refactor the snapshot stays
  two-family; the only win is replacing Family B's three copies with value semantics.
- The genuinely intricate logic — the override lifecycle (`_overrideActive`, the first-
  `save:false` capture gate, the mid-run rebaseline) and the honest-carrier RAII swap — stays
  welded to `parseCommand` and run-state regardless, because *that* is where the behaviour is,
  not in the data copy. The struct moves the copy, not the complexity.

A large, risky, half-covering refactor that fights an existing ADR, on a responsibility with
no cited bug or test pain, is not justified.

### Rejected: a snapshot DTO as a middle ground

A tempting half-measure is to leave config as scattered members but introduce a snapshot
**DTO** (`struct { BuzzerMode buzzer; FinishedMode finished; String icon[4]; uint32_t
duration; String end, tick; }`) so capture, restore and the scope-stash share one field list.
This is a mirage: without also making the *live* config a struct (the ~100-site refactor
above), each copy site still assigns field-by-field (`_snap.buzzer = buzzerMode`), so the DTO
merely **relocates** the three 9-field lists rather than collapsing them to `_snap = _live`.
It adds a type without removing the drift risk it appears to fix, so it is not worth the churn.

### Accepted resting state

The current structure is the accepted endpoint: **Family A snapshotted generically through the
settings table** (already deep, already extracted into `TimerSettings.cpp`) and **Family B
copied member-by-member** inside `TimerManager`. The triple member-copy (capture / restore /
`SavedConfigScope` stash) is an acknowledged minor cost, retained because the only thing that
would remove it — a unified live-config struct — is the rejected ~100-site refactor.

## Consequences

- No new source files, no new test env, no API/NVS/wire change. The override store, the
  `SavedConfigScope` swap and the `returnToIdle` seam stay private to `TimerManager`.
- The #131 deepening trajectory closes here: three cuts taken (ADR-0021/0022/0023), one
  declined (this ADR). A future architecture pass that re-surfaces "extract the override
  snapshot" should read this ADR first.
- If a `TimerConfig` value struct is ever introduced for an **independent** reason (e.g. a
  broader config-as-value refactor with its own payoff), the Family-B half of the snapshot
  would then collapse to value semantics as a free side effect — but that refactor must carry
  its own justification; it is not motivated by the override store alone.

See also: [ADR-0017](0017-one-shot-timer-commands.md) (the override model this store serves),
[ADR-0007](0007-timer-settings-descriptor-table.md) / [ADR-0009](0009-member-config-hook-table.md)
(the A/B family boundary that makes the store two-family),
[ADR-0021](0021-peer-registry-extraction.md) / [ADR-0022](0022-sync-seen-cache-extraction.md) /
[ADR-0023](0023-sync-gate-extraction.md) (the three sibling cuts whose deep shape this
candidate lacks).
