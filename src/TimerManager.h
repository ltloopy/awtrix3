#ifndef TimerManager_h
#define TimerManager_h

#include <Arduino.h>
#include <ArduinoJson.h>

#include "TimerEnums.h"          // TimerState + BuzzerMode / FinishedMode + their codec tables (ADR-0010)
#include "TimerConfigEditor.h"   // display-free duration editor; owns the config-mode working state (ADR-0011)
#include "TimerHa.h"             // TimerHaEntity (the HA carrier publishAttributeGroup targets)
#include "TimerSettings.h"       // TcValue + TIMER_SETTINGS_DESC_CAP (one-shot override snapshot, PRD #99)
#include "PeerRegistry.h"        // the LAN peer set (extracted from this class, ADR-0019/0021)
#include "SyncSeenCache.h"       // the sync dedup set (extracted from this class, ADR-0022)

// Result of parseCommand. All control surfaces share one validation policy
// (reject invalid input atomically); only the HTTP API surfaces this as a
// status code — MQTT ignores it. See docs/adr/0001-timer-command-validation-parity.md.
enum class TimerCmdResult : uint8_t { Ok = 0, BadJson = 1, BadField = 2, Disabled = 3 };

// Globals.cpp's full "awtrix"-namespace flush (declared in Globals.h, repeated
// here so the PersistBatch guard below can perform the table-half commit).
void saveSettings();

class TimerManager_
{
private:
    TimerManager_() = default;

    TimerState state = TimerState::Idle;
    BuzzerMode buzzerMode = BuzzerMode::End;
    FinishedMode finishedMode = FinishedMode::AutoClear;
    uint32_t durationSec = 300;
    uint32_t remainingSec = 300;

    String iconIdle;
    String iconRunning;
    String iconPaused;
    String iconFinished;

    String endRtttl;
    String tickRtttl;

    unsigned long runStartMs = 0;
    uint32_t runStartRemainingSec = 0;
    uint32_t runDurationSec = 0;   // duration in force when the current run began; the progress-bar denominator (buffers a mid-run duration edit)
    unsigned long enteredFinishedMs = 0;
    unsigned long lastRealertMs = 0;
    unsigned long lastPublishMs = 0;

    // Config-mode working state (field cursor + HH/MM/SS buffers + cap-aware adjust)
    // AND timing (30 s no-input timeout + button hold-to-repeat) live in the
    // display-free TimerConfigEditor; tick() feeds it the current time and injected
    // button presses. See docs/adr/0011 (extraction) and docs/adr/0012 (timing).
    TimerConfigEditor configEditor;

    bool _suspendPersist = false;
    // Member-backed RAM state differs from the "timer" NVS namespace: set by a
    // suspended persistIfDirty() inside a PersistBatch window, or by a deferred
    // (persist=false) enum edit during a TIMER-menu scroll session. Cleared by
    // the flush that writes it (persistIfDirty / PersistBatch scope exit).
    bool _dirty          = false;

    // -- Propagation surface (device-to-device timer sync) --
    // While true, an inbound sync packet is being applied via parseCommand; the
    // broadcast* methods early-return so a received command is never re-emitted
    // (one-hop topology). See CONTEXT.md "Propagation surface".
    bool     _remoteApply = false;
    uint32_t _syncSeq     = 0;     // per-command sequence; only needs uniqueness within the dedup window

    // Bounded recently-seen (src,seq) dedup set so the 3x redundant send is applied
    // once. Extracted to its own host-testable module (SyncSeenCache, ADR-0022); the
    // UDP transport + parseCommand re-entry stay here. TTL-based, so a sender reboot
    // (seq restart) self-clears by ageing out.
    SyncSeenCache _seen;

    // -- Peer presence registry (#111 / ADR-0019, extracted to PeerRegistry per
    //    ADR-0021) --
    // The LAN peer set lives in its own host-testable module (PeerRegistry); this
    // class keeps only the beacon cadence + UDP send. Harvested UNGATED from
    // inbound presence beacons (presence is informational, not a command — it
    // bypasses the follow/target gate and applies no timer state).
    static constexpr unsigned long kPresenceIntervalMs = 30000;   // beacon cadence
    PeerRegistry  _registry;
    unsigned long _lastPresenceMs    = 0;
    bool          _presenceEverSent  = false;
    void broadcastPresence();                                  // emit one {_sync,presence:true} beacon

