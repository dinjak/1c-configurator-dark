// SPDX-License-Identifier: MIT
// Раскладка cairo_glyph_t { unsigned long index; double x; double y; }.
// x64 (LLP64): 24 байта, x по 8, y по 16 — проверяется при компиляции.
// x86: раскладка чужой сборки cairo не гарантирована (24/8/16 или 20/4/12),
// поэтому пропуск глифов включается только после живой самопроверки.
#pragma once
#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace dt {
namespace glyph {

struct CairoGlyph {
  unsigned long index;
  double x;
  double y;
};

#if defined(_WIN64)
static_assert(sizeof(CairoGlyph) == 24, "cairo_glyph_t x64: 24 bytes");
static_assert(offsetof(CairoGlyph, index) == 0, "cairo_glyph_t x64: index at 0");
static_assert(offsetof(CairoGlyph, x) == 8, "cairo_glyph_t x64: x at 8");
static_assert(offsetof(CairoGlyph, y) == 16, "cairo_glyph_t x64: y at 16");
#endif

enum Layout : uint8_t { kUnknown = 0, kStride24 = 1, kStride20 = 2 };

struct LayoutDesc {
  uint32_t stride;
  uint32_t x_off;
  uint32_t y_off;
};

LayoutDesc describe(Layout l);
// Раскладка, с которой собран этот модуль (по offsetof CairoGlyph).
Layout compiled_layout();

enum Verdict : uint8_t { kNoData = 0, kPlausible, kImplausible };

// Проверка прогона: читается не больше readable байт от glyphs; проверяются только
// глифы, целиком лежащие в этих байтах. Индекс < 0x10000; x, y конечны, |v| ≤ 1e5,
// ненулевые не меньше 1e-3 по модулю; разброс y ≤ 1e3.
Verdict check_run(const void* glyphs, uint32_t n, size_t readable, Layout l);

// Однозначный выбор: ровно одна раскладка правдоподобна. Читается ≤ 20·n байт.
Layout detect(const void* glyphs, uint32_t n);

// Живая самопроверка: required прогонов (n ≥ 2) подряд правдоподобны для ожидаемой
// раскладки — пройдено; первый неправдоподобный — провал до конца сессии.
class SelfCheck {
 public:
  enum State : uint8_t { kPending = 0, kPassed, kFailed };
  void init(Layout expected, uint32_t required, size_t min_stride);
  State feed(const void* glyphs, uint32_t n);
  State state() const { return static_cast<State>(state_.load(std::memory_order_acquire)); }
  uint32_t seen() const { return good_.load(std::memory_order_relaxed); }

 private:
  Layout expected_;
  uint32_t required_;
  size_t min_stride_;
  std::atomic<uint32_t> good_;
  std::atomic<uint8_t> state_;
};

}  // namespace glyph
}  // namespace dt
