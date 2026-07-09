# Timer Compile-Time Build Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-out compile-time flag `AWTRIX_DISABLE_TIMER` that removes the entire native Timer feature (app, on-device menu, MQTT/HTTP API, HA entities, multi-device sync) from the firmware, reclaiming flash and RAM. Default builds are unchanged.

**Architecture:** Raw `#ifndef AWTRIX_DISABLE_TIMER … #endif` preprocessor guards, matching the codebase's existing `#ifndef awtrix2_upgrade` convention. 16 timer-owned `.cpp` files get whole-file guards; 7 shared files get call-site guards. No `platformio.ini` changes; the flag is passed via `build_flags` or the `PLATFORMIO_BUILD_FLAGS` env var.

**Tech Stack:** C++ (Arduino/ESP32), PlatformIO. Spec: `docs/superpowers/specs/2026-07-08-timer-build-gate-design.md`.

## Global Constraints

- Flag name is exactly `AWTRIX_DISABLE_TIMER`; guards are always `#ifndef AWTRIX_DISABLE_TIMER` (never a normalized second macro — several files include timer headers before `Globals.h`, and an undefined macro in `#if` silently evaluates to 0).
- Default build (no flag) must produce byte-equivalent behavior: every guard only *removes* code when the flag is defined.
- `src/timer.cpp` / `src/timer.h` (`timer_tick`, `timer_localtime`, `timer_time`) are the **pre-existing upstream time helpers, NOT the Timer feature** — never guard them.
- `SHOW_TIMER`, `SHOW_TIMER_HA_PREV`, `TIMER_ICON_ENABLED`, `TIMER_ICON_IDLE/RUNNING/PAUSED/FINISHED` in `Globals.h`/`Globals.cpp` stay ungated (a few bytes; not worth spreading `#if` into settings persistence). The 13 table-backed `TIMER_*` externs in `Globals.h` also stay — their *definitions* live in the gated `TimerSettings.cpp`, and every reference is inside a gated region.
- No `platformio.ini` edits on `timer-upstream`.
- Working branch: `timer-upstream`. Base commit before this work: run `git rev-parse HEAD` in Task 1 and record it (expected `dd9caff`).
- Windows/PowerShell. Default build: `pio run -e ulanzi`. Disabled build: `$env:PLATFORMIO_BUILD_FLAGS='-DAWTRIX_DISABLE_TIMER'; pio run -e ulanzi; Remove-Item Env:\PLATFORMIO_BUILD_FLAGS`. Changing flags forces a full rebuild (~minutes), so disabled builds run only where the plan says.
- Intermediate commits use a `gate:` prefix; Task 11 squashes them into one `feat:` commit.

---

### Task 1: Baseline measurement

**Files:** none modified.

**Interfaces:**
- Produces: recorded base SHA and baseline flash/RAM numbers used by Task 10's delta report.

- [ ] **Step 1: Record base SHA**

Run: `git rev-parse HEAD`
Expected: a SHA (expected `dd9caff…`). Save it — Task 11's squash resets to it.

- [ ] **Step 2: Build the default firmware and record sizes**

Run: `pio run -e ulanzi`
Expected: SUCCESS, ending with lines like:

```
RAM:   [==        ]  xx.x% (used xxxxx bytes from 327680 bytes)
Flash: [==========]  99.x% (used xxxxxxx bytes from xxxxxxx bytes)
```

Record both `used … bytes` numbers verbatim (e.g. in a scratch note). These are the timer-ON baseline.

---

### Task 2: Whole-file guards on the 16 timer-owned .cpp files (red)

**Files:**
- Modify: `src/TimerCommand.cpp`, `src/TimerConfigEditor.cpp`, `src/TimerEnums.cpp`, `src/TimerHa.cpp`, `src/TimerHaHost.cpp`, `src/TimerManager.cpp`, `src/TimerMenu.cpp`, `src/TimerMenuNav.cpp`, `src/TimerRuntime.cpp`, `src/TimerSettings.cpp`, `src/TimerSettingsApply.cpp`, `src/TimerView.cpp`, `src/SyncEnvelope.cpp`, `src/SyncSeenCache.cpp`, `src/SyncTargetsDebounce.cpp`, `src/PeerRegistry.cpp`

**Interfaces:**
- Produces: with `-DAWTRIX_DISABLE_TIMER`, none of these translation units emit any code; Tasks 3–9 remove the now-dangling references from shared files.

