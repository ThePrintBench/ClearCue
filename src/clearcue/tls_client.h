// HTTPS transport for the printer-server API, built on wolfSSL instead of the
// core's mbedTLS-based NetworkClientSecure (mbedTLS can't parse certificates with
// critical Name Constraints, which Watchtower's local CA has).
//
// Drop-in for HTTPClient::begin(NetworkClient&, url): it subclasses NetworkClient
// for the type, and carries TCP on a separate NetworkClient member so the base
// class's socket state is never touched.
#pragma once
#include <Arduino.h>
#include <NetworkClient.h>
#include "tls_anchor.h"

// Opaque here on purpose; see tls_anchor.h.
struct WOLFSSL;
struct WOLFSSL_CTX;

class TlsClient : public NetworkClient {
 public:
  TlsClient() = default;
  ~TlsClient() { stop(); }

  // nullptr / "" = encrypt without verifying (like setInsecure()). Otherwise the PEM
  // is the only trust anchor and the leaf must match the host (name or IP literal).
  // The pointer must stay valid while connections are made.
  void setCACert(const char* pem) { _ca = pem; }

  // Keep the presented chain after connect() for trust-on-first-use.
  bool captureAnchor(bb_tls_anchor* out, String& err);

  int lastError() const { return _lastErr; }
  String lastErrorString() const;

  // NetworkClient / Client interface used by HTTPClient
  int connect(IPAddress ip, uint16_t port) override { return connect(ip, port, _connectTimeout); }
  int connect(IPAddress ip, uint16_t port, int32_t timeout) override;
  int connect(const char* host, uint16_t port) override { return connect(host, port, _connectTimeout); }
  int connect(const char* host, uint16_t port, int32_t timeout) override;
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* buf, size_t size) override;
  int available() override;
  int read() override;
  int read(uint8_t* buf, size_t size) override;
  int peek() override;
  void flush() override {}
  void stop() override;
  uint8_t connected() override;
  operator bool() { return connected(); }
  void setConnectionTimeout(uint32_t ms) override { _connectTimeout = (int32_t)ms; }

 private:
  NetworkClient _tcp;
  WOLFSSL_CTX*  _ctx = nullptr;
  WOLFSSL*      _ssl = nullptr;
  const char*   _ca = nullptr;
  int32_t       _connectTimeout = 8000;
  int           _lastErr = 0;
  bool          _peerClosed = false;

  int handshake(const char* host, int32_t timeout);
  static int ioRecv(WOLFSSL*, char* buf, int sz, void* ctx);
  static int ioSend(WOLFSSL*, char* buf, int sz, void* ctx);
};
