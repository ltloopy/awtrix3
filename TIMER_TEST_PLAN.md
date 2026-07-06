# AWTRIX Smart Clock — Timer App Manual Test Plan

> **Device:** AWTRIX Smart Clock (Smart LED Pixel Clock, Ulanzi TC001)
> **Feature under test:** Native countdown **Timer app** and its full control,
> observation, and propagation surface, as introduced on the
> `feat-timer-standalone` branch.
> **Reference docs:** [`docs/timer.md`](docs/timer.md), [`docs/api.md`](docs/api.md),
> [`docs/dev.md`](docs/dev.md), [`docs/apps.md`](docs/apps.md),
> [`docs/onscreen.md`](docs/onscreen.md), and ADRs
> [0001](docs/adr/0001-timer-command-validation-parity.md)–[0006](docs/adr/0006-timer-multi-device-sync.md).

---

## 1. Introduction

### 1.1 Objective

Verify, on real hardware, that the Timer app behaves correctly and consistently
across **every** surface introduced on this branch: `dev.json` boot
configuration, the native HTTP API, the MQTT command/observation topics, the
Home Assistant discovery entities, the on-device menus, and the multi-device UDP
sync channel. The plan confirms functional correctness, input validation,
state-machine integrity, persistence across reboots, and cross-surface parity.

### 1.2 Scope — In

Only features added on `feat-timer-standalone`:

