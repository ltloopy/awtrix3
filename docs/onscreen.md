# Onscreen menu

AWTRIX 3 provides a **onscreen menu** directly on your clock.  
Press and hold the middle button for 2 seconds to access the menu.   
Navigate through the items with the left and right buttons and choose the submenu with a push on the middle button.  
Hold down the middle button for 2s to exit the current menu and to save your setting.  

Any menu label too wide for the 32px panel **scrolls**: it holds briefly at the left so you can read the beginning, then scrolls left and loops continuously while the item stays selected (the same scroll speed as notifications and apps). Labels that fit stay centered and still. The scroll restarts from the left whenever you move to another item or re-open the menu.
  
!>  You can easily turn your AWTRIX matrix on or off by simply double-pressing the middle button if youre not in Menu.
    
| Menu Item | Description |
| --- | --- |
| `BRIGHT` | Allows adjustment of the brightness of the display. Switch between Auto and manual brightnesscontrol with the middle button. |
| `COLOR` | Allows selection of one of 13 different colors for text. Hex values displayed.  |
| `SWITCH` | Determines if pages should automatically switch. |
| `T-SPEED` | Adjusts transition speed between apps. |
| `APPTIME` | Adjusts duration of app display before switching to next. |
| `TIME` | Allows selection of time format. |
| `DATE` | Allows selection of date format. |
| `WEEKDAY` | Allows selection of start of week. |
| `TEMP` | Allows selection of temperature system (°C or °F).  |
| `TIMER` | Configure the Timer app. A **drill-in list** consistent with the rest of the menu: left/right walks the named items (and **wraps**), a short press drills into the highlighted item's editor, and a long press steps back up one level. You can open it from the main menu or by long-pressing the middle button from the Timer app while it is idle; a long press out of the list returns you to wherever you came from (the Timer app, or the main menu), while `MAIN` always returns to the main menu. The list, in order: `DURATION` (the HH:MM:SS timer length), `BUZZER` (Off/End/Countdown), `COUNTDOWN` (countdown beep window, 0–30 s), `FINISH` (`CLEAR`/`HOLD`/`RE-ALERT`), `CLEAR DELAY` (auto-clear delay, 1–300 s), `RE-ALERT INTERVAL` (re-alert cadence, 5–300 s), `ICON` (show/hide the timer icon, ADR-0005), `PROGRESS BAR` (show/hide the progress bar), `MAIN` (back to the main menu). `DURATION` is the same HH:MM:SS wheel as before: a short press cycles the hour/minute/second field, left/right adjusts the active field (hold to repeat), and a long press saves the duration and returns to the list. While a timer is **running or paused** the `DURATION` leaf is read-only (it shows the current value, no field underline, and any press returns to the list). For the other items only the **bare value** is shown (e.g. `END`, `30`, `ON`): left/right cycles the enum, adjusts the number (step 5 for `CLEAR DELAY`/`RE-ALERT INTERVAL`, step 1 for `COUNTDOWN` — see [ADR 0003](adr/0003-timer-tuning-knobs-on-device.md) for these magnitudes and why), or toggles the boolean (`ICON`/`PROGRESS BAR` — both buttons flip it); a short press confirms and returns to the list, and a long press saves and returns to the list. Mode changes (`BUZZER`/`FINISH`) apply and publish to MQTT/HA immediately as you scroll; the duration commits on its own when you leave the `DURATION` leaf; all other edits persist together when you leave the list (long-press out of the list, or select `MAIN`). See [ADR 0016](adr/0016-timer-menu-drillin-navigation.md). |
| `APPS` | Allows to enable or disable internal apps  |
| `SOUND` | Allows to enable or disable sound output   |
| `UPDATE` | Check and download new firmware if available. |

