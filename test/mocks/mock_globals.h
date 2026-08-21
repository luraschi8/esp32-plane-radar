#include <Arduino.h>
#include <Preferences.h>
unsigned long g_mock_millis = 0;
MockSerial Serial;
MockNvs g_nvs;
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
MockEsp ESP;
MockWiFi WiFi;
MockTlsStats g_tls;
MockHttp g_http;
int g_mutex_alloc_fail = 0;
int g_task_create_fail = 0;
