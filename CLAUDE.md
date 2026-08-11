# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

AWTRIX 3 — open-source ESP32 firmware for the Ulanzi TC001 smart pixel clock (and the AWTRIX 2 mainboard upgrade). Built with PlatformIO + the Arduino framework. The device shows rotating "apps" (pages) and is driven from a smarthome over MQTT/HTTP.

Key concept: a **CustomApp is a display-only page**, not a program. It holds no logic and runs nothing — it renders content pushed from an external system (Home Assistant, NodeRed, etc.) via the MQTT/HTTP CustomApp API. All app logic lives in the external system. Built-in *native* apps (time, date, temp, humidity, battery, the Timer app) are the exception — those run on-device.

## Build / flash / test

PlatformIO drives everything. Three environments in `platformio.ini`:

| Env | Target | Use |
| --- | --- | --- |
| `ulanzi` (default) | esp32dev | Production build for the TC001 |
| `awtrix2_upgrade` | wemos_d1_mini32 | AWTRIX 2 mainboard upgrade |
| `native` | host | Off-device unit tests (no hardware) |

```bash
pio run                          # build default (ulanzi)
pio run -e awtrix2_upgrade       # build the other firmware target
pio run -t upload                # build + flash over USB
pio device monitor               # serial monitor @115200 (esp32 exception decoder on)

pio test -e native               # host unit tests (TimerManager)
pio test -e native -f test_timer # run a single test folder
```

There is no separate lint step; firmware builds with `-Os -fno-exceptions`.

## Two-tier testing

**Native unit tests** (`test/test_timer/`, run via `pio test -e native`) — host-compiled, no device. The trick to understand before touching the test build: the `native` env compiles *only* `src/TimerManager.cpp` from production sources (`build_src_filter = +<TimerManager.cpp>`). Every dependency it touches — `MQTTManager`, `PeripheryManager`, `DisplayManager`, `Preferences`, `LittleFS`, `Overlays` — is replaced by a **stub header in `tests/stubs/`** that wins via include-path priority (`-I tests/stubs`). `ArduinoFake` supplies the `Arduino.h` surface (`String`, `millis`, …). `fixture.h` wires recording/stateful mocks and a virtual clock (`fixture::advance(ms)` instead of real time). So: to unit-test another manager on the host, add it to the src filter and stub *its* dependencies — don't expect the whole firmware to link.

**E2E tests** (`tests/e2e/`, pytest) — maintainer-only, run against a *real flashed device* + an MQTT broker (+ optional Home Assistant). **Not in CI.** See `tests/e2e/README.md`. The authoritative scenario list lives in `TIMER_TEST_PLAN.md` (`EDGE-*`, `HW-*`, `MQTT-*` sections).

## Architecture: the manager singleton pattern

The firmware is a set of manager singletons. Each is declared as a class `XManager_` with a private constructor and a `static getInstance()`, exposed through a global reference:

```cpp
extern MQTTManager_ &MQTTManager;   // in the header
```

`main.cpp` is the whole control flow. `setup()` initializes managers in order; `loop()` calls each manager's `tick()` every cycle (cooperative, non-blocking — no RTOS task per manager except the boot animation). When adding cross-manager behavior, the entry points are these `setup()`/`tick()` methods, not constructors.

Core managers (all in `src/`):
- **DisplayManager** — the LED matrix, app rotation loop, rendering (`MatrixDisplayUi`), Art-Net.
- **ServerManager** — WiFi, web UI / HTTP API, file browser (LittleFS).
- **MQTTManager** — MQTT (PubSubClient), Home Assistant discovery, stats publishing.
- **PeripheryManager** — buttons, buzzer, light/temp/humidity sensors.
- **TimerManager** — the native Timer app (state machine + HA entities); see below.
- **PowerManager**, **MenuManager**, **UpdateManager**, **Games/GameManager**.

`Globals.h` is the global settings/state surface: every tunable is an `extern` here, loaded/saved by `loadSettings()`/`saveSettings()` (NVS via `Preferences`). Boot-time-only dev tweaks come from a `dev.json` on the device filesystem — see `docs/dev.md` for the full key list.

`Apps.h`/`Apps.cpp` and `Overlays.h`/`Overlays.cpp` define the native app renderers and overlay effects. `Dictionary.h`/`.cpp` holds shared string/key constants.

## Timer feature (active work — branch `feat-timer-app`)

The Timer is a native countdown app with a `TimerState` machine (Idle/Running/Paused/Finished) plus `BuzzerMode` and `FinishedMode` enums (`TimerManager.h`). It's controllable three ways that must stay in parity: on-device buttons (config mode), `POST /api/timer`, and the `{prefix}/timer` MQTT topic. It publishes ~8 Home Assistant entities via discovery, gated by the `SHOW_TIMER` master switch — turning it off publishes empty retained payloads so HA prunes the entities.

Reference docs: `docs/timer.md` (string contract for durations, `parseHMS`/`formatHMS`) and the `timer_*` keys in `docs/dev.md`.

## Terminology — read CONTEXT.md

`CONTEXT.md` is the project glossary and resolves a real ambiguity: **"duration" is overloaded**. Always qualify it:
- **Timer Duration** — the Timer app's countdown length.
- **App / Notification Duration** — how long a page/notification stays on screen (`Apps.h`, `Overlays.h`; stored ms, set in seconds).
- **Note Duration** — a musical note's length in RTTTL parsing (`MelodyPlayer/`).

Match this vocabulary in code and discussion.

## Licensing note

CC BY-NC-SA 4.0 (non-commercial). Not affiliated with Ulanzi.