- [ ] **Step 1: Wrap each of the 16 files**

For every file in the list: insert as the **very first line**

```cpp
#ifndef AWTRIX_DISABLE_TIMER
```

and as the **very last line** (after the final line of code, with a trailing newline)

```cpp
#endif // AWTRIX_DISABLE_TIMER
```

The wrap encloses the file's `#include`s too — that is intentional (skips heavy headers like ArduinoHA in disabled builds). Headers (`.h`) are NOT touched in this task.

- [ ] **Step 2: Verify the default build is unaffected**

Run: `pio run -e ulanzi`
Expected: SUCCESS with flash/RAM identical to Task 1's baseline.

- [ ] **Step 3: Verify the disabled build now fails (proves the guards bite)**

Run: `$env:PLATFORMIO_BUILD_FLAGS='-DAWTRIX_DISABLE_TIMER'; pio run -e ulanzi; Remove-Item Env:\PLATFORMIO_BUILD_FLAGS`
Expected: **FAIL** at link with undefined references to timer symbols (e.g. `TimerManager`, `TimerMenuNav::…`). This failure is the "red" state; Tasks 3–9 turn it green. If it *succeeds*, the guards didn't take — stop and investigate.

- [ ] **Step 4: Commit**

```powershell
git add src/Timer*.cpp src/Sync*.cpp src/PeerRegistry.cpp
git commit -m "gate: whole-file AWTRIX_DISABLE_TIMER guards on timer-owned sources"
```

---

### Task 3: main.cpp and Globals.cpp call-site guards

**Files:**
- Modify: `src/main.cpp` (includes at lines 38, 43; calls at 74, 106, 126–127, 132)
- Modify: `src/Globals.cpp` (include at line 3; call at ~line 215–218)

**Interfaces:**
- Consumes: flag name `AWTRIX_DISABLE_TIMER` (Global Constraints).

- [ ] **Step 1: Guard the two timer includes in main.cpp**

`#include "TimerHaHost.h"` (line 38) and `#include "TimerManager.h"` (line 43) each become:

```cpp
#ifndef AWTRIX_DISABLE_TIMER
#include "TimerHaHost.h"
#endif
```

```cpp
#ifndef AWTRIX_DISABLE_TIMER
#include "TimerManager.h"
#endif
```

Do NOT touch `#include "timer.h"` (line 42 — time helper).

- [ ] **Step 2: Guard the four call-sites in main.cpp**

In `setup()`:

```cpp
#ifndef AWTRIX_DISABLE_TIMER
  TimerManager.setup();
#endif
```

```cpp
#ifndef AWTRIX_DISABLE_TIMER
        TimerHaHost.reconcile();
#endif
```

In `loop()` (`timer_tick();` on line 123 stays untouched):

```cpp
#ifndef AWTRIX_DISABLE_TIMER
  TimerManager.tick();
  TimerManager.tickPresence(millis());   // peer presence beacon + registry aging
#endif
```

```cpp
#ifndef AWTRIX_DISABLE_TIMER
    TimerHaHost.refreshTargets(millis());   // dynamic HA Targets select republish
#endif
```

- [ ] **Step 3: Guard Globals.cpp**

Line 3:

```cpp
#ifndef AWTRIX_DISABLE_TIMER
#include "TimerSettings.h"
#endif
```

The dev.json apply call (~line 215; keep the comment inside the guard):

```cpp
#ifndef AWTRIX_DISABLE_TIMER
        // Timer value-config keys: validated + applied per-key best-effort from the
        // single TIMER_SETTINGS_DESCS table (ranges live there, once). dev.json is a
        // boot override layer, so an invalid key is skipped, not atomic-rejected.
        timerSettingsLoadDevJson(doc.as<JsonObjectConst>());
#endif
```

Leave the `show_timer` and `timer_icon_*` dev.json blocks (lines ~210–225) ungated — they only touch Globals-owned variables.

Also guard the two NVS table calls (added post-plan; found by the first disabled-build link — lowercase names escaped the original grep): `timerSettingsLoadNvs(Settings);` in `loadSettings()` (~line 311) and `timerSettingsSaveNvs(Settings);` in `saveSettings()` (~line 364), each wrapped alone with its trailing comment; the neighboring `Settings.remove("TSTEP")` and `SHOW_TIMER`/`TIMERPREV` persistence lines stay ungated.

