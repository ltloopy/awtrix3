# Extract the Timer-app duration editor into a display-free TimerConfigEditor

Status: accepted

## Context

The Timer-app's on-device duration editor — the HH/MM/SS wheels reached by long-pressing middle
from `Idle` while the Timer app is on screen — lived as six private members and four methods inside
the ~960-line `TimerManager.cpp`:

```cpp
bool          inConfig            = false;
uint8_t       configField         = 0;          // 0=HH, 1=MM, 2=SS
uint8_t       configHH, configMM, configSS;     // edit buffers
unsigned long configLastInputMs;                // 30 s no-input timeout
unsigned long configRepeatLeftMs, configRepeatRightMs;  // hold-to-repeat

void enterConfigMode();   // decompose durationSec -> HH/MM/SS
void exitConfigMode();    // recompose -> setDuration + drain + broadcast
void configCycleField();  // HH -> MM -> SS -> HH
void configAdjust(int);   // cap-aware wrap of the current field
```

This is a cohesive sub-FSM — a field cursor, three edit buffers, and cap-aware adjust math —
entangled in a class that also owns run-state, persistence, MQTT/HA command parsing, multi-device
sync, and icons. The deterministic cap/wrap behaviour (`test_U14..U17`) could only be exercised
by reaching through the `TimerManager` singleton, even though none of it depends on run-state,
persistence, or the display.

This continues the branch's trajectory of making Timer pieces explicit and unit-testable in
isolation: ADR-0007/0009/0010 pulled the config block into descriptor tables, and #21 made
`TimerViewModel::compute()` take an explicit `TimerSnapshot` instead of reaching into `TimerManager`
globals. The duration editor is the next seam.

## Decision

### A display-free `TimerConfigEditor` value object owns the config-mode working state

`TimerConfigEditor` ([src/TimerConfigEditor.h](../../src/TimerConfigEditor.h) /
[TimerConfigEditor.cpp](../../src/TimerConfigEditor.cpp)) owns the field cursor and the three
edit buffers, and the editing logic:

```cpp
void     enter(uint32_t durationSec);   // decompose, clamp HH<=99, field=0, active=true
uint32_t exit();                         // active=false; return hmsToSeconds(HH,MM,SS)
void     cycleField();                   // HH -> MM -> SS -> HH
void     adjust(int delta);              // cap-aware wrap of the current field
bool     isActive() const;
uint8_t  field()/hh()/mm()/ss() const;
```

It is a **pure value object**: duration flows in via `enter()` and back out via `exit()`. It has no
`DisplayManager` dependency and no `TimerManager` back-reference (it reuses the static
`secondsToHMS`/`hmsToSeconds` math helpers, which `TimerManager.h` already documents as "shared by
the MQTT/HA string path and the on-device config editor"). The cap math reads the
`TIMER_MAX_DURATION` global directly, exactly as before.

### Committing stays the caller's choice; `exit()` is the deactivation primitive

`exit()` deactivates and *returns* the edited duration — it does not commit. The caller decides:

- `TimerManager::exitConfigMode()` commits: `setDuration(configEditor.exit())`, then drains
  deferred notifications and broadcasts the new length as run-state.
- The abort paths (`setup()` on boot, and `parseCommand` discarding an in-flight edit when an
  inbound command is accepted) call `configEditor.exit()` and **discard** the return — the edit is
  aborted, not committed.

This keeps the editor free of persistence/broadcast/display concerns while preserving the exact
ADR-0001 atomic-reject behaviour (`test_U22`/`U31`): a rejected command leaves the edit untouched;
an accepted one discards it.

### Why the editor over alternatives

- **vs. the editor holding a `TimerManager&`** and calling `getDuration()`/`setDuration()` itself —
  rejected: it re-couples the editor to the singleton and makes the cap-math tests drag in the whole
  manager. The duration-in / duration-out shape is what makes `test_CE1..CE7` drive the editor with
  a bare local `TimerConfigEditor ed;`, mirroring #21's explicit-snapshot direction.
- **vs. the editor owning the display/commit** — rejected by the issue's display-free requirement;
  the run-state mutation (the enter-time 99h clamp on `durationSec`, the exit-time
  `setDuration`/drain/broadcast) is run-state, not editor state, and stays on `TimerManager`.

### The 30 s timeout and hold-to-repeat stay in `TimerManager::tick()`

The auto-apply-on-idle timeout and the button auto-repeat read `millis()` and
`PeripheryManager.buttonL/R` — device/tick-loop concerns. They stay in `TimerManager`
(`configLastInputMs`, `configRepeatLeftMs/RightMs` remain members; the `tick()` config block is
unchanged) so the editor stays a pure, time-free, button-free value object. The thin forwarders
(`configCycleField`/`configAdjust`) refresh `configLastInputMs = millis()` around the delegated
call, so any input — single press via `PeripheryManager` or auto-repeat via `tick()` — still
resets the idle timer exactly as before.

### Forwarders keep the call sites unchanged

`TimerManager::enterConfigMode/exitConfigMode/configCycleField/configAdjust` and the
`isInConfig`/`getConfigField`/`getConfigHH/MM/SS` getters are retained as thin forwarders, so
`TimerView`, `Apps.cpp`, and `PeripheryManager` are untouched. `TimerSnapshot` (the view's input)
is unchanged.

## Consequences

- `TimerManager` sheds the editor's value logic: `configAdjust`'s cap math and the HH/MM/SS buffers
  move out; `enterConfigMode`/`exitConfigMode`/`configCycleField`/`configAdjust` collapse to
  forwarders. The class keeps run-state mutation and the tick-loop timeout/repeat bookkeeping.
- The deterministic cap/wrap tests are **retargeted** to drive `TimerConfigEditor` directly:
  `test_U14..U17` become `test_CE2..CE5`, plus `test_CE1` (enter/decompose), `test_CE6` (cycleField)
  and `test_CE7` (exit/recompose). They construct a bare `TimerConfigEditor` with no singleton,
  Preferences, or display in scope.
- The `TimerManager`-level integration tests are unchanged and stay green, proving the extraction is
  behaviour-preserving: `test_U18` (exit commit -> `getDuration`), `test_U19` (enter clamps
  `durationSec` to 99h), `test_U22`/`U31` (`parseCommand` aborts an in-flight edit atomically),
  `test_U27` (enter->exit roundtrip preserves the duration), and all run-state/sync/view suites.
- New source files `TimerConfigEditor.{h,cpp}`; `TimerConfigEditor.cpp` is added to the `[env:native]`
  `build_src_filter` in `platformio.ini` (the firmware envs glob `src/` and pick it up automatically).
- No NVS format change, no new HA entities, no MQTT/HTTP surface change. Public `TimerManager` API is
  unchanged (same forwarders + getters); only the implementation moved behind them.
