// Platform hooks wolfSSL / tls_policy call into. Kept free of wolfSSL headers so
// lwIP's socket API can be included without clashing with wolfSSL's names.
#include <Arduino.h>
#include <esp_random.h>
#include <lwip/sockets.h>
#include <time.h>

// Entropy for wolfSSL's DRBG: hardware RNG (true random while the radio is on).
extern "C" int bb_tls_seed(unsigned char* output, unsigned int sz) {
  esp_fill_random(output, sz);
  return 0;
}

// Certificate validity dates are only enforced once SNTP has set the clock.
extern "C" int bb_tls_clock_valid(void) {
  return time(nullptr) > 1735689600;   // 2025-01-01
}

extern "C" int bb_inet_pton(int af, const char* src, void* dst) {
  return lwip_inet_pton(af, src, dst);
}

extern "C" const char* bb_inet_ntop(int af, const void* src, char* dst, unsigned int size) {
  return lwip_inet_ntop(af, src, dst, (socklen_t)size);
}