- [ ] **Step 4: Verify default build**

Run: `pio run -e ulanzi`
Expected: SUCCESS.

- [ ] **Step 5: Commit**

```powershell
git add src/main.cpp src/Globals.cpp
git commit -m "gate: main + Globals call-site guards"
```

---

### Task 4: Apps.h / Apps.cpp guards

**Files:**
- Modify: `src/Apps.h` (line 88), `src/Apps.cpp` (includes 14–15; block 420–549)

- [ ] **Step 1: Guard the TimerApp declaration in Apps.h**

```cpp
#ifndef AWTRIX_DISABLE_TIMER
void TimerApp(FastLED_NeoMatrix *matrix, MatrixDisplayUiState *state, int16_t x, int16_t y, GifPlayer *gifPlayer);
#endif
```

(Same shape as the `BatApp` guard four lines above it.)

- [ ] **Step 2: Guard the includes in Apps.cpp**

Lines 14–15 (`timer.h` on line 13 stays):

```cpp
#ifndef AWTRIX_DISABLE_TIMER
#include "TimerManager.h"
#include "TimerView.h"
#endif
```

- [ ] **Step 3: Guard the timer painter block in Apps.cpp**

One guard around the whole contiguous region from the anonymous namespace (line 420, `namespace {` with the `kTimerTextY` constants) through the closing brace of `TimerApp` (line 549). Insert `#ifndef AWTRIX_DISABLE_TIMER` on the line before `namespace {` and `#endif // AWTRIX_DISABLE_TIMER` on the line after `TimerApp`'s closing `}`. The region covers: the `kTimer*` constants namespace, `drawTimerIcon()`, and `TimerApp()`.

- [ ] **Step 4: Verify default build**

Run: `pio run -e ulanzi`
Expected: SUCCESS.

- [ ] **Step 5: Commit**

```powershell
git add src/Apps.h src/Apps.cpp
git commit -m "gate: Apps timer painter guards"
```

---

### Task 5: DisplayManager.cpp guards

**Files:**
- Modify: `src/DisplayManager.cpp` (includes at lines 8, 26; app registration at 1118; settings block at 2135 and 2260–2272)

- [ ] **Step 1: Guard the two includes**

`#include "TimerHaHost.h"` (line 8) and `#include "TimerManager.h"` (line 26), each wrapped exactly like in Task 3 Step 1.

- [ ] **Step 2: Guard the app registration in loadNativeApps()**

```cpp
#ifndef AWTRIX_DISABLE_TIMER
  updateApp("Timer", TimerApp, SHOW_TIMER, Apps.size());
#endif
```

- [ ] **Step 3: Guard the SHOW_TIMER change handling in setsettings()**

Line 2135:

```cpp
#ifndef AWTRIX_DISABLE_TIMER
  bool prevShowTimer = SHOW_TIMER;
#endif
```

(Line 2141 `SHOW_TIMER = doc.containsKey("TIMER") …` stays ungated — plain Globals write.)

Lines 2260–2272:

```cpp
#ifndef AWTRIX_DISABLE_TIMER
  TimerManager.onShowTimerChange(prevShowTimer, SHOW_TIMER);
  if (prevShowTimer && !SHOW_TIMER)
  {
    TimerHaHost.remove();
  }
  else if (!prevShowTimer && SHOW_TIMER)
  {
    TimerHaHost.enable();
  }
  if (prevShowTimer != SHOW_TIMER)
  {
    SHOW_TIMER_HA_PREV = SHOW_TIMER;
  }
#endif
```

- [ ] **Step 4: Verify default build**

Run: `pio run -e ulanzi`
Expected: SUCCESS.

- [ ] **Step 5: Commit**

```powershell
git add src/DisplayManager.cpp
git commit -m "gate: DisplayManager guards"
```

---

### Task 6: PeripheryManager.cpp guards

**Files:**
- Modify: `src/PeripheryManager.cpp` (includes at lines 21–22; button blocks at 179–193 and 227–247)

- [ ] **Step 1: Guard the includes**

Lines 21–22:

```cpp
#ifndef AWTRIX_DISABLE_TIMER
#include "TimerManager.h"
#include "TimerCommand.h"        // TimerCommand::Action for the runStateAction seam
#endif
```

- [ ] **Step 2: Guard the timer branch in select_button_pressed()**