    // -- One-shot override (save:false), PRD #99 / issue #100 --
    // A save:false command applies its config for the CURRENT RUN only: the saved
    // config is snapshotted, the command applies live, and returnToIdle() restores
    // the snapshot when the timer next returns to Idle (reset or auto-clear). While
    // an override is active no NVS write, no config broadcast and no HA config-
    // attribute republish occur. A normal (save:true) config command mid-override
    // promotes the live config to the new saved baseline and ends the override.
    bool         _overrideActive = false;
    TcValue      _snapTable[TIMER_SETTINGS_DESC_CAP];   // Family A (inSnapshot rows), generic capture
    BuzzerMode   _snapBuzzer     = BuzzerMode::End;
    FinishedMode _snapFinished   = FinishedMode::AutoClear;
    String       _snapIconIdle, _snapIconRunning, _snapIconPaused, _snapIconFinished;
    uint32_t     _snapDuration   = 300;
    String       _snapEndRtttl, _snapTickRtttl;
    void captureSnapshot();   // record the saved config (both tables + duration + melody RAM)
    void restoreSnapshot();   // write the snapshot back (no publish/persist side effects)
    void returnToIdle();      // revert seam shared by reset() and the tick auto-clear transition

    // Honest observation carriers (issue #101). RAII: while an override is active,
    // present the SAVED config block (table inSnapshot rows + member-backed half) in
    // live storage so a carrier projection reads saved values, then restore the
    // effective (one-shot) values on scope exit. No-op when no override is active.
    // The const_cast is sound — the singleton is non-const and the swap is fully
    // reverted, so wrapping a const projection method stays observably const. Run-state
    // (duration) and sync_* (inSnapshot=false) are intentionally untouched.
    class SavedConfigScope
    {
    public:
        explicit SavedConfigScope(const TimerManager_ &t);
        ~SavedConfigScope();
        SavedConfigScope(const SavedConfigScope &) = delete;
        SavedConfigScope &operator=(const SavedConfigScope &) = delete;
    private:
        TimerManager_ &tm;
        bool           active;
        TcValue        effTable[TIMER_SETTINGS_DESC_CAP];
        BuzzerMode     effBuzzer     = BuzzerMode::End;
        FinishedMode   effFinished   = FinishedMode::AutoClear;
        String         effIconIdle, effIconRunning, effIconPaused, effIconFinished;
    };

    void buildConfigSnapshot(JsonDocument &doc) const;   // config keys only; no action/duration/sync_*
    void addSyncEnvelope(JsonObject &sync);              // forwards to SyncEnvelope::build (injects _syncSeq)

    uint32_t computeCurrentRemaining() const;
    void enterRunning();
    void enterFinished();
    void persist();
    void persistIfDirty();
    void loadMelodiesCached();

    static String validateIconName(const String &name);

public:
    // RAII guard that makes the persist-batching window a visible lexical scope
    // (PRD #29) — the ONE commit seam shared by both batch call sites:
    // parseCommand's apply block and the TIMER menu's long-press commit
    // (MenuManager). While the guard lives, member-backed persistence is
    // suspended; scope exit commits the whole Timer config, flushing both NVS
    // namespaces at most once each: the member-backed "timer" namespace iff RAM
    // holds uncommitted member state (a setter dirtied it inside the window, or
    // deferred persist=false menu edits dirtied it beforehand), then the
    // table-backed "awtrix" namespace (saveSettings) iff markTableDirty() was
    // called. Exceptions are off on this target, so "scope exit" means the
    // normal return paths.
    class PersistBatch
    {
    public:
        explicit PersistBatch(TimerManager_ &tm) : tm(tm)
        {
            tm._suspendPersist = true;
        }
        ~PersistBatch()
        {
            tm._suspendPersist = false;
            // Transient (one-shot, save:false) window: scope exit writes NOTHING to
            // flash. RAM intentionally diverges from NVS for the duration of the run
            // (returnToIdle() restores it), so the pending member-half dirty flag is
            // discarded rather than flushed, and the table half is left untouched.
            if (transient)
            {
                tm._dirty = false;
                return;
            }
            if (tm._dirty)
            {
                tm._dirty = false;
                tm.persist();
            }
            if (tableDirty)
                saveSettings();
        }
        // A table-backed ("awtrix"-namespace) value changed inside the window.
        void markTableDirty() { tableDirty = true; }
        // Mark this window one-shot: scope exit skips both NVS flushes (issue #100).
        void setTransient() { transient = true; }

