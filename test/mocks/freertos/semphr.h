#pragma once
#include <freertos/FreeRTOS.h>
// Single-threaded host: a mutex that counts use so tests can assert on locking.
struct MockMutex { int taken = 0; int given = 0; bool exists = true; };
using SemaphoreHandle_t = MockMutex*;
extern int g_mutex_alloc_fail;
/** Force the next N takes to time out, so a caller's skip path is reachable. */
extern int g_mutex_take_fails;
inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  if (g_mutex_alloc_fail) { --g_mutex_alloc_fail; return nullptr; }
  return new MockMutex();
}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t) {
  if (g_mutex_take_fails > 0) { --g_mutex_take_fails; return pdFALSE; }
  if (m) ++m->taken;
  return pdTRUE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { if (m) ++m->given; return pdTRUE; }
inline void vSemaphoreDelete(SemaphoreHandle_t m) { delete m; }
