// TLS policy on top of wolfSSL: context setup, peer verification, and
// trust-on-first-use capture. No Arduino dependencies, so the same code is
// exercised by the host test in test/tls_host/.
#pragma once
#include <stddef.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>
#include "tls_anchor.h"

#ifdef __cplusplus
extern "C" {
#endif

// Provided by the platform: nonzero once the wall clock is trustworthy (NTP synced).
// While it returns 0, certificate not-before/not-after failures are ignored; every
// other check (signature, chain, Name Constraints, host/IP) still applies.
int bb_tls_clock_valid(void);

// One-time library init. Safe to call repeatedly.
void bb_tls_init(void);

// Build a client context. caPem == NULL or "" -> no peer verification (encrypt only).
// Otherwise the PEM (one or more certs) becomes the only trust anchor set.
// Returns NULL on failure with a message in err.
WOLFSSL_CTX* bb_tls_ctx_new(const char* caPem, char* err, size_t errSz);

// Create a session for host. When the context verifies, the leaf must match host:
// IPv4/IPv6 literals are checked against iPAddress SANs, names against dNSName SANs.
// SNI is sent for names only (never for IP literals).
WOLFSSL* bb_tls_ssl_new(WOLFSSL_CTX* ctx, const char* host, int verify);

// Human-readable description of a wolfSSL error code.
void bb_tls_strerror(int code, char* buf, size_t sz);

// Last error the verify callback rejected (0 if none since the last reset).
int  bb_tls_last_verify_error(void);
void bb_tls_reset_verify_error(void);

// After a completed handshake: take the top-most certificate the server presented
// (for Watchtower, its local CA) and return it as PEM plus its SHA-256 fingerprint
// ("AB:CD:..."), subject and issuer. Returns 0 on success.

int bb_tls_capture_anchor(WOLFSSL* ssl, bb_tls_anchor* out, char* err, size_t errSz);

// Nonzero if s is an IPv4 or IPv6 literal.
int bb_tls_is_ip_literal(const char* s);

#ifdef __cplusplus
}
#endif