| Area | Items |
| --- | --- |
| **dev.json config** | `show_timer` plus the `timer_*` keys; the `timer_publish_interval`→`timer_remaining_publish_interval` rename; the "dev.json overrides NVS on every boot" rule. (`timer_app_config_timeout` was removed in PRD #83 / #88 — the menu is timeout-free; an old key in the payload is silently ignored.) |
| **New API calls** | `POST /api/timer` (atomic-reject validation, `200`/`400`/`409`); `GET /api/timer` (read-only observation snapshot). |
| **MQTT calls** | `{prefix}/timer` command topic; retained `{prefix}/timer/icons`; the new `TIMER` key on `{prefix}/settings`. |
| **Home Assistant** | The 8 discovery entities and discovery prune on `SHOW_TIMER` off. |
| **Supporting / interacting** | State machine (Idle/Running/Paused/Finished), buzzer & finished modes, the on-device `TIMER` global menu (drill-in list incl. the `DURATION` leaf; opened from the main menu or the Timer-app idle long-press), display reflow (`icon_enabled`/`bar_enabled`), multi-device UDP sync (port 4212). |

### 1.3 Scope — Out

- Pre-existing native apps (clock, date, temperature, humidity, battery) except
  where the new `TIMER` settings key sits alongside them.
- The generic notification system (tested only where the timer **buffers** or
  **dismisses** notifications).
- Firmware OTA / update flow, Wi-Fi provisioning, and unrelated `/api/settings`
  keys.
- Automated unit/e2e tests — covered by the 11 host suites under `test/`
  (8 native PlatformIO envs, 289 tests — see the [§8 matrix](#8-automated-companion--native-test-matrix))
  and `tests/e2e/`. This plan is the **manual hardware** companion.

---

## 2. Types of Testing Required

| Type | Definition / why it applies here |
| --- | --- |
| **Functional** | Each control does what it claims: start/pause/reset, set duration, change buzzer/finished modes, set icons/melodies/colors. |
| **Boundary / Validation** | Inputs at and just outside documented ranges (durations, the `1–300`/`5–300`/`0–30`/`1–60`/`1–99` knob ranges, hex colors, 32-char caps). The contract is **atomic-reject, never clamp** (ADR-0001). |
| **State-transition** | The Idle→Running→Paused→Finished machine, including edge transitions (start-from-finished re-arm, pause-while-paused = resume, duration-edit-while-paused resets to Idle). |
| **Integration / Cross-surface parity** | The three control surfaces (on-device, HTTP, MQTT) and the HA face must apply identical validation; the observation surfaces (`GET /api/timer`, HA sensors) must report identical live values (ADR-0001, CONTEXT.md "Control vs. observation surface"). |
| **Persistence / Reboot** | NVS-backed config survives reboot; `dev.json` overrides NVS on every boot; runtime *run-state* deliberately does **not** survive reboot (returns to Idle). |
| **Negative** | Malformed JSON, unknown enum values, missing icon/melody files, disabled-feature commands, oversized payloads. |
| **Multi-device interoperability** | Leader/follower/peer/standalone roles, run-state vs. config propagation, follow consent gate, `all` targeting, one-hop rule, packet-loss self-heal (ADR-0006). Requires ≥2 units. |

---

## 3. Testing Strategy

Tests are ordered to build state knowledge incrementally and **avoid
re-validating the same logic on every surface**:

1. **Configuration & defaults first.** Boot with a clean device, then with a
   `dev.json`, to establish the baseline and the override rule. Everything after
   assumes known defaults.
2. **State machine on one reference surface.** Exercise the full
   Idle/Running/Paused/Finished machine via **MQTT `{prefix}/timer`** (fast to
   script, easy to observe). This is the canonical behavioural pass.
3. **Parity, not repetition.** For HTTP, HA, and on-device surfaces, run only the
   *parity* deltas — that the same action produces the same state — rather than
   re-running every state case per surface. The **atomic-reject validation
   contract is tested thoroughly once on `POST /api/timer`** (the only surface
   that returns explicit error codes), then **spot-checked** for parity on MQTT
   (silent ignore), HA `timer_dur` (revert), and on-device (range-capped wheels).
4. **Display & audio.** Visual/audible behaviour (progress bar, reflow, icon
   fallback, buzzer/countdown beeps, blinking finished screen) — done once the
   state machine is trusted.
5. **Persistence & reboot.** Confirm NVS survival, dev.json override, and
   run-state non-survival.
6. **Multi-device sync last.** Needs ≥2 provisioned units on one LAN; build on
   everything proven above.

**Observation parity** (`GET /api/timer` vs. HA `_timer_rem`/`_timer_state`) is
checked alongside the relevant state cases rather than as a separate pass, since
the live values are already on screen.

---

## 4. Test Cases

**Priority:** **P1** = critical (core function / data integrity / validation
contract) · **P2** = high (important behaviour, secondary surfaces) ·
**P3** = medium (cosmetic, convenience, rare config).

**Test Result** column: tester fills `Pass` / `Fail` (left as `—`).

Conventions: `[PREFIX]` = the device's MQTT prefix (default `awtrix_xxxx`);
`[IP]` = device IP; `[ID]` = device `uniqueID`. Unless stated, the device boots
with `show_timer=true` and factory-default timer config.

### 4.1 dev.json configuration items

| Test ID | Priority | Test Result | Test Description | Pre-requisites | Test Steps | Expected Results |
| --- | --- | --- | --- | --- | --- | --- |
| DEV-01 | P1 | PASS | Defaults applied when no timer keys present | Device with a `dev.json` that omits all `timer_*` keys | 1. Upload `dev.json`. 2. Reboot. 3. `GET /api/timer` and open Timer app. | Timer uses documented defaults: duration honors NVS, buzzer=`end`, finished=`auto-clear`, bar shown, icon shown; `timer_max_duration`=86400, publish interval=1 s, finished hold=10, realert=15, countdown=3, config timeout=30. |
| DEV-02 | P1 | PASS | `show_timer:false` removes app and suppresses HA | HA_DISCOVERY on; HA paired | 1. Set `show_timer:false` in `dev.json`. 2. Reboot. 3. Inspect app rotation, HA device card, and `POST /api/timer`. | Timer app absent from rotation; 8 HA entities not published (pruned via empty retained discovery after reconnect); `POST`/`{prefix}/timer` return/behave as disabled; any running timer reset. |
| DEV-03 | P1 | Pass | dev.json overrides NVS on every boot | Set duration/buzzer/finished to non-default values via MQTT (writes NVS) first | 1. Confirm NVS values active. 2. Add conflicting values to `dev.json` (e.g. `timer_bar_enabled:false`, `timer_buzzer`-equivalent keys). 3. Reboot. | On boot, `dev.json` values win over the stored NVS values; changing them back requires editing/removing the dev.json key. |
| DEV-04 | P2 | Pass | Renamed key: `timer_remaining_publish_interval` | HA paired or MQTT monitor on `{prefix}/...timer_rem` | 1. Set `timer_remaining_publish_interval:5`. 2. Reboot, start a timer. 3. Observe `timer_rem` publish cadence. | `timer_rem` republishes every ~5 s while Running. Old `timer_publish_interval` name ignored. |
| DEV-05 | P2 | Pass | Removed key: `timer_app_config_timeout` ignored | — | 1. Set `timer_app_config_timeout:10` in dev.json and/or send it on `{prefix}/timer`. 2. Reboot, open the TIMER menu / DURATION leaf, leave untouched. | The key is silently ignored (no error, no rejection). The menu never auto-exits on idle (timeout-free, #88). |
| DEV-06 | P2 | Pass | Per-state icon keys load from dev.json | Valid icon files in `/ICONS/` | 1. Set `timer_icon_idle`/`timer_icon_running`/`timer_icon_paused`/`timer_icon_finished` to existing bare names. 2. Reboot. 3. Cycle states. | Each state shows its configured icon; an empty slot falls back to `timer_icon_idle`; values mirrored to retained `{prefix}/timer/icons`. |
| DEV-07 | P2 | — | Melody override keys | `/MELODIES/<name>.txt` present | 1. Set `timer_melody_tick`/`timer_melody_end` to existing names. 2. Reboot, run countdown-buzzer timer. | Configured RTTTL melodies play for ticks/end; empty value resets to `timer_tick`/`timer_end`. |
| DEV-08 | P3 | Pass | Display toggles via dev.json | — | 1. Set `timer_bar_enabled:false` and `timer_icon_enabled:false`. 2. Reboot, start a timer. | Progress bar hidden; icon region suppressed and time text + bar reflow across the full 32px panel (ADR-0005). |
| DEV-09 | P3 | Pass | `timer_bar_color` hex | — | 1. Set `timer_bar_color` to a non-zero hex (e.g. `255` = blue). 2. Reboot, run a timer. | Progress bar renders in the configured color; `0` follows `TEXTCOLOR_888`. |
| DEV-10 | P3 | Pass | Knob ranges from dev.json | — | 1. Set `timer_finished_hold`, `timer_realert_interval`, `timer_countdown_seconds`, `timer_max_duration` to valid in-range values. 2. Reboot, verify each. | Each value takes effect (e.g. max_duration caps the config-mode wheels and rejects larger `duration` commands). |
| DEV-11 | P2 | — | Multi-device sync identity keys | — | 1. Set `timer_sync_follow:true` and `timer_sync_targets:"all"`. 2. Reboot. 3. `GET`/inspect role behaviour. | Clock obeys inbound sync and commands all peers; values are local-only (never propagated). |

### 4.2 New API calls — `POST` / `GET /api/timer`

| Test ID | Priority | Test Result | Test Description | Pre-requisites | Test Steps | Expected Results |
| --- | --- | --- | --- | --- | --- | --- |
| API-01 | P1 | Pass | POST start a timer | Timer idle | `POST /api/timer` `{"duration":300,"action":"start"}` | `200 OK` body `OK`; display force-switches to Timer app (no active game); state `running`, counts down from 5:00. |
| API-02 | P1 | Pass | POST pause / resume / reset | Running timer | 1. `{"action":"pause"}` 2. `{"action":"pause"}` again 3. `{"action":"reset"}` | Step 1 → `paused` (frozen); step 2 → `running` (resumes); step 3 → `idle` at configured duration. Each `200 OK`. |
| API-03 | P1 | Pass | Combined setter + action atomicity | Idle | `{"duration":600,"buzzer":"countdown","action":"start"}` | Setters apply first, then action: starts a fresh 10:00 countdown-buzzer timer in one request. `200 OK`. |
| API-04 | P1 | Pass | Malformed JSON rejected | — | `POST` body `{"duration":` (truncated) | `400 Bad Request`, body `ErrorParsingJson`; no state/config change. |
| API-05 | P1 | Pass | Out-of-range duration rejected, not clamped | `timer_max_duration`=86400 | `{"duration":"30:00:00"}` (108000 s) | `400 Bad Request`, body `InvalidValue`; configured duration unchanged (NOT clamped to 24 h). |
| API-06 | P1 | Pass | Unknown enum value rejected | — | `{"buzzer":"banana"}`; then `{"finished":"nope"}`; then `{"action":"go"}` | Each → `400 InvalidValue`; nothing applied. |
| API-07 | P1 | Pass | Partial-validity = whole rejection | Idle, buzzer=`end` | `{"buzzer":"off","duration":"banana"}` | `400 InvalidValue`; buzzer stays `end` (the good field is NOT applied). |
| API-08 | P1 | Pass | Disabled feature returns 409 | `show_timer=false` | `POST /api/timer` `{"action":"start"}` | `409 Conflict`, body `TimerDisabled`; no timer starts. |
| API-09 | P2 | Pass | Duration colon-parsing & echo | Idle | POST each of `{"duration":"1:30:00"}`, `{"duration":"3:00"}`, `{"duration":"3:90"}`, `{"duration":90}` then `GET` after each | Parsed to 5400 / 180 / 270 / 90 s respectively; `duration_str` echoes trimmed clock string (`300`→`5:00`, `3661`→`1:01:01`, `45`→`0:45`). |
| API-10 | P1 | Pass | GET observation snapshot fields | Running timer set to 5:00 | `GET /api/timer` mid-run | `200 OK`; JSON has `state`,`enabled`,`remaining`,`remaining_str`,`duration`,`duration_str`,`buzzer`,`finished`; `remaining` decreasing live, `remaining_str` matches. |
| API-11 | P2 | Pass | GET canonical spellings | Set finished via alias `realert`, buzzer `end` | `GET /api/timer` | `finished` reports canonical `re-alert` (hyphenated lowercase); buzzer `end`. Read endpoint never echoes aliases. |
| API-12 | P2 | Pass | GET returns 200 even when disabled | `show_timer=false` | `GET /api/timer` | `200 OK` (no 409); `enabled:false`, `state:idle`. Observation never blocked. |
| API-13 | P2 | Pass | GET never mutates | Running timer | `GET /api/timer` several times | State/remaining unaffected by the reads (read-only). |
| API-14 | P3 | Pass | Icon/melody 32-char cap & case-sensitivity | — | POST `icon_idle` with a 40-char name; then a valid mixed-case name matching a file | Over-cap name rejected (`400`); case must match the LittleFS filename exactly. |
| API-15 | P2 | Pass | Start from Finished re-arms | Timer in `finished` (hold mode) | `{"action":"start"}` | Finished screen clears; counts down fresh from configured duration. |
| API-16 | P3 | Pass | Duration edit while Paused resets to Idle | Paused timer at 2:00 remaining | `{"duration":600}` (no action) | State → `idle` re-armed to full new 10:00; a later `start` counts the full new value. |
| API-17 | P3 | Pass | Duration edit while Running buffers | Running timer | `{"duration":600}` | In-progress countdown is NOT restarted; new duration takes effect on next `start`/`reset`. |

### 4.3 MQTT calls

| Test ID | Priority | Test Result | Test Description | Pre-requisites | Test Steps | Expected Results |
| --- | --- | --- | --- | --- | --- | --- |
| MQTT-01 | P1 | Pass | `{prefix}/timer` start/pause/reset | MQTT broker connected | Publish to `[PREFIX]/timer`: `{"duration":120,"action":"start"}`, then `{"action":"pause"}`, then `{"action":"reset"}` | Timer starts (2:00), pauses, resets to idle — same state outcomes as API-01/02. |
| MQTT-02 | P1 | Pass | Invalid payload silently ignored | Known `timer_dur` retained value | Publish `[PREFIX]/timer` `{"duration":"banana"}` and a malformed payload | No error published (fire-and-forget); retained `timer_dur` state shows the unchanged value; no state/config change. |
| MQTT-03 | P1 | Pass | Out-of-range duration silently ignored | max=86400 | Publish `{"duration":"30:00:00"}` | Ignored; configured duration unchanged (parity with API-05 but no error report). |
| MQTT-04 | P2 | Pass | Key processing order in one publish | Idle; valid icon files present | Publish `{"icon_running":"<gif>","duration":300,"action":"start"}` | Icons set, then duration, then action — single publish configures icon and starts the timer (`duration`→`buzzer`→`finished`→`icon_*`→`action`). |
| MQTT-05 | P2 | Pass | Retained `{prefix}/timer/icons` mirror updates (read-only observation topic) | MQTT monitor subscribed to `[PREFIX]/timer/icons` | 1. Publish an icon change as a **command** to `[PREFIX]/timer`, e.g. `{"icon_running":"<gif>"}` (do NOT publish to `.../timer/icons` — it is a read-only mirror the device only writes). 2. Observe (do not publish to) `[PREFIX]/timer/icons`. | Retained JSON `{"idle":...,"running":...,"paused":...,"finished":...}` reflects the new value immediately. Note the mirror echoes stripped keys (`running`), not the command key (`icon_running`). |
| MQTT-06 | P2 | Pass | Icon topic republished on reconnect | Subscriber that connects after device boot | 1. Reboot/reconnect device. 2. New subscriber connects. | Retained `{prefix}/timer/icons` delivered to the new subscriber on connect (current values, no NVS read needed). |
| MQTT-07 | P1 | Pass | `TIMER` key on `{prefix}/settings` | HA paired | Publish `[PREFIX]/settings` `{"TIMER":false}` then `{"TIMER":true}` | `false` → app hidden, HA entities suppressed, `/api/timer` + `{prefix}/timer` ignored, running timer reset; HA prune completes after next reconnect. `true` → entities/app return. App rotation refreshes immediately (no reboot). |
| MQTT-08 | P3 | Pass | `{prefix}/timer` config without action | Idle, buzzer=`end` | Publish `{"buzzer":"countdown"}` only | Buzzer mode changes & persists; timer state untouched. |
| MQTT-09 | P3 | — | Sync identity reachable via MQTT | — | Publish `{"sync_follow":true,"sync_targets":"all"}` | Values applied & persisted; not propagated to peers (local identity). |
| MQTT-10 | P1 | — | Retained command does NOT replay on boot | MQTT broker connected | 1. Publish `[PREFIX]/timer` **with retain set**: `mosquitto_pub -r -t '[PREFIX]/timer' -m '{"duration":120,"action":"start"}'`. 2. Power-cycle the device (or force an MQTT reconnect). 3. After reconnect, re-subscribe: `mosquitto_sub -v -t '[PREFIX]/timer'`. | Timer comes up **Idle** — it does **not** auto-start from the retained command (EC-03 / `docs/timer.md` contract). The device clears the retained payload on connect (before subscribing), so the fresh subscribe shows **no** retained message on `[PREFIX]/timer`. |

### 4.4 Home Assistant controls

| Test ID | Priority | Test Result | Test Description | Pre-requisites | Test Steps | Expected Results |
| --- | --- | --- | --- | --- | --- | --- |
| HA-01 | P1 | Pass | All 8 entities discovered | `HA_DISCOVERY=true`, HA paired, `show_timer=true` | Open the device card in Home Assistant | Entities present: `[ID]_timer_dur` (text), `_timer_rem` (sensor), `_timer_state` (sensor), `_timer_buz` (select), `_timer_fin` (select), `_timer_start`/`_timer_pause`/`_timer_reset` (buttons). |
| HA-02 | P1 | Pass | Start/Pause/Reset buttons | Discovered entities | Press `_timer_start`, then `_timer_pause`, then `_timer_reset` | Map to `{"action":...}`; `_timer_state` transitions idle→running→paused→idle accordingly; start from idle auto-switches display to Timer app. |
| HA-03 | P1 | Pass | `_timer_dur` accepts valid input | Idle | Set `_timer_dur` to `10:00`, then `90` (bare seconds), then `1:30:00` | Each accepted; duration updates; `_timer_state` & `GET /api/timer` agree. |
| HA-04 | P1 | Pass | `_timer_dur` reverts invalid input | Known valid duration | Enter `banana`, then `30:00:00` (over max) | Field reverts to the previous valid time (no clamp, no apply) — the HA face of atomic-reject. |
| HA-05 | P2 | Pass | `_timer_buz` / `_timer_fin` selects | — | Change `_timer_buz` through off/end/countdown; `_timer_fin` through the 3 modes | Each selection applies & persists immediately; reflected in `GET /api/timer` canonical spellings. |
| HA-06 | P2 | Pass | `_timer_rem` sensor cadence | `timer_remaining_publish_interval`=1 (default) | Start a timer, watch `_timer_rem` | Updates ~every 1 s while running; frozen while paused; `0` at finished. |
| HA-07 | P2 | Pass | `_timer_state` vocabulary | — | Drive through all states | Reports exactly `idle`/`running`/`paused`/`finished`. |
| HA-08 | P1 | Pass | Entities pruned on disable | Entities present | Set `TIMER=false` (settings) or `show_timer=false` (dev.json) + reboot/reconnect | Firmware publishes empty retained discovery payloads; HA prunes the 8 entities after next reconnect. |
| HA-09 | P3 | Pass | Re-discovery on re-enable | Previously disabled | Re-enable timer, reconnect | All 8 entities reappear with current values. |
| HA-10 | P3 | — | Observation freshness note | Running timer | Compare `GET /api/timer` `remaining` vs HA `_timer_rem` | HTTP read may read 1–2 s lower than throttled HA sensor — expected, not a defect. |

### 4.5 Multi-device sync (propagation surface, ADR-0006) — requires ≥2 units

| Test ID | Priority | Test Result | Test Description | Pre-requisites | Test Steps | Expected Results |
| --- | --- | --- | --- | --- | --- | --- |
| SYNC-01 | P1 | — | Leader → follower run-state | Clock A: `sync_targets=[B id]`, follow off. Clock B: `sync_follow=true`, no targets. Same LAN. | On A: start (with duration), then pause, then reset. | B mirrors each action within network latency; a `start` carries `duration` so B counts the same countdown; B never commands A. |
| SYNC-02 | P1 | — | Follow consent gate | Clock B: `sync_follow=false` | A (targeting B) starts a timer | B ignores inbound sync entirely (never acts on un-consented sync). |
| SYNC-03 | P1 | — | Config snapshot propagation | A & B peers (both follow + targets) | On A: change buzzer + finished + bar_color (a config edit) | A propagates a full config snapshot; B ends up configured identically (last-config-writer-wins). No `action`/`duration` in the config block. |
| SYNC-04 | P1 | — | Bare start never clobbers config | B has custom buzzer/finished; A targets B | On A: a bare `start` (no config fields) | B starts the countdown but its buzzer/finished config is unchanged (duration is run-state, not config). |
| SYNC-05 | P2 | — | `all` keyword | 3 clocks; A `sync_targets=all`; B & C follow on | On A: start | Both B and C mirror directly (no need to list ids). |
| SYNC-06 | P2 | — | One-hop, no re-chaining | A targets B only; B targets C; all follow | On A: start | B applies but does NOT re-broadcast to C; C stays unaffected (applied inbound is never re-broadcast). |
| SYNC-07 | P2 | — | Identity not propagated | Peers configured | On A: edit config | B's own `sync_follow`/`sync_targets` remain unchanged after receiving A's config snapshot. |
| SYNC-08 | P2 | — | Disabled peer ignores inbound | B: `show_timer=false`; A targets B | On A: start | B ignores the packet (its `parseCommand` is disabled). |
| SYNC-09 | P3 | — | Packet-loss self-heal | Peers; introduce a dropped frame if possible | Cause/observe a missed run-state packet, then issue a fresh start on leader | Each packet sent 3× and de-duplicated by `(src,seq)`; a single drop rarely desyncs; next start/reset re-establishes shared state. |

### 4.6 On-device controls (the `TIMER` global menu, drill-in)

| Test ID | Priority | Test Result | Test Description | Pre-requisites | Test Steps | Expected Results |
| --- | --- | --- | --- | --- | --- | --- |
| DEV-DEVICE-01 | P1 | — | `DURATION` leaf duration edit | TIMER menu open, timer Idle | Select `DURATION`; cycle HH→MM→SS with short SELECT; adjust with LEFT/RIGHT; long-press SELECT to save & return to the list | Underline marks active field; LEFT/RIGHT step by 1; HH wraps 99↔0, MM/SS wrap 59↔0 (no carry); saved value clamped to `[1, max_duration]`; the duration commits on leaf back-out. |
| DEV-DEVICE-02 | P2 | — | `DURATION` hold auto-repeat; no idle timeout | In the `DURATION` leaf | Hold RIGHT ≥500 ms; separately, leave the menu idle for >30 s | Hold auto-repeats ~4/s; the menu **never** auto-exits on idle (timeout-free, #88). |
| DEV-DEVICE-03 | P2 | — | `DURATION` read-only while Running/Paused | Timer Running or Paused | Open the TIMER menu, select `DURATION` | Leaf shows the current value with **no** underline; LEFT/RIGHT do nothing; any press returns to the list (running timer undisturbed). |
| DEV-DEVICE-04 | P1 | — | `TIMER` global menu drill-in walk | Any app on screen | Open the menu → `TIMER` (now before `APPS`); walk the 9 items: DURATION, BUZZER, COUNTDOWN, FINISH, CLEAR DELAY, RE-ALERT INTERVAL, ICON, PROGRESS BAR, MAIN; drill into each value item | LEFT/RIGHT walk the named list and wrap; short SELECT drills into the leaf (bare value shown); LEFT/RIGHT then cycle enums (FINISH = CLEAR/HOLD/RE-ALERT) / step numbers (step 5 CLEAR DELAY/RE-ALERT INTERVAL, step 1 COUNTDOWN) / flip ICON & PROGRESS BAR; short SELECT confirms back, long SELECT saves & returns; BUZZER & FINISH apply+publish immediately; numeric/toggle persist once on leaving the list. |
| DEV-DEVICE-07 | P1 | — | Over-wide menu labels scroll (general marquee, PRD #96) | TIMER menu open | Walk the list; rest on `CLEAR DELAY`, `RE-ALERT INTERVAL`, `PROGRESS BAR`; then on short items (`DURATION`, `BUZZER`, `ICON`, `MAIN`); move between items and re-open the menu; also eyeball a non-timer over-wide menu (e.g. `COLOR` hex, a wide `DATE` preview) | Each over-wide label holds briefly at the left, scrolls left, wraps and loops while selected; short labels stay centered and still; the marquee restarts from the left when moving to another item and on re-entry; the `DURATION` leaf wheel + active-field underline stay centered and aligned (never scroll). |
| DEV-DEVICE-06 | P1 | — | Entry points & context-aware exit | — | (a) Open from main menu, long-press out of the list. (b) Open from the Timer-app idle long-press, long-press out of the list. (c) From either, select `MAIN`. | (a) returns to the main menu; (b) returns to the Timer app; (c) always returns to the main menu. The bare duration wheel has no other entry point. |
| DEV-DEVICE-05 | P2 | Pass | Physical buttons drive state machine | Timer app current | Short SELECT to start (idle)/pause (running)/resume (paused)/restart (finished); long SELECT to reset | Behaviour per `docs/apps.md`; while in `finished`, SELECT works from any app screen. Idle long-press now opens the TIMER menu (not the bare wheel). |

---

## 5. Defect Management & Severity

### 5.1 Pass / Fail criteria

- **Pass:** Actual result matches the Expected Results exactly, with no
  side-effects on adjacent state/config and no regression on other surfaces.
- **Fail:** Any deviation — wrong state, clamped-instead-of-rejected input,
  inconsistent values across surfaces, missing/extra HA entity, lost
  persistence, crash/reboot, or visual/audio defect.
- **Blocked:** Cannot execute (e.g. HA not pairing, second unit unavailable) —
  record and re-run once unblocked.

### 5.2 Severity classification

| Severity | Definition | Timer-specific examples |
| --- | --- | --- |
| **Critical** | Device unusable, data loss, crash/reboot loop, or validation-contract breach that corrupts state. | `POST /api/timer` crashes the firmware; an out-of-range duration is **clamped and started** instead of rejected; device reboot-loops when Timer app is in rotation; NVS corruption. |
| **High** | Core timer function broken on a primary surface; cross-surface parity broken. | Start/pause/reset wrong via MQTT; `_timer_dur` accepts invalid input without reverting; `show_timer=false` doesn't suppress HA entities; sync follower obeys without consent. |
| **Medium** | Secondary feature wrong, recoverable; affects one surface only. | `timer_rem` cadence ignores `remaining_publish_interval`; icon doesn't fall back to idle slot; config-mode auto-repeat rate off; melody override ignored. |
| **Low** | Cosmetic / documentation / minor UX. | Progress bar off by one pixel; bar color slightly wrong; blink cadence marginally off; canonical-spelling vs alias display nit. |

### 5.3 Defect report contents

Each logged defect should include: Test ID, severity, surface(s) affected,
firmware/branch + commit, exact request/payload and response, observed vs.
expected, reproduction steps, and screenshots/serial-log where relevant.

---

## 6. Edge Cases

| # | Edge case | Expected handling |
| --- | --- | --- |
| EC-01 | Duration exactly `1` s and exactly `timer_max_duration` | Both accepted (inclusive bounds); `0` and `max+1` rejected. |
| EC-02 | Colon-parsing quirks: `"3:90"`, `"1:30:00"`, `":30"`, `"5:"`, `"1:2:3:4"` | `"3:90"`=270 s (fields summed, no 0–59 cap); `"1:30:00"`=5400 s; `":30"`/`"5:"` empty-field → rejected; >2 colons → rejected. |
| EC-03 | Reboot mid-run (Running/Paused/Finished) | Returns to **Idle** with saved duration — run-state intentionally not persisted (no RTC backup). |
| EC-03a | Retained `{prefix}/timer` command present on boot/reconnect | Device clears the retained command on connect *before* subscribing, so the broker has nothing to replay; the timer stays **Idle** and never auto-starts. Command topics must not be retained. |
| EC-04 | dev.json key conflicts with NVS value | dev.json wins on every boot (re-applied each reboot). |
| EC-05 | Missing icon file referenced | Loader checks `/ICONS/<name>.jpg` then `.gif`; if neither exists, falls back to idle slot, then to the built-in 8×8 hourglass. |
| EC-06 | Missing melody file | Falls back to built-in RTTTL; no crash, no silence-by-error. |
| EC-07 | `icon_enabled=false` reflow | Icon region (incl. hourglass fallback) suppressed; time text + bar span full 32px panel. |
| EC-08 | `SHOW_TIMER=false` while a timer is Running | Running timer reset to Idle; app hidden; commands ignored; HA pruned. |
| EC-09 | Animated GIF icon mid-state-change | GIF restarts at frame 0 on each resolved-icon change; previous file handle released (no leak). |
| EC-10 | Pause while Idle / start while Running | Pause-while-idle = no-op; start-while-running = no-op; pause-while-paused = resume. |
| EC-11 | Empty icon/melody string | Icon slot cleared (falls back to idle); melody resets to default (`timer_tick`/`timer_end`). |
| EC-12 | Over-cap (32-char) or illegal-char icon/melody name | Rejected (atomic) — alphanumeric, `_`, `-` only. |
| EC-13 | `bar_color` boundary | `0`=follow text color; `0xFFFFFF` accepted; out-of-range numeric / non-6-digit hex rejected (strict format per ADR). |
| EC-14 | `bar_enabled` strict boolean | Only `true`/`false` accepted; non-boolean rejected (no truthy coercion). |
| EC-15 | Combined request where setter is valid but action invalid (or vice-versa) | Whole command rejected atomically; nothing applied. |
| EC-16 | Sync: dropped UDP frame | 3× send + `(src,seq)` de-dup absorbs a single drop; next start/reset self-heals. |
| EC-17 | Sync: leader reset vs follower locally paused | Follower obeys leader's reset (consent assumed via follow); local desync resolves on shared start/reset. |
| EC-18 | `app_config_timeout` sent on `{prefix}/timer` / dev.json | Silently ignored (unknown key, no rejection); the TIMER menu remains timeout-free. No NVS migration of the dead `TCFGT` key. |
| EC-19 | Oversized JSON body to `POST /api/timer` | Exceeds parse buffer → `400 ErrorParsingJson`. |
| EC-20 | Clearing the Finished alert (per finished-mode) | The finished `0:00` is a native Timer-app screen, **not** a notification (ADR-0002): `notify/dismiss` has no effect on it. Auto-clear returns to Idle after `timer_finished_hold` s automatically; Hold and Re-alert are cleared only by an explicit `start`/`reset` (on-device button, `POST`/`{prefix}/timer`, or HA Start/Reset). |

---

## 7. Test Environment

- **Hardware:** ≥1 Ulanzi TC001 (≥2 for §4.5 sync) on the branch firmware.
- **Network:** Wi-Fi with an MQTT broker reachable; a Home Assistant instance
  with MQTT discovery enabled and the device paired.
- **Tools:** an MQTT client (e.g. MQTT Explorer) for publish/subscribe; an HTTP
  client (curl/Postman) for `/api/timer`; the device web file manager for
  uploading `/ICONS/` and `/MELODIES/` assets; serial console for logs.
- **Pre-flight:** valid `timer.jpg` and `timer_tick.txt`/`timer_end.txt` (plus
  any custom assets) uploaded before icon/melody cases.

---

## 8. Automated companion — native test matrix

The automated host suites run on 8 native PlatformIO envs (11 suites, 289
tests). Each env allowlists its own suite(s) via `test_filter` — never
`test_ignore` (see the convention comment atop the native-env section of
`platformio.ini`). Which env compiles which `src/` files is documented per-env
in `platformio.ini`; this table maps env→suite→count. CI
(`.github/workflows/test.yml`) gates **all 8 envs** on every PR touching
`src/`, `test(s)/`, or the build config.

| Env | Suite(s) | Tests |
| --- | --- | --- |
| `native` | `test_timer` | 190 |
| `native_validate` | `test_validate` | 28 |
| `native_runtime` | `test_runtime` | 21 |
| `native_sync` | `test_syncenvelope` | 14 |
| `native_peer` | `test_peerregistry` | 9 |
| `native_targets` | `test_synctargets` | 9 |
| `native_seen` | `test_syncseen` | 6 |
| `native_ha` | `test_haselect` (4), `test_hasensor` (3), `test_hatext` (3), `test_hasync` (2) | 12 |
| **Total** | **11 suites** | **289** |

Run everything locally with:

```sh
pio test -e native -e native_peer -e native_seen -e native_sync -e native_validate -e native_runtime -e native_targets -e native_ha
```

### Local gotchas

- Set `PLATFORMIO_BUILD_JOBS=1` — parallel native builds can OOM `cc1plus`.
- After editing `tests/stubs/*`, delete `.pio/build/native*` — the stub headers
  are force-included into library TUs and PlatformIO does not rebuild them when
  a stub changes, so stale objects keep the old stub behaviour.

---

*Traceability: every case above derives from documented branch behaviour in
[`docs/timer.md`](docs/timer.md), [`docs/api.md`](docs/api.md),
[`docs/dev.md`](docs/dev.md), [`docs/apps.md`](docs/apps.md),
[`docs/onscreen.md`](docs/onscreen.md), and ADRs 0001–0006. No undocumented
endpoints are assumed.*
