// SPDX-License-Identifier: MIT
#include "core/dup.h"

#include <string.h>

#include "core/hash.h"

namespace dt {
namespace dup {
namespace {

uint64_t mix64(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
}

uint64_t dbits(double v) {
  uint64_t u;
  memcpy(&u, &v, sizeof(u));
  return u;
}

double bitsd(uint64_t u) {
  double v;
  memcpy(&v, &u, sizeof(v));
  return v;
}

uint64_t checksum(const uint64_t* w) {
  uint64_t c = 0x9e3779b97f4a7c15ull;
  for (int i = 0; i < 6; ++i) c = mix64(c ^ w[i]);
  return c | 1;  // 0 — пустой слот
}

}  // namespace

uint64_t glyph_index_hash(const void* glyphs, uint32_t n, uint32_t stride) {
  const uint8_t* p = static_cast<const uint8_t*>(glyphs);
  uint64_t h = kFnv64Offset;
  for (uint32_t i = 0; i < n; ++i) h = fnv1a64(p + static_cast<size_t>(i) * stride, 4, h);
  return h;
}

bool Detector::init(const Params& p) {
  if (p.ring < kMinRing || p.ring > kMaxRing || (p.ring & (p.ring - 1)) != 0) return false;
  if (p.max_dx < 0 || p.max_dy < 0) return false;
  p_ = p;
  reset();
  return true;
}

void Detector::reset() {
  seq_.store(0, std::memory_order_relaxed);
  for (uint32_t i = 0; i < kMaxRing; ++i) {
    for (int k = 0; k < 8; ++k) slots_[i].w[k].store(0, std::memory_order_relaxed);
  }
}

bool Detector::check(uint64_t glyph_hash, uint32_t count, uint32_t rgb, uint64_t surface, double x,
                     double y) {
  uint64_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t meta = (static_cast<uint64_t>(count) << 32) | (rgb & 0xFFFFFF);
  uint64_t key = mix64(glyph_hash ^ mix64(meta) ^ mix64(surface + 0x632be59bd9b4e019ull));
  Slot& s = slots_[key & (p_.ring - 1)];

  uint64_t cur[8];
  for (int k = 0; k < 7; ++k) cur[k] = s.w[k].load(std::memory_order_acquire);
  bool consistent = cur[6] != 0 && cur[6] == checksum(cur);
  if (consistent && cur[0] == key && cur[1] == surface && cur[2] == meta && seq - cur[5] <= p_.ring) {
    double dx = x - bitsd(cur[3]);
    double dy = y - bitsd(cur[4]);
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx <= p_.max_dx && dy <= p_.max_dy) {
      // Потребить: следующий такой же прогон — уже новая пара, а не третий проход.
      s.w[6].store(0, std::memory_order_release);
      return true;
    }
  }
  uint64_t w[6] = {key, surface, meta, dbits(x), dbits(y), seq};
  s.w[6].store(0, std::memory_order_release);
  for (int k = 0; k < 6; ++k) s.w[k].store(w[k], std::memory_order_relaxed);
  s.w[6].store(checksum(w), std::memory_order_release);
  return false;
}

}  // namespace dup
}  // namespace dt
