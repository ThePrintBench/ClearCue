#include "tls_policy.h"   // wolfSSL first: settings.h must precede other wolfSSL headers
#include "tls_client.h"

#include <wolfssl/error-ssl.h>

// ---- I/O: non-blocking callbacks onto the TCP NetworkClient ----
int TlsClient::ioRecv(WOLFSSL*, char* buf, int sz, void* ctx) {
  NetworkClient* tcp = static_cast<NetworkClient*>(ctx);
  int avail = tcp->available();
  if (avail <= 0) {
    if (!tcp->connected()) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    return WOLFSSL_CBIO_ERR_WANT_READ;
  }
  int r = tcp->read(reinterpret_cast<uint8_t*>(buf), (size_t)min(avail, sz));
  if (r > 0) return r;
  return tcp->connected() ? WOLFSSL_CBIO_ERR_WANT_READ : WOLFSSL_CBIO_ERR_CONN_CLOSE;
}

int TlsClient::ioSend(WOLFSSL*, char* buf, int sz, void* ctx) {
  NetworkClient* tcp = static_cast<NetworkClient*>(ctx);
  size_t w = tcp->write(reinterpret_cast<const uint8_t*>(buf), (size_t)sz);
  if (w > 0) return (int)w;
  return tcp->connected() ? WOLFSSL_CBIO_ERR_WANT_WRITE : WOLFSSL_CBIO_ERR_CONN_CLOSE;
}

String TlsClient::lastErrorString() const {
  char b[160];
  bb_tls_strerror(_lastErr, b, sizeof(b));
  return String(b);
}

int TlsClient::handshake(const char* host, int32_t timeout) {
  char err[160];
  _ctx = bb_tls_ctx_new(_ca, err, sizeof(err));
  if (!_ctx) { _lastErr = WOLFSSL_FATAL_ERROR; log_e("%s", err); return 0; }
  wolfSSL_CTX_SetIORecv(_ctx, ioRecv);
  wolfSSL_CTX_SetIOSend(_ctx, ioSend);

  bool verify = _ca && *_ca;
  _ssl = bb_tls_ssl_new(_ctx, host, verify);
  if (!_ssl) { _lastErr = MEMORY_E; return 0; }
  wolfSSL_SetIOReadCtx(_ssl, &_tcp);
  wolfSSL_SetIOWriteCtx(_ssl, &_tcp);

  bb_tls_reset_verify_error();
  uint32_t t0 = millis();
  for (;;) {
    int r = wolfSSL_connect(_ssl);
    if (r == WOLFSSL_SUCCESS) { _lastErr = 0; return 1; }
    int e = wolfSSL_get_error(_ssl, r);
    if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) {
      int v = bb_tls_last_verify_error();
      _lastErr = v ? v : e;               // prefer the specific certificate reason
      return 0;
    }
    if ((int32_t)(millis() - t0) > timeout) { _lastErr = SOCKET_ERROR_E; return 0; }
    delay(2);
  }
}

int TlsClient::connect(IPAddress ip, uint16_t port, int32_t timeout) {
  return connect(ip.toString().c_str(), port, timeout);
}

int TlsClient::connect(const char* host, uint16_t port, int32_t timeout) {
  stop();
  _peerClosed = false;
  _lastErr = 0;
  if (!_tcp.connect(host, port, timeout)) { _lastErr = SOCKET_ERROR_E; return 0; }
  if (!handshake(host, timeout)) {
    log_w("TLS handshake with %s failed: %s", host, lastErrorString().c_str());
    // Keep the error; drop the session.
    int keep = _lastErr; stop(); _lastErr = keep;
    return 0;
  }
  return 1;
}

size_t TlsClient::write(const uint8_t* buf, size_t size) {
  if (!_ssl) return 0;
  size_t sent = 0;
  uint32_t t0 = millis();
  while (sent < size) {
    int r = wolfSSL_write(_ssl, buf + sent, (int)(size - sent));
    if (r > 0) { sent += (size_t)r; continue; }
    int e = wolfSSL_get_error(_ssl, r);
    if ((e == WOLFSSL_ERROR_WANT_WRITE || e == WOLFSSL_ERROR_WANT_READ) &&
        (int32_t)(millis() - t0) < _connectTimeout) { delay(2); continue; }
    _lastErr = e;
    break;
  }
  return sent;
}

int TlsClient::available() {
  if (!_ssl) return 0;
  int p = wolfSSL_pending(_ssl);
  if (p > 0) return p;
  if (_peerClosed) return 0;
  // Pull at most one record without blocking: the recv callback returns WANT_READ
  // when nothing is buffered on the socket.
  char b;
  int r = wolfSSL_peek(_ssl, &b, 1);
  if (r > 0) return wolfSSL_pending(_ssl);
  int e = wolfSSL_get_error(_ssl, r);
  if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) _peerClosed = true;
  return 0;
}

int TlsClient::read(uint8_t* buf, size_t size) {
  if (!_ssl || !size) return -1;
  int r = wolfSSL_read(_ssl, buf, (int)size);
  if (r > 0) return r;
  int e = wolfSSL_get_error(_ssl, r);
  if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) _peerClosed = true;
  return -1;
}

int TlsClient::read() {
  uint8_t b;
  return read(&b, 1) == 1 ? b : -1;
}

int TlsClient::peek() {
  if (!_ssl) return -1;
  uint8_t b;
  return wolfSSL_peek(_ssl, &b, 1) == 1 ? b : -1;
}

uint8_t TlsClient::connected() {
  if (!_ssl) return 0;
  if (wolfSSL_pending(_ssl) > 0) return 1;
  if (_peerClosed) return 0;
  return _tcp.connected();
}

void TlsClient::stop() {
  if (_ssl) {
    if (_tcp.connected() && !_peerClosed) wolfSSL_shutdown(_ssl);   // best-effort close_notify
    wolfSSL_free(_ssl);
    _ssl = nullptr;
  }
  if (_ctx) { wolfSSL_CTX_free(_ctx); _ctx = nullptr; }
  _tcp.stop();
}

bool TlsClient::captureAnchor(bb_tls_anchor* out, String& err) {
  if (!_ssl) { err = "not connected"; return false; }
  char e[160];
  if (bb_tls_capture_anchor(_ssl, out, e, sizeof(e)) != 0) { err = e; return false; }
  return true;
}
