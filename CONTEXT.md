# AWTRIX3 Firmware

Firmware for the Ulanzi TC001 / AWTRIX smart pixel clock. This glossary records terms whose meaning is non-obvious or overloaded across the codebase.

## Language

**Timer Duration**:
The configured length of a countdown for the built-in Timer app — how long it counts down from. Externally expressed as a trimmed clock-style value (e.g. `3:00`, `1:01:01` — hours dropped when zero, leading field unpadded); a bare number is interpreted as seconds.
_Avoid_: just "duration" (overloaded — see Flagged ambiguities), "time", "length".

**Timer Remaining**:
The seconds still left on a running/paused countdown. Distinct from **Timer Duration** (the starting length).
_Avoid_: "time left" when precision matters.

**Timer Display String**:
The compact on-screen rendering of **Timer Duration** / **Timer Remaining**, fitted to the 24px text region: `M:SS` under an hour, `H:MM` for 1–9 h (seconds dropped to fit), `HH:MM` at 10 h+. Produced by `formatTimerDisplay` (`src/TimerView`). Distinct from the **Timer wire string** (`formatHMS`, `H:MM:SS`), which always carries seconds and is the external string contract (`docs/timer.md`).
_Avoid_: conflating the display string with the wire string — they intentionally differ past 1 h (`3661` → `1:01` on screen, `1:01:01` on the wire).

**Timer View**:
The pure, per-frame description of what the Timer app should draw — screen (Config / Finished / Time), the **Timer Display String** and its centering region, progress-bar length/anchor, and the config underline — computed by `TimerView::compute(now)` (`src/TimerView`) from Timer state with no display, font, or filesystem dependency. The `TimerApp` renderer is its painter: it owns only font-metric centering and the icon-file lookup. Being display-free is what makes the bar geometry, blink cadence, and display-string selection host-testable (tests D1–D6).
_Avoid_: putting render-decision logic back in `TimerApp` — that re-creates the untestable tangle the view was extracted to remove.

**Finished Mode**:
What the timer does after it reaches zero: auto-clear, hold, or re-alert.

**Buzzer Mode**:
Whether and when the timer makes sound: off, end-only, or countdown beeps.

**Control Surface**:
One of the ways a user/integration commands the Timer: the on-device config buttons, the `POST /api/timer` HTTP endpoint, and the `{prefix}/timer` MQTT topic. (The Home Assistant entities are the discovery-layer face of the MQTT surface.)

**Timer HA Presence**:
The set of Home Assistant entities that project the Timer over the MQTT discovery layer — duration, remaining, state, buzzer, finished, start, pause, reset — defined by a single descriptor table (`TimerHaDescriptor` / `TIMER_HA_DESCRIPTORS` in `src/TimerHa.h`). It is the discovery-layer face of the MQTT **Control Surface**: `MQTTManager` reads the same table to both build the entities and prune them (empty retained payloads) when `SHOW_TIMER` is off, so setup and teardown cannot drift.
_Avoid_: treating the HA entities as a separate control surface — they are a projection of the MQTT one.

**Parity** (of control surfaces):
The principle that every **Control Surface** accepts the same operations *and* treats an invalid command the same way — none of them apply it. Surfaces differ only in how their transport can report the outcome, not in what they do.
_Avoid_: reading "parity" as identical wire responses; a fire-and-forget transport simply can't report what a request/response one can.

## Flagged ambiguities

**"duration" is overloaded** across the codebase and must always be qualified:
- **Timer Duration** — the Timer app's countdown length (the subject of the timer feature).
- **App / Notification Duration** — how long an app screen or notification stays on the display (`Apps.h`, `Overlays.h`; stored in milliseconds, set in seconds).
- **Note Duration** — a musical note's length inside RTTTL melody parsing (`MelodyPlayer/`).

When writing or discussing code, prefer the qualified term. Unqualified "duration" should only appear where the surrounding scope makes the sense unambiguous.
