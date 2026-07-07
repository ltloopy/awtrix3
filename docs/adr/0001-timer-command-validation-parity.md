# Timer command validation: reject invalid input on every control surface

Status: accepted

## Context

The Timer is driven by three **control surfaces** that must stay in parity
(see `CLAUDE.md`): the on-device config buttons, `POST /api/timer`, and the
`{prefix}/timer` MQTT topic — plus the Home Assistant `timer_dur` text entity,
which is the discovery-layer face of the MQTT surface. Historically a duration
outside `1 .. TIMER_MAX_DURATION` was silently **clamped** to the limit, and
`POST /api/timer` always returned `200 OK`.

Clamping silently rewrites the user's intent — `"30:00:00"` quietly becomes
`24:00:00` — and an unconditional `200` hides the fact that a command did
nothing. The surfaces also disagreed on what counted as an error.

## Decision

Every control surface applies **one** validation policy: an invalid command —
**malformed OR out-of-range** — is **rejected atomically**; nothing is applied.
"Parity" is on *handling*, not on wire feedback (a fire-and-forget transport
can't report what a request/response one can):

- `parseCommand` is always strict and returns `TimerCmdResult { Ok, BadJson, BadField, Disabled }`. It validates the whole payload **before** mutating any state (including before discarding an in-progress on-device edit), so a rejected command changes nothing.
- `POST /api/timer` maps the result: `200 OK` / `400 ErrorParsingJson` / `400 InvalidValue` / `409 TimerDisabled`. **`400`, not the legacy `500`** used elsewhere in the firmware, because malformed input is a client error.
- The `{prefix}/timer` MQTT topic is fire-and-forget: it runs the same validation but ignores the result (silent reject). The retained `timer_dur` state, unchanged, is the only confirmation.
- The Home Assistant `timer_dur` text entity reverts the box to the previous valid time (it echoes the canonical value; the entity is non-optimistic).
- The on-device config buttons cannot produce invalid input — the wheels are capped at the valid range — so they satisfy parity by *prevention*.

`setDuration`'s clamp is kept only as an internal backstop (it still guards the
config editor); external input never reaches it out of range.

**Addendum — multi-key validation ordering.** Range-defining keys in the same
payload are validated first, into effective-bound locals; dependent keys are
then validated against those effective bounds. The whole payload still rejects
atomically if any individual key is invalid against its own absolute bounds, or
if a dependent key is out of range against the effective bound. Today the only
such pair is `max_duration` → `duration`: a payload like
`{"max_duration": 10000, "duration": 9000}` against a current ceiling of `5000`
is accepted as one atomic call, because `duration` sees the in-payload ceiling
of `10000`, not the pre-payload `5000`. Future range-defining keys follow the
same pattern.

## Consequences

- Revokes the previously documented "clamped on out-of-range" behavior. Clients that relied on clamping (e.g. sending `99999` expecting the max) must now send in-range values, or they get a `400` / silent no-op.
- The MQTT surface cannot return an error code; parity there is best-effort (silent reject + retained state), by transport necessity.
- A single, predictable validation contract across surfaces, and honest HTTP status codes for automations.
