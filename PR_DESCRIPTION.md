# PR description — copy/paste ready

**Title:** `feat(timer): kitchen-style countdown app with MQTT, HA, and button control`

**Target branch:** depends on whether you're opening this against your fork's `main` or staging it for an upstream PR to `Blueforcer/awtrix-light` — description below is written for upstream.

---

## Summary

Adds a first-class kitchen-style countdown Timer app to the firmware, controllable from MQTT, the HTTP API, Home Assistant auto-discovery, and the device's three physical buttons. Includes a four-state machine (Idle / Running / Paused / Finished), configurable buzzer and finished-mode behaviors, per-state icons, NVS persistence, a master enable flag (`SHOW_TIMER`) wired across all four control surfaces, and a full test suite (native unit tests + Python e2e MQTT harness + manual smoke checklist + CI workflow).

## Scope (happy to split if preferred)

This branch bundles six independently-shippable pieces. They're submitted together because each later piece exists to support the Timer app, but each could land as its own PR if maintainers prefer:

1. **Timer app** — feature itself ([src/TimerManager.{cpp,h}](src/TimerManager.cpp), [src/Apps.cpp](src/Apps.cpp), [src/MenuManager.cpp](src/MenuManager.cpp))
2. **HANotify + HAText components** — added to the vendored `home-assistant-integration` library to back the timer's notify/dismiss-by-channel entities ([lib/home-assistant-integration/src/device-types/](lib/home-assistant-integration/src/device-types/))
3. **Channel-based notification dismiss** — `DisplayManager.dismissNotify(uint8_t, const char *)` overload + `refreshCurrentApp()` so the timer can dismiss only its own notifications ([src/DisplayManager.cpp](src/DisplayManager.cpp), [src/Overlays.cpp](src/Overlays.cpp))
4. **CI workflow** — `pio test -e native` on every PR ([.github/workflows/test.yml](.github/workflows/test.yml))
5. **Native test stub framework** — host-buildable stubs so unit tests don't require hardware ([tests/stubs/](tests/stubs/))
6. **E2E MQTT harness** — Docker + Python harness for round-trip publish/subscribe validation ([tests/e2e/](tests/e2e/))

Glad to split into 2–6 PRs in any combination — just say the word.

## What's Changed

**Timer feature**
- `TimerManager` ([src/TimerManager.cpp](src/TimerManager.cpp)) — state machine + persistence (NVS namespace `"timer"`), finished-mode policies (`auto-clear` / `hold` / `re-alert`), buzzer policies (`off` / `end` / `countdown`), per-state icon resolution with fallback chain to `icon_idle` then built-in pixel-art.
- Timer app rendering ([src/Apps.cpp](src/Apps.cpp)) — icon + time text + left-draining progress bar; blinking `0:00` on Finished; app is always present in rotation.
- Config mode — middle long-press from Idle enters HH/MM/SS adjust; left/right adjust with hold-to-repeat (500 ms then 250 ms cadence); 30 s idle auto-applies + exits.

**Control surfaces**
- MQTT `{prefix}/timer` — JSON payload with optional `duration` / `buzzer` / `finished` / `icon_*` / `action` keys, processed in that order so config + start in one publish works ([src/MQTTManager.cpp](src/MQTTManager.cpp)).
- Retained `{prefix}/timer/icons` mirror topic so subscribers see icon state on reconnect.
- `POST /api/timer` mirrors the MQTT schema; `TIMER` key added to `/api/settings` for the master enable ([src/ServerManager.cpp](src/ServerManager.cpp)).
- Eight HA entities via auto-discovery: `timer_dur` (number), `timer_rem` (sensor), `timer_state` (sensor), `timer_buz` / `timer_fin` (select), `timer_start` / `timer_pause` / `timer_reset` (button). Empty retained discovery payloads published on `SHOW_TIMER` true→false transition so HA prunes stale entities on reconnect.
- Physical-button grammar ([docs/timer.md](docs/timer.md)) — short/long-press semantics differ per state, including a Finished long-press shortcut that re-arms straight into Running.
- On-device APPS menu entry to toggle `SHOW_TIMER` without a host ([src/MenuManager.cpp](src/MenuManager.cpp)).

