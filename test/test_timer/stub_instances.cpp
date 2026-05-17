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
#include "Globals.h"

std::map<String, uint32_t> Preferences::u32_;
std::map<String, uint8_t>  Preferences::u8_;

LittleFS_Stub LittleFS;

std::vector<Notification> notifications;

MQTTManager_      MQTTManager;
PeripheryManager_ PeripheryManager;
DisplayManager_   DisplayManager;

String   CURRENT_APP       = "Time";
bool     SOUND_ACTIVE      = true;
bool     BLOCK_NAVIGATION  = false;
bool     GAME_ACTIVE       = false;
bool     MATRIX_OFF        = false;
uint8_t  BRIGHTNESS        = 100;

bool     SHOW_TIMER             = true;
bool     SHOW_TIMER_HA_PREV     = true;
uint32_t TIMER_MAX_DURATION     = 86400;
uint32_t TIMER_STEP             = 1;
uint16_t TIMER_PUBLISH_INTERVAL = 1;
uint16_t TIMER_FINISHED_HOLD    = 10;
uint16_t TIMER_REALERT_INTERVAL = 15;
uint16_t TIMER_COUNTDOWN_SECONDS = 3;
uint16_t TIMER_CONFIG_TIMEOUT   = 30;

uint32_t TEXTCOLOR_888 = 0xFFFFFF;
