# Extract the Targets republish debounce into a stateful SyncTargetsDebounce

Status: accepted

Continues the deepening family of [ADR-0021](0021-peer-registry-extraction.md)
(PeerRegistry), [ADR-0022](0022-sync-seen-cache-extraction.md) (SyncSeenCache),
[ADR-0023](0023-sync-gate-extraction.md) (SyncEnvelope) and
[ADR-0025](0025-command-plan-extraction.md) (TimerCommand), applied to the one piece of
decision logic [ADR-0026](0026-timer-ha-host-extraction.md) left inline in the
device-bound `TimerHaHost`: the dynamic Targets select's republish **debounce**. The
debounce timing, the republish payload and the retained values are all **unchanged**;
this ADR only moves the settle-window **decision** behind its own host-testable seam
(PRD #192, issues #199/#200).

## Context

`TimerHaHost::refreshTargets` debounced the Targets select's discovery republish through
three file-scope statics — the last-published options signature, a dirty flag, and the
window-start timestamp — welded to the effects (rebuild options, `resetOptions`/
`setOptions`, `publishConfigForDeviceType`, `setState`). `TimerHaHost` is device-bound by
design and ships **no** host-test env (ADR-0026), so the one non-trivial decision in the
file — a four-row settle-window state machine with revert-cancel and no-restart-on-drift
semantics, running over `millis()` wrap arithmetic — was exercisable only by flashing a
device and churning real beacons. Everything else in that file is plumbing; this was
logic, untested.

## Decision

### A stateful `SyncTargetsDebounce` owns the decision; `refreshTargets` keeps the shell

`SyncTargetsDebounce` ([src/SyncTargetsDebounce.h](../../src/SyncTargetsDebounce.h) /
[SyncTargetsDebounce.cpp](../../src/SyncTargetsDebounce.cpp)) is a small class over
injected time — no globals, no clock, no ArduinoHA:

```cpp
enum class Action { Unchanged, StartWindow, Waiting, Republish };
Action step(const char *currentOpts, unsigned long nowMs);  // advance the window, atomically
void   seed(const char *opts);                              // baseline at carrier creation
```

The transition table (in the header, pinned row-for-row by `test/test_synctargets`) is
byte-for-byte the logic it was carved from. `TimerHaHost` keeps one static instance as
the **only** debounce state: carrier creation `seed()`s it with the options it publishes
(no spurious first republish), and `refreshTargets` keeps its shell **in order** — the
carrier-nullptr/`isConnected` guards FIRST, then the options build, then `step(opts,
nowMs)`; only `Republish` reaches the effect block, unchanged. Guards-before-`step()` is
load-bearing: window state ages across MQTT-down gaps exactly as before, so a change that
settled while the broker was unreachable republishes on the first connected tick.

### Stateful with an action enum — deviating from PRD #192's proposed pure function

PRD #192 sketched this cut as a **stateless pure function** in the SyncEnvelope/
TimerCommand mold: `decide(sig, dirty, sinceMs, cur, now) -> {action, newSig, newDirty,
newSinceMs}`, with the host writing the results back. The extraction deliberately took
the **SyncSeenCache shape instead** — a stateful class whose `step()` adopts internally —
because the decision is **inherently stateful**: signature, dirty flag and window start
exist *only* to carry this decision between ticks; no second consumer reads them. The
pure-fn shape would have left **four state write-backs in the untested device code** —
exactly the copy-out/copy-in plumbing the extraction is meant to remove, and each
write-back a spot where the host and the decision could drift apart (e.g. forgetting to
adopt the signature on Republish re-introduces a republish storm). With the stateful
shape, adopt-on-Republish is atomic inside `step()` and the host owns **zero** debounce
state. The enum return keeps the module effect-free: `step()` names the action; the host
performs it — the ADR-0021/0022 cut-line (the decision leaves; the side effects stay).

### Terminology: HA presentation consuming the registry — not the propagation surface

Despite the `Sync` prefix, this module is **HA-presentation** logic: it decides when the
Home Assistant Targets *select entity* republishes its discovery options as peer-registry
membership shifts. It is **not** propagation-surface logic — nothing here touches the UDP
channel, the follow/target gate, or `_sync` envelopes. The glossary trap is "same words,
opposite surfaces": `SyncSeenCache` sits *on* the propagation surface (inbound dedupe);
`SyncTargetsDebounce` sits on the HA surface, *consuming* the registry the propagation
surface fills. Filing it with the sync receive path because of the name would be the
mistake this section exists to prevent.

## A dedicated `native_targets` test env — links nothing but ArduinoFake's String

`test_synctargets` lives in its own `[env:native_targets]`, compiling **only**
`SyncTargetsDebounce.cpp` — no Timer sources, no stubs, no `native_prelude`
force-include — the strong PeerRegistry/SyncSeenCache claim, not the weaker
ArduinoJson-linking one of SyncEnvelope/TimerCommand. That dependency-free link IS the
architectural claim of the extraction.

## Consequences

- `TimerHaHost` sheds its three debounce statics; the module instance is the only
  debounce state. Discovery payload, options string, debounce timing (3 s settle) and
  retained values are byte-for-byte unchanged.
- The decision is host-testable: revert-cancel, no-restart-on-drift, seed baseline and
  `millis()` wrap are pinned in `test/test_synctargets` without a device.
- `TimerHaHost` still ships no host-test env (ADR-0026 stands): what remains there is
  plumbing, and the host TU is excluded from every `native*` build — so the mandatory
  regression check for the rewire is the `ulanzi` device build compiling, alongside the
  unchanged-green native suites.
- The Pomodoro host (#119) inherits the seam: a second dynamic select debounces by
  owning its own instance, not by copying statics.

See also: [ADR-0022](0022-sync-seen-cache-extraction.md) (the stateful sibling whose
shape this takes), [ADR-0026](0026-timer-ha-host-extraction.md) (the device-bound host
this carves from), [ADR-0019](0019-peer-presence-registry.md) /
[ADR-0021](0021-peer-registry-extraction.md) (the peer registry this presentation layer
consumes).
