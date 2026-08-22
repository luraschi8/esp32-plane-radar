// Host-side stand-ins for the Arduino APIs the firmware touches. Only exists
// for the native test environment; never compiled into the device image.
#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <cmath>
#include <Stream.h>
#include <vector>

/** Ordered log of notable mock events, for asserting sequence not just counts. */
extern std::vector<std::string> g_events;
inline void mockEvent(const char* what) { g_events.push_back(what); }
inline int mockEventIndex(const char* what) {
  for (size_t i = 0; i < g_events.size(); ++i) if (g_events[i] == what) return (int)i;
  return -1;
}

/**
 * 32-bit like the device (Arduino's millis() is uint32_t there, though
 * `unsigned long` is 64-bit on this host). Code that stores it in uint32_t
 * therefore wraps faithfully and its rollover behaviour is testable.
 */
extern uint32_t g_mock_millis;
inline uint32_t millis() { return g_mock_millis; }
inline uint32_t micros() { return g_mock_millis * 1000U; }
inline void delay(uint32_t ms) { g_mock_millis += ms; }
/** Tests drive time explicitly; nothing here advances it on its own. */
inline void mockAdvanceMs(uint32_t ms) { g_mock_millis += ms; }
inline void mockSetMs(uint32_t ms) { g_mock_millis = ms; }

struct MockSerial {
  bool capture = false;
  std::string log;
  template <typename... A> void printf(const char* f, A... a) {
    char b[512]; snprintf(b, sizeof(b), f, a...); if (capture) log += b;
  }
  void println(const char* s = "") { if (capture) { log += s; log += "\n"; } }
  void print(const char* s) { if (capture) log += s; }
  void begin(unsigned long) {}
};
extern MockSerial Serial;

// FreeRTOS critical sections are no-ops on a single-threaded host test.
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
inline void portENTER_CRITICAL(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL(portMUX_TYPE*) {}

// --- Arduino String, enough for the URL building in adsb_client ---
class String {
 public:
  String() {}
  String(const char* s) : s_(s ? s : "") {}
  String(double v, int dp) { char b[48]; snprintf(b, sizeof(b), "%.*f", dp, v); s_ = b; }
  String(int v) { s_ = std::to_string(v); }
  String& operator+=(const char* o) { s_ += o; return *this; }
  String& operator+=(const String& o) { s_ += o.s_; return *this; }
  const char* c_str() const { return s_.c_str(); }
  size_t length() const { return s_.size(); }
  bool reserve(size_t n) { s_.reserve(n); return true; }
  bool concat(const char* p, unsigned n) { s_.append(p, n); return true; }
  std::string s_;
};
inline String operator+(const String& a, const String& b) { String r = a; r += b; return r; }

// --- heap accounting the firmware logs ---
struct MockEsp {
  unsigned free_heap = 71000, max_alloc = 34000;
  unsigned getFreeHeap() const { return free_heap; }
  unsigned getMaxAllocHeap() const { return max_alloc; }
  unsigned getMinFreeHeap() const { return 12340; }
};
extern MockEsp ESP;

// --- GPIO / interrupts, driven explicitly by tests ---
#define INPUT_PULLUP 2
#define LOW 0
#define HIGH 1
#define CHANGE 1
#define IRAM_ATTR
struct MockGpio {
  int level = HIGH;                 // BOOT is active LOW, idle high
  int isr_attached = 0;
  void (*isr)() = nullptr;
  /** Release the button but KEEP the handler: the firmware attaches it once. */
  void release() { level = HIGH; }
  void reset() { level = HIGH; isr_attached = 0; isr = nullptr; }
};
extern MockGpio g_gpio;
inline void pinMode(int, int) {}
inline int digitalRead(int) { return g_gpio.level; }
inline int digitalPinToInterrupt(int p) { return p; }
inline void attachInterrupt(int, void (*fn)(), int) { ++g_gpio.isr_attached; g_gpio.isr = fn; }
#define portENTER_CRITICAL_ISR(m) portENTER_CRITICAL(m)
#define portEXIT_CRITICAL_ISR(m) portEXIT_CRITICAL(m)

/** Move the button and fire the edge interrupt, as the hardware would. */
inline void mockBootButton(bool pressed) {
  g_gpio.level = pressed ? LOW : HIGH;
  if (g_gpio.isr) g_gpio.isr();
}
