#pragma once

#include <Arduino.h>

class File {
public:
    explicit operator bool() const { return false; }
    int available() { return 0; }
    int read() { return -1; }
    size_t size() { return 0; }
    void close() {}
};

class LittleFS_Stub {
public:
    bool exists(const char *) { return false; }
    File open(const char *, const char *) { return File(); }
};

extern LittleFS_Stub LittleFS;
