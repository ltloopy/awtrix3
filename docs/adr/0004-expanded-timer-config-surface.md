# Expanded Timer config surface: behavior parameters + new options on `{prefix}/timer`

Status: accepted

## Context

After ADR-0003 landed the three on-device-tunable "tuning knobs"
(`TIMER_FINISHED_HOLD` / `TIMER_REALERT_INTERVAL` / `TIMER_COUNTDOWN_SECONDS`)
on the `{prefix}/timer` command surface, an audit of the Timer's full config
surface (CONTEXT.md → "Timer behavior parameters") revealed two unresolved
gaps:

1. **Three globals were dev.json-only.** `TIMER_MAX_DURATION`,
   `TIMER_PUBLISH_INTERVAL`, `TIMER_CONFIG_TIMEOUT` were declared in
   [src/Globals.h](../../src/Globals.h), loaded once at boot from
   [src/Globals.cpp::loadDevSettings](../../src/Globals.cpp), and never
   touched again — no NVS persistence, no MQTT/HTTP exposure, no HA.
2. **A few user-visible behaviors had no knob at all.** Melody filenames
   were hardcoded literals at
   [src/TimerManager.cpp::loadMelodiesCached](../../src/TimerManager.cpp);
   the progress bar was always-on at a fixed color
   ([src/Apps.cpp::TimerApp](../../src/Apps.cpp)).

A grilling session (see `/grill-with-docs` transcript captured in the plan
file) walked the design tree decision-by-decision. This ADR records the
load-bearing choices.

## Decision

### Promotion of three "behavior parameters" (not "tuning knobs")

`TIMER_MAX_DURATION`, `TIMER_PUBLISH_INTERVAL`, and
`TIMER_CONFIG_TIMEOUT` are now editable via:

- `dev.json` (renamed to `timer_max_duration` /
  `timer_remaining_publish_interval` / `timer_app_config_timeout`)
- `POST /api/timer` and `{prefix}/timer` MQTT (`max_duration` /
  `remaining_publish_interval` / `app_config_timeout`)
- NVS namespace `"awtrix"` (keys `TMAXD` / `TPUBI` / `TCFGT`)

They are **not** "tuning knobs" in the ADR-0003 sense. The CONTEXT.md
glossary calls them out as **three distinct categories** — input bound,
output cadence, UI timing — to prevent future readers
from lumping them and to make scope decisions for follow-up changes (e.g.
on-device menu coverage) easier to reason about. ADR-0003's "tuning
knobs" term stays scoped to FINISHED_HOLD / REALERT_INTERVAL /
COUNTDOWN_SECONDS, the three on-device-editable knobs.

### Principled bounds, not paternalistic