Wrap the whole `if (!MenuManager.inMenu) { … }` block (lines 179–193):

```cpp
#ifndef AWTRIX_DISABLE_TIMER
        if (!MenuManager.inMenu)
        {
            TimerState ts = TimerManager.getState();
            if (ts == TimerState::Finished)
            {
                TimerManager.runStateAction(TimerCommand::Action::Reset);
                return;
            }
            if (CURRENT_APP == "Timer")
            {
                if (ts == TimerState::Running) { TimerManager.runStateAction(TimerCommand::Action::Pause); }
                else                           { TimerManager.runStateAction(TimerCommand::Action::Start); }
                return;
            }
        }
#endif
```

- [ ] **Step 3: Guard the timer branch in select_button_pressed_long()**

Wrap the analogous `if (!MenuManager.inMenu) { … }` block (lines 227–247) the same way — the block containing `TimerState ts`, the `Finished`→`Start` branch, `MenuManager.openTimerMenuFromApp()`, and the `Reset` branch.

- [ ] **Step 4: Verify default build**

Run: `pio run -e ulanzi`
Expected: SUCCESS.

- [ ] **Step 5: Commit**

```powershell
git add src/PeripheryManager.cpp
git commit -m "gate: PeripheryManager button-seam guards"
```

---

### Task 7: ServerManager.h / ServerManager.cpp guards

**Files:**
- Modify: `src/ServerManager.h` (lines 19–21), `src/ServerManager.cpp` (include 17; globals 28–30; handlers 127–139; 275; 335–346; 387–401)

- [ ] **Step 1: Guard the declaration in ServerManager.h**

```cpp
#ifndef AWTRIX_DISABLE_TIMER
    // Broadcast a timer-sync packet on the LAN (propagation surface). Sent 3x for
    // best-effort delivery; receivers dedup by (src,seq).
    void sendTimerSync(const String &payload);
#endif
```

- [ ] **Step 2: Guard include and globals in ServerManager.cpp**

Line 17 `#include "TimerManager.h"` wrapped as usual. Lines 28–30:

```cpp
#ifndef AWTRIX_DISABLE_TIMER
WiFiUDP syncUdp;
const uint16_t kTimerSyncPort = 4212;
char syncBuffer[1024];
#endif
```

- [ ] **Step 3: Guard the /api/timer handlers (lines 127–139)**

Wrap both handlers (POST and GET, including the "Observation surface" comment) in one guard:

```cpp
#ifndef AWTRIX_DISABLE_TIMER
    mws.addHandler("/api/timer", HTTP_POST, []()
                   { … });          // (existing body unchanged)
    // Observation surface (read-only): always 200; `enabled` carries SHOW_TIMER.
    mws.addHandler("/api/timer", HTTP_GET, []()
                   { … });          // (existing body unchanged)
#endif
```

(Keep the existing bodies verbatim; only add the two guard lines around them.)

- [ ] **Step 4: Guard the sync socket lifecycle**

Line 275:

```cpp
#ifndef AWTRIX_DISABLE_TIMER
        syncUdp.begin(kTimerSyncPort);
#endif
```

Lines 335–346 in `tick()` (comment included):

```cpp
#ifndef AWTRIX_DISABLE_TIMER
        // Propagation surface: inbound timer-sync packets (echo/follow/target/dedup
        // gating happens inside applySyncCommand).
        int syncSize = syncUdp.parsePacket();
        if (syncSize > 0)
        {
            int len = syncUdp.read(syncBuffer, sizeof(syncBuffer) - 1);
            if (len > 0)
            {
                syncBuffer[len] = 0;
                TimerManager.applySyncCommand(syncBuffer);
            }
        }
#endif
```

Whole `sendTimerSync` definition (lines 387–401) wrapped in the same guard pair.

- [ ] **Step 5: Verify default build**

Run: `pio run -e ulanzi`
Expected: SUCCESS.

- [ ] **Step 6: Commit**

```powershell
git add src/ServerManager.h src/ServerManager.cpp
git commit -m "gate: ServerManager HTTP + sync-UDP guards"
```

---

### Task 8: MQTTManager.h / MQTTManager.cpp guards

**Files:**
- Modify: `src/MQTTManager.h` (include 7; declarations 43–50, 53), `src/MQTTManager.cpp` (includes 12–14; dispatch 107–117; delegations 257, 281, 290; purge 419–425; topic 452; onConnected 474–476; setup 783–787; wire seam 812–857)

