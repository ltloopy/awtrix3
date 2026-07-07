# Extract the peer-presence registry into a host-testable PeerRegistry

Status: accepted

Builds on [ADR-0019](0019-peer-presence-registry.md), which introduced the peer
registry as private members + methods on `TimerManager`. The presence beacon, the
ungated harvest, the bound/TTL/eviction policy and the dynamic HA Targets select are
all **unchanged**; this ADR only moves the registry *set* behind its own seam. It
continues the extraction trajectory of [ADR-0011](0011-timer-config-editor-extraction.md)
(the display-free `TimerConfigEditor`).

## Context

`TimerManager` has accreted nine responsibilities behind one singleton — run-state
machine, NVS persistence/batching, command parse+validate, one-shot override, HA
control adapter, wire publish, HA attribute groups, the sync propagation surface, and
the peer-presence registry. Its only test surface is the 4,794-line `test_timer.cpp`
fixture, which must stand up `uniqueID`, `AP_MODE`, `ServerManager`, the MQTT and
Preferences stubs before any assertion.

The peer registry (ADR-0019) is the most cleanly separable of the nine: a bounded
`{uniqueID, lastSeen}` set with ageing + stalest-slot eviction and a sorted id export,
with **zero coupling to timer run-state**. Its methods already take an injected `nowMs`
(ADR-0019 §4), so the only thing tying them to the singleton was co-location — the
own-id exclusion read the `uniqueID` global, and the set lived as `TimerManager`
members. The set's mechanics could only be exercised through the full fixture.

## Decision

### A standalone `PeerRegistry` owns the set; the own-id is injected

`PeerRegistry` ([src/PeerRegistry.h](../../src/PeerRegistry.h) /
[PeerRegistry.cpp](../../src/PeerRegistry.cpp)) owns the bounded array and the
record/age/query logic:

```cpp
void   setOwnId(const String &id);                 // injected once, no global read
void   record(const String &src, unsigned long nowMs);
void   prune(unsigned long nowMs);
bool   has(const String &id) const;
size_t ids(String *out, size_t cap) const;         // sorted asc, cap-bounded
int    count() const;
void   clear();                                    // reboot: pure RAM
```

`kPeerMax` and `kPeerTtlMs` move with it. The own-id is injected via `setOwnId`
(called from `TimerManager::setup()`), so `record()` self-filters echoed own beacons
**without reading the `uniqueID` global** — the whole contract is host-testable with no
globals in scope.

### The cut line is the *set* only — cadence and transport stay on TimerManager

`PeerRegistry` deliberately knows nothing about beacon cadence, the UDP transport, or
AP mode. `TimerManager` keeps `_lastPresenceMs`, `_presenceEverSent`,
`kPresenceIntervalMs`, `broadcastPresence()` and `tickPresence(nowMs)` — one place
still owns "when do I announce myself." `tickPresence` now calls `_registry.prune(now)`
then the unchanged AP gate + cadence + send.

- **vs. also pulling beacon cadence into the registry** — rejected: it would split the
  beacon logic across two modules and make the registry mean both "who is on the LAN"
  *and* "when do I announce," muddying the concept boundary for little gain.
- **vs. the registry reading `uniqueID` directly** (as the old `recordPeer` did) —
  rejected: it re-couples the unit to a global and forces the set's tests to define
  `uniqueID`. Injection keeps the link dependency-free.

### Forwarders keep consumers unchanged

`TimerManager::peerCount`/`hasPeer`/`peerIds` are retained as thin forwarders onto the
`PeerRegistry` member, so `MQTTManager`'s dynamic Targets select and `main.cpp`'s loop
hook are untouched. The seam is entirely internal to `TimerManager`.

### A dedicated `native_peer` test env proves the dependency-free link

PlatformIO eagerly links every `build_src_filter` object into each test program, so a
suite in the existing `[env:native]` would drag in `TimerManager.o`/`TimerSettings.o`
and their globals. `PeerRegistry` needs none of that — so `test_peerregistry` lives in
its own `[env:native_peer]`, which compiles **only** `PeerRegistry.cpp` with no Timer
sources, no stubs, and no `native_prelude` force-include. The suite linking against
nothing but ArduinoFake's `String` *is* the architectural claim, made executable.

## Consequences

- `TimerManager` sheds `struct Peer`, `_peers[]`, `_peerCount`, `kPeerMax`,
  `kPeerTtlMs`, and the `recordPeer`/`prunePeers`/`hasPeer`/`peerIds` bodies; it gains a
  `PeerRegistry _registry` member. `setup()` calls `setOwnId`/`clear`, the presence
  harvest calls `_registry.record`, and `tickPresence` calls `_registry.prune`. Public
  API unchanged (same forwarders + getters).
- New source files `PeerRegistry.{h,cpp}`. `PeerRegistry.cpp` is added to `[env:native]`'s
  `build_src_filter` (so `TimerManager.o` resolves it for `test_timer`) and is the sole
  source of `[env:native_peer]`; the firmware envs glob `src/` and pick it up automatically.
- New host suite `test/test_peerregistry` (`test_P1..P8`) drives a bare `PeerRegistry`:
  own-id exclusion, refresh-not-duplicate, TTL age-out + compaction, stalest-slot
  eviction at the bound, sorted + cap-bounded `ids()`, and `clear()`. No singleton, no
  globals. The ADR-0019 `tickPresence`/cadence cases in `test_timer` stay green
  unchanged, proving the extraction is behaviour-preserving.
- CI (`.github/workflows/test.yml`) runs `pio test -e native -e native_peer`.
- No NVS format change, no new HA entities, no MQTT/HTTP/sync surface change. Device
  build (`ulanzi`) unaffected.

See also: [ADR-0019](0019-peer-presence-registry.md) (the registry this extracts),
[ADR-0011](0011-timer-config-editor-extraction.md) (the same extraction move for the
duration editor).
