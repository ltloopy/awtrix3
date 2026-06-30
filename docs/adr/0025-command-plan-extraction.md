# Extract the command plan into a host-testable TimerCommand (pure classify)

Status: accepted

Builds on [ADR-0001](0001-timer-command-validation-parity.md) (the atomic-reject
validation contract this lifts) and continues the #131 deepening trajectory after
[ADR-0021](0021-peer-registry-extraction.md) (PeerRegistry),
[ADR-0022](0022-sync-seen-cache-extraction.md) (SyncSeenCache) and
[ADR-0023](0023-sync-gate-extraction.md) (SyncEnvelope). The validation **policy**, the
wire-feedback mapping, the override store and the run-state side effects are all
**unchanged**; this ADR only moves the validation **decision** and the staging it produces
behind their own seam. It is the **fourth cut taken** — and it deliberately **reopens** the
trajectory that [ADR-0024](0024-one-shot-override-store-stays-in-timermanager.md) closed.

## Context

`TimerManager::parseCommand` was a 244-line method that interleaved the **decision** (is every
field valid?) with the **act** (mutate globals, members, run-state, persistence, HA republish,
sync broadcast). The atomic-reject contract (ADR-0001) demands the whole payload be validated
**before** any mutation, so the validation pass already existed as a logically separable
mutate-nothing phase — but it sat welded to the apply, exercisable only through the
multi-thousand-line `test_timer` fixture that must stand up the singleton, `MQTTManager`,
`Preferences` and the stubs before any assertion can run.

This is the same shape ADR-0023 found in `applySyncCommand`: a **pure decision** buried in an
impure shell. The thing being lifted is not a stateful set (PeerRegistry/SyncSeenCache) but a
pure function over value structs — so, like SyncEnvelope, the module is **stateless** (free
functions in a namespace), the **fourth sibling** of that family.

### Why this reopens ADR-0024 without reversing it

ADR-0024 declined a *fourth cut* — extracting the one-shot override **store** into a
`ConfigSnapshot` — and closed the deepening trajectory there. That decision stands, unedited.
This ADR is **not** a reversal of it, because it lifts a **different responsibility** with the
**opposite module shape**:

- **ADR-0024's declined cut was shallow.** The override store *is* the singleton's own live
  member layout (Family B, ADR-0007/0009). A `ConfigSnapshot` extracted as-is would reach into
  ~9 private members to capture and ~9 to restore — an interface as wide as its body — and its
  behaviour (persistence-suppression, the run-state revert, the one-shot sync path) is
  inseparable from the singleton, so an isolated unit could only exercise field-copy plumbing
  with no logic to catch. The grilling found no seam that makes it deep, so it stayed put.
- **This cut is deep.** The validation decision has a **narrow interface** — `classify(packet,
  Context) -> Plan` — over a **substantial body**: the entire atomic-reject contract (table
  rows, the `duration` cross-field check, the member-backed half, the `action`, the
  payload-level `save` flag, inline-melody classification). Its only non-packet inputs are the
  saved ceiling and the `_remoteApply` flag, injected as a `Context` value exactly as
  SyncEnvelope injects `{ownId, follow}`. So it links the dependency-light descriptor-table
  family alone and is host-testable against any ceiling/remote scenario as a plain value.

The trajectory was closed on a *shallow* candidate (the store) and is reopened on a *deep* one
(the decision). A future architecture pass must read the two ADRs as a pair: ADR-0024's "no"
applies to lifting the override **store**; it says nothing against lifting the validation
**decision**, which ADR-0024 was not even considering. Conflating the reopening with reversing
ADR-0024 would be the mistake this section exists to prevent.

## Decision

### A stateless `TimerCommand` owns the decision and the staging; `TimerManager` is the shell

`TimerCommand` ([src/TimerCommand.h](../../src/TimerCommand.h) /
[TimerCommand.cpp](../../src/TimerCommand.cpp)) is a namespace of free functions over value
structs:

```cpp
struct Context { uint32_t savedMaxDuration; bool remoteApply; };  // the only non-packet inputs
struct Plan    { bool ok; /* staged table/member values, durationSec, oneShot, action,
                             configInCommand, the dirty HA-attr-carrier set, inline melodies */ };

Plan classify(JsonObjectConst packet, const Context &ctx);   // the whole atomic-reject pass, once
```

`classify` runs the entire validation contract in **one mutate-nothing pass** and, on the first
invalid field, returns `{ok=false}` having staged nothing — the shell maps `!ok` to the coarse
`TimerCmdResult::BadField` (wire parity preserved; `BadJson`/`Disabled` remain shell-level
guards *before* `classify`). On success the `Plan` carries **everything apply needs** so the
shell never re-reads the packet.

`TimerManager::parseCommand` keeps the **impure shell**: fill `Context` from the globals it
owns, call `classify`, then on `ok` apply the `Plan` — one `PersistBatch`, the override
capture/rebaseline, the HA-attr republish, the run-state broadcast. The cut-line mirrors
ADR-0021/0022/0023: **the decision leaves; the side effects stay.** One place still owns "how a
validated command is committed."

### `Context` carries only the two non-packet inputs — *not* the receiver's own state

`classify` reads only the packet plus `{savedMaxDuration, remoteApply}`. This is the SyncEnvelope
discipline (ADR-0023): the context is the minimal injected surface, and nothing of the
receiver's broader state is admitted. Putting any other singleton state into `Context` invites
the same failure ADR-0023 designed against — an unused field a future maintainer "wires up"
wrong. The saved ceiling is injected (not read as a global) precisely so a command that **also**
raises `max_duration` can re-base the ceiling for its own `duration` check within the one atomic
pass (the ADR-0001 addendum), host-testable by constructing any ceiling as a plain value.

