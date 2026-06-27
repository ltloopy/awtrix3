# Extract the sync dedup window into a host-testable SyncSeenCache

Status: accepted

Builds on [ADR-0006](0006-timer-multi-device-sync.md), which made each sync command a
small burst of redundant copies (loss tolerance) de-duplicated by `(src, seq)` at the
receiver. The wire format, the 3× send, the consent gate and the one-hop re-entry are all
**unchanged**; this ADR only moves the recently-seen *set* behind its own seam. It is the
deliberate twin of [ADR-0021](0021-peer-registry-extraction.md) (the same extraction move
for the peer-presence registry) and continues the trajectory of
[ADR-0011](0011-timer-config-editor-extraction.md) (the display-free `TimerConfigEditor`).

## Context

`TimerManager` has accreted nine responsibilities behind one singleton — run-state machine,
NVS persistence/batching, command parse+validate, one-shot override, HA control adapter,
wire publish, HA attribute groups, the sync propagation surface, and the peer-presence
registry. Its only test surface is the multi-thousand-line `test_timer.cpp` fixture, which
must stand up `uniqueID`, `AP_MODE`, `ServerManager`, the MQTT and Preferences stubs before
any assertion.

The receive-side dedup set (ADR-0006) is, like the peer registry of ADR-0019/0021, cleanly
separable: a bounded `{src, seq, atMs}` RAM-set with TTL ageing and round-robin eviction,
with **zero coupling to timer run-state**. Its one method already took an injected `nowMs`,
so the only thing tying it to the singleton was co-location — the set lived as
`TimerManager` members (`struct SyncSeen`, `_syncSeen[]`, `kSyncSeenMax`, `_syncSeenIdx`)
and the test-and-record body (`syncSeenRecently`). The set's mechanics could only be
exercised through the full fixture (`test_S4`'s 3× redundant-send assertion drives it
indirectly).

## Decision

### A standalone `SyncSeenCache` owns the set; `nowMs` is injected

`SyncSeenCache` ([src/SyncSeenCache.h](../../src/SyncSeenCache.h) /
[SyncSeenCache.cpp](../../src/SyncSeenCache.cpp)) owns the bounded array and the
test-and-record/age logic behind a deliberately tiny interface:

```cpp
bool seen(const String &src, uint32_t seq, unsigned long nowMs);  // test-and-record, atomic
void clear();                                                     // reboot: pure RAM
```

`kMax` and `kTtlMs` move with it. `seen()` does the check **and** the record in one call —
there is exactly one call site (`applySyncCommand`), so a separate `check`/`record` split
would be pointless and racy. It returns `true` when `(src, seq)` was seen within the TTL (a
redundant copy the caller drops) **without refreshing** the existing entry, and otherwise
records it and returns `false`. The whole contract is host-testable with no globals in
scope.

### First-seen ages out — the opposite expiry semantic to `PeerRegistry`

This is the load-bearing distinction, and the reason the two siblings are **not** merged
(see rejected alternatives). On a hit, `PeerRegistry.record()` *refreshes* `lastSeen` — a
peer that keeps beaconing stays "present." `SyncSeenCache.seen()` must do the **opposite**:
it leaves the original `atMs` untouched, so a command ages out a fixed TTL after it was
*first* applied, not after the last redundant copy arrived. That is what lets a sender
reboot (which restarts `seq` from 0) self-clear by ageing out rather than colliding with a
stale high-water entry. The `nowMs == 0` empty-slot sentinel collision is preserved (it
folds to `1`), and a FIFO ring evicts at the bound.

### The cut line is the *set* only — transport and re-entry stay on TimerManager

`SyncSeenCache` deliberately knows nothing about the UDP broadcast, the `_sync` envelope, or
the `parseCommand` re-entry. `TimerManager` keeps the transport and calls the cache once,
inside `applySyncCommand`, *after* the consent gate and targeting check:

```cpp
if (!TIMER_SYNC_FOLLOW) return;                  // consent gate
if (!syncTargetsMe(sync["tgt"])) return;         // not addressed here
if (_seen.seen(src, sync["seq"], millis())) return;  // redundant copy → drop
```

The cut-line mirrors ADR-0021: the algorithm leaves, the I/O stays — one place still owns
"how a sync packet arrives and re-enters the local control surface."

- **vs. also pulling the consent/targeting checks into the cache** — rejected: the cache
  would then mean both "have I seen this" *and* "should I obey this," re-coupling it to
  `TIMER_SYNC_FOLLOW` and the targeting globals and defeating the dependency-free link.
- **vs. the cache reading `millis()` itself** — rejected: injecting `nowMs` keeps the unit
  testable without a clock stub, exactly as `PeerRegistry` takes its `nowMs`.

