// Host-side test of src/clearcue/tls_policy.cpp with the firmware's exact
// include/user_settings.h, against `openssl s_server`. Run via run.sh.
#include "../../src/clearcue/tls_policy.h"
#include <wolfssl/error-ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>

extern "C" int bb_tls_seed(unsigned char* out, unsigned int sz) {
  return getrandom(out, sz, 0) == (ssize_t)sz ? 0 : -1;
}
extern "C" int bb_inet_pton(int af, const char* s, void* d) { return inet_pton(af, s, d); }
extern "C" const char* bb_inet_ntop(int af, const void* s, char* d, unsigned int n) { return inet_ntop(af, s, d, n); }
static int g_clock = 1;
extern "C" int bb_tls_clock_valid(void) { return g_clock; }

static int io_recv(WOLFSSL*, char* buf, int sz, void* ctx) {
  int r = (int)recv(*(int*)ctx, buf, sz, 0);
  return r > 0 ? r : (r == 0 ? WOLFSSL_CBIO_ERR_CONN_CLOSE : WOLFSSL_CBIO_ERR_GENERAL);
}
static int io_send(WOLFSSL*, char* buf, int sz, void* ctx) {
  int r = (int)send(*(int*)ctx, buf, sz, 0);
  return r > 0 ? r : WOLFSSL_CBIO_ERR_GENERAL;
}

static std::string slurp(const char* p) {
  FILE* f = fopen(p, "rb"); if (!f) { perror(p); exit(2); }
  std::string s; char b[4096]; size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
  fclose(f); return s;
}

// Returns 0 on handshake success, else the wolfSSL error.
static int handshake(int port, const char* ca, const char* host, bb_tls_anchor* anchor, std::string* ver) {
  char err[200];
  WOLFSSL_CTX* ctx = bb_tls_ctx_new(ca, err, sizeof err);
  if (!ctx) { printf("      ctx: %s\n", err); return -1; }
  wolfSSL_CTX_SetIORecv(ctx, io_recv);
  wolfSSL_CTX_SetIOSend(ctx, io_send);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (connect(fd, (sockaddr*)&a, sizeof a)) { perror("connect"); exit(2); }
  bb_tls_reset_verify_error();
  WOLFSSL* ssl = bb_tls_ssl_new(ctx, host, ca && *ca);
  wolfSSL_SetIOReadCtx(ssl, &fd);
  wolfSSL_SetIOWriteCtx(ssl, &fd);
  int rc = wolfSSL_connect(ssl), e = 0;
  if (rc != WOLFSSL_SUCCESS) e = wolfSSL_get_error(ssl, rc);
  else {
    if (ver) *ver = std::string(wolfSSL_get_version(ssl)) + " " + wolfSSL_get_cipher(ssl);
    if (anchor && bb_tls_capture_anchor(ssl, anchor, err, sizeof err)) printf("      capture: %s\n", err);
    wolfSSL_shutdown(ssl);
  }
  wolfSSL_free(ssl); wolfSSL_CTX_free(ctx); close(fd);
  return e;
}

static int failures = 0;
static void expect(const char* name, int got, bool wantOk) {
  bool ok = (got == 0) == wantOk;
  char m[160]; bb_tls_strerror(got, m, sizeof m);
  printf("%s %-62s -> %s\n", ok ? "PASS" : "FAIL", name, got == 0 ? "handshake ok" : m);
  if (!ok) failures++;
}

int main(int argc, char** argv) {
  // argv: port_good port_evil port_expired port_leafonly real_watchtower_ca.pem
  int pGood = atoi(argv[1]), pEvil = atoi(argv[2]), pExp = atoi(argv[3]), pLeafOnly = atoi(argv[4]);
  std::string ca = slurp("ca.pem"), other = slurp("other.pem"), realCa = slurp(argv[5]);
  std::string ver;

  // The actual Watchtower CA that mbedTLS rejects with -9570.
  { char err[200]; WOLFSSL_CTX* c = bb_tls_ctx_new(realCa.c_str(), err, sizeof err);
    expect("real Watchtower CA (critical Name Constraints) loads as anchor", c ? 0 : -1, true);
    if (c) wolfSSL_CTX_free(c); }

  expect("no CA pinned (encrypt only), chain with NC CA", handshake(pGood, nullptr, "192.168.20.14", nullptr, &ver), true);
  printf("      negotiated: %s\n", ver.c_str());
  expect("pinned CA, host 192.168.20.14 (in IP SANs)", handshake(pGood, ca.c_str(), "192.168.20.14", nullptr, nullptr), true);
  expect("pinned CA, host watchtower.local (DNS SAN)", handshake(pGood, ca.c_str(), "watchtower.local", nullptr, nullptr), true);
  expect("pinned CA, host ::1 (IPv6 SAN)", handshake(pGood, ca.c_str(), "::1", nullptr, nullptr), true);
  expect("pinned CA, host 0:0:0:0:0:0:0:1 (IPv6, uncompressed)", handshake(pGood, ca.c_str(), "0:0:0:0:0:0:0:1", nullptr, nullptr), true);
  expect("pinned CA, host ::2 (IPv6 not in SANs) must fail", handshake(pGood, ca.c_str(), "::2", nullptr, nullptr), false);
  expect("pinned CA, host 192.168.20.99 (not in SANs) must fail", handshake(pGood, ca.c_str(), "192.168.20.99", nullptr, nullptr), false);
  expect("pinned CA, host other.local (not in SANs) must fail", handshake(pGood, ca.c_str(), "other.local", nullptr, nullptr), false);
  expect("wrong CA pinned must fail", handshake(pGood, other.c_str(), "192.168.20.14", nullptr, nullptr), false);
  expect("leaf with 8.8.8.8 SAN violates CA name constraints, must fail", handshake(pEvil, ca.c_str(), "127.0.0.1", nullptr, nullptr), false);
  g_clock = 1;
  expect("expired leaf, clock synced, must fail", handshake(pExp, ca.c_str(), "192.168.20.14", nullptr, nullptr), false);
  g_clock = 0;
  expect("expired leaf, clock NOT synced -> dates skipped", handshake(pExp, ca.c_str(), "192.168.20.14", nullptr, nullptr), true);
  g_clock = 1;
  expect("server sends leaf only, pinned CA still verifies", handshake(pLeafOnly, ca.c_str(), "192.168.20.14", nullptr, nullptr), true);

  if (argc > 6) {
    ver.clear();
    expect("TLS 1.2-only server, pinned CA", handshake(atoi(argv[6]), ca.c_str(), "192.168.20.14", nullptr, &ver), true);
    printf("      negotiated: %s\n", ver.c_str());
  }

  bb_tls_anchor an;
  int r = handshake(pGood, nullptr, "192.168.20.14", &an, nullptr);
  expect("TOFU capture handshake", r, true);
  printf("      chainLen=%d isCA=%d selfSigned=%d\n      subject=%s\n      sha256=%s\n",
         an.chainLen, an.isCA, an.selfSigned, an.subject, an.sha256);
  FILE* f = fopen("captured.pem", "w"); fputs(an.pem, f); fclose(f);
  expect("captured anchor then verifies the server", handshake(pGood, an.pem, "192.168.20.14", nullptr, nullptr), true);

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
