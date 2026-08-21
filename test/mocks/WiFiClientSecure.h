#pragma once
#include <Arduino.h>
#include <Stream.h>

/** Serves a scripted body and records how it was torn down. */
class WiFiClient : public Stream {
 public:
  std::string body;
  size_t pos = 0;
  int available() override { return (int)(body.size() - pos); }
  int read() override { return pos < body.size() ? (unsigned char)body[pos++] : -1; }
  int peek() override { return pos < body.size() ? (unsigned char)body[pos] : -1; }
};

struct MockTlsStats { int setInsecure = 0; int stop = 0; };
extern MockTlsStats g_tls;

class WiFiClientSecure : public WiFiClient {
 public:
  void setInsecure() { ++g_tls.setInsecure; }
  void stop() { ++g_tls.stop; }
};
