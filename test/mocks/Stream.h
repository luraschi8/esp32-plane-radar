#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
// ArduinoJson selects its Stream reader via is_base_of<Stream, T>, so the
// mock client must genuinely derive from a type named Stream.
class Stream {
 public:
  virtual ~Stream() = default;
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;
  virtual size_t readBytes(char* buf, size_t len) {
    size_t n = 0;
    while (n < len) { int c = read(); if (c < 0) break; buf[n++] = (char)c; }
    return n;
  }
  virtual void flush() {}
};
