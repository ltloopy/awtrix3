#include <MQTTManager.h>
#include "Globals.h"
#include "DisplayManager.h"
#include "ServerManager.h"
#include <ArduinoHA.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "Dictionary.h"
#include "PeripheryManager.h"
#include "UpdateManager.h"
#include "PowerManager.h"
#include "TimerManager.h"
#include "TimerHa.h"

const uint16_t PORT = 1883;

namespace {
    constexpr uint8_t  kMaxHAEntities  = 34;
}

WiFiClient espClient;
HADevice device;
HAMqtt mqtt(espClient, device, kMaxHAEntities);

HALight *Matrix, *Indikator1, *Indikator2, *Indikator3 = nullptr;
HASelect *BriMode, *transEffect = nullptr;
HAButton *dismiss, *nextApp, *prevApp, *doUpdate = nullptr;
HASwitch *transition = nullptr;
#ifndef awtrix2_upgrade
HASensor *battery = nullptr;
#endif
HASensor *temperature, *humidity, *illuminance, *uptime, *strength, *version, *ram, *curApp, *myOwnID, *ipAddr = nullptr;
HABinarySensor *btnleft, *btnmid, *btnright = nullptr;
HAText *timerDuration = nullptr;
HASensorNumber *timerRemaining = nullptr;
HASensor *timerStateSensor = nullptr;
HASelect *timerBuzzer = nullptr, *timerFinishedSel = nullptr;
HAButton *timerStartBtn = nullptr, *timerPauseBtn = nullptr, *timerResetBtn = nullptr;
// Sync-control entities (issue #110): the writable Follow switch and static
// Off/All Targets select that make the two sync settings controllable from HA.
HASwitch *timerSyncFollowSw = nullptr;
HASelect *timerSyncTargetsSel = nullptr;
bool connected;
char matID[40], ind1ID[40], ind2ID[40], ind3ID[40], briID[40], btnAID[40], btnBID[40], btnCID[40], appID[40], tempID[40], humID[40], luxID[40], verID[40], ramID[40], upID[40], sigID[40], btnLID[40], btnMID[40], btnRID[40], transID[40], doUpdateID[40], batID[40], myID[40], sSpeed[40], effectID[40], ipAddrID[40];
// Each Timer entity's resolved HA discovery unique id ("%s" filled with the MAC),
// indexed by TimerHaEntity slot (timerHaIds[(size_t)slot]). Filled once in setup()
// via formatTimerHaEntityId() and read by both createTimerHAEntities() and
// removeTimerHAEntities() through timerHaId(slot) — never by hand-ordered position,
// so create and teardown cannot drift. The strings must outlive the entities:
// ArduinoHA stores the unique-id pointer, not a copy.
char timerHaIds[TIMER_HA_DESCRIPTOR_COUNT][40];
bool pendingTimerHADiscoveryCleanup = false;

// The id buffer for a given Timer HA slot. See timerHaIds above.
static char *timerHaId(TimerHaEntity slot) { return timerHaIds[static_cast<size_t>(slot)]; }

void reconcileTimerHAState()
{
    if (SHOW_TIMER_HA_PREV && !SHOW_TIMER)
    {
        pendingTimerHADiscoveryCleanup = true;
    }
    if (SHOW_TIMER_HA_PREV != SHOW_TIMER)
    {
        SHOW_TIMER_HA_PREV = SHOW_TIMER;
        saveSettings();
    }
}

// Forward declarations: the HA command callbacks are defined further down, but
// createTimerHAEntities() (placed here, next to removeTimerHAEntities) wires them.
void onButtonCommand(HAButton *sender);
void onSelectCommand(int8_t index, HASelect *sender);
void onSwitchCommand(bool state, HASwitch *sender);
void onTimerDurationMessage(const char *message, uint16_t length, HAText *sender);

// --- Dynamic Targets select (#112) -------------------------------------------
// The Targets select consumes the peer registry: its options are "Off;All" plus
// each currently-discovered peer id (sorted), rebuilt at runtime. These helpers
// snapshot the current peer ids and join them through the pure TimerHa builder.
// kSyncTargetPeerCap matches TimerManager's kPeerMax; the worst-case option string
// is "Off;All" + kPeerMax ids (<=32 chars each, the sync_targets token cap) + seps.
static const size_t kSyncTargetPeerCap = 16;
static const size_t kSyncTargetOptsCap = 8 + kSyncTargetPeerCap * 33 + 1; // "Off;All" + ";<id>"...

// Snapshot the current sorted peer ids and build the select's option string into
// optsOut. Returns the peer count; idsOut holds the ids (whose c_str() backs ptrsOut)
// so the caller can also map a value<->index against the SAME list.
static size_t buildCurrentSyncTargetsOptions(String *idsOut, const char **ptrsOut, size_t cap,
                                             char *optsOut, size_t optsLen)
{
    size_t n = TimerManager.peerIds(idsOut, cap);
    for (size_t i = 0; i < n; ++i) ptrsOut[i] = idsOut[i].c_str();
    timerSyncTargetsBuildOptions(ptrsOut, n, optsOut, optsLen);
    return n;
}

// The options string last published to the select, and the debounce bookkeeping for
// re-publishing discovery only after registry membership has settled (issue #112).
static String        syncTargetsOptionsSig;
static bool          syncTargetsDirty = false;
static unsigned long syncTargetsDirtySinceMs = 0;
static const unsigned long kSyncTargetsRepublishDebounceMs = 3000;

