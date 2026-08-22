#pragma once
struct MockRestart { int count = 0; };
extern MockRestart g_restart;
inline void esp_restart() { ++g_restart.count; }
