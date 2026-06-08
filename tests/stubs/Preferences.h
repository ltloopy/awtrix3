#pragma once

#include <Arduino.h>
#include <map>

class Preferences {
public:
    static int begin_calls;

    bool begin(const char *name, bool readOnly = false) { (void)name; (void)readOnly; begin_calls++; return true; }
    void end() {}

    uint32_t getUInt(const char *key, uint32_t def = 0) {
        auto it = u32_.find(String(key));
        return it == u32_.end() ? def : it->second;
    }
    void putUInt(const char *key, uint32_t value) { u32_[String(key)] = value; }

    uint8_t getUChar(const char *key, uint8_t def = 0) {
        auto it = u8_.find(String(key));
        return it == u8_.end() ? def : it->second;
    }
    void putUChar(const char *key, uint8_t value) { u8_[String(key)] = value; }

    String getString(const char *key, const String &def = String()) {
        auto it = str_.find(String(key));
        return it == str_.end() ? def : it->second;
    }
    size_t putString(const char *key, const String &value) {
        str_[String(key)] = value;
        return value.length();
    }

    bool getBool(const char *key, bool def = false) {
        auto it = b_.find(String(key));
        return it == b_.end() ? def : it->second;
    }
    void putBool(const char *key, bool value) { b_[String(key)] = value; }

    void remove(const char *key) {
        String k(key);
        u32_.erase(k); u8_.erase(k); str_.erase(k); b_.erase(k);
    }

    static void __test_reset() { u32_.clear(); u8_.clear(); str_.clear(); b_.clear(); begin_calls = 0; }

private:
    static std::map<String, uint32_t> u32_;
    static std::map<String, uint8_t>  u8_;
    static std::map<String, String>   str_;
    static std::map<String, bool>     b_;
};