- [ ] **Step 1: Guard MQTTManager.h**

Line 7:

```cpp
#ifndef AWTRIX_DISABLE_TIMER
#include "TimerHa.h"
#endif
```

Declarations (keep the wire-seam comment inside the guard):

```cpp
#ifndef AWTRIX_DISABLE_TIMER
    // The Timer wire seam: …(existing comment unchanged)…
    void publishTimerWire(const char *topic, const char *payload);
    String timerWireTopic(TimerHaEntity slot);
    String timerWireAttrTopic(TimerHaEntity slot);
    String timerIconsTopic();
#endif
```

And:

```cpp
#ifndef AWTRIX_DISABLE_TIMER
void reconcileTimerHAState();
#endif
```

`kMaxHAEntities = 40` stays as-is (const, negligible; its comment references Timer but the value must not change per-build — the HA registration-order contract).

- [ ] **Step 2: Guard MQTTManager.cpp includes**

Lines 12–14:

```cpp
#ifndef AWTRIX_DISABLE_TIMER
#include "TimerManager.h"
#include "TimerHa.h"
#include "TimerHaHost.h"
#endif
```

- [ ] **Step 3: Guard the /timer command dispatch (lines 107–117)**

Wrap the whole `{ size_t plen … }` block:

```cpp
#ifndef AWTRIX_DISABLE_TIMER
    {
        size_t plen = MQTT_PREFIX.length();
        const char *t = strTopic.c_str();
        if (strTopic.length() > plen
            && strncmp(t, MQTT_PREFIX.c_str(), plen) == 0
            && strcmp(t + plen, "/timer") == 0)
        {
            TimerManager.parseCommand(payloadCopy.c_str());
            return;
        }
    }
#endif
```

- [ ] **Step 4: Guard the three TimerHaHost delegations**

Each single line (257, 281, 290) individually:

```cpp
#ifndef AWTRIX_DISABLE_TIMER
    if (TimerHaHost.tryHandleButton(sender)) return;   // Timer carrier: routed by the host
#endif
```

(same pattern for `tryHandleSwitch` and `tryHandleSelect`).

- [ ] **Step 5: Guard the retained-command purge and subscription**

Lines 419–425 (comment included):

```cpp
#ifndef AWTRIX_DISABLE_TIMER
    // Command topics must never carry a retained payload. …(existing comment unchanged)…
    mqtt.publish((MQTT_PREFIX + "/timer").c_str(), "", true);
#endif
```

In the `topics[]` array, the last entry (line 452):

```cpp
        "/r2d2",
#ifndef AWTRIX_DISABLE_TIMER
        "/timer",
#endif
    };
```

(A trailing comma before `};` is legal C++; adjust the preceding `"/r2d2"};` line accordingly.)

- [ ] **Step 6: Guard onConnected + setup hooks**

Lines 474–476 (comment inside guard):

```cpp
#ifndef AWTRIX_DISABLE_TIMER
        // onConnected() also flushes the pending discovery cleanup reconcile() latched
        // when SHOW_TIMER went off across a reboot.
        TimerHaHost.onConnected();   // Timer wire + attribute groups when SHOW_TIMER
#endif
```

Lines 783–787 (comment inside guard):

```cpp
#ifndef AWTRIX_DISABLE_TIMER
        // Resolve the Timer carrier ids and (when SHOW_TIMER) construct/register the
        // carriers — …(existing comment unchanged)…
        TimerHaHost.setup();
#endif
```

- [ ] **Step 7: Guard the wire-seam definitions (lines 812–857)**

One guard pair around the four functions `publishTimerWire`, `timerWireTopic`, `timerWireAttrTopic`, `timerIconsTopic` including their leading comments: `#ifndef AWTRIX_DISABLE_TIMER` before the first comment (line 812), `#endif // AWTRIX_DISABLE_TIMER` after `timerIconsTopic`'s closing brace (line 857).

- [ ] **Step 8: Verify default build**

Run: `pio run -e ulanzi`
Expected: SUCCESS.

- [ ] **Step 9: Commit**

```powershell
git add src/MQTTManager.h src/MQTTManager.cpp
git commit -m "gate: MQTTManager dispatch + HA carrier + wire-seam guards"
```

---

### Task 9: MenuManager.h / MenuManager.cpp guards