// Creates the Timer HA entity objects (and registers them with HAMqtt via their
// constructors). Idempotent: the objects persist for the device lifetime, so a second
// call is a no-op. This must run with the Timer entity id buffers already resolved
// (setup() fills them in the HA_DISCOVERY block, before SHOW_TIMER is consulted).
void MQTTManager_::createTimerHAEntities()
{
    if (timerDuration) return;

    // Each entity's strings come from the descriptor table; the ArduinoHA
    // object type and the type-specific wiring (callbacks, initial state)
    // stay here because they're heterogeneous across HA component types.
    const TimerHaDescriptor &dDur   = timerHaDescriptor(TimerHaEntity::Duration);
    const TimerHaDescriptor &dRem   = timerHaDescriptor(TimerHaEntity::Remaining);
    const TimerHaDescriptor &dState = timerHaDescriptor(TimerHaEntity::State);
    const TimerHaDescriptor &dBuz   = timerHaDescriptor(TimerHaEntity::Buzzer);
    const TimerHaDescriptor &dFin   = timerHaDescriptor(TimerHaEntity::Finished);
    const TimerHaDescriptor &dStart = timerHaDescriptor(TimerHaEntity::Start);
    const TimerHaDescriptor &dPause = timerHaDescriptor(TimerHaEntity::Pause);
    const TimerHaDescriptor &dReset = timerHaDescriptor(TimerHaEntity::Reset);

    timerDuration = new HAText(timerHaId(TimerHaEntity::Duration));
    timerDuration->setIcon(dDur.icon);
    timerDuration->setName(dDur.name);
    timerDuration->setRetain(true);
    timerDuration->onMessage(onTimerDurationMessage);
    timerDuration->setState(TimerManager_::formatHMS(TimerManager.getDuration()).c_str(), true);
    // Opt the Duration text entity into JSON attributes (HAText opt-in, issue #67)
    // so its discovery config advertises json_attr_t; its retained {max_duration}
    // object — the cap in carrier-native clock form ("24:00:00") — rides the wire
    // seam to that topic (PRD #66 / issue #68). No TIMER_HA_DESCRIPTORS change.
    timerDuration->setJsonAttributes(true);

    timerRemaining = new HASensorNumber(timerHaId(TimerHaEntity::Remaining), HASensorNumber::PrecisionP0);
    timerRemaining->setIcon(dRem.icon);
    timerRemaining->setName(dRem.name);
    timerRemaining->setUnitOfMeasurement(dRem.unit);
    timerRemaining->setDeviceClass(dRem.deviceClass);
    timerRemaining->setCurrentValue((uint32_t)TimerManager.getRemaining());
    // Opt the remaining sensor into JSON attributes (HASensorNumber inherits the
    // opt-in from HASensor): its retained {remaining_publish_interval} object rides
    // the wire seam to json_attr_t (PRD #57 / issue #59). No descriptor change.
    timerRemaining->setJsonAttributes(true);

    timerStateSensor = new HASensor(timerHaId(TimerHaEntity::State));
    timerStateSensor->setIcon(dState.icon);
    timerStateSensor->setName(dState.name);
    // Opt the state sensor into JSON attributes so its discovery config advertises
    // json_attr_t; the retained config-view object (max_duration, bar_color as
    // "#RRGGBB", sync roles, …) rides the wire seam to that topic (issue #59).
    timerStateSensor->setJsonAttributes(true);

    timerBuzzer = new HASelect(timerHaId(TimerHaEntity::Buzzer));
    timerBuzzer->setOptions(dBuz.options);
    timerBuzzer->onCommand(onSelectCommand);
    timerBuzzer->setIcon(dBuz.icon);
    timerBuzzer->setName(dBuz.name);
    timerBuzzer->setState((uint8_t)TimerManager.getBuzzerMode(), true);
    // Opt the buzzer select into JSON attributes so its discovery config advertises
    // json_attr_t; its retained {countdown_seconds, melody_tick, melody_end} object
    // rides the wire seam to that topic (PRD #57 / issue #58). No descriptor change.
    timerBuzzer->setJsonAttributes(true);

    timerFinishedSel = new HASelect(timerHaId(TimerHaEntity::Finished));
    timerFinishedSel->setOptions(dFin.options);
    timerFinishedSel->onCommand(onSelectCommand);
    timerFinishedSel->setIcon(dFin.icon);
    timerFinishedSel->setName(dFin.name);
    timerFinishedSel->setState((uint8_t)TimerManager.getFinishedMode(), true);
    // Opt the finished select into JSON attributes so its discovery config
    // advertises json_attr_t; the retained {"realert_interval":N} then rides the
    // wire seam to that topic (issue #51 / PRD #17). No TIMER_HA_DESCRIPTORS change.
    timerFinishedSel->setJsonAttributes(true);

    timerStartBtn = new HAButton(timerHaId(TimerHaEntity::Start));
    timerStartBtn->setIcon(dStart.icon);
    timerStartBtn->setName(dStart.name);
    timerStartBtn->onCommand(onButtonCommand);

    timerPauseBtn = new HAButton(timerHaId(TimerHaEntity::Pause));
    timerPauseBtn->setIcon(dPause.icon);
    timerPauseBtn->setName(dPause.name);
    timerPauseBtn->onCommand(onButtonCommand);

    timerResetBtn = new HAButton(timerHaId(TimerHaEntity::Reset));
    timerResetBtn->setIcon(dReset.icon);
    timerResetBtn->setName(dReset.name);
    timerResetBtn->onCommand(onButtonCommand);

    // Sync-control entities (issue #110). The two sync settings — until now
    // read-only attributes on the state sensor — become writable here. Both
    // callbacks route through TimerManager::timerHaApply -> parseCommand, so they
    // inherit the atomic-reject validation and NVS persistence the rest of the
    // control surface has. sync_* are local identity (inSnapshot=false) and never
    // propagate to peers — see ADR-0006/0014.
    const TimerHaDescriptor &dSyncF = timerHaDescriptor(TimerHaEntity::SyncFollow);
    const TimerHaDescriptor &dSyncT = timerHaDescriptor(TimerHaEntity::SyncTargets);

    timerSyncFollowSw = new HASwitch(timerHaId(TimerHaEntity::SyncFollow));
    timerSyncFollowSw->setIcon(dSyncF.icon);
    timerSyncFollowSw->setName(dSyncF.name);
    timerSyncFollowSw->onCommand(onSwitchCommand);
    timerSyncFollowSw->setState(TIMER_SYNC_FOLLOW, true);

    timerSyncTargetsSel = new HASelect(timerHaId(TimerHaEntity::SyncTargets));
    // Dynamic options (issue #112): "Off;All" plus each currently-discovered peer id,
    // not the static dSyncT.options table field — the select tracks the peer registry.
    String        stIds[kSyncTargetPeerCap];
    const char   *stPtrs[kSyncTargetPeerCap];
    char          stOpts[kSyncTargetOptsCap];
    size_t        stN = buildCurrentSyncTargetsOptions(stIds, stPtrs, kSyncTargetPeerCap,
                                                       stOpts, sizeof(stOpts));
    timerSyncTargetsSel->setOptions(stOpts);
    syncTargetsOptionsSig = stOpts;   // seed the republish baseline (no spurious first republish)
    syncTargetsDirty = false;
    timerSyncTargetsSel->onCommand(onSelectCommand);
    timerSyncTargetsSel->setIcon(dSyncT.icon);
    timerSyncTargetsSel->setName(dSyncT.name);
    // Reflect the current sync_targets against the CURRENT id list: Off/All, the
    // matching peer's option, or unknown (-1) for a multi-ID CSV / an id no longer
    // present — the read-only attribute stays authoritative for the exact value.
    timerSyncTargetsSel->setState(
        timerSyncTargetsIndexForValue(TIMER_SYNC_TARGETS.c_str(), stPtrs, stN), true);
}

