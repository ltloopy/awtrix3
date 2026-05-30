# TIMER top menu and command-surface exposure for tuning knobs and modes

Status: accepted

## Context

The Timer feature exposes two enum **modes** (`buzzerMode`, `finishedMode`) and three numeric
**per-mode timing knobs** (`TIMER_FINISHED_HOLD`, `TIMER_REALERT_INTERVAL`,
`TIMER_COUNTDOWN_SECONDS`). The modes were already editable from MQTT, the HA `timer_buz` /
`timer_fin` selects, and `dev.json`. The timing knobs were configurable only via `dev.json` at boot
— there was no way to adjust them at the device.

The Timer's existing **on-device** control surface (per ADR-0001) is the Timer-app's own config
mode, but that one only edits **duration**. We added a new top-level `TIMER` entry to the global
`MenuManager` menu with a five-item submenu (`BUZZER → CDOWN → FINISH → CLEAR → ALERT`), and at
the same time exposed the three tuning knobs on the same MQTT/HTTP command surface that already
carried the two modes — so all five settings reach every relevant control surface.

## Decision

1. **The `TIMER` global menu is a new on-device control surface** for buzzer mode, finished mode,
   and the three per-mode timing knobs. It exists alongside — not in place of — the Timer-app
   config mode (which continues to own duration editing). On-device timer configuration is now
   split deliberately between the two surfaces:
   - *Timer-app config mode*: duration (HH/MM/SS, auto-repeat, 30 s timeout-apply).
   - *TIMER global menu*: modes + tuning knobs (discrete left/right, no auto-repeat, long-press save).

2. **Parity with MQTT/HA is satisfied automatically** per ADR-0001's principles:
   - Mode changes route through the canonical `TimerManager.setBuzzerMode()` /
     `setFinishedMode()` setters ([TimerManager.cpp:453-466](../../src/TimerManager.cpp#L453)),
     the same path MQTT command handling uses ([MQTTManager.cpp:359,369](../../src/MQTTManager.cpp#L359)).
     Each press persists to the `"timer"` NVS namespace AND publishes to MQTT, which fans out to HA.
   - The three tuning knobs are now also exposed on the timer command surface as
     `finished_hold` / `realert_interval` / `countdown_seconds` keys on `{prefix}/timer` MQTT and
     `POST /api/timer` (handled by `TimerManager.parseCommand`). They follow the same atomic
     validation contract as `duration`/`buzzer`/`finished` — out-of-range is rejected, not
     clamped (ranges: `finished_hold` 1–300, `realert_interval` 5–300, `countdown_seconds` 0–30).
     On the on-device menu the same ranges are enforced by clamping the wheel — that path
     satisfies parity by *prevention* (it cannot produce invalid input), exactly as ADR-0001
     specifies for the on-device surface.

3. **Persistence is split between two NVS namespaces, on purpose.** Modes stay in the existing
   `"timer"` namespace owned by `TimerManager`. The tuning knobs go to `"awtrix"` via the existing
   `saveSettings()`/`loadSettings()`. The reason is the documented *"dev.json overrides NVS on
   every boot"* contract: `loadSettings()` calls `loadDevSettings()` *last*
   ([Globals.cpp:348](../../src/Globals.cpp#L348)), so a `dev.json` key naturally wins after the
   NVS read with zero extra code. Putting the tuning knobs in the `"timer"` namespace would have
   required replaying the dev.json value *after* the NVS read (the icons do this at
   [TimerManager.cpp:50-53](../../src/TimerManager.cpp#L50) using empty-string as the
   "not set in dev.json" sentinel) — a non-trivial pattern with no equivalent sentinel for numeric
   values.

4. **Mode changes apply immediately on each left/right press; tuning-knob changes commit on
   long-press save.** Mode changes are observable events (they affect HA state and what an active
   timer will do on expiry), and the canonical setter is the right tool — bypassing it to defer
   would require expanding `TimerManager`'s deliberately-minimal public surface. Numeric changes
   are bulk-editable in the menu and reach NVS only on long-press, matching how every other global
   menu setting persists.

## Consequences

- The previously-documented "compile-time defaults overridable via dev.json only" status of the
  three timing knobs is **revoked** — they are now editable from three places: the on-device
  `TIMER` menu, the MQTT `{prefix}/timer` topic, and `POST /api/timer`. All edits persist to NVS
  `"awtrix"`. `dev.json` continues to override on every boot.
- Tuning-knob writes via `parseCommand` go through `saveSettings()` (which writes the full
  `"awtrix"` namespace), not through `TimerManager.persist()`. This keeps `TimerManager`'s
  persistence scoped to its own `"timer"` namespace and is the simplest path that preserves the
  dev.json-override contract — see decision 3 below.
- No new HA entities are added for the tuning knobs. Adding `number` entities for
  `finished_hold`/`realert_interval`/`countdown_seconds` is an obvious next step but was held out
  of scope here.
- Timer-related persistence now spans two NVS namespaces (`"timer"` for runtime config,
  `"awtrix"` for tuning knobs). Readers inspecting the `"timer"` namespace alone won't find the
  tuning keys; the persistence table in [`docs/timer.md`](../timer.md) cross-references both.
- ADR-0001's "the on-device config buttons" wording, originally referring to the Timer-app config
  mode as a single surface, now covers two on-device surfaces. Both still satisfy parity by
  prevention (numeric clamping) or by going through the canonical setters (modes).
- Adjusting buzzer/finished mode on-device generates one MQTT publish per left/right press. This is
  consistent with how MQTT-driven mode changes already behave (the setter always publishes), and is
  bounded by the press cadence — not a flood concern in practice.
