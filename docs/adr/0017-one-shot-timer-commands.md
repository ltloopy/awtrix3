# One-shot Timer commands (`save:false`) and the saved-vs-effective config split

Status: accepted

## Context

Every Timer config key sent via `POST /api/timer` or the `{prefix}/timer` MQTT
topic is **persisted to NVS and becomes the new default**. Sending
`{"duration":300,"action":"start"}` permanently changes the saved duration, so
the *next* run also uses 300. There was no way to run a timer with one-off
parameters — most importantly a one-off `duration` and a one-off custom alarm
melody — without those values overwriting the settings kept for next time.

PRD #99 (for #93) adds a single payload-level boolean **`save`** (default
`true`). `save:false` makes a command **one-shot**: its config changes apply to
the *current run only* and revert to the saved settings the moment the timer
returns to Idle, writing nothing to flash. Additionally, `melody_end` and
`melody_tick` accept an **inline RTTTL** tune (a literal melody string), which is
one-shot by nature. It landed in three code slices plus this ADR: the override
core (#100), honest observation carriers under override (#101), and inline
melodies (#102). Terminology lives in [CONTEXT.md](../../CONTEXT.md); the
user-facing surface in [timer.md](../timer.md) and [api.md](../api.md).

This change must preserve the existing Timer invariants: the atomic-reject
validation contract ([ADR-0001](0001-timer-command-validation-parity.md)),
multi-device sync ([ADR-0006](0006-timer-multi-device-sync.md)), the HA
attribute projection ([ADR-0014](0014-timer-ha-attribute-projection.md)), and the
HTTP observation mirror ([ADR-0015](0015-http-observation-mirrors-full-config.md)).

## Decision

### 1. A single payload-level `save` flag, default `true`

`save` is one **payload-level** boolean covering *all* config keys in that
payload — duration, buzzer, finished mode, the timing knobs, bar/icon toggles,
colours, melodies — not a per-key selector. `save:false` makes the whole command
one-shot. It is validated like any other field in the atomic-reject pass (a
non-boolean `save` is `BadField`/400); nothing is applied if it — or any sibling
field — is invalid (ADR-0001 preserved).

The name `save` deliberately avoids the project's reserved vocabulary: *persisted*
(the NVS write) and *retained* (the MQTT retain flag) both already mean specific
things. `save:false` reads as "do not save this command's settings."

- **vs. a per-key transient selector** — rejected: a single payload-level flag
  matches the issue's "allow all variables to be one-time" and spares the user
  from reasoning about which keys persist.
- **On-device TIMER menu always persists** — the menu never emits `save:false`;
  physical edits behave predictably and commit to NVS (ADR-0008 unchanged). A
  documented parity note, not a control-surface divergence (ADR-0001): `save` is a
  wire-payload concept the menu has no payload for.

### 2. Saved config vs effective run config (the core model)

The load-bearing distinction: **saved config** (the flash truth every
observation/sync carrier reports) vs **effective run config** (what the active
run actually uses). `save:false` writes only the effective layer.

On a one-shot apply, before the apply window, a snapshot of the saved config is
captured — done **generically via the two existing Timer descriptor tables**
(`TIMER_SETTINGS_DESCS`' `inSnapshot` rows and `TIMER_MEMBER_CONFIG_DESCS`), plus
`durationSec` and the resolved melody RAM, so no per-key code is required. The
command then applies **live** so run behaviour uses the new values, but the
persistence/config-publish side effects are **suppressed**:

- the persist batch runs in a **transient** mode so its scope exit skips both NVS
  flushes (no `persist()`, no `saveSettings()`);
- `broadcastConfig()` does not fire (config must not propagate to sync followers,
  who have no notion of revert — ADR-0006);
- the HA config-attribute republish is skipped (the retained bags stay at saved).

Run-state still flows: `duration` is applied live, the run-state broadcast fires,
and `state`/`remaining` publish as normal.

**Revert on return to Idle.** One private `returnToIdle()` seam, called by **both**
the `reset()` path and the tick auto-clear transition, restores the snapshot
(generically, back through the same two tables) and clears the override. The
previously-saved default is used again on the next run.

**Concurrency: latest command wins, single snapshot.** A second `save:false`
command applies on top of the same baseline (no re-capture). A normal
(`save:true`) config command arriving mid-override commits the live config —
including the prior one-shot values — as the **new saved baseline** and ends the
override (so the user can deliberately commit settings mid-run); a later revert
then leaves the promoted truth in place.

`sync_*` (local identity) and `duration` (run-state) are excluded from the config
snapshot by construction: `sync_*` are `inSnapshot=false`, and `duration` is not a
config row.

- **vs. "merely skip the NVS write"** — rejected: full-surface coverage requires
  the carriers to stay honest (§3), which a skip-the-write-only approach cannot
  guarantee.

### 3. Carriers report saved config during an override

While an override is active, every observation/sync carrier serializes from the
captured saved config, never the one-off values — so dashboards and sync
followers are not polluted by transient run-only settings. A single RAII scope
(`SavedConfigScope`) presents the saved config block in live storage for the
duration of a carrier projection, then restores the effective values, wrapping the
three carrier reads:

- `GET /api/timer`'s nested `config` mirror (ADR-0015) shows **saved** values;
- the device-to-device (UDP) config snapshot shows **saved** values;
- the HA attribute-group bags stay at **saved** values (a reconnect republish
  serializes the saved truth — ADR-0014).

Run-state stays live and honest: top-level `duration` reflects the **live**
one-shot value (so a client can still observe what is counting down) — it is never
inside the `config` block (ADR-0015) — and run-state (start/pause/reset + the
one-shot `duration`) still propagates to followers so synced timers start together
(ADR-0006). The split is coherent: **top level = effective, `config` = saved.**

### 4. Inline RTTTL melodies are always one-shot

`melody_end`/`melody_tick` accept **either** a bare file-name token (as today) **or**
an inline RTTTL tune, distinguished by content: a bare token is `[A-Za-z0-9_-]*`
(no colon), an inline tune carries RTTTL structure (`name:control:notes` with a
`d=`/`o=`/`b=` control section). A malformed inline tune is `BadField`/400
(atomic-reject). A pure classifier/validator pair is reused by command validation
and melody resolution.

An inline tune is **always one-shot regardless of `save`**, because it has no
persistable file form: it is staged directly into the resolved melody RAM, the
saved melody-name global is never touched, and its presence forces the command
one-shot (reusing §2/§3). So the `config` mirror and HA bags keep showing the
**saved bare name** throughout, an inline tune **never appears** in any carrier
(it is audible-only run-state), and `returnToIdle()` reverts it. A bare name
persists and obeys `save` exactly as before (back-compat).

## Consequences

- An automation author can run a timer once with a one-off `duration` and a custom
  inline alarm, leaving saved defaults, HA entities, and sync followers untouched.
- One-off runs write **nothing** to flash, sparing NVS wear for throwaway runs.
- The model is generic over the two descriptor tables, so a new config key is
  one-shot-capable and honestly reported with no per-key code.
- The ADR-0001 atomic-reject, ADR-0006 sync, ADR-0014 HA-attribute, and ADR-0015
  observation-mirror contracts are all preserved; the only addition each sees is
  "report saved values while an override is active."
- The on-device TIMER menu is unchanged: it always persists.

See also: [ADR-0001](0001-timer-command-validation-parity.md) (atomic-reject),
[ADR-0006](0006-timer-multi-device-sync.md) (sync),
[ADR-0014](0014-timer-ha-attribute-projection.md) (HA attribute projection),
[ADR-0015](0015-http-observation-mirrors-full-config.md) (HTTP observation mirror).
