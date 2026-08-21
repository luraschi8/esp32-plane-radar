#pragma once
#include <freertos/FreeRTOS.h>
struct MockTask { int dummy = 0; };
using TaskHandle_t = MockTask*;
extern int g_task_create_fail;
inline BaseType_t xTaskCreate(void (*)(void*), const char*, uint32_t, void*, int, TaskHandle_t* h) {
  if (g_task_create_fail) { --g_task_create_fail; return pdFALSE; }
  if (h) *h = new MockTask(); return pdPASS;
}
inline uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 3600; }
inline void vTaskDelay(TickType_t) {}
