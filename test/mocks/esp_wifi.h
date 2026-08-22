#pragma once
#include <WiFi.h>
#define ESP_OK 0
enum { WIFI_IF_STA = 0 };
struct wifi_sta_config_t { char ssid[33]; char password[65]; };
struct wifi_config_t { wifi_sta_config_t sta; };
struct MockEspWifi { bool has_creds = false; wifi_mode_t mode = WIFI_MODE_STA; };
extern MockEspWifi g_espwifi;
inline int esp_wifi_get_mode(wifi_mode_t* m) { *m = g_espwifi.mode; return ESP_OK; }
inline int esp_wifi_get_config(int, wifi_config_t* c) {
  memset(c, 0, sizeof(*c));
  if (g_espwifi.has_creds) strcpy(c->sta.ssid, "TestNet");
  return ESP_OK;
}