        PersistBatch(const PersistBatch &) = delete;
        PersistBatch &operator=(const PersistBatch &) = delete;

    private:
        TimerManager_ &tm;
        bool tableDirty = false;
        bool transient  = false;
    };

    static TimerManager_ &getInstance();
    void setup();
    void tick();

    void start();
    void pause();
    void reset();

    void setDuration(uint32_t seconds);
    // persist=false applies + publishes live but defers the NVS write (the TIMER
    // menu's deferred-to-commit path; mirrors setIcon*'s publish flag). Every other
    // caller uses the default and persists immediately. See docs/adr/0008.
    void setBuzzerMode(BuzzerMode m, bool persist = true);
    void setFinishedMode(FinishedMode m, bool persist = true);

    // Time <-> seconds helpers shared by the MQTT/HA string path and the
    // on-device config editor. parseHMS/formatHMS are the external string
    // contract (see docs/timer.md); secondsToHMS/hmsToSeconds are the raw math.
    static void     secondsToHMS(uint32_t sec, uint32_t &h, uint32_t &m, uint32_t &s);
    static uint32_t hmsToSeconds(uint32_t h, uint32_t m, uint32_t s);
    static String   formatHMS(uint32_t seconds);
    static bool     parseHMS(const String &s, uint32_t &outSeconds);

    // Validation predicates shared by parseCommand (every control surface) and
    // the HA duration callback. Range/out-of-range is rejected, not clamped.
    static bool     isValidDuration(uint32_t seconds);
    static bool     parseBuzzerMode(const String &s, BuzzerMode &out);
    static bool     parseFinishedMode(const String &s, FinishedMode &out);
    static bool     isValidIconName(const String &name);
    static bool     isValidAction(const String &s);

    void setIconIdle    (const String &name, bool publish = true);
    void setIconRunning (const String &name, bool publish = true);
    void setIconPaused  (const String &name, bool publish = true);
    void setIconFinished(const String &name, bool publish = true);

    void publishIcons();

    // Republish the current run-state string through the wire seam (issue #31).
    // Public because MQTTManager re-emits it on connect / discovery enable; it
    // is the only path that puts the state key on the wire.
    void publishState();

    // Same for the remaining-seconds key (issue #32): the only path that puts
    // remaining on the wire; the periodic republish throttle stays in tick().
    void publishRemaining();

    // Same for the duration key (issue #34): the only path that puts the
    // trimmed-HMS duration on the wire. Public for the same connect /
    // discovery-enable republish sites.
    void publishDuration();

    // Same for the two enum keys (issue #33), dispatching through their member-
    // config rows' publish hooks: the only paths that put buzzer/finished on
    // the wire. Public for the same connect / discovery-enable republish sites.
    void publishBuzzerMode();
    void publishFinishedMode();

    // Full wire refresh (issue #41): republish every Timer wire artifact once —
    // the run-state trio plus every member-config row's publish hook, derived
    // from TIMER_MEMBER_CONFIG_DESCS so a new published row cannot be skipped.
    // The single call the connect / discovery-enable republish sites make.
    void publishAllWire();

    // Publish one HA carrier's read-only JSON attribute object — the carrier's
    // mapped settings keys built via timerBuildAttributeGroup — onto its
    // json_attr_t topic via the wire seam (PRD #57 / issue #58, generalizing the
    // bespoke realert_interval publish of issue #51). Retained, so HA repopulates
    // after a restart for free. No-op for a carrier with no mapped keys.
    void publishAttributeGroup(TimerHaEntity carrier);

    // Publish every distinct carrier's attribute object once. The single call the
    // discovery-enable / reconnect paths make right after publishAllWire(), so HA
    // never sees an entity with missing attributes.
    void publishAllAttributeGroups();

    // Clear (empty retained payload) every distinct carrier's json_attr_t topic —
    // the teardown mirror of publishAllAttributeGroups(). The discovery teardown
    // (removeTimerHAEntities, SHOW_TIMER true->false) calls this so disabling the
    // Timer leaves no orphaned attribute object retained on the broker (issue #60).
    void clearAllAttributeGroups();

    TimerCmdResult parseCommand(const char *json);

