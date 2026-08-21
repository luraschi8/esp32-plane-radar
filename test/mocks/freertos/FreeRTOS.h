#pragma once
#include <cstdint>
using TickType_t = uint32_t;
using StackType_t = uint8_t;
using BaseType_t = int;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xFFFFFFFF
#define pdMS_TO_TICKS(x) (x)
