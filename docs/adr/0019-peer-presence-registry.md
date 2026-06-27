# Peer presence registry & dynamic HA sync targeting

Status: accepted

Builds on [ADR-0006](0006-timer-multi-device-sync.md) (the broker-free UDP
propagation surface, port 4212, the `_sync` `src`/`seq` envelope, `uniqueID`
targeting, the `_remoteApply` one-hop guard) and is referenced by PRD #27. The
run-state propagation, sync roles, transport and gating of ADR-0006/0018 are all
**unchanged**; this ADR adds a passive *presence* sub-channel alongside them.

## Context

To target a peer over the propagation surface a user must name it by its stable
`uniqueID` (`awtrix_ab12`, MAC-derived — the targeting key, ADR-0006). Today they
must hand-type those ids: there is no way for a clock to learn which peers are on
the LAN. The existing `FIND_AWTRIX` discovery handshake is unsuitable — it returns
the user-mutable **hostname**, not the `uniqueID`, so a target list built from it
breaks silently when a clock is renamed.

We need a clock to know, at any moment, the set of peer `uniqueID`s reachable on
the LAN, and to surface them so a user can pick a target without hand-typing ids.
This ADR defines that presence subsystem (the beacon + registry, #111) and the
dynamic Home Assistant **Targets select** that consumes it (#112).

## Decision

### 1. A periodic presence beacon, unconditional on a real network

Each clock broadcasts a small `{_sync:{src,seq}, presence:true}` packet on the sync
port roughly every 30s (`kPresenceIntervalMs`), emitted from `tickPresence(nowMs)`
driven by the device loop. The beacon is sent **unconditionally** when on a real
network — independent of `TIMER_SYNC_TARGETS` — so a clock that commands nobody is
still discoverable. It is **suppressed in AP mode**: a clock on its own
config-portal AP has no real LAN to be discovered on. (The send transport
`sendTimerSync` is itself AP-gated; `tickPresence` also gates, so the rule is
testable at the manager layer.)

- **vs reusing `FIND_AWTRIX`** — rejected: it carries the hostname, not the
  `uniqueID`, and rides the 255-byte discovery buffer that must never be parsed as
  JSON (ADR-0006).
- **vs beacon only when sync is on** — rejected: a standalone clock could then
  never be targeted, defeating the point.

### 2. The beacon is not a command; harvest is UNGATED

A presence packet carries **no** `action`/`duration`/config — it is informational.
On receive, `applySyncCommand` recognises the `presence` marker and harvests the
sender's `uniqueID` into the registry **before and bypassing** the follow/target
consent gate that every real command passes, applies **no** timer state, and
short-circuits before the `parseCommand` path entirely.

This is the single deliberately ungated inbound path on the surface. It is safe
precisely because it changes nothing: learning that a peer exists is not obeying
it. Own-`src` echoes are still dropped (a clock never registers itself). Gating
harvest behind `follow` was rejected — a leader-only clock (follow off) must still
learn its peers to offer them as targets.

### 3. A bounded, self-aging RAM registry keyed by `uniqueID`

The registry is a bounded array (`kPeerMax` ~16) of `{uniqueID, lastSeen}`:

- **own id excluded** by construction;
- **aged out** after `kPeerTtlMs` (~100s ≈ 3 missed 30s beacons), pruned on each
  `tickPresence()` so a powered-off peer disappears on its own;
- **full-registry policy:** a new peer past the bound overwrites the **stalest**
  slot, so a busy LAN keeps the freshest peers rather than rejecting newcomers;
- **pure RAM / LAN-derived:** never persisted, cleared on boot — a rebooted clock
  re-learns peers from the next round of beacons.

Keyed by `uniqueID`, not hostname, for the same reason targets are (ADR-0006):
hostnames are user-mutable and would break the key out from under the registry.

### 4. Testability via injected time

Beacon cadence, TTL and bound are exercised at the manager layer: `tickPresence`
takes the current `millis()` as a parameter and the host tests advance virtual
time, so the period throttle, the age-out, the bound, the own-id exclusion and the
AP-mode suppression are all deterministic without wall-clock or a live socket.

### 5. The HA Targets select is built dynamically from the registry (#112)

The Home Assistant **Timer sync targets** select (a writable entity since #110)
consumes the registry: its options are built at runtime as `Off`, `All`, then every
currently-discovered peer `uniqueID` **sorted ascending** — not the static `Off;All`
table field. The select **opts out** of the descriptor's static `options`; the option
string is assembled from `TimerManager::peerIds()` through the pure
`timerSyncTargetsBuildOptions` helper.

- **Single-target by design.** An HA select expresses exactly one option, so picking a
  peer sets `sync_targets` to that one id (`Off` → `""`, `All` → `"all"`). A multi-id
  CSV target list (settable out-of-band via the API/`dev.json`) cannot be represented
  and displays as **unknown** (no option selected); the read-only `sync_targets`
  attribute stays authoritative for the exact value.
- **Discovery re-published on membership change, debounced.** `HASelect::setOptions` is
  set-once, so the select gains a `resetOptions()` that releases the list; when the
  sorted id set changes, `refreshTimerSyncTargetsOptions` rebuilds the options and
  re-publishes the entity's discovery config so Home Assistant sees the new list. It is
  debounced (and only fires on an actual change) so beacon churn yields one republish,
  not a storm.
- **Mapping is always against the CURRENT list.** Command (index → value) and
  state-reflection (value → index) both resolve through the live sorted id list
  (`timerSyncTargetsValueForIndex` / `timerSyncTargetsIndexForValue`); the selected
  state is re-applied after each republish. The command still routes through
  `timerHaApply` → `parseCommand`, inheriting the bespoke `sync_targets` validator and
  atomic-reject persistence (#109/#110). `sync_targets` remains local identity
  (`inSnapshot=false`) and never propagates.

This **reverses ADR-0006's "no HA entity for sync settings"**: #110 made the two sync
settings writable HA entities, and #112 completes the targeting half by sourcing the
options from live peer presence. The reversal is safe because sync settings stay local
identity — exposing them in HA changed no propagation semantics.

## Out of scope

- **No multi-target HA control** — the select is single-target by the platform's
  nature; CSV target lists remain an API/`dev.json` capability (shown as unknown).
- **No presence persistence, no late-joiner pull, no acks** — best-effort, exactly
  like the run-state channel (ADR-0006).
- **No change to run-state/config propagation, roles, gating or the one-hop guard.**

## Consequences

- `CONTEXT.md` gains the **Peer presence / peer registry** entry.
- `TimerManager` gains the peer registry (`_peers`/`_peerCount`), `recordPeer` /
  `prunePeers` / `hasPeer` / `peerCount` / `peerIds` (sorted accessor), the
  `broadcastPresence` beacon and the `tickPresence(nowMs)` loop hook; `applySyncCommand`
  grows the ungated presence short-circuit. `main.cpp`'s loop calls `tickPresence(millis())`.
  > **Superseded in part by [ADR-0021](0021-peer-registry-extraction.md):** the registry
  > *set* (`recordPeer`/`prunePeers`/`hasPeer`/`peerIds` + `_peers`/`_peerCount`) moved into a
  > standalone host-testable `PeerRegistry`; `TimerManager` now owns a `PeerRegistry` and
  > exposes `peerCount`/`hasPeer`/`peerIds` as forwarders, keeping only the beacon
  > cadence + send. The behaviour above is unchanged.
- `TimerHa` gains the pure option-build + id↔index helpers
  (`timerSyncTargetsBuildOptions`, `timerSyncTargetsIndexForValue`,
  `timerSyncTargetsValueForIndex`); `HASelect` gains `resetOptions()`; `MQTTManager`
  builds the select's options from the registry, re-publishes its discovery on a
  debounced membership change (`refreshTimerSyncTargetsOptions`, called from the loop),
  and maps command/state through the current id list. `timerHaApply`'s SyncTargets case
  is now value-based.
- Host tests `test_PP1`..`test_PP5` cover the presence subsystem; `test_DT1`..`test_DT6`
  (native) cover the dynamic option build + id↔index mapping incl. the unknown case +
  the sorted `peerIds` accessor, and `test_DT7` (native_ha) covers `resetOptions` rebuild
  + discovery republish.

See also: [ADR-0006](0006-timer-multi-device-sync.md) (the propagation surface this
extends), [ADR-0018](0018-run-scoped-config-mirror.md) (the run-scoped config
mirror on the same channel).
