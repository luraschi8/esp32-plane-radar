#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>

enum {
  HTTP_CODE_OK = 200,
  HTTPC_ERROR_CONNECTION_REFUSED = -1,
  HTTPC_ERROR_NOT_CONNECTED = -4,
  HTTPC_ERROR_READ_TIMEOUT = -11,
};

/** Script the network for a test: what each GET returns, and what body follows. */
struct MockHttp {
  std::string body;
  int code = HTTP_CODE_OK;
  int fail_first_n_gets = 0;      // return CONNECTION_REFUSED this many times
  int content_length_override = 0;  // 0 = use body.size()
  int get_calls = 0;
  int begin_calls = 0;
  int end_calls = 0;
  std::string last_url;
  void reset() { *this = MockHttp(); }
};
extern MockHttp g_http;

class HTTPClient {
 public:
  bool begin(WiFiClient& c, const String& url) {
    ++g_http.begin_calls; g_http.last_url = url.c_str(); client_ = &c;
    static_cast<WiFiClient*>(client_)->body = g_http.body;
    static_cast<WiFiClient*>(client_)->pos = 0;
    return true;
  }
  void setTimeout(unsigned long) {}
  void setConnectTimeout(int) {}
  int GET() {
    ++g_http.get_calls;
    if (g_http.fail_first_n_gets > 0) {
      --g_http.fail_first_n_gets;
      return HTTPC_ERROR_CONNECTION_REFUSED;   // no socket was ever opened
    }
    if (auto* s = dynamic_cast<WiFiClientSecure*>(client_)) s->connectSocket();
    return g_http.code;
  }
  int getSize() const {
    return g_http.content_length_override ? g_http.content_length_override : (int)g_http.body.size();
  }
  WiFiClient* getStreamPtr() { return static_cast<WiFiClient*>(client_); }
  void end() { ++g_http.end_calls; }
 private:
  WiFiClient* client_ = nullptr;
};
