#include "tls_policy.h"

#include <stdio.h>
#include <string.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/error-ssl.h>

static int g_lastVerifyErr = 0;
static int g_inited = 0;

int  bb_tls_last_verify_error(void) { return g_lastVerifyErr; }
void bb_tls_reset_verify_error(void) { g_lastVerifyErr = 0; }

void bb_tls_init(void) {
  if (!g_inited) { wolfSSL_Init(); g_inited = 1; }
}

int bb_tls_is_ip_literal(const char* s) {
  if (!s || !*s) return 0;
  if (strchr(s, ':')) return 1;                       // IPv6 (names never contain ':')
  int dots = 0, digits = 0;
  for (const char* p = s; *p; p++) {
    if (*p == '.') { if (!digits) return 0; dots++; digits = 0; }
    else if (*p >= '0' && *p <= '9') { if (++digits > 3) return 0; }
    else return 0;
  }
  return dots == 3 && digits > 0;
}

// wolfSSL renders iPAddress SANs as dotted IPv4 or fully expanded, uppercase IPv6
// ("0000:0000:...:0001") and string-compares. Expand an IPv6 literal to match.
// Returns 0 on success.
static int expand_ipv6(const char* in, char* out, size_t outSz) {
  unsigned short g[8] = {0};
  int n = 0, gap = -1;
  const char* p = in;
  if (p[0] == ':' && p[1] == ':') { gap = 0; p += 2; }
  while (*p) {
    if (n >= 8) return -1;
    unsigned v = 0; int digits = 0;
    while (*p && *p != ':') {
      char c = *p++; int d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else return -1;                               // no embedded IPv4 / zone ids
      if (++digits > 4) return -1;
      v = (v << 4) | (unsigned)d;
    }
    if (!digits) return -1;
    g[n++] = (unsigned short)v;
    if (*p == ':') {
      p++;
      if (*p == ':') { if (gap >= 0) return -1; gap = n; p++; }
      else if (!*p) return -1;
    }
  }
  if (gap >= 0) {
    int move = n - gap;
    for (int i = 0; i < move; i++) g[7 - i] = g[n - 1 - i];
    for (int i = gap; i < 8 - move; i++) g[i] = 0;
  } else if (n != 8) return -1;
  if (outSz < 40) return -1;
  for (int i = 0; i < 8; i++) sprintf(out + i * 5, i < 7 ? "%04X:" : "%04X", g[i]);
  return 0;
}

static int is_date_error(int e) {
  return e == ASN_BEFORE_DATE_E || e == ASN_AFTER_DATE_E ||
         e == WOLFSSL_X509_V_ERR_CERT_NOT_YET_VALID || e == WOLFSSL_X509_V_ERR_CERT_HAS_EXPIRED;
}

static int verify_cb(int preverify, WOLFSSL_X509_STORE_CTX* store) {
  if (preverify) return 1;
  int e = wolfSSL_X509_STORE_CTX_get_error(store);
  if (is_date_error(e) && !bb_tls_clock_valid()) return 1;   // no trustworthy clock yet
  g_lastVerifyErr = e;
  return 0;
}

void bb_tls_strerror(int code, char* buf, size_t sz) {
  if (!buf || !sz) return;
  if (code == 0) { buf[0] = 0; return; }
  char tmp[WOLFSSL_MAX_ERROR_SZ];
  wolfSSL_ERR_error_string_n((unsigned long)code, tmp, sizeof(tmp));
  // Make the two failures users will actually hit readable.
  const char* hint = "";
  switch (code) {
    case DOMAIN_NAME_MISMATCH: hint = " (certificate is not for this host name)"; break;
    case IPADDR_MISMATCH:      hint = " (certificate does not list this IP address)"; break;
    case ASN_NO_SIGNER_E:      hint = " (not signed by the trusted CA)"; break;
    case ASN_NAME_INVALID_E:   hint = " (outside the CA's name constraints)"; break;
    default: break;
  }
  snprintf(buf, sz, "%d %s%s", code, tmp, hint);
}

