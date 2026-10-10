/* wolfSSL build configuration for the product firmware's API client (tls_client.cpp).
 *
 * Picked up ahead of Arduino-wolfSSL's bundled src/user_settings.h because
 * tools/wolfssl_config.py adds this folder with -iquote, which GCC searches before
 * -I paths for #include "user_settings.h".
 *
 * Requires wolfSSL >= 5.9.4 (pinned in tools/wolfssl_config.py). 5.8.4 wrongly rejects
 * valid leaves under a DNS name constraint (verified in test/tls_host), and 5.9.2 and
 * earlier have published name-constraint bypass fixes in the 5.9.x changelog.
 *
 * Why wolfSSL at all: mbedTLS (all versions through 4.x) has no Name Constraints
 * support, and rejects any certificate carrying it as a critical extension. Watchtower's
 * local CA does exactly that (RFC 5280-correct), so mbedTLS can't even parse the chain.
 * wolfSSL parses and enforces Name Constraints.
 *
 * mbedTLS stays in the image (the Arduino core links it) and is still used for OTA.
 */
#ifndef CLEARCUE_WOLFSSL_USER_SETTINGS_H
#define CLEARCUE_WOLFSSL_USER_SETTINGS_H

#define WOLFSSL_USER_SETTINGS_ID "ClearCue product firmware"

/* settings.h turns this on for PlatformIO + ESP_PLATFORM, then errors out because
 * ARDUINO is also defined. We build as a plain Arduino library: software crypto. */
#undef  WOLFSSL_ESPIDF
#undef  WOLFSSL_ESP32
#define WOLFSSL_IGNORE_FILE_WARN
/* Skip settings.h's Arduino+ESP32 defaults: they define WOLFSSL_LWIP, which drags lwIP
 * socket headers into every wolfSSL include and collides with Arduino's INADDR_NONE.
 * We do our own I/O, and set the hardening flags below ourselves. */
#define NO_ARDUINO_DEFAULT
#define WC_NO_CACHE_RESISTANT
#define NO_WOLFSSL_DIR
#define WOLFSSL_NO_CURRDIR

/* ---- platform ---- */
#define SINGLE_THREADED            /* all TLS runs on the Arduino loop task */
#define NO_FILESYSTEM
#define NO_WRITEV
#define WOLFSSL_USER_IO            /* I/O goes through callbacks onto a NetworkClient */
#define WOLFSSL_NO_SOCK
#define NO_MAIN_DRIVER
#define WOLFSSL_SMALL_STACK
#define WOLFSSL_SMALL_STACK_CACHE
#define SIZEOF_LONG_LONG 8
/* OPENSSL_EXTRA's X509 altname helpers reference AF_INET/AF_INET6 even with
 * WOLFSSL_NO_SOCK. Same values as lwIP and Linux. */
#ifndef AF_INET
  #define AF_INET 2
#endif
#ifndef AF_INET6
  #define AF_INET6 10
#endif

/* X509 altname helpers we never call still reference inet_pton/inet_ntop; route them
 * to lwIP (implemented in tls_platform.cpp) so no socket headers enter wolfSSL. */
#ifdef __cplusplus
extern "C" {
#endif
int bb_inet_pton(int af, const char* src, void* dst);
const char* bb_inet_ntop(int af, const void* src, char* dst, unsigned int size);
#ifdef __cplusplus
}
#endif
#define XINET_PTON(a,b,c)   bb_inet_pton((a),(b),(c))
#define XINET_NTOP(a,b,c,d) bb_inet_ntop((a),(b),(c),(d))

/* Entropy: ESP32 hardware RNG (true random while the radio is on). */
#ifdef __cplusplus
extern "C" {
#endif
int bb_tls_seed(unsigned char* output, unsigned int sz);
#ifdef __cplusplus
}
#endif
#define CUSTOM_RAND_GENERATE_SEED bb_tls_seed
#define HAVE_HASHDRBG

/* ---- protocol: client only, TLS 1.2 + 1.3 ---- */
#define NO_WOLFSSL_SERVER
#define NO_OLD_TLS
#define WOLFSSL_TLS13
#define HAVE_TLS_EXTENSIONS
#define HAVE_SUPPORTED_CURVES
#define HAVE_SNI
#define HAVE_EXTENDED_MASTER
#define HAVE_ENCRYPT_THEN_MAC
#define HAVE_HKDF
#define WC_RSA_PSS
#define NO_SESSION_CACHE           /* full handshake per poll; a cache costs ~220 KB of RAM */
#define NO_CLIENT_CACHE

/* ---- algorithms ---- */
#define HAVE_AESGCM
#define GCM_SMALL
#define WOLFSSL_AES_128
#define WOLFSSL_AES_256
#define HAVE_CHACHA
#define HAVE_POLY1305
#define HAVE_ECC
#define ECC_TIMING_RESISTANT
#define ECC_USER_CURVES             /* P-256 + P-384 only */
#define HAVE_ECC384
#define HAVE_CURVE25519
#define WOLFSSL_SHA384
#define WOLFSSL_SHA512
#define WC_RSA_BLINDING
#define NO_DH
#define NO_DSA
#define NO_RC4
#define NO_MD4
#define NO_DES3
#define NO_PSK
#define NO_PWDBASED
#define WOLFSSL_NO_SHAKE128
#define WOLFSSL_NO_SHAKE256

/* ---- math: single-precision, small code, up to RSA-4096 (Watchtower's CA key) ---- */
#define WOLFSSL_SP_MATH_ALL
#define WOLFSSL_SP_SMALL
#define WOLFSSL_HAVE_SP_RSA
#define WOLFSSL_HAVE_SP_ECC
#define WOLFSSL_SP_4096
#define SP_INT_BITS 4096
#define TFM_TIMING_RESISTANT

/* ---- X.509 ---- */
/* Name Constraints are parsed and enforced by default; never define
 * IGNORE_NAME_CONSTRAINTS or WOLFSSL_X509_TINY here. */
#define WOLFSSL_IP_ALT_NAME         /* iPAddress SANs: name-constraint checks + IP host match */
#define OPENSSL_EXTRA               /* verify-param IP check, X509 accessors for "Trust this server" */
#define SESSION_CERTS               /* keep the presented chain for TOFU pinning */
#define KEEP_PEER_CERT
#define WOLFSSL_DER_TO_PEM
#define WOLFSSL_ALT_CERT_CHAINS
#define WOLFSSL_BASE64_ENCODE

#endif /* CLEARCUE_WOLFSSL_USER_SETTINGS_H */