// Re-publish the Targets select's discovery when peer-registry membership changes,
// debounced so a burst of beacon churn yields one republish (issue #112). No-op when
// the timer HA entities are absent or MQTT is down (discovery re-publishes at the next
// connect). On a settled change it rebuilds the options, re-publishes the discovery
// config so Home Assistant sees the new list, and re-applies the selected state.
void refreshTimerSyncTargetsOptions(unsigned long nowMs)
{
    if (timerSyncTargetsSel == nullptr) return;
    if (!mqtt.isConnected()) return;

    String      ids[kSyncTargetPeerCap];
    const char *ptrs[kSyncTargetPeerCap];
    char        opts[kSyncTargetOptsCap];
    size_t      n = buildCurrentSyncTargetsOptions(ids, ptrs, kSyncTargetPeerCap,
                                                   opts, sizeof(opts));

    if (syncTargetsOptionsSig == opts) { syncTargetsDirty = false; return; }  // unchanged

    if (!syncTargetsDirty)   // first sighting of the change: start the debounce window
    {
        syncTargetsDirty = true;
        syncTargetsDirtySinceMs = nowMs;
        return;
    }
    if ((nowMs - syncTargetsDirtySinceMs) < kSyncTargetsRepublishDebounceMs) return;

    timerSyncTargetsSel->resetOptions();
    timerSyncTargetsSel->setOptions(opts);
    mqtt.publishConfigForDeviceType(timerSyncTargetsSel);
    timerSyncTargetsSel->setState(
        timerSyncTargetsIndexForValue(TIMER_SYNC_TARGETS.c_str(), ptrs, n), true);

    syncTargetsOptionsSig = opts;
    syncTargetsDirty = false;
}

// Brings the Timer HA entities online at runtime when SHOW_TIMER flips false->true.
// Mirror of removeTimerHAEntities(): creates the entities if missing, then publishes
// their discovery config (so HA adds them without a full reconnect) and current values.
void MQTTManager_::enableTimerHADiscovery()
{
    if (!HA_DISCOVERY) return;
    createTimerHAEntities();
    if (!mqtt.isConnected()) return; // discovery will publish at next connect via onConnectedLogic

    HABaseDeviceType *timerTypes[] = {
        timerDuration, timerRemaining, timerStateSensor, timerBuzzer,
        timerFinishedSel, timerStartBtn, timerPauseBtn, timerResetBtn,
        timerSyncFollowSw, timerSyncTargetsSel};
    for (HABaseDeviceType *dt : timerTypes)
        mqtt.publishConfigForDeviceType(dt);

    TimerManager.publishAllWire();   // every wire artifact, derived from the member table (issue #41)
    TimerManager.publishAllAttributeGroups();   // every carrier's read-only attribute object (PRD #57)
}

void MQTTManager_::removeTimerHAEntities()
{
    const char *deviceUniqueId = device.getUniqueId();
    if (!deviceUniqueId) return;
    char topic[160];
    for (size_t i = 0; i < TIMER_HA_DESCRIPTOR_COUNT; ++i)
    {
        const TimerHaDescriptor &d = TIMER_HA_DESCRIPTORS[i];
        snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config",
                 HA_PREFIX.c_str(), d.component, deviceUniqueId, timerHaId(d.slot));
        mqtt.publish(topic, "", true);
    }
    // Clear each carrier's retained json_attr_t object too, so pruning the
    // discovery config does not leave an orphaned attribute payload behind on the
    // broker (issue #60). Rides the same wire seam the attribute publish does.
    TimerManager.clearAllAttributeGroups();
}
long previousMillis_Stats;
std::map<String, String> mqttValues;
std::vector<String> topicsToSubscribe;

MQTTManager_ &MQTTManager_::getInstance()
{
    static MQTTManager_ instance;
    return instance;
}

MQTTManager_ &MQTTManager = MQTTManager.getInstance();