    // -- Home Assistant control adapter (issue #109) --
    // Route a single HA timer callback through parseCommand instead of a deep
    // setter, so HA edits get the SAME atomic-reject validation, the same
    // propagation, and the same codec strings as the {prefix}/timer MQTT surface.
    // Each entity builds the minimal JSON command it represents and hands it to
    // parseCommand, mirroring the sync receive path. Display-free (no ArduinoHA,
    // no MQTT client) so the HA->parseCommand path is host-testable.
    //
    // rawValue per entity:
    //   * Buzzer / Finished : the selected select-option INDEX as a decimal
    //     string; mapped through the per-enum codec (ADR-0010) to the canonical
    //     wire spelling, so emitted and accepted JSON cannot drift.
    //   * Duration          : the raw HH:MM:SS text; parseCommand owns the
    //     parse/validate (parseHMS + range), so that logic is NOT duplicated here.
    //   * Start/Pause/Reset : ignored; the entity selects the action.
    // Returns parseCommand's result so the caller can echo the canonical live
    // value back on a non-Ok result (snap-back to the last valid value).
    TimerCmdResult timerHaApply(TimerHaEntity entity, const String &rawValue);

    // -- Propagation surface --
    // Emit one UDP broadcast mirroring a locally-accepted action to peers. No-ops
    // when sync is off (empty target list) or while applying an inbound packet
    // (_remoteApply). Run-state and config travel on separate packets; `action`
    // may be nullptr for a duration-only edit. See docs/adr/0006.
    void broadcastRunState(const char *action);
    void broadcastConfig();
    // Validate, gate (echo/follow/target/dedup), then apply an inbound sync packet
    // through parseCommand under the _remoteApply guard. A presence beacon
    // (presence:true) is harvested into the peer registry UNGATED and short-circuits
    // before any command path (it carries no action/config and changes no state).
    void applySyncCommand(const char *json);

    // Peer presence (#111 / ADR-0019). Called from the device loop with the current
    // millis(): emits a presence beacon at most once per kPresenceIntervalMs when on
    // a real network (NEVER in AP mode — broadcasts unconditionally otherwise so a
    // standalone clock is still discoverable), and ages out stale peers each call.
    void tickPresence(unsigned long nowMs);
    // Peer registry observers (consumed by the dynamic HA Targets select in #112).
    // Thin forwarders onto the extracted PeerRegistry, so consumers (MQTTManager)
    // are unchanged by the extraction (ADR-0021).
    int  peerCount() const { return _registry.count(); }
    bool hasPeer(const String &id) const { return _registry.has(id); }
    size_t peerIds(String *out, size_t cap) const { return _registry.ids(out, cap); }

    void onShowTimerChange(bool prev, bool now);

    void enterConfigMode();
    void exitConfigMode();
    void configCycleField();
    void configAdjust(int delta);
    bool    isInConfig()      const { return configEditor.isActive(); }
    uint8_t getConfigField()  const { return configEditor.field(); }
    uint8_t getConfigHH()     const { return configEditor.hh(); }
    uint8_t getConfigMM()     const { return configEditor.mm(); }
    uint8_t getConfigSS()     const { return configEditor.ss(); }

    TimerState   getState()        const { return state; }
    uint32_t     getRemaining()    const { return remainingSec; }
    uint32_t     getDuration()     const { return durationSec; }
    uint32_t     getRunDuration()  const { return runDurationSec; }
    BuzzerMode   getBuzzerMode()   const { return buzzerMode; }
    FinishedMode getFinishedMode() const { return finishedMode; }

    const String &getIconIdle()     const { return iconIdle; }
    const String &getIconRunning()  const { return iconRunning; }
    const String &getIconPaused()   const { return iconPaused; }
    const String &getIconFinished() const { return iconFinished; }
    const String &getIconForState(TimerState s) const;

    const char *getStateString() const;

    // Canonical output spellings for the timer enums, co-located with
    // getStateString() so the one true spelling of each enum lives in one place.
    // (The command parser additionally tolerates non-hyphen aliases on input.)
    // Public so the member-config table's emit hooks (TimerSettings.cpp) can read
    // them when building the propagated config snapshot. See docs/adr/0009.
    const char *buzzerModeString() const;
    const char *finishedModeString() const;

    // Live read-only snapshot for the GET /api/timer observation surface.
    // Reports computeCurrentRemaining() (wall-clock fresh), not the throttled
    // cached value. See docs/api.md and CONTEXT.md "observation surface".
    String getStateJson() const;
};

extern TimerManager_ &TimerManager;

#endif
