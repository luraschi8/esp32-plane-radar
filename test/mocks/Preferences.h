// In-memory NVS stand-in. Records opens/writes so tests can assert on
// persistence behaviour, not just on values.
#pragma once
#include <map>
#include <string>
#include <cstdint>

struct MockNvs {
  std::map<std::string, std::string> store;   // "ns/key" -> raw bytes
  int open_fail_count = 0;                    // force begin() failures
  void reset() { store.clear(); open_fail_count = 0; }
};
extern MockNvs g_nvs;

class Preferences {
 public:
  bool begin(const char* ns, bool /*read_only*/ = false) {
    if (g_nvs.open_fail_count > 0) { --g_nvs.open_fail_count; return false; }
    ns_ = ns; open_ = true; return true;
  }
  void end() { open_ = false; }
  bool isKey(const char* k) { return g_nvs.store.count(key(k)) != 0; }
  void remove(const char* k) { g_nvs.store.erase(key(k)); }

  void putUChar(const char* k, uint8_t v) { put(k, &v, sizeof(v)); }
  uint8_t getUChar(const char* k, uint8_t d = 0) { return get<uint8_t>(k, d); }
  void putBool(const char* k, bool v) { put(k, &v, sizeof(v)); }
  bool getBool(const char* k, bool d = false) { return get<bool>(k, d); }
  void putDouble(const char* k, double v) { put(k, &v, sizeof(v)); }
  double getDouble(const char* k, double d = 0) { return get<double>(k, d); }

 private:
  std::string key(const char* k) const { return ns_ + "/" + k; }
  void put(const char* k, const void* p, size_t n) {
    g_nvs.store[key(k)] = std::string(static_cast<const char*>(p), n);
  }
  template <typename T> T get(const char* k, T d) {
    auto it = g_nvs.store.find(key(k));
    if (it == g_nvs.store.end() || it->second.size() != sizeof(T)) return d;
    T v; memcpy(&v, it->second.data(), sizeof(T)); return v;
  }
  std::string ns_;
  bool open_ = false;
};
