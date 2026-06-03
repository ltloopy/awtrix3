#ifndef TimerManager_h
#define TimerManager_h

#include <Arduino.h>
#include <ArduinoJson.h>

enum class TimerState : uint8_t { Idle = 0, Running = 1, Paused = 2, Finished = 3 };
enum class BuzzerMode : uint8_t { Off = 0, End = 1, Countdown = 2 };
enum class FinishedMode : uint8_t { AutoClear = 0, Hold = 1, ReAlert = 2 };

// Result of parseCommand. All control surfaces share one validation policy
// (reject invalid input atomically); only the HTTP API surfaces this as a
// status code — MQTT ignores it. See docs/adr/0001-timer-command-validation-parity.md.
enum class TimerCmdResult : uint8_t { Ok = 0, BadJson = 1, BadField = 2, Disabled = 3 };

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
    unsigned long enteredFinishedMs = 0;
    unsigned long lastRealertMs = 0;
    unsigned long lastPublishMs = 0;

    bool          inConfig            = false;
    uint8_t       configField         = 0;
    uint8_t       configHH = 0, configMM = 0, configSS = 0;
    unsigned long configLastInputMs   = 0;
    unsigned long configRepeatLeftMs  = 0;
    unsigned long configRepeatRightMs = 0;

    bool _suspendPersist = false;
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
    void publishState();
    void publishRemaining();
    void publishDuration();
    void publishBuzzerMode();
    void publishFinishedMode();
    void persist();
    void persistIfDirty();
    void loadMelodiesCached();

    static String validateIconName(const String &name);

    // Canonical output spellings for the timer enums, co-located with
    // getStateString() so the one true spelling of each enum lives in one place.
    // (The command parser additionally tolerates non-hyphen aliases on input.)
    const char *buzzerModeString() const;
    const char *finishedModeString() const;

public:
    static TimerManager_ &getInstance();
    void setup();
    void tick();

    void start();
    void pause();
    void reset();

    void setDuration(uint32_t seconds);
    void setBuzzerMode(BuzzerMode m);
    void setFinishedMode(FinishedMode m);

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
    bool    isInConfig()      const { return inConfig; }
    uint8_t getConfigField()  const { return configField; }
    uint8_t getConfigHH()     const { return configHH; }
    uint8_t getConfigMM()     const { return configMM; }
    uint8_t getConfigSS()     const { return configSS; }

    TimerState   getState()        const { return state; }
    uint32_t     getRemaining()    const { return remainingSec; }
    uint32_t     getDuration()     const { return durationSec; }
    BuzzerMode   getBuzzerMode()   const { return buzzerMode; }
    FinishedMode getFinishedMode() const { return finishedMode; }

    const String &getIconIdle()     const { return iconIdle; }
    const String &getIconRunning()  const { return iconRunning; }
    const String &getIconPaused()   const { return iconPaused; }
    const String &getIconFinished() const { return iconFinished; }
    const String &getIconForState(TimerState s) const;

    const char *getStateString() const;

    // Live read-only snapshot for the GET /api/timer observation surface.
    // Reports computeCurrentRemaining() (wall-clock fresh), not the throttled
    // cached value. See docs/api.md and CONTEXT.md "observation surface".
    String getStateJson() const;
};

extern TimerManager_ &TimerManager;

#endif
