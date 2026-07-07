# Project every Timer knob as a read-only Home Assistant attribute

Status: accepted

## Context

From a Home Assistant user's view the Timer exposed **eight entities** but its
*configuration* was almost entirely invisible. Only one tuning value — the re-alert
interval — surfaced, as a read-only JSON attribute on the finished-mode select (PRD
#17, then issues #51/#52). Everything else (auto-clear hold, countdown beep window,
the two melodies, bar color, the display toggles, max duration, the remaining-sensor
cadence, the on-device config-editor timeout, and the multi-device **sync identity**)
could only be discovered by reading `dev.json`, issuing an MQTT/HTTP query, or walking
the on-device menu. A user could not answer "what is this timer configured to do right
now?" from inside HA.

PRD #57 generalized the bespoke `realert_interval` attribute into a table-driven,
multi-carrier projection of **every** persisted-settings row, delivered across three
slices — table-driven groups on the two selects (#58), the `HASensor` opt-in lighting
up the remaining and state sensors (#59), and the on-device-commit refresh plus
retained-topic teardown (#60). A later stacked unit (PRD #66) added a **fifth carrier**:
`max_duration` also rides the writable Duration text entity, in the clock form that
entity's own state speaks — adopting the opt-in for a third device type (`HAText`, #67)
and sharpening the per-row formatter to a **carrier-native** representation (#68). This
ADR records the four load-bearing design choices now that the implementation is
complete, amended in place as PRD #66 extended §2 and §3 rather than spawning a new ADR
(the `HAText` opt-in is the adoption §2 anticipated; the per-carrier divergence sharpens
§3's existing concept). Terminology lives in [CONTEXT.md](../../CONTEXT.md); the
user-facing surface in [timer.md](../timer.md).

## Decision

### 1. Read-only attributes, not new entities

Each knob becomes observable as a retained `json_attr_t` object on the entity it is
semantically about — it does **not** become a controllable entity. The knobs stay on
the **observation surface** and out of the ADR-0001 **control surface** and its
atomic-reject parity contract; the existing write paths (`{prefix}/timer` config keys,
`dev.json`, the on-device `TIMER` menu) remain the only ways to change a value. The
entity count stays **8**.

- **vs. new `number` / `switch` / `select` control entities** — rejected: that would
  drag every knob onto the control surface and oblige it to honor the atomic-reject
  validation contract (ADR-0001), for values that have a perfectly good write path
  already. The goal is *visibility*, not a second control surface.

### 2. The JSON-attributes opt-in is per-type, not lifted into the base device type

The capability is added **per subclass** — `HASelect` first (#58), then `HASensor`
(#59); `HASensorNumber` inherits it, so the remaining sensor came for free. A **third**
type, `HAText`, adopted it later (PRD #66 / #67) so the writable Duration input can
carry its own attribute — the adoption this section anticipated, landing with no
shared-base change. It is an opt-in (`setJsonAttributes(true)` adds the `json_attr_t`
line to that type's discovery config; `publishJsonAttributes(json)` sends a retained
object) that defaults **off**, so a type that has not opted in keeps its discovery
payload byte-for-byte unchanged.

- **vs. lifting the capability into the shared base device type** — rejected. The
  `json_attr_t` **discovery-topic line is irreducibly per-subclass**: it must be
  emitted where each type's serializer is constructed and sized, so a base-class lift
  *cannot* remove the per-type line. It would only dedupe a trivial flag + setter +
  publish method while touching the shared base and the already-shipped `HASelect` —
  more blast radius for a smaller saving. Net negative. The `HAText` adoption (#67)
  confirmed this: a third type opted in by adding only its own per-type line, with the
  flag/setter/publish boilerplate repeated — exactly the trade-off this rejection
  accepts, and cheap enough that three adoptions never justified the lift.

### 3. A table-driven key→carrier facet, joining the descriptor-table family

`TIMER_ATTR_GROUP_DESCS` ([src/TimerSettings.cpp](../../src/TimerSettings.cpp)) holds
one row per projection — `{carrier, settings-key, optional formatter}` — and joins the
**Timer descriptor-table family** alongside `TIMER_HA_DESCRIPTORS`,
`TIMER_SETTINGS_DESCS`, `TIMER_MEMBER_CONFIG_DESCS` and `TIMER_MENU_SLOTS`. One generic
builder (`timerBuildAttributeGroup`) turns a carrier's rows into a JSON object, reusing
the same `timerSettingEmitValue` the config snapshot uses, so an attribute and the
snapshot can **never disagree** about a value. A key may map to **multiple carriers**
(one row each — `remaining_publish_interval` rides both the remaining sensor, its own
cadence, and the state sensor, a complete config view).

The optional per-row **formatter** renders a key in its **carrier-native
representation**: each carrier renders a projected key in the representation that
carrier already uses for its own state. This is a sharpening of the concept's first
form. `bar_color` first showed the representation can differ from the **config
snapshot** — it formats as the human string `"default"` (0 = follow text color) or
`"#RRGGBB"`, mirroring the persisted-settings table's optional `bespoke` validators.
`max_duration` (PRD #66 / #68) sharpens that to differ **per carrier**: the same key
rides the state sensor as raw seconds (`86400`, no formatter) **and** the Duration text
entity as the trimmed `H:MM:SS` clock string (`"24:00:00"`, via a formatter reusing the
exact `formatHMS` the Duration entity's own *state* speaks). A multi-carrier key may
therefore **read differently on each carrier**, keyed to each carrier's own state
language — yet the underlying value **cannot drift**: every carrier reads the same
persisted storage, raw via the shared emit or through a formatter over that same
storage. The five carriers and their bags:

| Carrier | Type | Attribute object |
| --- | --- | --- |
| `{id}_timer_dur` | text | `{max_duration}` — clock form, e.g. `"24:00:00"` |
| `{id}_timer_fin` | select | `{realert_interval, finished_hold}` |
| `{id}_timer_buz` | select | `{countdown_seconds, melody_tick, melody_end}` |
| `{id}_timer_rem` | sensor | `{remaining_publish_interval}` |
| `{id}_timer_state` | sensor | `{max_duration, remaining_publish_interval, icon_enabled, bar_enabled, bar_color, sync_follow, sync_targets}` — `max_duration` raw seconds `86400` (`app_config_timeout` removed, PRD #83 / #88) |

- **vs. keeping per-key bespoke publish paths** (the PRD #17 `realert_interval` shape)
  — rejected: it does not scale to this many knobs across five carriers without drift
  between the publish, the full-refresh, and the per-edit republish. The table makes
  adding or moving a knob's HA projection a one-row change, and the refresh/republish
  derive from the same table, so a new row cannot be silently skipped.

### 4. Read-only sync attributes do not reverse ADR-0006

The two sync-role keys (`sync_follow` / `sync_targets`) are projected as read-only
attributes on the state sensor — they become **visible** in HA. They remain
**non-writable from HA** (the attribute is observation only) and, crucially, **never
propagated** to peers: they are still the `inSnapshot == false` rows of
`TIMER_SETTINGS_DESCS` (ADR-0007), deliberately excluded from the config snapshot so
peers cannot hijack each other's targeting. Making a value *observable* is orthogonal
to making it *propagated* or *writable*; [ADR-0006](0006-timer-multi-device-sync.md)
stands unchanged.

**Addendum (PRD #27, issue #110): the sync rows become writable HA control
entities.** The two sync keys now also get dedicated **writable** entities — a
Follow `HASwitch` (`sync_follow`) and a static `Off`/`All` Targets `HASelect`
(`sync_targets`) — so they join the **control surface** as well as the observation
surface. This *narrows* §4's "non-writable from HA" without reversing its load-bearing
half: writes funnel through `parseCommand` (the HA callback adapter of issue #109), so
they inherit the same atomic-reject validation (`sync_follow` strict-bool,
`sync_targets` bespoke validator) and NVS persistence as every other control input —
no second control surface, no bypass. The keys stay `inSnapshot=false` local identity
and are **still never propagated** to peers, so [ADR-0006](0006-timer-multi-device-sync.md)
remains unchanged. The read-only state-sensor attribute stays **authoritative for the
exact `sync_targets` value**: the static select renders only `Off`/`All` and reflects
**unknown** (HASelect index `-1`) when `sync_targets` holds a specific-ID CSV set
out-of-band (the select becomes a dynamic peer list in a later peer-discovery slice).
The entity count rises from **8** to **10**. The Follow switch is the Timer's first
`HASwitch`; its discovery payload (and the static select's) is pinned on the host under
the `native_ha` env (`test_hasync`).

## Consequences

- Adding or moving a knob's HA projection is now a **single row** in
  `TIMER_ATTR_GROUP_DESCS`; the full wire refresh (`publishAllAttributeGroups()` at
  discovery-enable and every MQTT reconnect) and the per-edit republish (locally and on
  peer-propagated config edits alike) both derive from the table, so a new row is
  picked up everywhere automatically. The on-device `TIMER`-menu long-press commit
  refreshes every carrier after its peer broadcast (it does not track which knob
  changed) — closing a staleness gap that existed for `realert_interval` (#60).
- Teardown (`SHOW_TIMER` true→false) now clears each carrier's retained `json_attr_t`
  object (`clearAllAttributeGroups()`) alongside pruning the discovery entities, so the
  broker is not left holding orphaned attribute payloads — retroactively fixing the same
  gap for the legacy `realert_interval` attribute (#60).
- The `HASelect`, `HASensor` and `HAText` opt-ins are each exercised on the host under
  the `native_ha` environment (`test_haselect` / `test_hasensor` / `test_hatext`, the
  third mirroring the first two); the table + builder and the publish/refresh/republish
  are covered by pure host tests and wire-seam stub tests in `test/test_timer` —
  including the Duration-bag builder and the `max_duration` edit fan-out that pins the
  per-carrier representation divergence end-to-end (#68). No device behavior changed
  outside the HA observation surface.
- Documentation reconciled: CONTEXT.md's "Timer HA presence" / "Reusable opt-in
  capability" and timer.md's "Home Assistant entities" / "Behavior-tuning knobs" now
  describe all **five** carriers, the `HAText` opt-in, and the carrier-native
  representation principle.

Generalizes PRD #17; see [PRD #57](https://github.com/ltloopy/awtrix3/issues/57) and
issues #58–#60, extended by [PRD #66](https://github.com/ltloopy/awtrix3/issues/66)
(the `HAText` opt-in and carrier-native `max_duration`, issues #67–#69). Related:
[ADR-0001](0001-timer-command-validation-parity.md)
(control vs. observation surface), [ADR-0006](0006-timer-multi-device-sync.md) (sync
identity stays local), [ADR-0007](0007-timer-settings-descriptor-table.md) (the
`inSnapshot` boundary and the descriptor-table family).