**Files:**
- Modify: `src/MenuManager.h` (lines 21–24), `src/MenuManager.cpp` (includes 8–13; enum 30; menuItems 48; globals 91–121; menutext 236–245 and 259–295; rightButton 357–367; leftButton 430–438; selectButton 473–477, 488–510, 526–543; selectButtonLong 607–635; openTimerMenuFromApp 647–654; appsCount 85–89)

This is the deepest integration. The `MainMenu` mapping `currentState = (MenuState)(menuIndex + 1)` depends on `menuItems[]` order matching the `MenuState` enum order — so the enum value and the `"TIMER"` string MUST be guarded together.

- [ ] **Step 1: Guard the declaration in MenuManager.h**

```cpp
#ifndef AWTRIX_DISABLE_TIMER
    // Open the TIMER menu directly from the Timer app's idle long-press:
    // list focus, first item, origin = App so a long-press out of the
    // list returns to the Timer app rather than the main menu.
    void openTimerMenuFromApp();
#endif
```

- [ ] **Step 2: Guard the includes in MenuManager.cpp**

Lines 8–11 and 13 (`timer.h` on line 7 and `MQTTManager.h` on line 12 stay):

```cpp
#include "timer.h"
#ifndef AWTRIX_DISABLE_TIMER
#include "TimerManager.h"
#include "TimerMenu.h"
#include "TimerMenuNav.h"
#include "TimerConfigEditor.h"
#endif
#include "MQTTManager.h"
#ifndef AWTRIX_DISABLE_TIMER
#include "TimerHaHost.h"
#endif
```

(Or reorder so `TimerHaHost.h` joins the first guard block — either is fine; keep `MQTTManager.h` ungated.)

- [ ] **Step 3: Guard the enum value and menu label together**

```cpp
    TempMenu,
#ifndef AWTRIX_DISABLE_TIMER
    TimerConfigMenu,
#endif
    Appmenu,
```

```cpp
    "TEMP",
#ifndef AWTRIX_DISABLE_TIMER
    "TIMER",
#endif
    "APPS",
```

(`menuItemCount = MaxMenu - 1` self-adjusts because the enum shrinks.)

- [ ] **Step 4: Guard the timer-menu globals and helpers (lines 91–121)**

One guard pair around: `timerConfigCount`, `timerNav`, `timerDurationEditor`, `timerNavOnDuration()`, `commitTimerMenu()` — from line 91 (`uint8_t timerConfigCount …`) through line 121 (closing brace of `commitTimerMenu`), comments included.

- [ ] **Step 5: Guard appsCount (lines 85–89)**

Replace:

```cpp
#ifndef awtrix2_upgrade
uint8_t appsCount = 6;
#else
uint8_t appsCount = 5;
#endif
```

with:

```cpp
#ifndef AWTRIX_DISABLE_TIMER
#ifndef awtrix2_upgrade
uint8_t appsCount = 6;
#else
uint8_t appsCount = 5;
#endif
#else
#ifndef awtrix2_upgrade
uint8_t appsCount = 5;
#else
uint8_t appsCount = 4;
#endif
#endif
```

- [ ] **Step 6: Restructure the Appmenu timer entry in menutext() (lines 236–245)**

Replace:

```cpp
#ifndef awtrix2_upgrade
        case 4:
            DisplayManager.drawBMP(0, 0, icon_1486, 8, 8);
            return SHOW_BAT ? "ON" : "OFF";
        case 5:
#else
        case 4:
#endif
            DisplayManager.drawBMP(0, 0, icon_timer, 8, 8);
            return SHOW_TIMER ? "ON" : "OFF";
```

with:

```cpp
#ifndef awtrix2_upgrade
        case 4:
            DisplayManager.drawBMP(0, 0, icon_1486, 8, 8);
            return SHOW_BAT ? "ON" : "OFF";
#endif
#ifndef AWTRIX_DISABLE_TIMER
#ifndef awtrix2_upgrade
        case 5:
#else
        case 4:
#endif
            DisplayManager.drawBMP(0, 0, icon_timer, 8, 8);
            return SHOW_TIMER ? "ON" : "OFF";
#endif
```

- [ ] **Step 7: Guard `case TimerConfigMenu:` in menutext() (lines 259–295)**

One guard pair around the whole case block (from `case TimerConfigMenu:` through `return timerMenuValue(timerNav.index());` just before `default:`).

