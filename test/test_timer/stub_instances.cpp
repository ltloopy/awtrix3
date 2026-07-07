// Definitions for every stub singleton and global TimerManager.cpp expects
// at link time. Lives in test/test_timer/ (not tests/stubs/) because the
// PlatformIO test runner only compiles files under test/<testname>/.

#include <Arduino.h>
#include <map>
#include <vector>

#include "Preferences.h"
#include "LittleFS.h"
#include "Overlays.h"
#include "MQTTManager.h"
#include "PeripheryManager.h"
#include "DisplayManager.h"
#include "MenuManager.h"
#include "ServerManager.h"
#include "Globals.h"

std::map<String, uint32_t> Preferences::u32_;
std::map<String, uint8_t>  Preferences::u8_;
std::map<String, String>   Preferences::str_;
std::map<String, bool>     Preferences::b_;
int Preferences::begin_calls = 0;

LittleFS_Stub LittleFS;

std::vector<Notification> notifications;

MQTTManager_      MQTTManager;
PeripheryManager_ PeripheryManager;
DisplayManager_   DisplayManager;
MenuManager_      MenuManager;
ServerManager_    ServerManager;

String   CURRENT_APP       = "Time";
bool     SOUND_ACTIVE      = true;
bool     BLOCK_NAVIGATION  = false;
bool     GAME_ACTIVE       = false;
bool     MATRIX_OFF        = false;
uint8_t  BRIGHTNESS        = 100;

bool     SHOW_TIMER             = true;
bool     SHOW_TIMER_HA_PREV     = true;
// The 13 table-backed timer settings are now DEFINED in TimerSettings.cpp (#143),
// which this env links, so the stub must NOT redefine them (one definition only).
// The four member-backed icon_<state> globals still live here (TimerSettings owns
// only the table-backed half).
String   TIMER_ICON_IDLE        = "";
String   TIMER_ICON_RUNNING     = "";
String   TIMER_ICON_PAUSED      = "";
String   TIMER_ICON_FINISHED    = "";
const char *uniqueID            = "awtrix_self";
bool        AP_MODE             = false;

uint32_t TEXTCOLOR_888 = 0xFFFFFF;

bool DEBUG_MODE = false;

int saveSettings_calls = 0;
void saveSettings() { ++saveSettings_calls; }
