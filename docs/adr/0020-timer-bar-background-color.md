# Timer bar background color (`bar_bg_color`): always-draw trough, default black, persistent

Status: accepted

## Context

The Timer progress bar drew a single right-anchored line — the draining "remaining"
segment in `bar_color` (`TIMER_BAR_COLOR`, ADR-0004), one `drawFastHLine` in the
painter ([src/Apps.cpp](../../src/Apps.cpp)). The drained portion was simply blank.
The custom-app progress bar, by contrast, draws a full-width **background track**
(`progressBC`) *plus* a foreground fill (`progressC`) via
`DisplayManager::drawProgressBar`. Users wanted the same on the Timer: a background
color behind the bar so the drained portion shows a distinct color instead of nothing.

We added `bar_bg_color` as a persisted color key mirroring `bar_color`: wire key
`bar_bg_color`, dev.json `timer_bar_bg_color`, global `TIMER_BAR_BG_COLOR`, NVS
`"awtrix"` key `TBARBC`, **reusing `parseBarColor`** (numeric `0..0xFFFFFF` or
`"#RRGGBB"`/`"RRGGBB"`) under the ADR-0001 atomic-reject contract, and `inSnapshot=true`
so it rides the run-scoped config mirror and obeys `save:false` one-shot exactly like
`bar_color`. The three decisions below are where it *diverges* from the well-trodden
`bar_color` row and are worth recording.

## Decision

### 1. The track is always drawn, but its default is black (opt-in via a literal color)

The background is painted as the full bar trough whenever the bar is active, then the
foreground draws over it — the app's `drawProgressBar` order. But the NVS/parse default
is `0x000000` (**black**), which on the LED matrix is off pixels, so a device that never
sets `bar_bg_color` renders **identically to before this feature**. The background is
thus opt-in via a literal color rather than via a sentinel/enable flag: the painter
simply skips the track draw when `TIMER_BAR_BG_COLOR == 0`.

The rejected alternative was app parity by default — a non-black (e.g. white) default,
so every existing timer would immediately gain a visible track. That changes the display
for all current users for a purely additive feature; opt-in-via-black keeps the change
invisible until requested while still allowing the full app look once a color is set.

### 2. The background track persists, decoupled from `showBar`

The foreground bar disappears once `barLen` rounds below one cell (the view-model leaves
`showBar=false`) — a window that can be minutes long for a multi-hour timer. The
background trough must **not** vanish with it: like the app's background, it stays drawn
for the whole Running/Paused window. So the view-model ([src/TimerView.cpp](../../src/TimerView.cpp))
sets a new `showBarTrack` flag (plus `barTrackStartX` / `barTrackLen`, the full bar
extent) *inside* the `ts != Idle && barDuration > 0` block but *outside* the `len > 0`
guard. `showBar`/`barLen`/`barStartX` keep their exact existing semantics.

This is the same kind of view-model render decision as [ADR-0005](0005-icon-enabled-reflow-and-view-input.md)
(icon-enabled reflow): geometry that the host tests own lives in the display-free
view-model, not the painter. The rejected alternative was to AND the track into the
existing `showBar`, which is simpler (no new flag) but makes the trough blink out for the
final stretch — defeating the point of a persistent background. Both the track and the
foreground remain gated on `TIMER_BAR_ENABLED` in the painter, so `bar_enabled = false`
still hides the whole bar including its trough.

### 3. The `0`-sentinel is asymmetric between foreground and background

For `bar_color`, `0` is a **sentinel** meaning "follow the text color" (`TEXTCOLOR_888`,
ADR-0004) — a non-black value is still drawn. For `bar_bg_color`, `0` is a **literal**
black = no track. Same number, deliberately different meaning, because the two have
different natural "unset" behaviors: a foreground bar wants to be visible (so default to
the text color), a background trough wants to be removable (so default/`0` to off). The
asymmetry surfaces on the read carriers as `"default"` (foreground) vs `"none"`
(background): `bar_bg_color` carries its own formatter `formatBarBgColor`
([src/TimerSettings.cpp](../../src/TimerSettings.cpp)) rather than reusing
`formatBarColor`, precisely so `0` reads as `"none"` and is not mistaken for the
foreground's text-color sentinel.

## Out of scope

- **No HA control entity and no on-device `TIMER` menu slot** — consistent with
  `bar_color`; a 24-bit color is impractical to edit on the 32×8 panel, and the read
  projection (state-sensor attribute + `GET /api/timer` `config`) is enough.
- `bar_color` semantics, the foreground draw, and the `showBar` foreground gate are
  unchanged.

## Consequences

- `docs/timer.md`, `docs/api.md`, `docs/dev.md` document `bar_bg_color` /
  `timer_bar_bg_color`, its `"none"`/`"#RRGGBB"` carrier-native rendering, and the
  persistent-trough behavior.
- The NVS `"awtrix"` namespace gains key `TBARBC`; `TIMER_SETTINGS_DESC_CAP` bumps to 13.
- The `{id}_timer_state` HA attribute bag and the `GET /api/timer` `config` mirror gain
  `bar_bg_color`; it rides the run-scoped config snapshot and obeys `save:false` one-shot
  (all inherited from `inSnapshot=true`, no extra code).
- `CONTEXT.md` records the foreground-vs-background color pair and the `0`-sentinel
  asymmetry.
- Host tests cover the parse contract (U41, T3), NVS round-trip (T4), snapshot membership
  (T6), the `"none"`/`"#RRGGBB"` formatter (T17b) on the state bag (T15, W19) and the GET
  mirror (T20), and the persistent-trough geometry (D4, including the drained `len==0`
  case and the icon-off reflow).
