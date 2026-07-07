# Timer multi-device sync: broker-free UDP propagation surface

Status: accepted

## Context

The Timer was single-device: a start/pause/reset or config edit on one clock had no
effect on any other. Users with several clocks wanted one clock's timer to drive a
chosen set of others — or *all* clocks on the LAN — to run the same countdown, with the
group sharing identical configuration.

The obvious reuse — the existing `{prefix}/timer` MQTT topic — cannot do this: `MQTT_PREFIX`
is derived per-device from the unique ID ([src/Globals.cpp](../../src/Globals.cpp)), so
every clock listens on a *different* topic. A device-independent channel is required.

A `/grill-with-docs` session walked the design tree decision-by-decision. This ADR
records the load-bearing choices. The terminology (propagation surface, run-state vs
config propagation, sync roles) lives in [CONTEXT.md](../../CONTEXT.md).

## Decision

### Transport: broker-free UDP broadcast (not MQTT, not ESP-NOW)

Sync rides a dedicated `WiFiUDP` socket on **port 4212**
([src/ServerManager.cpp](../../src/ServerManager.cpp)), broadcast to the subnet via
`WiFi.broadcastIP()`. This reuses the UDP infrastructure already present for the
`FIND_AWTRIX` discovery handshake without requiring a broker.

- **vs MQTT** — would work but forces a broker dependency on a feature whose appeal is
  "two clocks on a shelf just mirror each other." Per-device prefixes would also need a
  shared group topic bolted on.
- **vs ESP-NOW** — broker-free and routerless, but a whole new protocol stack plus device
  pairing — far more surface for a timer.

