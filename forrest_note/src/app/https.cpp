#include "https.h"
#include "WiFiClientSecure.h"

// IDF built-in Mozilla CA root bundle (libmbedtls.a). Auto-maintained with the
// esp32 core, so server certs validate without shipping/rotating a pinned PEM.
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

void httpsAttachCa(WiFiClientSecure& client) {
  client.setCACertBundle(x509_crt_bundle_start,
                         (size_t)(x509_crt_bundle_end - x509_crt_bundle_start));
}

String dechunkBody(const String& in) {
  String out; int i = 0, n = in.length();
  while (i < n) {
    int eol = in.indexOf('\n', i);
    if (eol < 0) break;
    String sizeLine = in.substring(i, eol);
    int semi = sizeLine.indexOf(';');                 // ignore chunk extensions
    if (semi >= 0) sizeLine = sizeLine.substring(0, semi);
    sizeLine.trim();
    long sz = strtol(sizeLine.c_str(), nullptr, 16);
    i = eol + 1;
    if (sz <= 0) break;                               // terminating 0-chunk
    if (i + sz > n) sz = n - i;                        // guard against truncation
    out += in.substring(i, i + sz);
    i += sz;
    while (i < n && (in[i] == '\r' || in[i] == '\n')) i++;  // skip CRLF after data
  }
  return out;
}
