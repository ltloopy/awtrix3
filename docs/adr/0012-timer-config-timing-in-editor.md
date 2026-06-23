# Move config-mode timing into `TimerConfigEditor::tick(nowMs, buttonState)`

Status: accepted

## Context

[ADR-0011](0011-timer-config-editor-extraction.md) extracted the Timer-app's on-device duration
editor into a display-free `TimerConfigEditor` value object, but **deliberately left** the
config-mode *timing* inside `TimerManager::tick()`:

- the button **hold-to-repeat** (a 500 ms long-press threshold, then a 250 ms repeat cadence),
  driven by reading `PeripheryManager.buttonL/R` directly, and
- the **30 s no-input auto-apply** timeout, driven by `millis()` and a `configLastInputMs` member.

That kept the editor pure, but it also meant the cadence and the timeout could only be exercised
on real hardware with a real clock — the deterministic timing was untestable. The branch's
trajectory (#18–#22, ADR-0011) has been to make Timer pieces explicit and unit-testable in
isolation; the config-mode timing was the remaining device-coupled seam.

## Decision

### `TimerConfigEditor::tick(nowMs, buttonState)` owns the config-mode timing

The editor gains a tick that receives the current time and **injected** button state:

```cpp
struct ButtonState { bool leftPressed = false; bool rightPressed = false; };
enum class TickOutcome : uint8_t { Active, TimedOut };

void       noteInput(unsigned long nowMs);            // reset the no-input idle clock
TickOutcome tick(unsigned long nowMs, ButtonState);    // hold-to-repeat + 30 s timeout
```

The editor owns both the 500 ms long-press threshold and the 250 ms cadence, plus the
`TIMER_CONFIG_TIMEOUT`-second idle window (read from `Globals.h`, exactly as it already reads
`TIMER_MAX_DURATION` for the cap math). `tick()` checks the timeout first, then runs each button's
hold-to-repeat. An auto-repeat counts as input (it resets the idle clock), so holding a button
never times out — matching the prior behaviour.

### Button state is injected as raw pressed booleans; the editor derives held time

`ButtonState` carries only `isPressed()` reads. The editor derives held duration itself, by
recording a per-button press-start timestamp on the first tick it observes the button pressed and
comparing against `nowMs`. This is what makes the cadence testable purely by advancing `nowMs`,
and it drops the editor's dependence on `EasyButton::pressedFor` (the third-party button's own
hold tracking). The alternatives — injecting a pre-computed held-duration (production `EasyButton`
exposes no raw held-ms accessor) or a pre-thresholded "held past long-press" boolean (keeps the
threshold in `TimerManager`, contradicting the goal) — were rejected.

### `TimerManager::tick()` delegates while editing; the run-state machine is untouched

```cpp
if (configEditor.isActive()) {
    EasyButton *bL = PeripheryManager.buttonL, *bR = PeripheryManager.buttonR;
    TimerConfigEditor::ButtonState b{ bL && bL->isPressed(), bR && bR->isPressed() };
    if (configEditor.tick(millis(), b) == TimerConfigEditor::TickOutcome::TimedOut)
        exitConfigMode();   // commit through setDuration + drain + broadcast
    return;
}
// ... run-state machine (countdown / finish / auto-clear / re-alert) unchanged ...
```

`exitConfigMode()` is unchanged: a `TimedOut` signal commits the edited duration through the same
`setDuration` + drain + broadcast path as an explicit long-press exit. The editor stays active on
`TimedOut` (it does not deactivate itself) so `exitConfigMode()`'s `exit()` does the recompose.

The thin forwarders carry input timestamps to the editor: `enterConfigMode` seeds it via
`configEditor.noteInput(millis())`, and `configAdjust`/`configCycleField` `noteInput(millis())`
after the delegated value mutation — so a single press via `PeripheryManager` resets the idle
timer exactly as the old `configLastInputMs = millis()` did.

## Consequences

- **Supersedes** the *"the 30 s timeout and hold-to-repeat stay in `TimerManager::tick()`"* decision
  of [ADR-0011](0011-timer-config-editor-extraction.md). The editor is no longer time-free/
  button-free; it is time- and button-**injected** (which preserves testability — the injection is
  the test seam).
- `TimerManager` sheds the `configLastInputMs`/`configRepeatLeftMs`/`configRepeatRightMs` members
  and the `kBtnLongPressMs`/`kBtnRepeatMs` constants; its `tick()` config block collapses to the
  delegation above. The pure value methods (`enter`/`exit`/`cycleField`/`adjust`) are unchanged, so
  `test_CE1`–`test_CE7` stay green.
- New isolated timing tests drive a bare `TimerConfigEditor`: `test_CE8` (30 s timeout →
  `TimedOut`), `test_CE9` (held button auto-repeats at the 500 ms-then-250 ms cadence), `test_CE10`
  (left decrements; release re-waits the threshold), `test_CE11` (a held button never times out).
  `TimerManager`-level `test_U52` proves `tick()` delegates the timeout (auto-applies + exits to
  Idle, committing via `setDuration`) and `test_U53` proves it runs the run-state machine otherwise.
- All run-state/config/sync/view suites (`test_U1`–`test_U8`, `test_U18`/`U19`/`U22`/`U27`/`U31`/
  `U51`, …) stay green, proving the run-state `tick()` path is unchanged.
- Behaviourally identical on device: a sub-tick difference between `EasyButton`'s debounced
  press-start and the editor's first-observed-press is immaterial because `tick()` runs every loop.
- No NVS format change, no new HA entities, no MQTT/HTTP surface change. Public `TimerManager` API
  (forwarders + getters) is unchanged. No `platformio.ini` change (`TimerConfigEditor.cpp` is
  already in the `[env:native]` `build_src_filter`).

## Addendum — the auto-apply timeout is removed; hold-to-repeat survives (PRD #83 / issue #88)

The two responsibilities this ADR moved into `editor.tick()` are now split: **hold-to-repeat
is retained**, the **30 s no-input auto-apply timeout is removed**. The editor's only host is
now the TIMER menu's `DURATION` leaf (#86), which is timeout-free, so the idle clock
(`lastInputMs_`/`noteInput`) and the `TickOutcome::TimedOut` signal are gone; `tick()` now
returns `void` and only drives the per-button repeat cadence. The `TIMER_CONFIG_TIMEOUT`
global it read is removed (#88). See ADR-0016 for the drill-in menu that replaced the
legacy Timer-app config mode.
