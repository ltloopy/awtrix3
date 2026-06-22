# Widen `GET /api/timer` to mirror the full persisted Timer configuration

Status: accepted

## Context

The Timer app's **control surface** and its **observation surface** were out of
balance on the HTTP carrier. `POST /api/timer` accepts ~25 keys — duration,
buzzer/finished, the four per-state icons, and ~17 persisted behaviour/config
knobs. But `GET /api/timer` returned only **8** fields: `state`, `enabled`,
`remaining(_str)`, `duration(_str)`, `buzzer`, `finished`. Everything else you
could *write* over HTTP you could not *read back* over HTTP.

A Home Assistant user already gets the full configuration picture via the
read-only JSON attribute groups ([ADR-0014](0014-timer-ha-attribute-projection.md))
plus the retained `{prefix}/timer/icons` topic. An **HTTP-only** integrator — no
HA, no MQTT subscription to the discovery `json_attr_t` topics — had **no way to
read the device's live Timer configuration**. HTTP was the lone observation
carrier that lagged.

PRD #73 widened the HTTP observation surface so `GET /api/timer` mirrors the full
persisted configuration, reaching read parity with what HA already exposes, while
leaving the existing 8 top-level fields byte-compatible for current consumers. It
landed in three slices: the raw two-table projection end-to-end (#74), the two
carrier-native renderings (#75), and this documentation/ADR (#76). Terminology
lives in [CONTEXT.md](../../CONTEXT.md); the user-facing surface in
[api.md](../api.md#state-observation) and [timer.md](../timer.md).

## Decision

### 1. An observation surface *widened*, not a second attribute-group carrier

Config-readback is the same concern as observation, so this is the existing
**observation surface** doing more, not a new carrier of HA-style attribute
groups. The glossary's observation-surface definition widens from "reports the
same live values" to "reports the same live values **and the same persisted
configuration**." On HTTP, config-read genuinely *is* just `GET`: there is no
per-entity attribute structure to honour, so `GET /api/timer` gains a nested
`config` object rather than an attribute facet.

The endpoint stays a **pure observation read**: always `200 OK`, never mutating,
no `409` when the Timer is disabled (`enabled` carries that bit). It does **not**
join the ADR-0001 control surface or its atomic-reject contract — there is no
input to validate.

- **vs. a second attribute-group carrier on HTTP** — rejected: HA attribute
  groups exist because HA needs a per-entity structure; HTTP has no entities, so
  a flat `config` object under `GET` is the natural shape.

### 2. Carrier-native HTTP representation (lifted to a general property of reads)

The "carrier-native representation" principle — a read surface renders each value
in the representation that carrier already uses for its own state — is **lifted**
out of attribute-group-only language ([ADR-0014](0014-timer-ha-attribute-projection.md) §3)
into a general property of reads on **any** carrier. The raw-vs-rendered question
thus has a documented home on every carrier.

The HTTP `config` object is a raw table dump with two deliberate overrides,
following this endpoint's own raw+`_str` precedent for durations:

- `bar_color` → rendered `"#RRGGBB"` / `"default"` (reusing the file-local color
  formatter), not the raw 24-bit integer.
- `max_duration` → kept **raw** *and* supplemented with `max_duration_str` (the
  trimmed `H:MM:SS` clock form, reusing the same `formatHMS` the duration fields
  speak).
- All other keys raw: interval seconds as ints, melodies/`sync_targets` as
  strings, toggles as bools.

This diverges from the raw propagation config snapshot by design; the values still
**cannot disagree** because both read the same persisted storage via the shared
emitters.

- **vs. matching the raw propagation snapshot exactly** — rejected: HTTP's own
  `duration`/`remaining` already pair a raw value with a `_str`, so the HTTP read
  has a native form of its own; forcing the snapshot's raw-only form would be less
  useful to an HTTP client and ignore the established precedent.

### 3. Nested `config`, full two-table dump, accept the 2-key duplication

Top level keeps the stable summary (`state`, `enabled`, `remaining(_str)`,
`duration(_str)`, `buzzer`, `finished`). The new `config` object is the complete
persisted-config mirror, built from **both** config tables (`TIMER_SETTINGS_DESCS`
walked with no `inSnapshot` filter, so the sync-role keys are included; plus
`TIMER_MEMBER_CONFIG_DESCS`) by a single pure projection
`timerBuildFullConfig` — so the read surface **cannot drift** from what is
writable, and a new persisted key becomes readable with no extra wiring (pinned by
a drift-guard test that walks both tables).

- `buzzer`/`finished` appear in **both** places: top-level (back-compat) and in
  `config` (so the mirror is a self-contained copy of the writable configuration).
- `duration` is **top-level only** — it is run-state, not config (excluded from
  the member-config table for exactly that reason), and its absence from `config`
  reinforces the run-state↔config boundary.
- `action` never appears in `config` — it is a transient command verb, not
  persisted.

- **vs. a flat body / no nesting** — rejected: nesting lets a client tell live
  run-state apart from persisted configuration at a glance, and isolates the
  back-compat top-level summary from the new mirror.

### 4. Icons stay HA-unprojected (a deliberate non-gap)

After this change the four `icon_<state>` names are readable over **HTTP**
(`config.icon_*`) and **MQTT** (retained `{prefix}/timer/icons`). They remain the
one capability with **no HA read path** — documented, not closed: they already
have two read paths, the dedicated MQTT topic is the intentional icon-readback
design, HA cannot usefully render an AWTRIX icon file, and icons sit outside the
settings/attribute-group tables.

## Consequences

- An HTTP-only integrator can read the device's full live Timer configuration and
  build **read-after-write** checks (`POST` a knob, `GET` it back) over HTTP alone.
- Existing HTTP consumers are unaffected: the 8 top-level fields are byte-compatible.
- The HA observation surface is unchanged: attribute groups and
  `{prefix}/timer/icons` behave exactly as before (ADR-0014 unchanged).
- No new MQTT config-read topic: MQTT config-read already rides the HA discovery
  layer (ADR-0014); HTTP was the sole carrier missing config-read.
- `sync_follow`/`sync_targets` are observable here but stay neither HA-writable nor
  propagated (ADR-0006 unchanged).
- The working JSON document for `getStateJson()` moved from the small fixed buffer
  to the shared command-size constant the snapshot/broadcast paths already use.

See also: [ADR-0004](0004-expanded-timer-config-surface.md) (observation-only
behaviour knobs), [ADR-0014](0014-timer-ha-attribute-projection.md) (the HA
attribute-group projection this reaches read parity with).
