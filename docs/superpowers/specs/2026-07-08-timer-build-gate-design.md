# Timer compile-time build gate — design

**Date:** 2026-07-08
**Status:** approved in session, pending spec review
**Branches:** `timer-upstream` (upstream PR) and the fork timer chain (stacked PR)

## Goal

Allow the native Timer feature to be excluded at compile time so flash- and
RAM-constrained builds can drop it entirely. Default is **ON**: a stock build
(no extra flags) is unchanged. Passing `-DAWTRIX_DISABLE_TIMER` removes all
timer code, data, and globals from the binary.

Motivation: make the upstream Blueforcer PR easier to accept (the ulanzi app
slot is near capacity) and give any user an opt-out that reclaims the space.

## Non-goals

- No change to the existing **runtime** enable (`SHOW_TIMER` setting); that
  keeps working in default builds.
- No sub-flags (e.g. "timer without sync" or "timer without HA"). One flag
  gates the whole stack. Revisit only if upstream asks.
- No separate release binary / PlatformIO env. Anyone who wants the gated
  build adds the flag themselves (or via `PLATFORMIO_BUILD_FLAGS`).

## Mechanism

One opt-out define, used raw at every guard site:

```cpp
#ifndef AWTRIX_DISABLE_TIMER
...timer code...
#endif
```

**Revised during planning** (was: a `Globals.h`-normalized
`AWTRIX_TIMER_ENABLED` macro). Two reasons: (1) several shared files include
timer headers *before* `Globals.h`, and an undefined macro in `#if` silently
evaluates to 0 — a default build would quietly lose the timer if any file got
the include order wrong; the raw flag comes from the compiler command line so
it is correct in every file with zero ordering constraints. (2) It matches
the codebase's existing `#ifndef awtrix2_upgrade` convention exactly.
`Globals.h` needs no changes at all.

### Timer-owned files — whole-body guards

Each of these `.cpp` files wraps its entire contents (first line to last,
includes and all) in `#ifndef AWTRIX_DISABLE_TIMER … #endif`:

`TimerCommand.cpp`, `TimerConfigEditor.cpp`, `TimerEnums.cpp`, `TimerHa.cpp`,
`TimerHaHost.cpp`, `TimerManager.cpp`, `TimerMenu.cpp`, `TimerMenuNav.cpp`,
`TimerRuntime.cpp`, `TimerSettings.cpp`, `TimerSettingsApply.cpp`,
`TimerView.cpp`, `SyncEnvelope.cpp`, `SyncSeenCache.cpp`,
`SyncTargetsDebounce.cpp`, `PeerRegistry.cpp`.

Headers stay unguarded **except** any that define non-trivial globals or
would otherwise emit code when included. Shared files stop including timer
headers when the gate is off (guarded includes), so unguarded headers are
inert.

### Shared files — call-site guards

| File | What gets guarded |
|---|---|
| `main.cpp` | includes; `TimerManager.setup()`, `TimerHaHost.reconcile()`, `TimerManager.tick()`, `tickPresence()`, `TimerHaHost.refreshTargets()` |
| `Apps.cpp` / `Apps.h` | includes; `TimerApp()` painter, `drawTimerIcon()`, the timer geometry constants; `TimerApp` declaration in the header |
| `DisplayManager.cpp` | includes; `updateApp("Timer", …)` registration; the `SHOW_TIMER` change-handling block (`onShowTimerChange`, `TimerHaHost.remove()/enable()`) |
| `MenuManager.cpp` / `.h` | includes; `TimerConfigMenu` enum value and every `case TimerConfigMenu:` block; `timerNav`/`timerDurationEditor` globals; `commitTimerMenu()`; `openTimerMenuFromApp()`; the `SHOW_TIMER` toggle handling inside the settings menu |
| `MQTTManager.cpp` / `.h` | includes; `TimerManager.parseCommand()` topic handler; the three `TimerHaHost.tryHandle*` delegations; `TimerHaHost.onConnected()`/`setup()`; `publishTimerWire()`, `timerWireTopic()`, `timerWireAttrTopic()` (definitions and header declarations) |
| `PeripheryManager.cpp` | includes; the button-handler blocks that drive `TimerManager.runStateAction()` and `MenuManager.openTimerMenuFromApp()` |
| `ServerManager.cpp` / `.h` | includes; the `/api/timer` HTTP endpoints; `kTimerSyncPort`, the `syncUdp` socket + receive buffer and its `loop()` polling; `sendTimerSync()` (definition and declaration) |
| `Globals.cpp` | `TimerSettings.h` include; the `timerSettingsLoadDevJson()` dev.json call; the `timerSettingsLoadNvs()` / `timerSettingsSaveNvs()` NVS calls in loadSettings()/saveSettings() (all three defined in the gated `TimerSettings.cpp`; the NVS pair was found by the first disabled-build link, not the original grep — lowercase names) |

### Deliberately left ungated

`SHOW_TIMER`, `SHOW_TIMER_HA_PREV`, `TIMER_ICON_ENABLED` in
`Globals.h`/`Globals.cpp` (three bools plus their settings persistence
lines). Cost is a few bytes; guarding them would spread `#if` noise into the
settings load/save code for no meaningful saving. A disabled build simply
ignores the stored values.

## RAM/flash effects when disabled

- Flash: no timer translation units linked — code, string literals, HA
  discovery payload builders, menu tables all gone.
- RAM: timer singletons (`TimerManager`, `TimerHaHost`, `PeerRegistry`,
  `SyncSeenCache`, menu nav/editor objects), the `syncUdp` `WiFiUDP` object
  and sync receive buffer, and the runtime heap for the ten Timer HA carrier
  entities and their MQTT subscriptions.

Measured deltas (flash + RAM, `pio run -e ulanzi` with vs. without the flag)
are reported in the PR description after implementation.

## Verification

- `pio run -e ulanzi` **with and without** `-DAWTRIX_DISABLE_TIMER`, on both
  branches. (Per project gotcha: native env never compiles real
  `MQTTManager.cpp`, so the device build is mandatory.)
- Fork only: full native test suite (`pio test -e native -f test_timer` etc.)
  stays green — the default path is unchanged, tests always build with the
  timer enabled.
- Fork CI (`test.yml`): add one device build step with
  `PLATFORMIO_BUILD_FLAGS=-DAWTRIX_DISABLE_TIMER` so the gate cannot rot.
- Record flash/RAM figures for both variants.

## Docs

One short paragraph in the timer docs (`docs/` timer page added by the
upstream PR): flag name, what it removes, default-ON statement.

## Branch strategy

1. Implement on `timer-upstream`; fold into the existing feature commit or
   add as a focused commit (decide at implementation; a separate
   `feat: compile-time gate` commit keeps review easy and can be squashed
   later).
2. Port to the fork as a stacked PR on the timer chain, per the usual
   issue/PR flow, plus the CI addition (CI change is fork-only).

**Note:** this spec file itself must NOT be committed on `timer-upstream`
(it would land in the upstream PR). It gets committed with the fork-side PR.