- [ ] **Step 8: Guard the rightButton/leftButton cases**

Lines 357–367 and 430–438 — each `case TimerConfigMenu: { … }` block wrapped in its own guard pair.

- [ ] **Step 9: Guard the selectButton() sites**

Lines 473–477 (the nested `case TimerConfigMenu:` inside the MainMenu dispatch, comment included) — guard pair.

Lines 488–510 (the outer `case TimerConfigMenu:` short-press block) — guard pair.

Lines 526–543 (Appmenu timer toggle) — same restructure as Step 6:

```cpp
#ifndef awtrix2_upgrade
        case 4:
            SHOW_BAT = !SHOW_BAT;
            break;
#endif
#ifndef AWTRIX_DISABLE_TIMER
#ifndef awtrix2_upgrade
        case 5:
#else
        case 4:
#endif
        {
            bool prev = SHOW_TIMER;
            SHOW_TIMER = !SHOW_TIMER;
            TimerManager.onShowTimerChange(prev, SHOW_TIMER);
            if (prev && !SHOW_TIMER)
                TimerHaHost.remove();
            else if (!prev && SHOW_TIMER)
                TimerHaHost.enable();
            break;
        }
#endif
```

- [ ] **Step 10: Guard selectButtonLong() and openTimerMenuFromApp()**

Lines 607–635: guard pair around the `case TimerConfigMenu: { … }` block.

Lines 647–654: guard pair around the entire `openTimerMenuFromApp()` definition (comment included).

- [ ] **Step 11: Verify default build**

Run: `pio run -e ulanzi`
Expected: SUCCESS, sizes still equal to baseline.

- [ ] **Step 12: Commit**

```powershell
git add src/MenuManager.h src/MenuManager.cpp
git commit -m "gate: MenuManager state-machine guards"
```

---

### Task 10: Green — verify all four build variants, measure deltas

**Files:** none modified.

**Interfaces:**
- Consumes: Task 1's baseline numbers.
- Produces: measured flash/RAM deltas for the commit message (Task 11) and PR body (Task 12).

- [ ] **Step 1: Disabled ulanzi build**

Run: `$env:PLATFORMIO_BUILD_FLAGS='-DAWTRIX_DISABLE_TIMER'; pio run -e ulanzi`
Expected: **SUCCESS** (this was failing since Task 2). Record RAM/Flash used bytes.

- [ ] **Step 2: Disabled awtrix2_upgrade build**

Run: `pio run -e awtrix2_upgrade` (env var still set)
Expected: SUCCESS. This exercises the `#else` arms of the nested MenuManager guards.

- [ ] **Step 3: Default builds of both envs**

Run: `Remove-Item Env:\PLATFORMIO_BUILD_FLAGS; pio run -e ulanzi; pio run -e awtrix2_upgrade`
Expected: SUCCESS ×2, ulanzi sizes equal to Task 1 baseline.

- [ ] **Step 4: Compute and record deltas**

Baseline minus disabled, for flash and RAM, on ulanzi. Write them down — they go in the squashed commit message and both PR bodies. Sanity check: flash delta should be tens of kB (the timer stack incl. HA discovery + sync); RAM delta several kB (singletons + 1 kB sync buffer + WiFiUDP).

---

### Task 11: Docs paragraph, squash, push (timer-upstream)

**Files:**
- Modify: `docs/timer.md`

- [ ] **Step 1: Add the build-time opt-out section**

Append at the end of `docs/timer.md` (after the "Settings and persistence" section):

```markdown
## Build-time opt-out

The Timer feature is included by default. To build a firmware without it —
reclaiming roughly <FLASH_DELTA> of flash and <RAM_DELTA> of RAM on
space-constrained devices — add `-DAWTRIX_DISABLE_TIMER` to `build_flags`
in `platformio.ini`, or set `PLATFORMIO_BUILD_FLAGS=-DAWTRIX_DISABLE_TIMER`
when invoking `pio run`. A disabled build has no Timer app, no `TIMER`
on-device menu entry, no `/api/timer` HTTP endpoint, no `{prefix}/timer`
MQTT topic, no Home Assistant timer entities, and no multi-device sync.
The runtime `SHOW_TIMER` setting is only meaningful in default builds.
```

Replace `<FLASH_DELTA>`/`<RAM_DELTA>` with Task 10's measured numbers (e.g. "60 kB" / "6 kB") — do not leave the placeholders.