| Parameter | Range | Justification |
|---|---|---|
| `max_duration` | 1..604800 (1 s .. 7 days) | Floor matches the `duration` accept-floor. Ceiling stops at a meaningful safety horizon (timer state doesn't survive reboot — anything past 7 days is power-loss territory). |
| `remaining_publish_interval` | 1..60 (s) | Floor `1` prevents MQTT flooding. Ceiling `60` because beyond a minute the HA `timer_rem` sensor looks stuck. |
| `app_config_timeout` | 5..300 (s) | Floor `5` because below is hostile UX. Ceiling `300` matches the existing ADR-0003 tuning-knob ceiling. |

We chose principled bounds (each tied to a physical/UX constraint of its
specific dimension) over paternalistic ones (uniform floors/ceilings to
"protect" users). Users who put `max_duration = 30` for a kitchen-timer
appliance get what they asked for; that's not a footgun.

### Four new options on the same surface, persisted to "awtrix"

`TIMER_MELODY_TICK`, `TIMER_MELODY_END`, `TIMER_BAR_ENABLED`,
`TIMER_BAR_COLOR` reach the timer via:

- `dev.json` (`timer_melody_tick` / `timer_melody_end` /
  `timer_bar_enabled` / `timer_bar_color`)
- `POST /api/timer` and `{prefix}/timer` MQTT (`melody_tick` /
  `melody_end` / `bar_enabled` / `bar_color`)
- NVS namespace `"awtrix"` (keys `TMTICK` / `TMEND` / `TBAREN` /
  `TBARC`)

**No HA entities** for any of the four. The HA contract today is the
8-entity surface defined in [src/TimerHa.h](../../src/TimerHa.h); none of
these four warrant a new entity:

- Color/bar are display preferences, edited rarely; HA-side automation
  has no compelling use for them.
- Melody filenames need string editing on a string-aware control —
  better served by the web file manager + a quick MQTT publish than by a
  text entity that has no validation.

This matches the deferral choice from ADR-0003 (no HA `number` entities
for the tuning knobs) — same philosophy: keep HA's Timer surface lean
and oriented to what a HA automation actually does (start/pause/reset,
set duration, react to state).

### Persistence: all eight new keys live in NVS `"awtrix"`

ADR-0003 Decision 3 already established that *"loadSettings() calls
loadDevSettings() last, so a dev.json key naturally wins after the NVS
read"*. We re-use that pattern here, including for the four new options.

The alternative — persisting to the `"timer"` namespace owned by
`TimerManager` — was considered and rejected. The reason is asymmetric
sentinel handling: melody strings could use the same empty-string
sentinel that icons use ([src/TimerManager.cpp::setup](../../src/TimerManager.cpp)),
but the `bar_enabled` bool has no natural "not set" value — both `true`
and `false` are valid user choices. Mixing two persistence policies in
one feature would force every reader to remember "where does this
specific key live?" The single-namespace choice removes that cognitive
load.

### Validation reuses ADR-0001's atomic-reject contract

Every new key is validated **before** state changes (per
[ADR-0001](0001-timer-command-validation-parity.md)). Malformed-or-out-of-range
rejects the whole command atomically; nothing is applied. Per-key checks:

- `max_duration` / `remaining_publish_interval` /
  `app_config_timeout`: integer in range; mirror the existing
  `finished_hold` / `realert_interval` / `countdown_seconds` validation
  shape exactly.
- `melody_tick` / `melody_end`: same validation as icon names
  (alphanumeric + `_` + `-`, ≤32 chars, or empty). Empty resets to
  canonical default (`"timer_tick"` / `"timer_end"`) — **not** a silence
  sentinel. For silence, use `buzzer: "off"`.
- `bar_enabled`: strict bool (`is<bool>()`); non-bool JSON values
  (string `"yes"`, integer `1`, etc.) are rejected.
- `bar_color`: numeric `0..0xFFFFFF`, or a hex string `"#RRGGBB"` /
  `"RRGGBB"`. Mirrors the project's `getColorFromJsonVariant` shape
  ([src/Functions.cpp](../../src/Functions.cpp)).

### Dev.json key renames (breaking on this branch)

Two pre-existing dev.json keys (`timer_publish_interval`,
`timer_config_timeout`) are **renamed** to match the new wire-keys.
This is a breaking change for testers of the `feat-timer-standalone`
branch, but the Timer feature is not yet in `main`, so the audience is
zero external users. Taking the break now is cheaper than carrying
asymmetric naming forever or maintaining transitional fallback code.

## Out of scope

- No on-device TIMER menu slot for any of the eight new keys. The
  five-slot menu (BUZZER / CDOWN / FINISH / CLEAR / ALERT) stays
  focused on expiry-behavior tuning. `bar_enabled` is a binary that
  would fit on-device, but expanding the menu to six slots was
  deferred — revisit if real demand emerges.
- No HA entities (see above).
- No retained `{prefix}/timer/config` mirror topic. The existing
  `{prefix}/timer/icons` retained mirror precedent applies only to
  icon state. Other new settings change rarely enough that on-boot
  publishing is sufficient; subscribers wanting current state can
  call `/api/timer` (read endpoint to be added separately if needed).

## Consequences

- `docs/timer.md`, `docs/api.md`, and `docs/dev.md` enumerate all
  seven new keys with ranges. The Persistence table in
  `docs/timer.md` records the seven new `"awtrix"` NVS keys.
- `CONTEXT.md` gains a "Timer behavior parameters" section that
  forbids calling these "tuning knobs" (term reserved for ADR-0003).
- The NVS `"awtrix"` namespace now holds 11 Timer-related keys
  (TFHOLD / TRALERT / TCDOWN + the seven added here + the master
  enable TIMER and TIMERPREV). That's still well under the namespace's
  practical limit.
- Future "add another timer config knob" PRs follow the well-trodden
  pattern: declare global → dev.json read → loadSettings/saveSettings
  → parseCommand validate+apply → docs in three places. No new
  decision-makings required.