void processMqttMessage(const String &strTopic, const String &payloadCopy)
{
    if (DEBUG_MODE)
    {
        DEBUG_PRINTF("Processing MQTT message for topic %s", strTopic.c_str());
        DEBUG_PRINTF("Payload: %s", payloadCopy.c_str());
    }

    ++RECEIVED_MESSAGES;

    if (strTopic.equals(MQTT_PREFIX + "/notify"))
    {
        if (payloadCopy[0] != '{' || payloadCopy[payloadCopy.length() - 1] != '}')
        {
            return;
        }
        DisplayManager.generateNotification(0, payloadCopy.c_str());
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/notify/dismiss"))
    {
        DisplayManager.dismissNotify();
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/doupdate"))
    {
        if (UpdateManager.checkUpdate(true))
        {
            UpdateManager.updateFirmware();
        }
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/apps"))
    {
        DisplayManager.updateAppVector(payloadCopy.c_str());
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/switch"))
    {
        DisplayManager.switchToApp(payloadCopy.c_str());
        return;
    }

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

    if (strTopic.equals(MQTT_PREFIX + "/sendscreen"))
    {
        MQTTManager.getInstance().publish("screen", DisplayManager.ledsAsJson().c_str());
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/settings"))
    {
        DisplayManager.setNewSettings(payloadCopy.c_str());
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/r2d2"))
    {
        PeripheryManager.r2d2(payloadCopy.c_str());
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/nextapp"))
    {
        DisplayManager.nextApp();
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/previousapp"))
    {
        DisplayManager.previousApp();
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/rtttl"))
    {
        PeripheryManager.playRTTTLString(payloadCopy.c_str());
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/power"))
    {
        StaticJsonDocument<128> doc;
        DeserializationError error = deserializeJson(doc, payloadCopy.c_str());
        if (error)
        {
            if (DEBUG_MODE)
                DEBUG_PRINTLN(F("Failed to parse json"));
            return;
        }
        if (doc.containsKey("power"))
        {
            DisplayManager.setPower(doc["power"].as<bool>());
        }
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/sleep"))
    {
        StaticJsonDocument<128> doc;
        DeserializationError error = deserializeJson(doc, payloadCopy.c_str());
        if (error)
        {
            if (DEBUG_MODE)
                DEBUG_PRINTLN(F("Failed to parse json"));
            return;
        }
        if (doc.containsKey("sleep"))
        {
            DisplayManager.setPower(false);
            PowerManager.sleep(doc["sleep"].as<uint64_t>());
        }
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/indicator1"))
    {
        DisplayManager.indicatorParser(1, payloadCopy.c_str());
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/indicator2"))
    {
        DisplayManager.indicatorParser(2, payloadCopy.c_str());
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/indicator3"))
    {
        DisplayManager.indicatorParser(3, payloadCopy.c_str());
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/moodlight"))
    {
        DisplayManager.moodlight(payloadCopy.c_str());
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/reboot"))
    {
        if (DEBUG_MODE)
            DEBUG_PRINTLN("REBOOT COMMAND RECEIVED");
        delay(1000);
        ESP.restart();
        return;
    }

    if (strTopic.equals(MQTT_PREFIX + "/sound"))
    {
        PeripheryManager.parseSound(payloadCopy.c_str());
        return;
    }

    if (strTopic.startsWith(MQTT_PREFIX + "/custom"))
    {
        String topic_str = strTopic;
        String prefix = MQTT_PREFIX + "/custom/";
        if (topic_str.startsWith(prefix))
        {
            topic_str = topic_str.substring(prefix.length());
            DisplayManager.parseCustomPage(topic_str, payloadCopy.c_str(), false);
        }
        return;
    }

    if (mqttValues.find(strTopic) != mqttValues.end())
    {
        mqttValues[strTopic] = payloadCopy;
        if (DEBUG_MODE)
        {
            Serial.print("Updated existing topic: ");
            Serial.println(strTopic);
            Serial.print("New value: ");
            Serial.println(mqttValues[strTopic]);
        }
        return;
    }
}

void onButtonCommand(HAButton *sender)
{
    if (sender == dismiss)
    {
        DisplayManager.dismissNotify();
    }
    else if (sender == nextApp)
    {
        DisplayManager.nextApp();
    }
    else if (sender == prevApp)
    {
        DisplayManager.previousApp();
    }
    else if (sender == doUpdate)
    {
        if (UpdateManager.checkUpdate(true))
        {
            UpdateManager.updateFirmware();
        }
    }
    else if (sender == timerStartBtn)
    {
        // Route through parseCommand (issue #109): start/pause/reset re-enter the
        // control surface, so HA buttons drive peer clocks (run-state propagation)
        // the same way the MQTT topic does. parseCommand also owns the
        // start-from-idle switch-to-Timer-app behaviour, so it isn't duplicated here.
        TimerManager.timerHaApply(TimerHaEntity::Start, "");
    }
    else if (sender == timerPauseBtn)
    {
        TimerManager.timerHaApply(TimerHaEntity::Pause, "");
    }
    else if (sender == timerResetBtn)
    {
        TimerManager.timerHaApply(TimerHaEntity::Reset, "");
    }
}

void onSwitchCommand(bool state, HASwitch *sender)
{
    if (sender == timerSyncFollowSw)
    {
        // Route through parseCommand (issue #110): the Follow switch re-enters the
        // control surface, so it gets the strict-bool atomic-reject validation and
        // NVS persistence. sync_follow is local identity and never propagates.
        TimerManager.timerHaApply(TimerHaEntity::SyncFollow, String(state ? 1 : 0));
        // Echo the value actually applied back (snap-back if a write were rejected).
        sender->setState(TIMER_SYNC_FOLLOW);
        return;
    }
    AUTO_TRANSITION = state;
    DisplayManager.setAutoTransition(state);
    saveSettings();
    sender->setState(state);
}

void onSelectCommand(int8_t index, HASelect *sender)
{
    if (sender == BriMode)
    {
        switch (index)
        {
        case 0:
            AUTO_BRIGHTNESS = false;
            Matrix->setBrightness(BRIGHTNESS, true);
            break;
        case 1:
            AUTO_BRIGHTNESS = true;
            break;
        }
    }
    else if (sender == transEffect)
    {
        TRANS_EFFECT = index;
    }
    else if (sender == timerBuzzer)
    {
        // Route through parseCommand (issue #109): the HA select re-enters the
        // control surface like every other edit, so it gets atomic-reject
        // validation and the per-enum codec wire spelling — no deep setter here.
        TimerManager.timerHaApply(TimerHaEntity::Buzzer, String(index));
        // Echo the canonical live mode back (snap-back to last valid on a reject).
        sender->setState((int8_t)TimerManager.getBuzzerMode());
        return;
    }
    else if (sender == timerFinishedSel)
    {
        TimerManager.timerHaApply(TimerHaEntity::Finished, String(index));
        sender->setState((int8_t)TimerManager.getFinishedMode());
        return;
    }
    else if (sender == timerSyncTargetsSel)
    {
        // Dynamic select (issue #112): resolve the chosen option index through the
        // CURRENT peer-id list to its sync_targets value (Off -> "", All -> "all", a
        // peer option -> that id), then route through parseCommand (#110) for the
        // bespoke validator + NVS persistence. Echo the canonical applied value back
        // against the same list (Off/All/peer, or unknown for a multi-ID CSV / an id
        // no longer present — the read-only attribute stays authoritative). sync_targets
        // never propagates.
        String      ids[kSyncTargetPeerCap];
        const char *ptrs[kSyncTargetPeerCap];
        char        opts[kSyncTargetOptsCap];
        size_t      n = buildCurrentSyncTargetsOptions(ids, ptrs, kSyncTargetPeerCap,
                                                       opts, sizeof(opts));
        char val[40];
        if (timerSyncTargetsValueForIndex(index, ptrs, n, val, sizeof(val)))
            TimerManager.timerHaApply(TimerHaEntity::SyncTargets, val);
        sender->setState(timerSyncTargetsIndexForValue(TIMER_SYNC_TARGETS.c_str(), ptrs, n));
        return;
    }
    saveSettings();
    sender->setState(index);
}

void onRGBColorCommand(HALight::RGBColor color, HALight *sender)
{
    if (sender == Matrix)
    {
        TEXTCOLOR_888 = (color.red << 16) | (color.green << 8) | color.blue;
        DisplayManager.setCustomAppColors(TEXTCOLOR_888);
        saveSettings();
    }
    else if (sender == Indikator1)
    {
        DisplayManager.setIndicator1Color((color.red << 16) | (color.green << 8) | color.blue);
    }
    else if (sender == Indikator2)
    {
        DisplayManager.setIndicator2Color((color.red << 16) | (color.green << 8) | color.blue);
    }
    else if (sender == Indikator3)
    {
        DisplayManager.setIndicator3Color((color.red << 16) | (color.green << 8) | color.blue);
    }
    sender->setRGBColor(color); // report color back to the Home Assistant
}

void onStateCommand(bool state, HALight *sender)
{
    if (sender == Matrix)
    {
        DisplayManager.setPower(state);
    }
    else if (sender == Indikator1)
    {
        DisplayManager.setIndicator1State(state);
    }
    else if (sender == Indikator2)
    {
        DisplayManager.setIndicator2State(state);
    }
    else if (sender == Indikator3)
    {
        DisplayManager.setIndicator3State(state);
    }
    sender->setState(state);
}

void onBrightnessCommand(uint8_t brightness, HALight *sender)
{
    sender->setBrightness(brightness);
    if (AUTO_BRIGHTNESS)
        return;
    BRIGHTNESS = brightness;
    saveSettings();
    DisplayManager.setBrightness(brightness);
}

void onTimerDurationMessage(const char *message, uint16_t length, HAText *sender)
{
    String in;
    in.reserve(length);
    for (uint16_t i = 0; i < length; i++) in += message[i];
    in.trim();

    // Route the raw HH:MM:SS text through parseCommand (issue #109): it owns the
    // parse/validate (parseHMS + range, reject-not-clamp) — the HA layer no longer
    // duplicates that logic. A rejected input applies nothing (atomic-reject).
    TimerManager.timerHaApply(TimerHaEntity::Duration, in);

    // Echo the canonical value back so rejected input snaps the field to the
    // previous valid time rather than leaving the bad text displayed.
    sender->setState(TimerManager_::formatHMS(TimerManager.getDuration()).c_str(), true);
}

void onNumberCommand(HANumeric number, HANumber *sender)
{
    if (!number.isSet())
    {
        // the reset command was send by Home Assistant
    }
    else
    {
        SCROLL_SPEED = number.toInt8();
        saveSettings();
    }

    sender->setState(number); // report the selected option back to the HA panel
}

void onMqttMessage(const char *topic, const uint8_t *payload, uint16_t length)
{
    if (DEBUG_MODE)
        DEBUG_PRINTF("MQTT message received at topic %s", topic);

    // Create a copy of the payload
    char *payloadCopy = new char[length + 1];
    memcpy(payloadCopy, payload, length);
    payloadCopy[length] = '\0';

    // Convert to String and handle the message
    processMqttMessage(String(topic), String(payloadCopy));

    // Clean up the payload copy
    delete[] payloadCopy;
}

String MQTTManager_::getValueForTopic(const String &topic)
{
    if (mqttValues.find(topic) != mqttValues.end())
    {
        return mqttValues[topic];
    }
    else
    {
        return "N/A"; // Return "N/A" if the topic is not found
    }
}

void onMqttConnected()
{

    if (DEBUG_MODE)
        DEBUG_PRINTLN(F("MQTT Connected"));

    // Command topics must never carry a retained payload. A retained
    // {prefix}/timer command (e.g. {"action":"start"}) is re-delivered by the
    // broker on every (re)connect and would auto-start the timer on boot, which
    // breaks the "a reboot returns the device to Idle" contract (docs/timer.md).
    // PubSubClient doesn't surface the retain flag to the receive callback, so we
    // purge the retained command at the source: clear it before subscribing.
    mqtt.publish((MQTT_PREFIX + "/timer").c_str(), "", true);

    const char *topics[] PROGMEM = {
        "/brightness",
        "/notify/dismiss",
        "/notify",
        "/custom/#",
        "/switch",
        "/settings",
        "/previousapp",
        "/nextapp",
        "/doupdate",
        "/nextapp",
        "/apps",
        "/power",
        "/sleep",
        "/indicator1",
        "/indicator2",
        "/indicator3",
        "/timeformat",
        "/dateformat",
        "/reboot",
        "/moodlight",
        "/sound",
        "/rtttl",
        "/sendscreen",
        "/r2d2",
        "/timer"};
    for (const char *topic : topics)
    {
        if (DEBUG_MODE)
            DEBUG_PRINTF("Subscribe to topic %s", topic);
        mqtt.subscribe((MQTT_PREFIX + topic).c_str());
        delay(30);
    }

    for (const auto &topic : topicsToSubscribe)
    {
        mqtt.subscribe(topic.c_str());
        if (DEBUG_MODE)
            Serial.printf("Subscribed to topic %s\n", topic.c_str());
    }

    delay(200);
    if (HA_DISCOVERY)
    {
        if (pendingTimerHADiscoveryCleanup)
        {
            MQTTManager.removeTimerHAEntities();
            pendingTimerHADiscoveryCleanup = false;
        }
        myOwnID->setValue(MQTT_PREFIX.c_str());
        version->setValue(VERSION);

        if (SHOW_TIMER)
        {
            TimerManager.publishAllWire();   // every wire artifact, derived from the member table (issue #41)
            TimerManager.publishAllAttributeGroups();   // every carrier's read-only attribute object (PRD #57)
        }
    }

    MQTTManager.publish("stats/effects", DisplayManager.getEffectNames().c_str());
    MQTTManager.publish("stats/transitions", DisplayManager.getTransitionNames().c_str());
    if (!HA_DISCOVERY)
    {
        MQTTManager.publish("stats/device", "online");
    }
    connected = true;
}

bool MQTTManager_::subscribe(const char *topic)
{
    mqttValues[topic] = "N/A";
    if (mqtt.isConnected())
    {
        mqtt.subscribe(topic);
    }
    else
    {
        topicsToSubscribe.push_back(topic);
    }
    return true;
}

bool MQTTManager_::isConnected()
{
    if (MQTT_HOST != "")
    {
        return mqtt.isConnected();
    }
    else
    {
        return true;
    }
}

void connect()
{

    mqtt.onMessage(onMqttMessage);
    mqtt.onConnected(onMqttConnected);

    if (!HA_DISCOVERY)
    {
        static char topic[50];
        snprintf(topic, sizeof(topic), "%s/stats/device", MQTT_PREFIX.c_str());
        mqtt.setLastWill(topic, "offline", false);
    }

    if (MQTT_USER == "" || MQTT_PASS == "")
    {
        if (DEBUG_MODE)
            DEBUG_PRINTLN(F("Connecting to MQTT w/o login"));
        mqtt.begin(MQTT_HOST.c_str(), MQTT_PORT, nullptr, nullptr, HOSTNAME.c_str());
    }
    else
    {
        if (DEBUG_MODE)
            DEBUG_PRINTLN(F("Connecting to MQTT with login"));
        mqtt.begin(MQTT_HOST.c_str(), MQTT_PORT, MQTT_USER.c_str(), MQTT_PASS.c_str(), HOSTNAME.c_str());
    }
}

void MQTTManager_::sendStats()
{

    if (HA_DISCOVERY && mqtt.isConnected())
    {
        char buffer[8];
#ifndef awtrix2_upgrade
        snprintf(buffer, 5, "%d", BATTERY_PERCENT);
        battery->setValue(buffer);
#endif
        if (SENSOR_READING)
        {
            snprintf(buffer, sizeof(buffer), "%.*f", TEMP_DECIMAL_PLACES, CURRENT_TEMP);
            temperature->setValue(buffer);
            snprintf(buffer, 5, "%.0f", CURRENT_HUM);
            humidity->setValue(buffer);
        }

        snprintf(buffer, 5, "%.0f", CURRENT_LUX);
        illuminance->setValue(buffer);
        BriMode->setState(AUTO_BRIGHTNESS, false);
        Matrix->setBrightness(BRIGHTNESS);
        Matrix->setState(!MATRIX_OFF, false);
        HALight::RGBColor color;
        color.isSet = true;
        color.red = (TEXTCOLOR_888 >> 16) & 0xFF;
        color.green = (TEXTCOLOR_888 >> 8) & 0xFF;
        color.blue = TEXTCOLOR_888 & 0xFF;
        Matrix->setRGBColor(color);
        int8_t rssiValue = WiFi.RSSI();
        char rssiString[4];
        snprintf(rssiString, sizeof(rssiString), "%d", rssiValue);
        strength->setValue(rssiString);

        char rambuffer[10];
        int freeHeapBytes = ESP.getFreeHeap();
        itoa(freeHeapBytes, rambuffer, 10);
        ram->setValue(rambuffer);
        char uptimeStr[25]; // Buffer for string representation
        sprintf(uptimeStr, "%ld", PeripheryManager.readUptime());
        uptime->setValue(uptimeStr);
        transition->setState(AUTO_TRANSITION, false);
        ipAddr->setValue(ServerManager.myIP.toString().c_str());
    }
    publish(StatsTopic, DisplayManager.getStats().c_str());
}

void MQTTManager_::setup()
{

    if (HA_DISCOVERY)
    {
        if (DEBUG_MODE)
            DEBUG_PRINTLN(F("Starting Homeassistant discovery"));
        mqtt.setDiscoveryPrefix(HA_PREFIX.c_str());
        mqtt.setDataPrefix(MQTT_PREFIX.c_str());
        uint8_t mac[6];
        WiFi.macAddress(mac);
        char macStr[7];
        snprintf(macStr, 7, "%02x%02x%02x", mac[3], mac[4], mac[5]);
        device.setUniqueId(mac, sizeof(mac));
        device.setName(HOSTNAME.c_str());
        device.setSoftwareVersion(VERSION);
        device.setManufacturer(HAmanufacturer);

        device.setModel(HAmodel);
        device.setAvailability(true);
        device.enableSharedAvailability();
        device.enableLastWill();

        IPAddress ip = WiFi.localIP();
        static char configurationUrl[32]; // static!
        sprintf(configurationUrl, "http://%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        device.setConfigurationUrl(configurationUrl);

        sprintf(matID, HAmatID, macStr);
        Matrix = new HALight(matID, HALight::BrightnessFeature | HALight::RGBFeature);

        Matrix->setIcon(HAmatIcon);
        Matrix->setName(HAmatName);
        Matrix->onStateCommand(onStateCommand);
        Matrix->onBrightnessCommand(onBrightnessCommand);
        Matrix->onRGBColorCommand(onRGBColorCommand);
        Matrix->setCurrentState(true);
        Matrix->setBRIGHTNESS(BRIGHTNESS);

        HALight::RGBColor color;
        color.isSet = true;
        color.red = (TEXTCOLOR_888 >> 16) & 0xFF;  // Die obersten 8 Bits für Rot
        color.green = (TEXTCOLOR_888 >> 8) & 0xFF; // Die mittleren 8 Bits für Grün
        color.blue = TEXTCOLOR_888 & 0xFF;         // Die untersten 8 Bits für Blau
        Matrix->setCurrentRGBColor(color);
        Matrix->setState(MATRIX_OFF, true);

        sprintf(ind1ID, HAi1ID, macStr);
        Indikator1 = new HALight(ind1ID, HALight::RGBFeature);
        Indikator1->setIcon(HAi1Icon);
        Indikator1->setName(HAi1Name);
        Indikator1->onStateCommand(onStateCommand);
        Indikator1->onRGBColorCommand(onRGBColorCommand);

        sprintf(ind2ID, HAi2ID, macStr);
        Indikator2 = new HALight(ind2ID, HALight::RGBFeature);
        Indikator2->setIcon(HAi2Icon);
        Indikator2->setName(HAi2Name);
        Indikator2->onStateCommand(onStateCommand);
        Indikator2->onRGBColorCommand(onRGBColorCommand);

        sprintf(ind3ID, HAi3ID, macStr);
        Indikator3 = new HALight(ind3ID, HALight::RGBFeature);
        Indikator3->setIcon(HAi3Icon);
        Indikator3->setName(HAi3Name);
        Indikator3->onStateCommand(onStateCommand);
        Indikator3->onRGBColorCommand(onRGBColorCommand);

        sprintf(briID, HAbriID, macStr);
        BriMode = new HASelect(briID);
        BriMode->setOptions(HAbriOptions);
        BriMode->onCommand(onSelectCommand);
        BriMode->setIcon(HAbriIcon);
        BriMode->setName(HAbriName);
        BriMode->setState(AUTO_BRIGHTNESS, true);

        sprintf(effectID, HAeffectID, macStr);
        transEffect = new HASelect(effectID);
        transEffect->setOptions(HAeffectOptions);
        transEffect->onCommand(onSelectCommand);
        transEffect->setIcon(HAeffectIcon);
        transEffect->setName(HAeffectName);
        transEffect->setState(TRANS_EFFECT, true);

        sprintf(btnAID, HAbtnaID, macStr);
        dismiss = new HAButton(btnAID);
        dismiss->setIcon(HAbtnaIcon);
        dismiss->setName(HAbtnaName);

        sprintf(doUpdateID, HAdoUpID, macStr);
        doUpdate = new HAButton(doUpdateID);
        doUpdate->setIcon(HAdoUpIcon);
        doUpdate->setName(HAdoUpName);
        doUpdate->onCommand(onButtonCommand);

        sprintf(transID, HAtransID, macStr);
        transition = new HASwitch(transID);
        transition->setIcon(HAtransIcon);
        transition->setName(HAtransName);
        transition->onCommand(onSwitchCommand);

        sprintf(appID, HAappID, macStr);
        curApp = new HASensor(appID);
        curApp->setIcon(HAappIcon);
        curApp->setName(HAappName);

        sprintf(myID, HAIDID, macStr);
        myOwnID = new HASensor(myID);
        myOwnID->setIcon(HAIDIcon);
        myOwnID->setName(HAIDName);

        sprintf(btnBID, HAbtnbID, macStr);
        nextApp = new HAButton(btnBID);
        nextApp->setIcon(HAbtnbIcon);
        nextApp->setName(HAbtnbName);

        sprintf(btnCID, HAbtncID, macStr);
        prevApp = new HAButton(btnCID);
        prevApp->setIcon(HAbtncIcon);
        prevApp->setName(HAbtncName);

        dismiss->onCommand(onButtonCommand);
        nextApp->onCommand(onButtonCommand);
        prevApp->onCommand(onButtonCommand);

        sprintf(tempID, HAtempID, macStr);
        temperature = new HASensor(tempID);
        temperature->setIcon(HAtempIcon);
        temperature->setName(HAtempName);
        temperature->setDeviceClass(HAtempClass);
        temperature->setUnitOfMeasurement(HAtempUnit);

        sprintf(humID, HAhumID, macStr);
        humidity = new HASensor(humID);
        humidity->setIcon(HAhumIcon);
        humidity->setName(HAhumName);
        humidity->setDeviceClass(HAhumClass);
        humidity->setUnitOfMeasurement(HAhumUnit);

#ifdef ULANZI
        sprintf(batID, HAbatID, macStr);
        battery = new HASensor(batID);
        battery->setIcon(HAbatIcon);
        battery->setName(HAbatName);
        battery->setDeviceClass(HAbatClass);
        battery->setUnitOfMeasurement(HAbatUnit);

#endif
        sprintf(luxID, HAluxID, macStr);
        illuminance = new HASensor(luxID);
        illuminance->setIcon(HAluxIcon);
        illuminance->setName(HAluxName);
        illuminance->setDeviceClass(HAluxClass);
        illuminance->setUnitOfMeasurement(HAluxUnit);

        sprintf(verID, HAverID, macStr);
        version = new HASensor(verID);
        version->setName(HAverName);

        sprintf(sigID, HAsigID, macStr);
        strength = new HASensor(sigID);
        strength->setName(HAsigName);
        strength->setDeviceClass(HAsigClass);
        strength->setUnitOfMeasurement(HAsigUnit);

        sprintf(upID, HAupID, macStr);
        uptime = new HASensor(upID);
        uptime->setName(HAupName);
        uptime->setDeviceClass(HAupClass);
        uptime->setUnitOfMeasurement("s");

        sprintf(btnLID, HAbtnLID, macStr);
        btnleft = new HABinarySensor(btnLID);
        btnleft->setName(HAbtnLName);

        sprintf(btnMID, HAbtnMID, macStr);
        btnmid = new HABinarySensor(btnMID);
        btnmid->setName(HAbtnMName);

        sprintf(btnRID, HAbtnRID, macStr);
        btnright = new HABinarySensor(btnRID);
        btnright->setName(HAbtnRName);

        sprintf(ramID, HAramRID, macStr);
        ram = new HASensor(ramID);
        ram->setDeviceClass(HAramClass);
        ram->setIcon(HAramIcon);
        ram->setName(HAramName);
        ram->setUnitOfMeasurement(HAramUnit);

        sprintf(ipAddrID, HAipAddrRID, macStr);
        ipAddr = new HASensor(ipAddrID);
        ipAddr->setName(HAipAddrName);
        ipAddr->setIcon(HAipAddrIcon);

        // Resolve every Timer entity's unique id from the Timer HA Presence table
        // into the slot-keyed timerHaIds buffers (also read by teardown), each
        // through the one formatTimerHaEntityId() helper so create and teardown
        // agree per slot. Row i describes slot i (pinned by test_U32).
        for (size_t i = 0; i < TIMER_HA_DESCRIPTOR_COUNT; ++i)
            formatTimerHaEntityId(TIMER_HA_DESCRIPTORS[i], macStr, timerHaIds[i], sizeof(timerHaIds[i]));

        if (SHOW_TIMER)
            createTimerHAEntities();
    }
    else
    {
        Serial.println(F("Homeassistant discovery disabled"));
        mqtt.disableHA();
    }

    connect();
}

void MQTTManager_::tick()
{
    if (MQTT_HOST != "")
    {
        mqtt.loop();
    }
    unsigned long currentMillis_Stats = millis();
    if ((currentMillis_Stats - previousMillis_Stats >= STATS_INTERVAL) && (SENSORS_STABLE))
    {
        previousMillis_Stats = currentMillis_Stats;
        sendStats();
    }
}

// The Timer wire seam (issue #31): publishes the exact (topic, payload) the
// caller hands over — retained, like the HASensor::setValue path it replaces.
// Gated on the Timer HA entities existing (timerDuration is the creation
// sentinel, see createTimerHAEntities), which preserves the per-entity
// null-check behaviour of the retired one-liner publish methods; mqtt.publish
// itself no-ops while disconnected, as beginPublish did before.
void MQTTManager_::publishTimerWire(const char *topic, const char *payload)
{
    if (!timerDuration) return;
    mqtt.publish(topic, payload, true);
}

// Canonical full data topic for a Timer HA entity slot, from the same inputs
// ArduinoHA's HASerializer::generateDataTopic uses: the data prefix installed
// in setup() (MQTT_PREFIX), the device unique id, and the entity id buffer
// resolved through formatTimerHaEntityId. Byte-identity is pinned by test W1.
String MQTTManager_::timerWireTopic(TimerHaEntity slot)
{
    const char *deviceUniqueId = device.getUniqueId();
    if (!deviceUniqueId) return String();
    char topic[160];
    formatTimerHaDataTopic(MQTT_PREFIX.c_str(), deviceUniqueId, timerHaId(slot), topic, sizeof(topic));
    return String(topic);
}

// A carrier entity's JSON-attributes topic (PRD #57, generalizing the issue-#51
// finished-only topic): same inputs as timerWireTopic but the json_attr_t suffix,
// so it matches the topic the carrier's HASelect advertised in discovery via
// setJsonAttributes. The retained attribute value rides the wire seam to here.
String MQTTManager_::timerWireAttrTopic(TimerHaEntity slot)
{
    const char *deviceUniqueId = device.getUniqueId();
    if (!deviceUniqueId) return String();
    char topic[160];
    formatTimerHaAttrTopic(MQTT_PREFIX.c_str(), deviceUniqueId,
                           timerHaId(slot), topic, sizeof(topic));
    return String(topic);
}

// Canonical topic for the aggregate icons JSON (issue #34) — the one published
// Timer topic that is NOT an HA entity data topic. The host-test stub mirrors
// this from its fixture prefix, so tests pin the same spelling.
String MQTTManager_::timerIconsTopic()
{
    return MQTT_PREFIX + "/timer/icons";
}

void MQTTManager_::publish(const char *topic, const char *payload)
{
    char result[100];
    strcpy(result, MQTT_PREFIX.c_str());
    strcat(result, "/");
    strcat(result, topic);

    if (!mqtt.isConnected())
        return;

    mqtt.publish(result, payload, false);
}

void MQTTManager_::rawPublish(const char *prefix, const char *topic, const char *payload)
{
    if (!mqtt.isConnected())
        return;
    char result[100];
    strcpy(result, prefix);
    strcat(result, "/");
    strcat(result, topic);
    mqtt.publish(result, payload, false);
}

void MQTTManager_::setCurrentApp(String appName)
{
    static String lastApp = "";

    if (lastApp == appName)
        return;

    if (DEBUG_MODE)
        DEBUG_PRINTF("Publish current app %s", appName.c_str());
    if (HA_DISCOVERY && mqtt.isConnected())
        curApp->setValue(appName.c_str());

    publish("stats/currentApp", appName.c_str());
    lastApp = appName;
}

void MQTTManager_::sendButton(byte btn, bool state)
{
    static bool btn0State, btn1State, btn2State;

    switch (btn)
    {
    case 0:
        if (btn0State != state)
        {
            if (HA_DISCOVERY && mqtt.isConnected())
                btnleft->setState(state, false);
            btn0State = state;
            publish(ButtonLeftTopic, state ? State1 : State0);
        }
        break;
    case 1:
        if (btn1State != state)
        {
            if (HA_DISCOVERY && mqtt.isConnected())
                btnmid->setState(state, false);
            btn1State = state;
            publish(ButtonSelectTopic, state ? State1 : State0);
        }

        break;
    case 2:
        if (btn2State != state)
        {
            if (HA_DISCOVERY && mqtt.isConnected())
                btnright->setState(state, false);
            btn2State = state;
            publish(ButtonRightTopic, state ? State1 : State0);
        }
        break;
    default:
        break;
    }
}

void MQTTManager_::setIndicatorState(uint8_t indicator, bool state, uint32_t color)
{
    if (HA_DISCOVERY && mqtt.isConnected())
    {
        HALight::RGBColor c;
        c.isSet = true;
        c.red = (color >> 16) & 0xFF;  // Rote Komponente 8-Bit
        c.green = (color >> 8) & 0xFF; // Grüne Komponente 8-Bit
        c.blue = color & 0xFF;

        switch (indicator)
        {
        case 1:
            Indikator1->setRGBColor(c);
            Indikator1->setState(state);
            break;
        case 2:
            Indikator2->setRGBColor(c);
            Indikator2->setState(state);
            break;
        case 3:
            Indikator3->setRGBColor(c);
            Indikator3->setState(state);
            break;
        default:
            break;
        }
    }
}

void MQTTManager_::beginPublish(const char *topic, unsigned int plength, boolean retained)
{
    mqtt.beginPublish(topic, plength, retained);
}

void MQTTManager_::writePayload(const char *data, const uint16_t length)
{
    mqtt.writePayload(data, length);
}

void MQTTManager_::endPublish()
{
    mqtt.endPublish();
}
