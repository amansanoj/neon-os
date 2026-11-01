#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "firmware-version.h"

namespace OtaPolicy {
constexpr size_t MAX_MANIFEST = 1536;
constexpr size_t SIGNATURE_SIZE = 384;  // RSA-3072 PSS / SHA-256, 32-byte salt.
constexpr size_t SLOT_SIZE = 0x1F0000;
struct Manifest {
  uint32_t sequence = 0, size = 0;
  char version[32]{};
  char sha256[65]{};
  char url[769]{};
};
inline bool httpsUrl(const char* url, size_t maxLength=768) {
  const size_t n = strlen(url);
  if (n < 10 || n > maxLength || strncmp(url, "https://", 8)) return false;
  const char* host = url + 8;
  const char* end = strchr(host, '/');
  if (!end || end == host || *host == '.' || end[-1] == '.') return false;
  for (const char* p = host; p < end; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) return false;
  for (const char* p = end; *p; ++p)
    if (static_cast<unsigned char>(*p) <= 32 || static_cast<unsigned char>(*p) >= 127 ||
        *p == '\\' || *p == '#' || *p == '"') return false;
  return true;
}
inline bool number(const char* s, uint32_t& out) {
  if (!*s || (*s == '0' && s[1])) return false;
  uint32_t n = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9' || n > (UINT32_MAX - (*s - '0')) / 10U) return false;
    n = n * 10U + (*s - '0');
  }
  out = n;
  return true;
}
// Parse only AFTER verifying the signature over the exact original bytes.
inline bool parse(const char* raw, size_t length, Manifest& out) {
  if (!length || length > MAX_MANIFEST || raw[length-1] != '\n' ||
      memchr(raw, '\0', length)) return false;
  char data[MAX_MANIFEST+1];
  memcpy(data, raw, length); data[length] = 0;
  char* lines[8];
  char* p = data;
  for (auto& line : lines) {
    line = p;
    char* end = strchr(p, '\n');
    if (!end) return false;
    *end = 0; p = end + 1;
  }
  if (*p || strcmp(lines[0], "NEON-OTA-1") || strcmp(lines[1], NEON_HARDWARE_ID) ||
      strcmp(lines[2], NEON_RELEASE_CHANNEL)) return false;
  Manifest candidate;
  if (!number(lines[3], candidate.sequence) || !candidate.sequence ||
      !number(lines[5], candidate.size) || !candidate.size || candidate.size > SLOT_SIZE)
    return false;
  const size_t versionLength = strlen(lines[4]);
  if (!versionLength || versionLength >= sizeof(candidate.version)) return false;
  for (const char* v = lines[4]; *v; ++v)
    if (!((*v >= 'a' && *v <= 'z') || (*v >= 'A' && *v <= 'Z') ||
          (*v >= '0' && *v <= '9') || *v == '.' || *v == '-' || *v == '_')) return false;
  if (strlen(lines[6]) != 64) return false;
  for (const char* h = lines[6]; *h; ++h)
    if (!((*h >= '0' && *h <= '9') || (*h >= 'a' && *h <= 'f'))) return false;
  if (!httpsUrl(lines[7])) return false;
  strcpy(candidate.version, lines[4]); strcpy(candidate.sha256, lines[6]);
  strcpy(candidate.url, lines[7]);
  out = candidate;
  return true;
}
inline bool eligible(const Manifest& m, uint32_t running, uint32_t highest, uint32_t failed) {
  return m.sequence > running && m.sequence > highest && m.sequence != failed;
}
}  // namespace OtaPolicy
