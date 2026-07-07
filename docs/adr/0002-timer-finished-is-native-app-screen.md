# Timer Finished is a pinned native-app screen, not a notification

Status: accepted

## Context

When the Timer reaches zero it enters the `Finished` state and must alert the
user. The obvious-looking way to do that is to reuse the firmware's general
notification/overlay mechanism — push an overlay when the timer fires, and
dismiss it when the user reacts. An earlier iteration of the Timer did exactly
that, on a dedicated `"timer"` notification channel with a channel-aware
dismiss, which is why some history and integrations may still reference it.

That approach couples the Timer to the notification subsystem: notifications
need a `channel` field, the dismiss path needs to be channel-aware, and the
Timer has to push and clear overlays — so Timer state becomes entangled with
the shared notification queue. It also pushes render decisions into the
notification/overlay code, which is not host-testable.

## Decision

The `Finished` state is rendered by the **Timer app itself**, not as a
notification:

- `TimerView::compute` emits a `Finished` screen — a blinking `0:00`
  ([src/TimerView.cpp](../../src/TimerView.cpp)) — and the `TimerApp` painter
  pins the Timer app in the app rotation while `Config` or `Finished` is on
  screen ([src/Apps.cpp](../../src/Apps.cpp)). The audible alert is the buzzer
  (`PeripheryManager`), governed by Buzzer/Finished mode.
- The Timer never calls `generateNotification` / `dismissNotify` and never
  tags a notification with a channel. "Clearing" the finished alert is a plain
  state transition — `start` or `reset` — issued from any control surface
  (on-device buttons, `POST /api/timer`, the `{prefix}/timer` MQTT topic, or
  the Home Assistant Start/Reset button entities).
- The Timer's only Home Assistant surface is the descriptor-table-driven entity
  set ([src/TimerHa.h](../../src/TimerHa.h)); the Duration entity is a `text`
  entity (see [ADR-0001](0001-timer-command-validation-parity.md)), not a
  notification entity.

## Consequences

- `TimerView` is free of display, font, and filesystem dependencies, so the
  finished-screen behaviour (blink cadence, display-string selection, bar
  geometry) is host-testable without the notification subsystem (tests D1–D6).
- The Timer carries **no** dependency on the notification/channel
  infrastructure: it compiles and behaves correctly with no `HANotify`, no
  `channel` field on notifications, and no channel-aware dismiss.
- The finished alert does not participate in the notification queue: it can't
  be cleared by a generic `notify/dismiss`, and it neither stacks with nor is
  pre-empted by unrelated notifications. Clearing it is always an explicit
  Timer command.
- **Do not** reroute the finished alert through the notification system "for
  consistency." That re-introduces the coupling this decision removed and
  breaks the host-testability of the view.
