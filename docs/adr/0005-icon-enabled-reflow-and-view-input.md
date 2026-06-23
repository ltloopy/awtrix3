# `icon_enabled`: full-width reflow, view-model input, and on-device toggles

Status: accepted

## Context

The Timer app always drew an 8px icon on the left of every non-Config screen.
When no icon image is configured (the default — `TIMER_ICON_*` default to `""`),
`drawTimerIcon` ([src/Apps.cpp](../../src/Apps.cpp)) falls back to a built-in
hourglass drawn in code, so there was *always* an icon and no way to show none.
Users wanted to suppress the icon region entirely (e.g. for a larger, panel-wide
countdown).

We added `icon_enabled` as a persisted on/off display flag, mirroring `bar_enabled`
(ADR-0004): wire key `icon_enabled`, dev.json `timer_icon_enabled`, global
`TIMER_ICON_ENABLED`, NVS `"awtrix"` key `TICONEN`, strict-bool validation under the
ADR-0001 atomic-reject contract. The decisions worth recording are the three places
this *diverges* from the well-trodden `bar_enabled` pattern.

## Decision

### 1. Hiding the icon triggers a full-width layout (text **and** bar reflow)

Unlike `bar_enabled`, which only hides its element, `icon_enabled = false` reclaims
the vacated space: the time text re-centers over the full 32px panel, and the
progress bar's origin/max-length extend from `kBarX0=9 / 23px` to `0 / 32px`. The
bar's `kBarX0=9` origin only ever existed to clear the icon, so leaving the bar
right-shifted while the text spans full width would be a visible inconsistency. The
right-edge anchor invariant `barStartX + barLen == 32` holds in both layouts
(`9+23` and `0+32`).

### 2. `TimerViewModel::compute` gains an `iconEnabled` input

The icon flag is read **view-side**, not painter-side. `compute(nowMs, iconEnabled)`
sets a new `showIcon` field *and* the reflowed text/bar geometry; the painter reads
`TIMER_ICON_ENABLED` once, passes it in, and honors `view.showIcon`.

This is a deliberate departure from `bar_enabled`, whose flag is AND-ed in the
painter ([src/Apps.cpp](../../src/Apps.cpp)) so the view stays a pure function of
*state + time*. The reason: the bar has a state-based eligibility (running/paused +
duration>0) that the view legitimately owns, with the config flag as a separate
painter-side gate. The icon has **no** state eligibility — it shows on every
non-Config screen — so the config flag *is* the whole visibility decision, and
because the reflow couples visibility to geometry, both must be decided together.
Putting them in the view keeps a single source of truth and keeps the geometry
host-testable (the view's stated reason to exist). The flag is passed as an input
(default `true`) rather than read from a global inside `compute`, preserving the
view as a pure function and leaving the ~existing `compute(nowMs)` test call sites
unchanged.

**Warning for future readers:** do not "simplify" the icon to match the bar's
painter-side pattern — it would break the reflow. And note the bar's max length is
now `iconEnabled`-dependent.

### 3. Two on-device `TIMER` menu toggle slots (`ICON` + `PROGRESS BAR`)

The on-device `TIMER` submenu grows from five to seven slots, appending `ICON`
(`ICON ON`/`ICON OFF`) and `PROGRESS BAR` (`PROGRESS BAR ON`/`PROGRESS BAR OFF`) boolean toggles where left and
right both flip the value. This **supersedes ADR-0004's deferral** of the
`bar_enabled` menu slot ("`bar_enabled` is a binary that would fit on-device, but
expanding the menu to six slots was deferred — revisit if real demand emerges") —
demand emerged. Changes are visible immediately (the painter reads the globals live)
and persist on the existing long-press `saveSettings()`; no MQTT publish, matching
the numeric knobs and `bar_enabled`.

## Out of scope

- **No HA entity** and **not echoed in `GET /api/timer`** — consistent with
  `bar_enabled`; display preferences and config flags are not part of the HA surface
  or the observation snapshot.
- `TIMER_BAR_COLOR` and the other ADR-0004 keys are unchanged.

## Consequences

- `docs/api.md`, `docs/timer.md`, `docs/dev.md`, and `docs/onscreen.md` document
  `icon_enabled` / `timer_icon_enabled` and the seven-slot `TIMER` menu.
- The NVS `"awtrix"` namespace gains key `TICONEN`.
- `CONTEXT.md` distinguishes the `_enabled` display-element toggles from the
  `icon_<state>` image-selection family and records the seven-slot on-device surface.
- Host tests cover the strict-bool contract (`test_U50`) and the reflow geometry
  for text and bar (`test_D7`).
