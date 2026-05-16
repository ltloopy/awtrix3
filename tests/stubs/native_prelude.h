// Force-included via `-include tests/stubs/native_prelude.h` in the
// [env:native] PlatformIO env. Runs before any source line in every TU.
//
// Each stub header below defines the same include guard as its production
// counterpart in src/. When src/TimerManager.cpp later does
// `#include "Globals.h"` (which resolves to src/Globals.h because of
// current-file-directory precedence for quoted includes), the production
// header sees its guard already defined and skips its body entirely. The
// production headers can't compile on a native host because they drag in
// FastLED, EasyButton, ArduinoHA, MatrixDisplayUi, etc.
//
// The stubs below provide the *minimal* surface area TimerManager.cpp
// actually uses. Anything else stays untested by design — see plan file.

#ifndef AWTRIX_NATIVE_PRELUDE_H
#define AWTRIX_NATIVE_PRELUDE_H

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

#endif
