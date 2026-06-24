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
the LAN — the backend a dynamic HA Targets select will consume in a later slice
(#112). This ADR defines that presence subsystem; it builds no UI.

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

## Out of scope

- **No dynamic HA Targets select** and no targeting UI — this ADR is the backend
  only; the select that consumes the registry is a later slice (#112).
- **No presence persistence, no late-joiner pull, no acks** — best-effort, exactly
  like the run-state channel (ADR-0006).
- **No change to run-state/config propagation, roles, gating or the one-hop guard.**

## Consequences

- `CONTEXT.md` gains the **Peer presence / peer registry** entry.
- `TimerManager` gains the peer registry (`_peers`/`_peerCount`), `recordPeer` /
  `prunePeers` / `hasPeer` / `peerCount`, the `broadcastPresence` beacon and the
  `tickPresence(nowMs)` loop hook; `applySyncCommand` grows the ungated presence
  short-circuit. `main.cpp`'s loop calls `tickPresence(millis())`.
- Host tests `test_PP1`..`test_PP5` cover ungated harvest + no state change,
  presence-never-a-command, own-id exclusion + the bound, age-out past TTL, and the
  periodic-but-not-in-AP-mode beacon.

See also: [ADR-0006](0006-timer-multi-device-sync.md) (the propagation surface this
extends), [ADR-0018](0018-run-scoped-config-mirror.md) (the run-scoped config
mirror on the same channel).
