# Extract the inbound sync gate into a host-testable SyncEnvelope (pure classify)

Status: accepted

Builds on [ADR-0006](0006-timer-multi-device-sync.md) (the propagation surface),
[ADR-0019](0019-peer-presence-registry.md) (the ungated presence harvest) and
[ADR-0022](0022-sync-seen-cache-extraction.md) (the dedup set). The wire format, the 3×
redundant send, the consent/targeting gate, the ungated presence harvest and the one-hop
`parseCommand` re-entry are all **unchanged**; this ADR only moves the inbound **decision**
and the envelope **construction** behind their own seam. It is the third cut in the #131
deepening trajectory, after [ADR-0021](0021-peer-registry-extraction.md) (PeerRegistry) and
ADR-0022 (SyncSeenCache), and continues the line of
[ADR-0011](0011-timer-config-editor-extraction.md) (the display-free `TimerConfigEditor`).

## Context

`TimerManager::applySyncCommand` was the inbound receive path, and its decision tree —
echo-drop → presence-harvest (ungated) → follow consent → target match → dedup → apply — sat
**inline**, interleaved with `deserializeJson`, `_registry.record`, the `_seen.seen` dedup,
the `_remoteApply` flag and the `parseCommand` re-entry. The many branches were therefore
exercisable only through the multi-thousand-line `test_timer` fixture (buried in `test_S4`),
which must stand up `uniqueID`, `AP_MODE`, `ServerManager`, and the MQTT/Preferences stubs
before any assertion. The send side had a matching tangle: `addSyncEnvelope` built the
`{src,seq,tgt}` envelope inline (including an untested CSV target-list split), and
`broadcastPresence` re-implemented the `{src,seq}` envelope a second time.

Unlike PeerRegistry and SyncSeenCache, the thing being lifted here is not a **stateful set**
but a **pure decision** plus a **pure constructor** — so the module is stateless (free
functions in a namespace), not a class.

## Decision

### A stateless `SyncEnvelope` owns the decision and the envelope; `TimerManager` is the shell

`SyncEnvelope` ([src/SyncEnvelope.h](../../src/SyncEnvelope.h) /
[SyncEnvelope.cpp](../../src/SyncEnvelope.cpp)) is a namespace of free functions over a value
struct:

```cpp
struct Context  { String ownId; bool follow; };
struct Decision { enum Kind { Ignore, HarvestPresence, Apply } kind; String src; uint32_t seq; };

Decision classify(JsonVariantConst packet, const Context &ctx);   // pure receive policy
bool     targetsMe(JsonVariantConst tgt, const String &ownId);
void     build(JsonObject sync, const String &ownId, uint32_t seq);                       // {src,seq}
void     build(JsonObject sync, const String &ownId, uint32_t seq, const String &targets); // + tgt
```

`TimerManager` keeps the impure shell: `applySyncCommand` deserializes, calls `classify`, then
acts on the `Decision` (record a presence harvest / dedup-then-`parseCommand` an apply under
`_remoteApply` / ignore). `addSyncEnvelope` becomes a thin forwarder into `build` (injecting the
`_syncSeq` counter it still owns), and `broadcastPresence` uses the bare `build` overload.

The cut-line mirrors ADR-0021/0022: **the decision and the envelope format leave; the UDP
transport and the `parseCommand` re-entry stay** — one place still owns "how a sync packet
arrives and re-enters the local control surface."

### `classify`'s context is `{ownId, follow}` — *not* the receiver's own target list

The receive decision reads only this clock's identity (`ownId`, for echo-drop and `targetsMe`)
and its **consent** axis (`follow`). It deliberately does **not** read this clock's own
`TIMER_SYNC_TARGETS`. Per the **Sync roles** model (CONTEXT.md), the two axes are independent:
a follower obeys on the **sender's** `tgt` plus its **own** `follow` consent — its own target
list is the **send** axis and has no role in receiving.

- **vs. carrying `targets` into `classify`** (as an early sketch did) — rejected. It would be a
  dead parameter at best; at worst a future maintainer "wires it up" to gate receiving on the
  local list, which silently **breaks the follower role** — a pure follower has an empty target
  list, so it would then obey nobody. Keeping the field out of `Context` makes that mistake
  unrepresentable. The local `targets` belongs only to `build` (the send axis), where it is
  actually consumed.

### `classify` is pure; the dedup stays the shell's stateful guard