**Supporting infrastructure**
- `HANotify` + `HAText` ([lib/home-assistant-integration/src/device-types/](lib/home-assistant-integration/src/device-types/)) — new HA component types.
- Channel-based dismiss ([src/DisplayManager.cpp:1461](src/DisplayManager.cpp#L1461)) — `dismissNotify(source, json)` overload accepts `{channel: "timer"}` or `{all: true}`.
- Configurable globals ([src/Globals.h](src/Globals.h)) — `SHOW_TIMER`, `TIMER_MAX_DURATION`, `TIMER_STEP`, `TIMER_PUBLISH_INTERVAL`, `TIMER_FINISHED_HOLD`, `TIMER_REALERT_INTERVAL`, `TIMER_COUNTDOWN_SECONDS`, `TIMER_CONFIG_TIMEOUT`, and four `TIMER_ICON_*` slots, all overridable via `dev.json` and (where applicable) `/api/settings`.

**Tests + docs**
- Native unit tests ([test/test_timer/test_timer.cpp](test/test_timer/test_timer.cpp)) — covers state transitions, finished-mode behavior, duration clamping, idempotent commands, config-mode adjust logic.
- Feature spec [docs/timer.md](docs/timer.md), manual test plan [TIMER_TEST_PLAN.md](TIMER_TEST_PLAN.md), updates to [docs/api.md](docs/api.md), [docs/apps.md](docs/apps.md), [docs/dev.md](docs/dev.md).

## Design rationale (decisions worth flagging)

- **Runtime state is intentionally not persisted across reboots.** Duration / buzzer / finished mode persist to NVS; remaining-seconds and elapsed time do not. The device has no RTC backup, and resuming a timer with a wrong elapsed estimate is worse than restarting at Idle.
- **Master enable flag (`SHOW_TIMER`).** Installers who don't want the timer can disable it without code changes via `dev.json`, `/api/settings`, or the on-device menu — the app disappears from rotation, the eight HA entities are pruned, and command surfaces become no-ops. Necessary because the timer can't be cleanly excluded at compile time without scattering `#ifdef`s across [src/Apps.cpp](src/Apps.cpp) and [src/MQTTManager.cpp](src/MQTTManager.cpp).
- **Eight HA entities, not three or four.** Mirrors the existing per-app HA pattern (each surface — set, observe, command — gets its own entity) so Home Assistant automations can target individual concerns rather than mux through one number entity.
- **Channel-based dismiss vs. global dismiss.** A bare `/notify/dismiss` would clobber unrelated notifications on screen at the moment the timer fires. The new `{channel: "timer"}` payload lets HA's `dismiss` button (and the re-alert cleanup path) act surgically.

## Breaking Changes

- **`SHOW_TIMER` defaults to `true`.** Existing users on the next firmware update will see eight new HA entities appear via auto-discovery. To suppress: `POST /api/settings {"TIMER": false}` or put `{"show_timer": false}` in `/dev.json` before flashing. Worth a release-note line.
- **`platformio.ini` restructured to enable native unit tests.** The `[env]` section is now deliberately empty and shared Arduino config has moved to `[arduino_common]`; per-target envs use `extends = arduino_common`. **Downstream forks with custom build envs that previously inherited from `[env]` will need to switch to `extends = arduino_common`.** The change is mechanical and documented inline in [platformio.ini](platformio.ini).

## Notes for reviewers

- HA entity cap raised from 26 → 36 in `HAMqtt mqtt(espClient, device, kMaxHAEntities)` ([src/MQTTManager.cpp](src/MQTTManager.cpp)) — accommodates the 8 new entities + headroom. Minor RAM impact (~10 additional `HAEntity` slots).
- The vendored `home-assistant-integration` library gets two new component types ([HANotify.{cpp,h}](lib/home-assistant-integration/src/device-types/HANotify.h), [HAText.{cpp,h}](lib/home-assistant-integration/src/device-types/HAText.h)). Anyone rebasing custom changes onto that library may hit conflicts.
- `DisplayManager.dismissNotify()` is **overloaded**, not signature-changed — existing callers using the no-arg form still compile unchanged.

## Review focus

Most worth scrutiny:
1. **`TimerManager` state machine + persistence semantics** ([src/TimerManager.cpp](src/TimerManager.cpp)) — the core feature; transitions are covered by unit tests but worth a human read.
2. **`MQTTManager::removeTimerHAEntities` + `reconcileTimerHAState`** ([src/MQTTManager.cpp](src/MQTTManager.cpp)) — HA discovery cleanup is the easiest place to leak stale entities or churn retained topics; the `SHOW_TIMER_HA_PREV` latch is load-bearing.
3. **`DisplayManager::dismissNotify(uint8_t, const char *)`** ([src/DisplayManager.cpp:1461](src/DisplayManager.cpp#L1461)) — channel-based filtering is new behavior touching the notification queue; worth confirming it doesn't regress global dismiss.

## Screenshots / GIFs

> _(to be added before review)_

- **Idle:** _<screenshot>_
- **Running:** _<screenshot — progress bar draining>_
- **Paused:** _<screenshot — frozen bar>_
- **Finished:** _<gif — `0:00` blinking>_

## Test Plan

- `pio test -e native` — native unit tests (also run in CI via [.github/workflows/test.yml](.github/workflows/test.yml)).
- Manual hardware verification: walk [TIMER_TEST_PLAN.md](TIMER_TEST_PLAN.md) against a flashed TC001. The `HW-*`, `EDGE-*`, and the `SHOW_TIMER` cases (`DEV-01`, `API-13`, `HA-08`) are the highest-signal subsets.
- Optional: e2e MQTT harness ([tests/e2e/README.md](tests/e2e/README.md)) — requires a device on the LAN + Docker.

## Commits

History is currently 19 commits, including some fixups. **Happy to squash to a single `feat(timer):` commit, or rebase into ~3 logical commits (timer / HA components + channel dismiss / tests + CI), whichever you prefer.**
