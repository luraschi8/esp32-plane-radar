#pragma once
#include <Arduino.h>
#include <WiFi.h>
#define WFM_LABEL_AFTER 0
class WiFiManagerParameter {
 public:
  WiFiManagerParameter(const char* id, const char* label, const char* v, int len,
                       const char* custom = "", int = 0)
      : id_(id), value_(v ? v : "") { (void)label; (void)len; (void)custom; }
  const char* getValue() const { return value_.c_str(); }
  void setValue(const char* v, int) { value_ = v ? v : ""; }
  std::string id_, value_;
};
/** Records what the portal was asked to do; returns benign defaults. */
struct MockWmStats { int reset = 0, erase = 0, start_portal = 0, start_web = 0, stop_web = 0, process = 0; };
extern MockWmStats g_wm;
class WiFiManager {
 public:
  void setConfigPortalTimeout(unsigned long) {}
  void setAPStaticIPConfig(IPAddress, IPAddress, IPAddress) {}
  void setHostname(const char*) {}
  void setAPCallback(void (*)(WiFiManager*)) {}
  void setSaveParamsCallback(void (*)()) {}
  void addParameter(WiFiManagerParameter*) {}
  void setConfigPortalBlocking(bool) {}
  void resetSettings() { ++g_wm.reset; }
  void erase() { ++g_wm.erase; }
  bool startConfigPortal(const char*) { ++g_wm.start_portal; return true; }
  void startWebPortal() { ++g_wm.start_web; web_ = true; }
  void stopWebPortal() { ++g_wm.stop_web; web_ = false; }
  bool getWebPortalActive() const { return web_; }
  bool getConfigPortalActive() const { return false; }
  bool process() { ++g_wm.process; return false; }
  String getWiFiSSID() { return WiFi.ssid; }
  String getWiFiPass() { return String("pw"); }
  bool web_ = false;
};