`classify` decides the **four pure gates** (echo / presence / follow / target). The **dedup**
is deliberately **not** in `classify`: `SyncSeenCache::seen()` is test-and-**record**
(stateful), and ADR-0022 just finished pulling that state out into its own module. Folding it
back into a function would re-couple `SyncEnvelope` to the cache and make a "pure" function
mutate. So the shell runs `_seen.seen(...)` as the single stateful guard between the `Apply`
verdict and the re-entry, next to the other side effects (`record`, `parseCommand`).

```cpp
auto d = SyncEnvelope::classify(doc.as<JsonVariantConst>(), {uniqueID, TIMER_SYNC_FOLLOW});
switch (d.kind) {
  case Decision::HarvestPresence: _registry.record(d.src, millis()); break;
  case Decision::Apply:
    if (_seen.seen(d.src, d.seq, millis())) return;   // redundant copy of a burst → drop
    _remoteApply = true; parseCommand(json); _remoteApply = false;
    break;
  case Decision::Ignore: break;
}
```

This refines the issue's draft AC wording ("the echo/presence/follow/target/**dedup** decision
is a pure function") to the honest split: **four pure gates in `classify`, one stateful dedup
in the shell.**

### `Decision` carries the parsed `src`/`seq`

`classify` parses `src`/`seq` once to decide; it returns them on the `Decision` so the shell
never re-reaches into the `JsonDocument` (the harvest needs `src`; the dedup needs `src`+`seq`).
The shell becomes a pure orchestrator of side effects. The small owned `String src` is
negligible against the `parseCommand` re-entry that immediately follows an `Apply`.

### A dedicated `native_sync` test env — but it needs ArduinoJson

`test_syncenvelope` lives in its own `[env:native_sync]`, which compiles **only**
`SyncEnvelope.cpp` with no Timer sources, no stubs and no `native_prelude` force-include —
exactly as `[env:native_peer]`/`[env:native_seen]` do for their modules. The suite linking
against no Timer code *is* the architectural claim: the receive decision needs nothing but the
packet + `{ownId, follow}`.

One honest caveat distinguishes it from its two siblings: `SyncEnvelope` reads
`JsonVariantConst`, so `native_sync` **must** link ArduinoJson (with
`ARDUINOJSON_ENABLE_ARDUINO_STRING=1`, as `[env:native]` does). Its claim is therefore "links
against no Timer code," slightly weaker than `native_peer`/`native_seen`'s "links against
nothing but ArduinoFake's `String`." The dependency is irreducible — the envelope *is* JSON.

## Consequences

- `TimerManager` sheds the inline gate, `syncTargetsMe`, and the inline envelope/CSV-split
  bodies; `applySyncCommand` becomes deserialize + act-on-`Decision`, `addSyncEnvelope`
  forwards to `SyncEnvelope::build`, and `broadcastPresence` uses the bare `build` overload.
  Public API and the sync wire format are unchanged. (`broadcastPresence`'s `StaticJsonDocument`
  grows 128→256, matching the pause/reset beacon, for the `build` path's host-side pool
  headroom — a host-test artifact, negligible on the 32-bit device.)
- New source files `SyncEnvelope.{h,cpp}`. `SyncEnvelope.cpp` is added to `[env:native]`'s
  `build_src_filter` (so `TimerManager.o` resolves it for `test_timer`) and is the sole source
  of `[env:native_sync]`; the firmware envs glob `src/` and pick it up automatically.
- New host suite `test/test_syncenvelope` (`test_SE1..SE14`) drives `classify`/`targetsMe`/`build`
  directly: no-envelope/empty-src/own-echo Ignore, ungated presence harvest, consent gate,
  all/array-hit/array-miss targeting, the **follower invariant** (applies regardless of any local
  target list), and `build`'s bare/`all`/CSV/empty forms. No singleton, no globals, no stubs. The
  ADR-0006 `test_S4` gating and the ADR-0019 presence cases in `test_timer` stay green unchanged,
  proving the extraction is behaviour-preserving.
- CI (`.github/workflows/test.yml`) runs `pio test -e native -e native_peer -e native_seen -e native_sync`.
- No NVS format change, no new HA entities, no MQTT/HTTP/sync surface change. Device build
  (`ulanzi`) unaffected.

See also: [ADR-0006](0006-timer-multi-device-sync.md) (the gate this extracts),
[ADR-0021](0021-peer-registry-extraction.md) / [ADR-0022](0022-sync-seen-cache-extraction.md)
(the two sibling cuts whose cut-line this mirrors).