- [ ] **Step 2: Squash the gate commits into one**

```powershell
git add docs/timer.md
git commit -m "gate: docs"
git reset --soft <BASE_SHA_FROM_TASK_1>
git commit -m "feat: AWTRIX_DISABLE_TIMER compile-time opt-out for the Timer feature

Building with -DAWTRIX_DISABLE_TIMER excludes the entire Timer stack
(app, on-device menu, MQTT/HTTP API, HA entities, multi-device sync):
<FLASH_DELTA> flash and <RAM_DELTA> RAM reclaimed on ulanzi. Default
builds are unchanged. Guards follow the existing #ifndef awtrix2_upgrade
convention.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

(Fill in the measured deltas.) Verify with `git log --oneline -3` and `git status` (clean, except the untracked `docs/superpowers/` files — those stay untracked here on purpose).

- [ ] **Step 3: Push**

Run: `git push origin timer-upstream`
Expected: fast-forward push (one new commit on top of `dd9caff`).

---

### Task 12: Fork port — stacked PR + CI variant build

**Files:**
- Modify (on fork branch): same source files as Tasks 2–9, `.github/workflows/test.yml`
- Create (on fork branch): `docs/superpowers/specs/2026-07-08-timer-build-gate-design.md`, `docs/superpowers/plans/2026-07-08-timer-build-gate.md` (commit the session's spec + this plan)

**Interfaces:**
- Consumes: the squashed gate commit SHA from Task 11; measured deltas from Task 10.

- [ ] **Step 1: Identify the fork timer chain tip**

Run: `gh pr list -R ltloopy/awtrix3 --state open --json number,title,headRefName,baseRefName`
The chain tip is the open stacked PR whose head branch is not the base of any other open PR (per memory, expected to be PR #227's head; verify, don't assume). ALWAYS pass `-R ltloopy/awtrix3` — bare `gh` resolves to upstream Blueforcer/awtrix3.

- [ ] **Step 2: Branch and cherry-pick**

```powershell
git checkout -b feat-timer-build-gate origin/<CHAIN_TIP_BRANCH>
git cherry-pick <GATE_COMMIT_SHA>
```

Resolve conflicts if the chain tip diverges from timer-upstream (likely candidates: PeripheryManager/TimerManager seam files). Preserve the guard placement semantics, not the exact hunks.

- [ ] **Step 3: Add the CI variant build**

In `.github/workflows/test.yml`, `device_build` job, after the "Build awtrix2_upgrade firmware" step, add:

```yaml
      - name: Build ulanzi firmware (timer disabled)
        run: pio run -e ulanzi
        env:
          PLATFORMIO_BUILD_FLAGS: -DAWTRIX_DISABLE_TIMER
```

- [ ] **Step 4: Commit the docs + CI**

```powershell
git add .github/workflows/test.yml docs/superpowers/
git commit -m "ci: build ulanzi with AWTRIX_DISABLE_TIMER; add build-gate spec/plan

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

- [ ] **Step 5: Verify on the fork branch**

```powershell
pio test -e native -f test_timer
$env:PLATFORMIO_BUILD_FLAGS='-DAWTRIX_DISABLE_TIMER'; pio run -e ulanzi; Remove-Item Env:\PLATFORMIO_BUILD_FLAGS
pio run -e ulanzi
```

Expected: native timer tests PASS (they never define the flag), disabled and default device builds SUCCEED. If cc1plus OOMs on native, set `PLATFORMIO_BUILD_JOBS=1`. Remember the stub gotcha: if anything under the native stubs was touched, clean `.pio/build/native` first.

- [ ] **Step 6: Push and open the stacked PR**

```powershell
git push -u origin feat-timer-build-gate
gh pr create -R ltloopy/awtrix3 --base <CHAIN_TIP_BRANCH> --head feat-timer-build-gate --title "feat: AWTRIX_DISABLE_TIMER compile-time opt-out for the Timer feature" --body "<body>"
```

PR body: one paragraph on the flag and polarity (default ON, opt-out), the measured flash/RAM deltas from Task 10, the CI variant-build addition, a link/reference to the spec file in `docs/superpowers/specs/`, and the footer:

```
🤖 Generated with [Claude Code](https://claude.com/claude-code)
```

Expected: PR created against the chain tip (stacked, left open per the chain convention — do not merge).
