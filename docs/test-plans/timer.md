---
checklist_version: 2
feature: timer
last_updated: 2026-05-16
---

# Manual test checklist — Timer

Canonical manual smoke test for any PR that touches the timer feature.
Copy this checklist into the PR description, tick boxes as you go, and
attach the filled-out copy to the PR.

## Prerequisites

- A Ulanzi TC001 flashed with the branch under test
- An MQTT broker the device publishes to
- A Home Assistant instance receiving the device's discovery messages
- Audible buzzer (built-in)
- Browser open at `http://<device-ip>/screen` for live matrix view

## Sign-off requirements

- Every checkbox below must be ticked or annotated with **WAIVED:
  &lt;reason&gt;**
- If any box fails, file an issue, link it from the PR, and either fix
  before merge or get explicit sign-off to defer
- Note device firmware version + `git rev-parse HEAD` of the branch at the
  top of your filled-out copy

---

## B1. HA discovery & entities
- [ ] All 7 timer entities appear within ~30 s of boot: `timer_dur` (number),
      `timer_rem` (sensor number), `timer_state` (sensor),
      `timer_buz` (select), `timer_fin` (select),
      `timer_start` / `timer_pause` / `timer_reset` (buttons)
- [ ] `timer_buz` shows options `Off / End / Countdown`;
      `timer_fin` shows `Auto-Clear / Hold / Re-Alert`
- [ ] Changing `timer_dur` from HA UI updates the device and survives a
      reboot

## B2. Display & rendering
- [ ] Timer app is always present in the rotation regardless of state
- [ ] Idle: icon + configured duration text, no progress bar
- [ ] Running: time advances once per second; progress bar drains from
      the left (right edge anchored at column 31)
- [ ] Paused: time display freezes; bar holds at its current width
- [ ] Finished: Timer app pulls to foreground, display wakes if asleep,
      icon + `0:00` blinking at 500 ms, no progress bar
- [ ] AutoClear: after ~10 s returns to Idle and rotation resumes
- [ ] Hold: blinking `0:00` persists until reset

## B3. Physical buttons (Timer app focused)
- [ ] Long-press middle from Idle → enters config mode, `HH` field
      highlighted
- [ ] Middle short-press cycles `HH → MM → SS → HH`
- [ ] Left/right adjust current field by `TIMER_STEP`; hold for 500 ms then
      auto-repeat every 250 ms
- [ ] Field bounds wrap: `HH 99 ↔ 0`, `MM/SS 59 ↔ 0`
- [ ] 30 s of no input in config → auto-applies HH:MM:SS to duration,
      exits config
- [ ] Middle short-press while Running pauses; while Paused resumes
- [ ] Long-press middle while Finished resets to Idle

## B4. Buzzer modes
- [ ] **Off**: no sound at expiry; no countdown beeps
- [ ] **End** (default): plays `/MELODIES/timer_end.txt` once on expiry;
      if file absent, fallback RTTTL plays (no crash, no infinite restart)
- [ ] **Countdown**: short beep each of the final 3 seconds + end melody

## B5. Finished modes (includes ReAlert audio — only place this is verified)
- [ ] **AutoClear**: notification dismisses on its own ≈10 s after expiry;
      state returns to Idle
- [ ] **Hold**: notification persists; display blinks 500 ms; dismiss via
      HA `dismiss` button OR physical middle long-press → returns to Idle
- [ ] **Re-Alert**: every ~15 s the buzzer re-plays end melody while
      notification is up; dismissing notification stops re-alerts

## B6. App-switch behavior
- [ ] Start a timer from Idle while viewing the Time app → display
      switches to Timer app within 1 s
- [ ] Start a timer while a game / blocking-nav app is active → display
      does NOT force-switch
- [ ] Start a timer while a non-timer notification is on screen → timer
      notification queues without dropping active one; surfaces after

## B7. Persistence
- [ ] Set `duration=600`, `buzzer=Countdown`, `finished=Hold`, reboot →
      all three survive; state is Idle, remaining = 600
- [ ] Start a 10-min timer, reboot mid-run → state is Idle, duration
      preserved, remaining reset to duration. (Documents intentional
      non-persistence of runtime state.)

## B8. Edge cases
- [ ] `duration=0` over MQTT → clamps to 1 s; one-shot expiry fires cleanly
- [ ] `duration=86400` (24 h) → accepted; display shows hours
- [ ] `duration=86401` → clamped to 86400
- [ ] Send `start` while Running → idempotent; remaining doesn't jump
- [ ] Send `pause` while Idle → no-op, no error
- [ ] Spam `start`/`pause`/`reset` 10×/s for 5 s → device responsive;
      final state consistent with last command
- [ ] Battery-powered device (if applicable): timer survives or fails
      cleanly across short sleep/wake

## B9. Disable flag (SHOW_TIMER)
Verifies the master enable across all four control surfaces. Run after
re-flashing or factory-resetting NVS so `SHOW_TIMER_HA_PREV` starts `true`.

### B9a. dev.json path
- [ ] Put `{"show_timer": false}` in `/dev.json`, reboot
- [ ] Timer app does NOT appear in the app rotation
- [ ] No `timer_*` entities present in HA after discovery completes
- [ ] `POST /api/timer {"action":"start"}` returns 200 OK but state stays Idle
- [ ] MQTT publish to `{prefix}/timer {"action":"start"}` is a no-op
- [ ] Remove the key from `/dev.json`, reboot — app + entities return

### B9b. Web settings path (immediate effect)
- [ ] `GET /api/settings` returns `"TIMER": true` (default)
- [ ] `POST /api/settings {"TIMER":false}` → Timer app removed from rotation
      WITHOUT requiring a reboot
- [ ] A timer that was running at the moment of toggle returns to Idle
      (no end-of-timer alert fires later)
- [ ] HA entities show as "unavailable" until next reboot; on reboot they
      are removed from HA's MQTT integration entirely (empty retained
      discovery payload sent on first MQTT reconnect)
- [ ] `TIMER` value persists across power-cycle (`GET /api/settings` after
      reboot still returns `"TIMER": false`)
- [ ] `POST /api/settings {"TIMER":true}` → app + 8 HA entities + command
      surfaces all return

### B9c. On-device menu path
- [ ] Open menu → **APPS** → scroll to the new entry showing the hourglass
      icon and `ON`/`OFF`
- [ ] Toggle to `OFF` with middle short-press → long-press to exit
- [ ] Power-cycle the device → toggle state persists; app + HA entities
      remain suppressed
- [ ] Re-enable from the menu → app + entities + command surfaces return
      after next MQTT reconnect

### B9d. awtrix2_upgrade build (Wemos D1 Mini32)
- [ ] On `awtrix2_upgrade`, the APPS menu shows exactly 5 entries
      (Time / Date / Temp / Humidity / Timer — no ghost slot, no Battery)
- [ ] Timer toggle is reachable at index 4 and behaves identically to the
      Ulanzi build

---

## Related

- Feature spec: [../timer.md](../timer.md)
- Automated tests: [test/test_timer/](../../test/test_timer/) (native unit),
  [tests/e2e/](../../tests/e2e/) (MQTT harness)