A **separate port** (and a 1024-byte buffer, vs discovery's 255) so a full config
snapshot fits and `FIND_AWTRIX` text is never parsed as JSON.

### Classification: a new *propagation surface*, not a fourth control surface

ADR-0001 rests on "the **three** control surfaces that must stay in parity." An inbound
sync packet writes timer state, so it looks like a control surface — but counting it as a
fourth would amend that foundational invariant. Instead it is classified as a distinct
**propagation surface**: a clock relaying its already-formed command to a peer, where it
**re-enters the local control surface** through the same `parseCommand` path and is
subject to the identical atomic-reject contract. Atomic-reject is *reused*, not weakened;
"the three" stays "the three."

### Two-axis roles with a `follow` consent gate

> **Amended (default) by [#124](https://github.com/ltloopy/awtrix3/issues/124).** `follow`
> now defaults **on**, not off (below): a fresh clock boots as a **follower** — it obeys
> sync it is targeted by but, with an empty target list, commands nobody — and
> **standalone is an explicit opt-out** (`follow=false`). NVS wins on migration, so a
> device that already stored `false` stays standalone. The two-axis model, role names and
> the consent gate are otherwise unchanged.

Participation is two independent axes, not one on/off:

- `TIMER_SYNC_TARGETS` (**send**) — whom this clock commands on a local action: `""`
  (off), `all`, or a comma list of peer `uniqueID`s.
- `TIMER_SYNC_FOLLOW` (**receive**) — whether this clock obeys inbound sync it is targeted
  by; default `false`.

The `follow` gate is a deliberate cost (to make "everyone mirrors," every clock needs
both `follow=true` and `targets=all`). It is the only thing between an unauthenticated
LAN broadcast and an arbitrary clock starting a timer, it matches the consent posture of
the web-UI/MQTT auth on the other surfaces, and it enables a leader-only clock (commands
without being commandable). Targeting alone — no gate — was rejected for letting any LAN
device drive any reachable clock.

### Run-state and config propagate separately; `duration` is run-state

> **Superseded by [ADR-0018](0018-run-scoped-config-mirror.md).** Two decisions below
> are replaced. *Config half:* a config edit now propagates **nothing**, and config
> travels **only bundled with a `start`** (the leader's *effective* config), which a
> follower applies one-shot and reverts on return to Idle. *Duration half (#126):* a
> **bare duration edit also propagates nothing** — `duration` rides **only** with a
> `start`, so a leader's length edit no longer moves a follower's displayed time; a
> follower adopts the leader's duration on the next start and reverts on Idle. The
> roles, transport, gating, and `start`/`pause`/`reset` propagation are unchanged.

A naive "broadcast everything on every action" makes starting a timer rewrite a peer's
config. Instead the two split on different triggers:

- **Run-state** — `start` / `pause` / `reset` broadcast `{action}`, **never** config. A
  bare duration edit propagates **nothing**; `duration` rides only inside a `start`.
- **Config** — a deliberate config edit broadcasts a **full config snapshot** (no
  `action`, no `duration`); last-config-writer-wins, so the group converges to identical
  config only when someone *intends* a config change.

`duration` is treated as **run-state, not config** — it defines "the same countdown" and
rides with the `start` that opens a run. The frequent action (set a new length) therefore
never clobbers a peer's bar color / buzzer / etc., and a bare length edit stays purely
local. Putting `duration` in the config snapshot was rejected for relocating the clobber
surprise; broadcasting a bare duration edit (#126) was rejected for moving a follower's
displayed time on an edit it never started.

`pause` **does** propagate (the user's explicit choice, overriding the narrower
start/reset-only option), so the group pauses and resumes together.

### Action-only packets (no live `remaining`); one-hop topology

Run-state packets carry the `action`, **never** the live `remaining`. Receivers snapshot
their own remaining, so a propagated pause aligns to within LAN latency and a *missed*
packet self-corrects on the next start/reset (which re-establishes a shared duration).
Carrying `remaining` was rejected: `parseCommand` has no `remaining` input, so applying it
would require a state-setting side-channel that bypasses the surface's only validated
entry point — breaking the "everything re-enters via `parseCommand`" through-line — and it
would let one clock's skew yank everyone's display.

Propagation is **one-hop**: a `_remoteApply` guard makes the `broadcast*` methods no-op
while an inbound packet is being applied, so a received command is never re-broadcast.
`all` reaches everyone with a single packet; partial target lists do not chain. This keeps
the loop guard trivially correct and avoids TTL/storm-control machinery.

### Best-effort delivery: 3× redundant send + `(src, seq)` dedup

UDP is lossy and a missed `start` leaves a clock visibly idle while the rest count down.
`sendTimerSync` emits each packet **3×** spaced ~15 ms; receivers keep a bounded,
TTL-based recently-seen cache of `(src, seq)` and drop the redundant copies. The dedup is
needed anyway for the loop guard, so redundancy is near-free — most of acking's
reliability with none of its per-target state, unicast, or timeouts. The TTL window
(~2 s) also means a sender reboot (seq restart) self-clears by ageing out, so no boot
nonce is needed.

### Targeting by `uniqueID`, not hostname

Targets and `_sync.src` use the stable `uniqueID` (`awtrix_ab12`, MAC-derived, already the
MQTT prefix and mDNS `id`). It never changes, can't collide, and compares trivially for
echo suppression. Hostname targeting was rejected: hostnames are user-mutable and can be
renamed out from under a target list, silently breaking sync. The `all` keyword means most
users never type an id.

### Persistence and validation reuse the ADR-0004/0005 pattern

`TIMER_SYNC_FOLLOW` / `TIMER_SYNC_TARGETS` follow the well-trodden path: global →
`dev.json` (`timer_sync_follow` / `timer_sync_targets`) → `POST /api/timer` /
`{prefix}/timer` (`sync_follow` / `sync_targets`) → NVS `"awtrix"` (`TSYNF` / `TSYNT`),
with strict-bool / validated-string atomic-reject in `parseCommand`. `sync_follow` mirrors
the `bar_enabled` strict-bool shape; `sync_targets` validates `""` / `all` / comma list of
`[A-Za-z0-9_-]` tokens (≤32 each).

## Out of scope

- **No on-device menu** and **no HA entity** for the two sync settings — consistent with
  the ADR-0004/0005 config flags; configured via `dev.json` / `POST /api/timer` /
  `{prefix}/timer` only.
- **`sync_follow` / `sync_targets` are never propagated** — they are each clock's local
  identity, deliberately excluded from the config snapshot so peers can't hijack each
  other's targeting.
- **No global ordering.** Two near-simultaneous starts on different peers race (each
  applies the other's); acceptable and self-healing on the next action.
- **No late-joiner state pull.** A clock that boots mid-run does not query peers; it picks
  up the next propagated action.

## Consequences

- `CONTEXT.md` gains the **Propagation surface** entry (with the run-state/config split)
  and the **Sync roles** entry; both warn against counting sync among "the three" parity
  surfaces.
- `docs/timer.md` gains a **Multi-device sync** section and persistence/settings rows;
  `docs/dev.md` and `docs/api.md` document the two new keys.
- The NVS `"awtrix"` namespace gains keys `TSYNF` / `TSYNT`.
- `kTimerCmdJsonSize` rose 512 → 2048 to hold the config snapshot plus envelope (with
  64-bit host-test headroom); `parseCommand` / `applySyncCommand` use heap
  (`DynamicJsonDocument`).
- Host tests `test_S1`..`test_S6` cover settings validation, the run-state/config split,
  echo/follow/target/dedup gating, and the one-hop loop guard; a `ServerManager` stub
  records broadcast payloads.
- Future "sync another action" PRs extend `broadcastRunState`; future config keys are
  picked up by `buildConfigSnapshot` automatically.
