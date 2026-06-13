#ifndef TimerManager_h
#define TimerManager_h

#include <Arduino.h>
#include <ArduinoJson.h>

#include "TimerEnums.h"          // TimerState + BuzzerMode / FinishedMode + their codec tables (ADR-0010)
#include "TimerConfigEditor.h"   // display-free duration editor; owns the config-mode working state (ADR-0011)

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

    // Bounded recently-seen (src,seq) cache so the 3x redundant send is applied
    // once. TTL-based, so a sender reboot (seq restart) self-clears by ageing out.
    struct SyncSeen { String src; uint32_t seq = 0; unsigned long atMs = 0; };
    static constexpr uint8_t kSyncSeenMax = 8;
    SyncSeen _syncSeen[kSyncSeenMax];
    uint8_t  _syncSeenIdx = 0;
    bool syncSeenRecently(const String &src, uint32_t seq, unsigned long nowMs);

    void buildConfigSnapshot(JsonDocument &doc) const;   // config keys only; no action/duration/sync_*
    void addSyncEnvelope(JsonObject &sync);              // src/seq/tgt
    bool syncTargetsMe(JsonVariantConst tgt) const;      // does _sync.tgt cover this clock's uniqueID?

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

        PersistBatch(const PersistBatch &) = delete;
        PersistBatch &operator=(const PersistBatch &) = delete;

    private:
        TimerManager_ &tm;
        bool tableDirty = false;
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

    // Publish the finished-mode select's JSON attributes — a retained
    // {"realert_interval":N} carrying the current TIMER_REALERT_INTERVAL — onto
    // the select's json_attr_t topic via the wire seam (issue #51 / PRD #17).
    // Called right after the finished-mode publish on the connect and
    // discovery-enable paths so HA shows the interval the moment the entity
    // comes online and after any HA/broker restart.
    void publishFinishedAttributes();

    TimerCmdResult parseCommand(const char *json);

    // -- Propagation surface --
    // Emit one UDP broadcast mirroring a locally-accepted action to peers. No-ops
    // when sync is off (empty target list) or while applying an inbound packet
    // (_remoteApply). Run-state and config travel on separate packets; `action`
    // may be nullptr for a duration-only edit. See docs/adr/0006.
    void broadcastRunState(const char *action);
    void broadcastConfig();
    // Validate, gate (echo/follow/target/dedup), then apply an inbound sync packet
    // through parseCommand under the _remoteApply guard.
    void applySyncCommand(const char *json);

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
