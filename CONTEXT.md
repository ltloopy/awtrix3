# Project glossary

Canonical terms used across the codebase and documentation. When a term appears in code review, docs, or commits, use the meaning defined here.

## Timer

### Finished mode

The categorical setting selecting *what happens when the timer reaches zero*. Three values: `auto-clear` / `hold` / `re-alert`. Stored in `TimerManager::finishedMode` (enum `FinishedMode`), persisted in NVS namespace `"timer"` key `FIN`. Editable via:

- MQTT `{prefix}/timer` (`"finished"` key)
- Home Assistant `{id}_timer_fin` select
- On-device: the `FINISH` slot of the `TIMER` top menu

Not to be confused with the per-mode *timing knobs* below.

### Per-mode timing knobs

Three numeric tunings, each only meaningful in one specific mode:

| Knob | Variable | Relevant when | Editable via |
|---|---|---|---|
| Auto-clear delay | `TIMER_FINISHED_HOLD` | `finished mode = auto-clear` | `dev.json` (`timer_finished_hold`), MQTT/HTTP `{prefix}/timer` (`finished_hold`), `TIMER` top menu (`CLEAR` slot) |
| Re-alert interval | `TIMER_REALERT_INTERVAL` | `finished mode = re-alert` | `dev.json` (`timer_realert_interval`), MQTT/HTTP `{prefix}/timer` (`realert_interval`), `TIMER` top menu (`ALERT` slot) |
| Countdown beep window | `TIMER_COUNTDOWN_SECONDS` | `buzzer mode = countdown` | `dev.json` (`timer_countdown_seconds`), MQTT/HTTP `{prefix}/timer` (`countdown_seconds`), `TIMER` top menu (`CDOWN` slot) |

Persisted in NVS namespace `"awtrix"` (keys `TFHOLD` / `TRALERT` / `TCDOWN`). `dev.json` overrides NVS on every boot.

### Timer behavior parameters

Four user-editable parameters that shape Timer behavior outside of the **per-mode timing knobs** above. **Each is a distinct category** — they are *not* "tuning knobs" in the ADR-0003 sense. Listed individually so future readers don't lump them. See ADR-0004.

| Parameter | Category | Variable | Default | Controls |
|---|---|---|---|---|
| **Max duration** | Input bound | `TIMER_MAX_DURATION` | 86400 (24 h) | Upper bound on accepted `duration` commands. Out-of-range is rejected, not clamped (ADR-0001). |
| **Button step** | Input granularity | `TIMER_STEP` | 1 | How much each left/right press changes the current field in the **Timer-app config mode**. |
| **Remaining publish interval** | Output cadence | `TIMER_PUBLISH_INTERVAL` | 1 s | How often `timer_rem` republishes while Running (also drives the HA `{id}_timer_rem` sensor). |
| **App config timeout** | UI timing | `TIMER_CONFIG_TIMEOUT` | 30 s | No-input idle window before **Timer-app config mode** auto-applies and exits to `Idle`. Does **not** apply to the **TIMER global menu**. |

All four reach: `dev.json` (`timer_max_duration` / `timer_button_step` / `timer_remaining_publish_interval` / `timer_app_config_timeout`), MQTT/HTTP `{prefix}/timer` (`max_duration` / `button_step` / `remaining_publish_interval` / `app_config_timeout`), NVS namespace `"awtrix"` (keys `TMAXD` / `TSTEP` / `TPUBI` / `TCFGT`). `dev.json` overrides NVS on every boot. No on-device menu; no HA entities.

_Avoid_: "tuning knobs" (reserved for the three per-mode knobs above); "compile-time globals" (they aren't — they're runtime-mutable as of ADR-0004).

### Two on-device timer-config surfaces

There are two physically distinct on-device places to configure the Timer; use the right name for the right one.

- **Timer-app config mode** — long-press middle from `Idle` while the Timer app is on screen. Edits **duration only** (HH/MM/SS wheels, auto-repeat on hold, 30 s no-input auto-applies). Lives in `TimerManager` ([TimerManager.cpp:483-499](src/TimerManager.cpp#L483)).
- **TIMER global menu** — long-press middle from any app to open the global menu, navigate to the `TIMER` top entry. Edits **buzzer mode, finished mode, and the three per-mode timing knobs**. Lives in `MenuManager` ([MenuManager.cpp](src/MenuManager.cpp)).

ADR-0001 originally named "the on-device config buttons" as the timer's single on-device control surface — that referred to the Timer-app config mode. With the global `TIMER` menu added, on-device timer configuration now spans both surfaces; ADR-0003 documents the addition.
