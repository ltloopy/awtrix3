# Run-scoped config mirror & one-shot sync receive

Status: accepted

Supersedes the config-propagation decision of
[ADR-0006](0006-timer-multi-device-sync.md) ("Run-state and config propagate
separately; `duration` is run-state" — specifically the *config edit broadcasts a
full snapshot* half). Refines [ADR-0017](0017-one-shot-timer-commands.md) §3
(carriers under a one-shot override). The run-state propagation, the two-axis
sync roles, the transport, and the gating of ADR-0006 are all **unchanged**.

## Context

ADR-0006 split the propagation surface into **run-state** (start/pause/reset +
`duration`) and **config** (a full snapshot, fired by a deliberate config edit,
last-config-writer-wins). The config half had a recurring surprise: editing a
setting on one clock silently rewrote *every* targeted peer's saved settings, so
two clocks on a shelf could not keep their own identity (one as a kitchen timer,
one as a desk timer) while still mirroring a shared *run*.

[ADR-0017](0017-one-shot-timer-commands.md) then introduced the **saved vs
effective** model: a `save:false` command applies to the current run only and
reverts on return to Idle, writing nothing to flash, with the carriers staying
honest at the **saved** values during the override. That gives us exactly the
machinery a follower needs: apply a synced run without persisting it.

This ADR redefines device-to-device config sync around those two facts.

## Decision

### 1. A config edit propagates nothing

Removing the config-edit `broadcastConfig()` trigger from `parseCommand` (and the
on-device `commitTimerMenu` broadcast in `MenuManager`). A deliberate config edit
— over any control surface, local or on-device menu — now emits **no sync
packet**. There is no config convergence on edit; each clock keeps its own saved
identity. `broadcastConfig()` survives as a method (the saved-snapshot builder),
but nothing in the command/menu path fires it.

- **vs ADR-0006's "config edit broadcasts a snapshot"** — rejected: it was the
  source of the "starting/editing rewrote my settings" surprise, and it forced
  peers to converge on config they may not want.

### 2. A `start` carries the leader's effective config (one combined packet)

A `start` broadcasts **one combined packet** = the leader's **effective** config
snapshot + `duration` + `action:"start"`. Config now travels *only* bundled with a
start, scoped to that run. `broadcastRunState("start")` builds the snapshot into
the full `kTimerCmdJsonSize` buffer (config snapshot + envelope); `pause`/`reset`
stay run-state-only on the small static buffer, and a bare duration edit still
propagates `duration` alone.

The bundled snapshot reports **effective** config, **not** saved — deliberately
diverging from the saved snapshot ADR-0017 §3 presents to the other carriers. The
reason: a leader running its own one-shot (`save:false`) run must still mirror
those one-off values to followers, so the snapshot has to report what is actually
running. `sync_*` (local identity) are `inSnapshot=false` and excluded by
construction; inline melodies never travel (the snapshot reads the saved bare-name
globals, ADR-0017 §4).

### 3. Receiver-forced one-shot

A remote-applied command is **always one-shot regardless of the leader's `save`
flag**, forced through the existing `_remoteApply` guard (which already marks the
one-hop inbound-apply window). A follower therefore:

- mirrors the leader's config + duration for the duration of the run (effective
  layer, RAM live), but
- persists **nothing** to flash, and
- reverts to its **own** saved config/duration on return to Idle (the shared
  `returnToIdle()` seam from ADR-0017), regardless of what the leader sent.

This reuses the ADR-0017 override core wholesale — capture-snapshot-on-apply,
revert-on-Idle, transient persist batch, carriers-stay-honest — with the single
addition that `_remoteApply` is another trigger for `oneShot` (alongside
`save:false` and an inline melody). No follower ever has a notion of reverting a
*saved* config, which is exactly why ADR-0006's config-snapshot-on-edit could not
work for it.

### 4. Carriers: the sync start-packet reports effective; everything else saved

The split sharpens ADR-0017 §3:

- the **device-to-device sync carrier** (the bundled start packet) reports
  **effective** config — so a leader's own one-shot run mirrors to followers;
- the `GET /api/timer` `config` mirror and the **HA attribute bags** stay
  **saved** (unchanged from ADR-0017 §3);
- a remote-applied config edit is one-shot on the follower, so its HA attribute
  republish is suppressed too — the follower's bags keep reporting its own saved
  config.

## Consequences

- Two clocks can share a *run* without sharing *identity*: a start mirrors the
  countdown and config for that run, then each reverts to its own saved settings.
- "Editing a setting rewrote my peers' settings" is gone — config edits are local.
- A follower never persists a synced run, sparing its NVS and preserving its own
  defaults, with no leader cooperation required (`save` is ignored on receive).
- The model is generic over the two descriptor tables (ADR-0007/0009): a new
  config key rides the combined start packet and is one-shot on the follower with
  no per-key code.
- ADR-0006's run-state propagation, sync roles, transport, gating, dedup and
  one-hop guard are all preserved; only the config-propagation trigger changed.

See also: [ADR-0006](0006-timer-multi-device-sync.md) (the sync surface this
refines), [ADR-0017](0017-one-shot-timer-commands.md) (the one-shot override core
reused), [ADR-0007](0007-timer-settings-descriptor-table.md) /
[ADR-0009](0009-member-config-hook-table.md) (the two descriptor tables the
snapshot is generic over).
