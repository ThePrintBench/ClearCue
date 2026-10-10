// Certificate captured for "Trust this server" (trust on first use).
// Plain struct, no wolfSSL headers: main.cpp must not see wolfSSL's OpenSSL
// compatibility names (they collide with the core's SHA_CTX etc.).
#pragma once

typedef struct {
  char pem[3072];
  char sha256[96];     // "AB:CD:..." over the DER
  char subject[160];
  char issuer[160];
  int  isCA;
  int  selfSigned;
  int  chainLen;       // certificates the server presented
} bb_tls_anchor;