WOLFSSL_CTX* bb_tls_ctx_new(const char* caPem, char* err, size_t errSz) {
  bb_tls_init();
  if (err && errSz) err[0] = 0;
  WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());   // TLS 1.2 or 1.3
  if (!ctx) { if (err) snprintf(err, errSz, "out of memory (ctx)"); return NULL; }
  wolfSSL_CTX_SetMinVersion(ctx, WOLFSSL_TLSV1_2);

  if (caPem && *caPem) {
    int r = wolfSSL_CTX_load_verify_buffer(ctx, (const unsigned char*)caPem,
                                           (long)strlen(caPem), WOLFSSL_FILETYPE_PEM);
    if (r != WOLFSSL_SUCCESS) {
      if (err) { char m[120]; bb_tls_strerror(r, m, sizeof(m)); snprintf(err, errSz, "CA certificate rejected: %s", m); }
      wolfSSL_CTX_free(ctx);
      return NULL;
    }
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, verify_cb);
  } else {
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, NULL);
  }
  return ctx;
}

WOLFSSL* bb_tls_ssl_new(WOLFSSL_CTX* ctx, const char* host, int verify) {
  WOLFSSL* ssl = wolfSSL_new(ctx);
  if (!ssl || !host || !*host) return ssl;
  int ip = bb_tls_is_ip_literal(host);
  if (!ip) wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, host, (unsigned short)strlen(host));
  if (verify) {
    if (ip) {
      char v6[48];
      const char* want = host;
      if (strchr(host, ':') && expand_ipv6(host, v6, sizeof(v6)) == 0) want = v6;
      wolfSSL_X509_VERIFY_PARAM_set1_ip_asc(wolfSSL_get0_param(ssl), want);
    }
    else    wolfSSL_check_domain_name(ssl, host);
  }
  return ssl;
}

int bb_tls_capture_anchor(WOLFSSL* ssl, bb_tls_anchor* out, char* err, size_t errSz) {
  memset(out, 0, sizeof(*out));
  if (err && errSz) err[0] = 0;
  WOLFSSL_X509_CHAIN* chain = wolfSSL_get_peer_chain(ssl);
  int n = chain ? wolfSSL_get_chain_count(chain) : 0;
  if (n <= 0) { if (err) snprintf(err, errSz, "server presented no certificates"); return -1; }
  out->chainLen = n;
  int idx = n - 1;
  unsigned char* der = wolfSSL_get_chain_cert(chain, idx);
  int derLen = wolfSSL_get_chain_length(chain, idx);
  if (!der || derLen <= 0) { if (err) snprintf(err, errSz, "could not read certificate"); return -1; }

  int pemLen = wc_DerToPem(der, (word32)derLen, (byte*)out->pem, (word32)sizeof(out->pem) - 1, CERT_TYPE);
  if (pemLen <= 0) { if (err) snprintf(err, errSz, "certificate too large to store (%d bytes DER)", derLen); return -1; }
  out->pem[pemLen] = 0;

  byte h[WC_SHA256_DIGEST_SIZE];
  if (wc_Sha256Hash(der, (word32)derLen, h) != 0) { if (err) snprintf(err, errSz, "hash failed"); return -1; }
  char* p = out->sha256;
  for (int i = 0; i < WC_SHA256_DIGEST_SIZE; i++) p += sprintf(p, i ? ":%02X" : "%02X", h[i]);

  WOLFSSL_X509* x = wolfSSL_X509_d2i(NULL, der, derLen);
  if (x) {
    wolfSSL_X509_NAME_oneline(wolfSSL_X509_get_subject_name(x), out->subject, sizeof(out->subject));
    wolfSSL_X509_NAME_oneline(wolfSSL_X509_get_issuer_name(x), out->issuer, sizeof(out->issuer));
    out->isCA = wolfSSL_X509_get_isCA(x);
    out->selfSigned = strcmp(out->subject, out->issuer) == 0;
    wolfSSL_X509_free(x);
  }
  return 0;
}
