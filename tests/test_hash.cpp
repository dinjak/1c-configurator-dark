// SPDX-License-Identifier: MIT
#include <string.h>

#include "core/hash.h"
#include "tests/test.h"

TEST(hash_fnv1a64_vectors) {
  // Опорные значения FNV-1a/64.
  CHECK_EQ(dt::fnv1a64("", 0), 0xcbf29ce484222325ull);
  CHECK_EQ(dt::fnv1a64("a", 1), 0xaf63dc4c8601ec8cull);
  CHECK_EQ(dt::fnv1a64("foobar", 6), 0x85944171f73967e8ull);
  // Продолжение с seed равно хешу склейки.
  uint64_t h = dt::fnv1a64("foo", 3);
  CHECK_EQ(dt::fnv1a64("bar", 3, h), dt::fnv1a64("foobar", 6));
}

TEST(hash_argb32_ignores_stride) {
  const uint32_t w = 3, h = 4;
  uint32_t tight[w * h];
  uint32_t padded[(w + 5) * h];
  memset(padded, 0xAB, sizeof(padded));
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      uint32_t v = 0xFF000000u | (x * 0x10101u) | (y << 4);
      tight[y * w + x] = v;
      padded[y * (w + 5) + x] = v;
    }
  }
  uint64_t a = 0, b = 0;
  CHECK(dt::hash_argb32(tight, w, h, w * 4, &a));
  CHECK(dt::hash_argb32(padded, w, h, (w + 5) * 4, &b));
  CHECK_EQ(a, b);
  CHECK_EQ(a, dt::fnv1a64(tight, sizeof(tight)));
  // Мусор в хвосте строки не влияет.
  padded[w] = 0x12345678u;
  CHECK(dt::hash_argb32(padded, w, h, (w + 5) * 4, &b));
  CHECK_EQ(a, b);
  // Изменение пикселя влияет.
  padded[0] ^= 1;
  CHECK(dt::hash_argb32(padded, w, h, (w + 5) * 4, &b));
  CHECK(a != b);
  // stride меньше строки — ошибка.
  CHECK(!dt::hash_argb32(tight, w, h, w * 4 - 1, &b));
}

TEST(hash_hex_roundtrip) {
  char buf[17];
  dt::hash_to_hex(0x0123456789abcdefull, buf);
  CHECK(strcmp(buf, "0123456789abcdef") == 0);
  uint64_t v = 0;
  CHECK(dt::hash_from_hex("0123456789ABCDEF", 16, &v));
  CHECK_EQ(v, 0x0123456789abcdefull);
  CHECK(!dt::hash_from_hex("0123456789abcde", 15, &v));
  CHECK(!dt::hash_from_hex("0123456789abcdeg", 16, &v));
}
