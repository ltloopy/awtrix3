// Shim for the native_ha host suite. The vendored ArduinoHA library includes
// <IPAddress.h> directly (HAMqtt.h), but ArduinoFake only ships it nested under
// arduino/. Forward to that so the bare include resolves on the host. ArduinoHA
// never constructs an IPAddress under -DARDUINOHA_TEST; the type just needs to
// exist for HAMqtt.h to compile.
#pragma once
#include <arduino/IPAddress.h>
