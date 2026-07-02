# Extract the Timer HA carrier lifecycle into a device-bound TimerHaHost

Status: accepted

Continues the extraction trajectory of [ADR-0021](0021-peer-registry-extraction.md),
[ADR-0022](0022-sync-seen-cache-extraction.md), [ADR-0023](0023-sync-gate-extraction.md)
and [ADR-0025](0025-command-plan-extraction.md), but with a deliberate divergence
(below): this cut is **device-bound**, so it ships no isolated host-test env. No
discovery payload, wire topic, retained value, attribute group, callback outcome, or
snap-back echo changes — this is a strictly behavior-preserving relocation.

## Context

`MQTTManager` is the device's **general** MQTT module (stats, buttons, indicators, app
switching, the generic publish paths). Every *other* Timer concern already had a
`Timer*` home — run-state engine, on-device menu, view model, command plan, peer
registry, sync dedup/gate, and the pure HA topic/descriptor/option builders
(`TimerHa`). Only the Timer's HA **carrier lifecycle** stayed behind: ten HA carrier
entity pointers held as file globals, a ~130-line construction routine, the runtime
enable/remove paths, the dedicated duration text callback, and the Timer branches of
the shared ArduinoHA callbacks — roughly a quarter of the file. So the Timer domain
leaked across the MQTT seam, `MQTTManager` was forced to know the `TimerHaEntity` slot
vocabulary, and the open Pomodoro PRD (#119) would have accreted a *second* ~10-entity
block the same way.

Unlike the `PeerRegistry`/`SyncEnvelope`/`SyncSeenCache` cuts — each a pure slice that
shipped its own dependency-free test env — the carrier lifecycle is **impure**: it
calls `new HAText(...)` against the real ArduinoHA client. There is no pure slice here
to test in isolation.

## Decision

### A device-bound `TimerHaHost` owns the carrier lifecycle

`TimerHaHost` ([src/TimerHaHost.h](../../src/TimerHaHost.h) /
[TimerHaHost.cpp](../../src/TimerHaHost.cpp)) owns the ten carrier pointers and their
resolved id buffers as private state, the carrier construction (idempotent, at the same
HA-setup sequence point as before), the runtime `enable`/`remove` paths, the dedicated
duration text callback, and the Timer branches of the shared select/switch/button
callbacks. It pairs with the pure `TimerHa` builder half (topics, descriptors, options,
`haRegistrationAtCap`), which is unchanged.

```cpp
void setup();        // resolve carrier ids + (when SHOW_TIMER) construct/register
void onConnected();  // publish Timer wire + attr groups; flush pending cleanup (#195)
void reconcile();    // boot-time SHOW_TIMER-vs-persisted reconcile + cleanup latch (#195)
void enable();       // SHOW_TIMER false->true: create if missing + publish
void remove();       // SHOW_TIMER true->false: prune discovery + clear attr bags
bool tryHandleSelect(HASelect*, int8_t);   // true iff a Timer carrier -> host handles it
bool tryHandleSwitch(bool, HASwitch*);
bool tryHandleButton(HAButton*);
```

The three shared ArduinoHA callbacks keep their non-Timer branches in `MQTTManager` and
gain a single leading `if (TimerHaHost.tryHandle…(…)) return;` delegation; the Timer
branch bodies move verbatim into the `tryHandle*` methods, preserving the
route-through-`timerHaApply` validation and the snap-back echo (ADR-0001 / ADR-0014).

### Client access via externed globals, not injection

The `HADevice`/`HAMqtt` instances stay owned in the general MQTT translation unit and
are reached from `TimerHaHost` via `extern`. The host calls `new HAText(...)` and cannot
be host-tested regardless, so an injected client would be a one-adapter seam forever;
externing is the least-ceremony choice and the coming Pomodoro host reads the same
globals the same way. `kMaxHAEntities` (the entity cap the carrier build's
`haRegistrationAtCap` guard reads) moved from an anonymous-namespace constant in
`MQTTManager.cpp` to a shared constant in `MQTTManager.h` for the same reason.

- **vs. injecting the client** — rejected: one implementation forever; pure ceremony.

### No new test env — an honest divergence from the sibling extractions

`TimerHaHost.cpp` is excluded from every `native*` `build_src_filter` (an explicit
allowlist), exactly as `MQTTManager.cpp` is, and compiles only on device. It ships **no**
host-test env, because there is no pure slice to test. Its value is **locality** (one home
for the ten-carrier lifecycle) and a shrunk `MQTTManager` interface — plus the seam
Pomodoro (#119) will reuse instead of duplicating. The regression surface is the existing
suites staying green (the pure collaborators the host orchestrates — `TimerHa`,
`timerHaApply`, the codecs, the Targets value↔index mapping — are already covered) plus
the `ulanzi`/`native` builds compiling.

### A transitional bridge, closed across the PRD

This ADR's decision was the first slice of PRD #191; the bridge it opened is now
closed. The later slices absorbed the pieces this slice left in `MQTTManager`: the
debounced dynamic Targets-select republish became `TimerHaHost.refreshTargets()`
(issue #194, dropping the external-linkage exposure of the select pointer + debounce
variables), and the `SHOW_TIMER` reconcile bookkeeping became `TimerHaHost.reconcile()`
with its connect-path pending-cleanup latch folded into `onConnected()` as **private**
host state (issue #195). After #195, `MQTTManager` holds **zero** Timer-HA-specific
state. `SHOW_TIMER_HA_PREV` deliberately stays a Globals-owned persisted (NVS-backed)
flag reached via `extern` — the settings-apply path writes it independently of
reconcile, so it is a shared global exactly like `SHOW_TIMER`, not host-private. The
wire seam (`publishTimerWire`/`timerWireTopic`/`timerWireAttrTopic`) stays in
`MQTTManager` and reaches the host's private carrier state through two small accessors
(`carriersReady()`, `entityId(slot)`).

## Consequences

- `MQTTManager` sheds the ten carrier pointers, the `timerHaIds` buffer, the carrier
  construction, `enableTimerHADiscovery`/`removeTimerHAEntities`, and the duration
  callback; its shared callbacks become one-line delegators to the host. `setup()` calls
  `TimerHaHost.setup()` at the same sequence point; the connect path calls
  `TimerHaHost.onConnected()` and `TimerHaHost.remove()`. The `SHOW_TIMER`-toggle sites in
  `DisplayManager.cpp` and `MenuManager.cpp` call `TimerHaHost.enable()`/`.remove()`.
- New source files `TimerHaHost.{h,cpp}`, compiled only by the firmware envs.
- No new HA entities, no discovery/wire/attribute/sync/peer-registry behavior change.
  ADR-0006 (sync), ADR-0014 (attribute projection), ADR-0019 (peer presence) and issue
  #60 (teardown clears retained attr bags) behaviors are preserved verbatim. The device
  (`ulanzi`) and host (`native`, `native_ha`) builds are unaffected.

See also: [ADR-0014](0014-timer-ha-attribute-projection.md) (the attribute projection the
host keeps opting carriers into), [ADR-0021](0021-peer-registry-extraction.md) (the
sibling extraction move, with the noted device-bound divergence).
