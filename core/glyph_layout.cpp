// SPDX-License-Identifier: MIT
#include "core/glyph_layout.h"

#include <string.h>

namespace dt {
namespace glyph {
namespace {

bool plausible_coord(double v) {
  if (!(v == v)) return false;  // NaN
  double a = v < 0 ? -v : v;
  if (a > 1e5) return false;    // заодно отсекает бесконечность
  return a == 0.0 || a >= 1e-3; // денормалы и «хвосты» соседних полей — мусор
}

}  // namespace

LayoutDesc describe(Layout l) {
  LayoutDesc d = {0, 0, 0};
  if (l == kStride24) {
    d.stride = 24;
    d.x_off = 8;
    d.y_off = 16;
  } else if (l == kStride20) {
    d.stride = 20;
    d.x_off = 4;
    d.y_off = 12;
  }
  return d;
}

Layout compiled_layout() {
  constexpr bool k24 = sizeof(CairoGlyph) == 24 && offsetof(CairoGlyph, x) == 8 && offsetof(CairoGlyph, y) == 16;
  constexpr bool k20 = sizeof(CairoGlyph) == 20 && offsetof(CairoGlyph, x) == 4 && offsetof(CairoGlyph, y) == 12;
  return k24 ? kStride24 : (k20 ? kStride20 : kUnknown);
}

Verdict check_run(const void* glyphs, uint32_t n, size_t readable, Layout l) {
  LayoutDesc d = describe(l);
  if (glyphs == nullptr || d.stride == 0 || n == 0) return kNoData;
  size_t fit = readable / d.stride;
  uint32_t m = fit < n ? static_cast<uint32_t>(fit) : n;
  if (m == 0) return kNoData;
  const uint8_t* p = static_cast<const uint8_t*>(glyphs);
  double y0 = 0.0;
  for (uint32_t i = 0; i < m; ++i) {
    const uint8_t* g = p + static_cast<size_t>(i) * d.stride;
    uint32_t index;
    double x, y;
    memcpy(&index, g, 4);
    memcpy(&x, g + d.x_off, 8);
    memcpy(&y, g + d.y_off, 8);
    if (index >= 0x10000u) return kImplausible;
    if (!plausible_coord(x) || !plausible_coord(y)) return kImplausible;
    if (i == 0) {
      y0 = y;
    } else {
      double dy = y - y0;
      if (dy < 0) dy = -dy;
      if (dy > 1e3) return kImplausible;
    }
  }
  return kPlausible;
}

Layout detect(const void* glyphs, uint32_t n) {
  size_t readable = static_cast<size_t>(n) * 20;
  Verdict a = check_run(glyphs, n, readable, kStride24);
  Verdict b = check_run(glyphs, n, readable, kStride20);
  if (a == kPlausible && b == kImplausible) return kStride24;
  if (b == kPlausible && a == kImplausible) return kStride20;
  return kUnknown;
}

void SelfCheck::init(Layout expected, uint32_t required, size_t min_stride) {
  expected_ = expected;
  required_ = required == 0 ? 1 : required;
  min_stride_ = min_stride;
  good_.store(0, std::memory_order_relaxed);
  state_.store(expected == kUnknown ? kFailed : kPending, std::memory_order_release);
}

SelfCheck::State SelfCheck::feed(const void* glyphs, uint32_t n) {
  State st = state();
  if (st != kPending) return st;
  // Читаем не дальше n·min_stride: при неверной гипотезе о шаге не выйти за массив.
  Verdict v = check_run(glyphs, n, static_cast<size_t>(n) * min_stride_, expected_);
  if (v == kImplausible) {
    state_.store(kFailed, std::memory_order_release);
    return kFailed;
  }
  if (v == kPlausible && n >= 2) {
    if (good_.fetch_add(1, std::memory_order_relaxed) + 1 >= required_) {
      uint8_t expect = kPending;
      state_.compare_exchange_strong(expect, kPassed, std::memory_order_acq_rel);
    }
  }
  return state();
}

}  // namespace glyph
}  // namespace dt
