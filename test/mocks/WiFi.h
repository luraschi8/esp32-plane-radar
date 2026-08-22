#pragma once
#include <Arduino.h>
enum { WL_CONNECTED = 3, WL_DISCONNECTED = 6 };
enum wifi_power_t { WIFI_POWER_8_5dBm = 34 };
enum { WIFI_PS_NONE = 0 };
enum wifi_mode_t { WIFI_MODE_NULL = 0, WIFI_STA = 1, WIFI_OFF = 0, WIFI_MODE_STA = 1 };

struct IPAddress {
  uint8_t a=0,b=0,c=0,d=0;
  IPAddress() {}
  IPAddress(uint8_t w,uint8_t x,uint8_t y,uint8_t z):a(w),b(x),c(y),d(z) {}
  bool operator!=(const IPAddress& o) const { return a!=o.a||b!=o.b||c!=o.c||d!=o.d; }
  bool operator==(const IPAddress& o) const { return !(*this != o); }
  String toString() const { char s[20]; snprintf(s,sizeof(s),"%u.%u.%u.%u",a,b,c,d); return String(s); }
};

struct MockWiFi {
  int status_ = WL_CONNECTED;
  IPAddress ip{192,168,1,96};
  String ssid{"TestNet"};
  int mode_calls = 0, begin_calls = 0, disconnect_calls = 0, txpower_calls = 0;
  int status() const { return status_; }
  void mode(int) { mockEvent("wifi_mode"); ++mode_calls; }
  void setTxPower(wifi_power_t) { ++txpower_calls; }
  void setSleep(int) {}
  void setAutoReconnect(bool) {}
  void persistent(bool) {}
  /**
   * When set, a begin() brings the link up. Without this the mock link can only
   * be moved by the test writing status_ directly, which meant no test ever
   * caused a reconnect -- wifiReconnect() could be replaced with `return false`
   * and the whole suite stayed green.
   */
  bool link_up_on_begin = false;
  void begin() { ++begin_calls; if (link_up_on_begin) status_ = WL_CONNECTED; }
  void begin(const char*, const char*) {
    ++begin_calls;
    if (link_up_on_begin) status_ = WL_CONNECTED;
  }
  void disconnect(bool = false, bool = false) { ++disconnect_calls; }
  IPAddress localIP() const { return ip; }
  String SSID() const { return ssid; }
  void reset() { status_ = WL_CONNECTED; ip = IPAddress(192,168,1,96); mode_calls = 0;
                 begin_calls = 0; disconnect_calls = 0; txpower_calls = 0;
                 link_up_on_begin = false; }
};
extern MockWiFi WiFi;