### A dedicated `native_seen` test env proves the dependency-free link

PlatformIO eagerly links every `build_src_filter` object into each test program, so a suite
in the existing `[env:native]` would drag in `TimerManager.o`/`TimerSettings.o` and their
globals. `SyncSeenCache` needs none of that — so `test_syncseen` lives in its own
`[env:native_seen]`, which compiles **only** `SyncSeenCache.cpp` with no Timer sources, no
stubs, and no `native_prelude` force-include. The suite linking against nothing but
ArduinoFake's `String` *is* the architectural claim, made executable — exactly as
`[env:native_peer]` is for `PeerRegistry`.

## Rejected alternatives

### Unify with `PeerRegistry` under a generic `BoundedTtlTable`

Both modules are a bounded, TTL-aged, src-keyed RAM-set with round-robin/stalest-slot
eviction, so an obvious suggestion is to hoist the shared shape into a generic
`BoundedTtlTable<Key, Value>` and have `PeerRegistry` and `SyncSeenCache` both wrap it.
**Rejected** — and recorded here so future architecture reviews don't re-suggest the merge.

Apply the deletion test: ask what genuinely disappears if the generic absorbs each wrapper.
The answer leaves the generic **borderline-shallow**. It would relocate roughly a dozen
mechanical lines — the backing array, the linear find, and the evict-oldest cursor — while
every *meaning-bearing* part stays behind a per-consumer wrapper anyway:

- **Opposite expiry semantics.** `PeerRegistry` refreshes `lastSeen` on a hit (keep-alive);
  `SyncSeenCache` must **not** refresh (first-seen ages out). A generic table would have to
  expose this as a per-call flag or two distinct methods — i.e. the wrappers keep owning the
  one decision that actually differs.
- **Own-id exclusion** lives only in `PeerRegistry` (a peer must not register itself); the
  dedup cache has no own-id concept (own echoes are dropped earlier in `applySyncCommand`).
- **Sorted, cap-bounded `ids()` export** is a `PeerRegistry` concern (the HA Targets select
  reads it); the dedup cache has no export at all — its result is a single `bool`.
- **Tuple key.** The dedup key is `(src, seq)`; the registry key is `uniqueID` alone. The
  generic would need a key-equality hook, again pushing the meaning back into the wrappers.
- **Test-and-record.** `SyncSeenCache` fuses check + record into one atomic op (one call
  site); `PeerRegistry` splits `record` / `prune` / `has` / `ids` across four consumers.

So the merge would trade two small, self-documenting modules — each named for exactly what
it means, each with its own dependency-free host suite — for one generic plus two wrappers
that still hold all the divergent semantics. That is more indirection for less clarity, and
it would couple two modules that have **no** reason to change together. Keeping them as
deliberate **siblings** (same shape, separate names) is the cheaper, clearer design.

## Consequences

- `TimerManager` sheds `struct SyncSeen`, `_syncSeen[]`, `kSyncSeenMax`, `_syncSeenIdx`, and
  the `syncSeenRecently` body; it gains a `SyncSeenCache _seen` member. `applySyncCommand`
  calls `_seen.seen(...)` for the redundant-copy drop, and `setup()` calls `_seen.clear()`
  (a reboot has applied nothing yet) alongside the peer-registry clear. Public API and the
  sync wire format are unchanged.
- New source files `SyncSeenCache.{h,cpp}`. `SyncSeenCache.cpp` is added to `[env:native]`'s
  `build_src_filter` (so `TimerManager.o` resolves it for `test_timer`) and is the sole
  source of `[env:native_seen]`; the firmware envs glob `src/` and pick it up automatically.
- New host suite `test/test_syncseen` (`test_SS1..SS6`) drives a bare `SyncSeenCache`:
  first-seen returns `false`, immediate re-deliver is deduped, same-src/new-seq is accepted,
  TTL age-out, FIFO wrap at the bound, and the `nowMs == 0` sentinel. No singleton, no
  globals. The ADR-0006 `test_S4` 3×-redundant-send case in `test_timer` stays green
  unchanged, proving the extraction is behaviour-preserving.
- CI (`.github/workflows/test.yml`) runs `pio test -e native -e native_peer -e native_seen`.
- No NVS format change, no new HA entities, no MQTT/HTTP/sync surface change. Device build
  (`ulanzi`) unaffected.

See also: [ADR-0006](0006-timer-multi-device-sync.md) (the dedup window this extracts),
[ADR-0021](0021-peer-registry-extraction.md) (the sibling extraction whose shape this
mirrors but whose expiry semantic it deliberately inverts).
