#pragma once
/* In-memory stand-in for the NVS-backed Settings wrapper. */
#include <map>
#include <string>
class Settings {
public:
    Settings(const std::string& ns, bool read_write = false) : ns_(ns) { (void)read_write; }
    int32_t GetInt(const std::string& key, int32_t def = 0) {
        auto it = store().find(ns_ + "/" + key);
        return it == store().end() ? def : it->second;
    }
    std::string GetString(const std::string& key, const std::string& def = "") { (void)key; return def; }
    void SetString(const std::string& key, const std::string& v) { (void)key; (void)v; }
    bool GetBool(const std::string& key, bool def = false) { return GetInt(key, def) != 0; }
    void SetBool(const std::string& key, bool v) { SetInt(key, v); }
    void SetInt(const std::string& key, int32_t v) { store()[ns_ + "/" + key] = v; }
private:
    static std::map<std::string, int32_t>& store() { static std::map<std::string, int32_t> s; return s; }
    std::string ns_;
};
