# Onscreen menu

AWTRIX 3 provides a **onscreen menu** directly on your clock.  
Press and hold the middle button for 2 seconds to access the menu.   
Navigate through the items with the left and right buttons and choose the submenu with a push on the middle button.  
Hold down the middle button for 2s to exit the current menu and to save your setting.  
  
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
| `TIMER` | Configure the Timer app. Submenu of seven items in order: `BUZZER` (Off/End/Countdown), `CDOWN` (countdown beep window, 0–30 s), `FINISH` (auto-clear/hold/re-alert), `CLEAR` (auto-clear delay, 1–300 s), `ALERT` (re-alert cadence, 5–300 s), `ICON` (`ICON ON`/`ICON OFF` — show/hide the timer icon, ADR-0005), `BAR` (`BAR ON`/`BAR OFF` — show/hide the progress bar). In the submenu: left/right cycles enum values, adjusts the number (step 5 for `CLEAR`/`ALERT`, step 1 for `CDOWN` — see [ADR 0003](adr/0003-timer-tuning-knobs-on-device.md) for these magnitudes and why), or toggles the boolean (`ICON`/`BAR` — both buttons flip it); short-press middle advances to the next field; long-press middle saves and exits. Mode changes (`BUZZER`/`FINISH`) apply and publish to MQTT/HA immediately on each press; numeric and toggle changes commit on long-press save. |
| `APPS` | Allows to enable or disable internal apps  |
| `SOUND` | Allows to enable or disable sound output   |
| `UPDATE` | Check and download new firmware if available. |

