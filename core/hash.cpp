// SPDX-License-Identifier: MIT
#include "core/hash.h"

namespace dt {

uint64_t fnv1a64(const void* data, size_t len, uint64_t seed) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint64_t h = seed;
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= kFnv64Prime;
  }
  return h;
}

bool hash_argb32(const void* pixels, uint32_t w, uint32_t h, uint32_t stride, uint64_t* out) {
  uint64_t row = static_cast<uint64_t>(w) * 4u;
  if (row > 0xFFFFFFFFull || stride < row) return false;
  if ((w != 0 && h != 0) && pixels == nullptr) return false;
  uint64_t v = kFnv64Offset;
  const uint8_t* p = static_cast<const uint8_t*>(pixels);
  for (uint32_t y = 0; y < h && w != 0; ++y) {
    v = fnv1a64(p + static_cast<size_t>(y) * stride, static_cast<size_t>(row), v);
  }
  *out = v;
  return true;
}

void hash_to_hex(uint64_t v, char out[17]) {
  static const char kDigits[] = "0123456789abcdef";
  for (int i = 15; i >= 0; --i) {
    out[i] = kDigits[v & 0xF];
    v >>= 4;
  }
  out[16] = 0;
}

bool hash_from_hex(const char* s, size_t len, uint64_t* out) {
  if (s == nullptr || len != 16) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < 16; ++i) {
    char c = s[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
    else return false;
    v = (v << 4) | d;
  }
  *out = v;
  return true;
}

}  // namespace dt
