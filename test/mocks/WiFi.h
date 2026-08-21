#pragma once
#include <Arduino.h>
enum { WL_CONNECTED = 3, WL_DISCONNECTED = 6 };
struct MockWiFi { int status_ = WL_CONNECTED; int status() const { return status_; } };
extern MockWiFi WiFi;
