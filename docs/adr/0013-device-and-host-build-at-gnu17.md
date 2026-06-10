# Build the device firmware at gnu++17, and compile it on PRs

Status: accepted

## Context

The `TimerConfigEditor` work ([ADR-0011](0011-timer-config-editor-extraction.md),
[ADR-0012](0012-timer-config-timing-in-editor.md)) introduced an injected button-state value:

```cpp
struct ButtonState { bool leftPressed = false; bool rightPressed = false; };
```

constructed at the `TimerManager::tick()` call site with a two-arg brace-init,
`TimerConfigEditor::ButtonState buttons{ bL && bL->isPressed(), bR && bR->isPressed() }`.

A class with **default member initializers is not an aggregate under C++11** (the rule was relaxed
in C++14), so that two-arg brace-init is ill-formed in C++11. The host test env builds at
`-std=gnu++17` (`[env:native]`), where the construct is legal, so `pio test -e native` was green.
The Arduino-ESP32 device envs set no `-std` and the platform compiles as `gnu++11`, where the same
line **fails to compile** at `TimerManager.cpp.o` — so `pio run -e ulanzi` was broken (issue #25).

The break stayed invisible because PR CI (`.github/workflows/test.yml`) ran **only** the native
suite; the device firmware was compiled (`.github/workflows/main.yml`) **only on push to `main`**.
A PR could be fully green while the device build was red — a host-green/device-red drift with no
guard on the path that merges code.

## Decision

### `ButtonState` carries an explicit constructor (standard-independent)

`ButtonState` gains an explicit `(bool, bool)` constructor (plus a defaulted default ctor), keeping
the member defaults. Once a constructor is user-declared the call-site `{ a, b }` is list-init that
selects it — valid in C++11/14/17 — so the construct no longer depends on the C++14 relaxed-aggregate
rule. This alone fixes the device build regardless of the compiler standard.

### Device and host both build at gnu++17

`[arduino_common]` unflags the platform's injected `-std=gnu++11` (`build_unflags = -std=gnu++11`,
inherited by the device envs via `extends`), and each device env adds `-std=gnu++17` to its own
`build_flags`. The per-env placement is deliberate: under `extends`, a child that redefines
`build_flags` *overrides* the parent's list rather than appending, so the flag must live in each env.
Device and host now agree on the language standard, so a C++14/17-ism can't compile on one and break
the other.

### PR CI compiles the device

`test.yml` gains a `device_build` job that runs `pio run -e ulanzi` and `pio run -e awtrix2_upgrade`
on `pull_request`, reusing the existing PlatformIO setup with its own cache key. A device-only break
now fails the PR instead of surfacing only after merge to `main`.

The three measures are complementary: the constructor makes the construct robust, the shared standard
prevents the *class* of standard-divergence drift, and the PR device build is the backstop that makes
any remaining device-only break loud on the PR.

## Consequences

- `pio run -e ulanzi` and `pio run -e awtrix2_upgrade` compile and link; `pio test -e native` stays
  green (all `test_U*`/`test_CE*` suites). New `test_CE12` pins the `ButtonState(bool, bool)` contract
  on the host (its paren-init has no overload until the constructor exists).
- The entire device firmware (and the Arduino-ESP32 framework sources) now compile at `gnu++17`
  instead of `gnu++11`. Verified by a full `pio run -e ulanzi -v` (the compile line shows
  `-std=gnu++17` with no surviving `-std=gnu++11`) and a clean link of both device envs.
- `TimerManager.cpp:501` is unchanged — its brace-init is now a constructor call.
- PR CI is slower (it downloads the device toolchain and compiles two firmware envs), traded for
  catching device-only breaks before merge. `main.yml` (release build) is unchanged.
- No NVS/MQTT/HTTP/HA surface change; no `TimerConfigEditor` / `TimerManager` API change beyond the
  added constructor.
