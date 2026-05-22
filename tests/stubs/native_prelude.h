// Force-included via `-include tests/stubs/native_prelude.h` in the
// [env:native] PlatformIO env. Runs before any source line in every TU,
// including Unity's pure-C files (e.g. unity_config.c) — so everything
// below is gated on __cplusplus to avoid dragging C++ headers into a C TU.
//
// For C++ TUs, each stub header defines the same include guard as its
// production counterpart in src/. When src/TimerManager.cpp later does
// `#include "Globals.h"` (which resolves to src/Globals.h because of
// current-file-directory precedence for quoted includes), the production
// header sees its guard already defined and skips its body entirely.
// The production headers can't compile on a native host because they
// drag in FastLED, EasyButton, ArduinoHA, MatrixDisplayUi, etc.

#ifndef AWTRIX_NATIVE_PRELUDE_H
#define AWTRIX_NATIVE_PRELUDE_H

#ifdef __cplusplus

// TimerManager.cpp uses std::remove_if without an explicit #include.
// The Arduino toolchain pulls <algorithm> in transitively; the native
// toolchain might not. Include it here defensively.
#include <algorithm>

#include "Globals.h"
#include "LittleFS.h"
#include "Overlays.h"
#include "PeripheryManager.h"
#include "DisplayManager.h"
#include "MQTTManager.h"
#include "MenuManager.h"

#endif  // __cplusplus

#endif