### Only the validation **decision** leaves — the override **store** stays (ADR-0024)

`classify` computes the one-shot **decision** (`oneShot = !save || inline melody || remoteApply`)
and stages it on the `Plan`. The one-shot override **store** — `captureSnapshot`/`restoreSnapshot`,
`SavedConfigScope`, the `_snap*` members — stays in `TimerManager` per ADR-0024. The decision is
deep and leaves; the store is shallow and stays. The `Plan` is the boundary between them.

## The apply ordering invariant (ADR-0001 addendum, now a property of the apply shell)

The `Plan` is **order-free data** — it stages values without prescribing apply order. Exactly
**one** ordering constraint survives the extraction, and it lives in the **apply shell**, not in
`classify`:

> The shell **must** write the table rows (landing a raised `max_duration`) **before** calling
> `setDuration`.

`setDuration` re-clamps against the **global** `TIMER_MAX_DURATION`, not the staged ceiling. But
`classify` validated the command's `duration` against the **staged** ceiling (the in-payload
`max_duration`, per the ADR-0001 addendum). So if the shell called `setDuration` before writing
the table row that lands the raised ceiling, a duration `classify` legitimately accepted against
the new ceiling would be silently clamped to the old one — re-introducing the exact silent-clamp
ADR-0001 abolished. Writing the table rows first closes that window. This invariant is recorded
here as a property of the apply shell because the `Plan` itself cannot encode it: order-free
data plus one ordering rule in the one place that applies it.

## A dedicated `native_validate` test env — links the table family, not nothing

`test_validate` lives in its own `[env:native_validate]`, which compiles **only**
`TimerCommand.cpp` + `TimerSettings.cpp` + the enum codecs (`TimerEnums.cpp`) — no
`TimerManager` singleton, no `MQTTManager`/`Preferences`, no stubs, no `native_prelude`
force-include. The impure `TimerSettingsApply.cpp` is deliberately **not** linked: that it isn't
needed is the architectural claim of the carve — `classify` reads only the packet plus a
`Context`, so it stages without applying.

One honest caveat, **weaker** than the three siblings' claim, distinguishes this env — and it is
analogous to ADR-0023's "`SyncEnvelope` must link ArduinoJson" caveat:

> `native_validate` links the **descriptor-table family** (`TimerSettings.cpp` + the codecs +
> the relocated `parseHMS`/`isValidAction`) and ArduinoJson — so its claim is **"links no Timer
> singleton,"** deliberately weaker than PeerRegistry/SyncSeenCache/SyncEnvelope's "links
> nothing but ArduinoFake's `String`."

The dependency is **irreducible**: the validator *is* the descriptor tables — the contract it
enforces. A `classify` that did not link `TIMER_SETTINGS_DESCS` and the member validators would
have nothing to validate against. So unlike SyncEnvelope (whose ArduinoJson tie is the *packet*
format), here the tie is the *contract* itself. The suite linking against no **singleton** is
still the architectural claim: the validation decision needs the tables and the packet, and
nothing of `TimerManager`. (Like SyncEnvelope, it also links ArduinoJson — `classify` reads the
packet.)

## Consequences

- `TimerManager::parseCommand` sheds the inline validation pass; it becomes fill-`Context` →
  `classify` → apply-`Plan`. Public API, the wire feedback (`200`/`400`/`409`), the NVS format,
  the HA entities and the sync wire format are all unchanged.
- New source files `TimerCommand.{h,cpp}`. `TimerCommand.cpp` is added to `[env:native]`'s
  `build_src_filter` (so `test_timer` resolves it) and is a source of `[env:native_validate]`;
  the firmware envs glob `src/` and pick it up automatically.
- New host suite `test/test_validate` (`test_V1..V24`, plus the atomic-reject cases migrated out
  of `test_timer` in #144, e.g. `test_U29/U37/U40/U41/U43`) drives `classify` directly: every
  reject path (`BadField` on each invalid table/member field, the `duration` cross-field check
  against both absolute and staged ceilings, the payload-level `save`), the `oneShot` decision
  (`!save` / inline melody / `remoteApply`), inline-melody staging, and the populated-`Plan`
  success shape. No singleton, no globals, no stubs. The `test_timer` apply-level cases stay
  green unchanged, proving the extraction is behaviour-preserving.
- CI (`.github/workflows/test.yml`) runs
  `pio test -e native -e native_peer -e native_seen -e native_sync -e native_validate`.
- The #131 deepening trajectory: three cuts taken (ADR-0021/0022/0023), one declined
  (ADR-0024, the override store), and this fourth cut taken on the validation decision —
  reopening ADR-0024's closed trajectory **on a different, deep responsibility**, not reversing
  it. ADR-0024 is left intact.

See also: [ADR-0001](0001-timer-command-validation-parity.md) (the validation contract
`classify` enforces, and the ordering addendum), [ADR-0023](0023-sync-gate-extraction.md) (the
pure-classify sibling whose cut-line and isolation caveat this mirrors),
[ADR-0024](0024-one-shot-override-store-stays-in-timermanager.md) (the declined shallow cut this
reopening must not be conflated with), [ADR-0007](0007-timer-settings-descriptor-table.md) /
[ADR-0009](0009-member-config-hook-table.md) (the descriptor-table family `native_validate`
links).
