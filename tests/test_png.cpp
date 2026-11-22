// SPDX-License-Identifier: MIT
#include <stdlib.h>
#include <string.h>

#include "core/png.h"
#include "tests/png_vectors.h"
#include "tests/test.h"

using namespace dt;

namespace {

// Совпадает с pixel() в tests/gen_png_vectors.py.
void expected_pixel(uint32_t x, uint32_t y, uint8_t out[4]) {
  out[0] = static_cast<uint8_t>((x * 37 + y * 11) & 255);
  out[1] = static_cast<uint8_t>(((x * 13) ^ (y * 29)) & 255);
  out[2] = static_cast<uint8_t>((x * y * 7 + 3) & 255);
  out[3] = static_cast<uint8_t>((255 - x * 9 - y * 5) & 255);
}

bool image_matches(const png::Image& img, bool has_alpha) {
  for (uint32_t y = 0; y < img.h; ++y) {
    for (uint32_t x = 0; x < img.w; ++x) {
      uint8_t e[4];
      expected_pixel(x, y, e);
      if (!has_alpha) e[3] = 255;
      if (memcmp(img.rgba + (static_cast<size_t>(y) * img.w + x) * 4, e, 4) != 0) return false;
    }
  }
  return true;
}

void check_vector(const unsigned char* data, size_t size, uint32_t w, uint32_t h, bool alpha) {
  png::Image img;
  CHECK_EQ(png::decode(data, size, &img), png::kOk);
  CHECK_EQ(img.w, w);
  CHECK_EQ(img.h, h);
  CHECK(img.rgba != nullptr && image_matches(img, alpha));
  png::release(&img);
}

// Копия вектора с изменённым байтом и пересчитанным CRC затронутого чанка.
unsigned char* mutate(const unsigned char* src, size_t size, size_t at, unsigned char value, bool fix_crc) {
  unsigned char* p = static_cast<unsigned char*>(malloc(size));
  memcpy(p, src, size);
  p[at] = value;
  if (fix_crc) {
    size_t pos = 8;
    while (pos + 12 <= size) {
      uint32_t len = (static_cast<uint32_t>(p[pos]) << 24) | (p[pos + 1] << 16) | (p[pos + 2] << 8) | p[pos + 3];
      if (at >= pos && at < pos + 12 + len) {
        uint32_t c = png::crc32(p + pos + 4, len + 4);
        p[pos + 8 + len] = static_cast<unsigned char>(c >> 24);
        p[pos + 9 + len] = static_cast<unsigned char>(c >> 16);
        p[pos + 10 + len] = static_cast<unsigned char>(c >> 8);
        p[pos + 11 + len] = static_cast<unsigned char>(c);
        break;
      }
      pos += 12 + len;
    }
  }
  return p;
}

png::Error decode_mut(const unsigned char* src, size_t size, size_t at, unsigned char value, bool fix_crc) {
  unsigned char* p = mutate(src, size, at, value, fix_crc);
  png::Image img;
  png::Error e = png::decode(p, size, &img);
  png::release(&img);
  free(p);
  return e;
}

void mixed_expected(uint8_t* out) {
  // Совпадает с mixed_zlib() в tests/gen_png_vectors.py.
  size_t n = 0;
  for (int k = 0; k < 2; ++k)
    for (int i = 0; i < 256; ++i) out[n++] = static_cast<uint8_t>(i);
  for (int i = 0; i < 200; ++i) {
    out[n++] = 'a';
    out[n++] = 'b';
  }
  for (int i = 0; i < 300; ++i) out[n++] = 'x';
  for (int i = 0; i < 3000; ++i) out[n++] = static_cast<uint8_t>((i * 7 + (i >> 3)) & 255);
  for (int i = 0; i < 50; ++i) {
    memcpy(out + n, "tail", 4);
    n += 4;
  }
}

}  // namespace

TEST(png_decode_all_deflate_modes_and_filters) {
  check_vector(kPngRgbaStored, sizeof(kPngRgbaStored), 7, 5, true);    // stored
  check_vector(kPngRgbaFixed, sizeof(kPngRgbaFixed), 13, 9, true);     // fixed Huffman
  check_vector(kPngRgbDynamic, sizeof(kPngRgbDynamic), 32, 24, false); // dynamic, RGB
  check_vector(kPngRgbaMultiIdat, sizeof(kPngRgbaMultiIdat), 48, 48, true);  // 3 IDAT
}

TEST(png_zlib_mixed_blocks) {
  uint8_t* want = static_cast<uint8_t*>(malloc(kZlibMixedPlainSize));
  uint8_t* got = static_cast<uint8_t*>(malloc(kZlibMixedPlainSize + 16));
  mixed_expected(want);
  size_t n = 0;
  CHECK_EQ(png::zlib_decompress(kZlibMixed, sizeof(kZlibMixed), got, kZlibMixedPlainSize, &n), png::kOk);
  CHECK_EQ(n, static_cast<size_t>(kZlibMixedPlainSize));
  CHECK(memcmp(want, got, kZlibMixedPlainSize) == 0);
  // Приёмник меньше данных — ошибка, без записи за границу.
  CHECK_EQ(png::zlib_decompress(kZlibMixed, sizeof(kZlibMixed), got, kZlibMixedPlainSize - 1, &n),
           png::kErrDataSize);
  // Порча Adler-32.
  unsigned char* bad = static_cast<unsigned char*>(malloc(sizeof(kZlibMixed)));
  memcpy(bad, kZlibMixed, sizeof(kZlibMixed));
  bad[sizeof(kZlibMixed) - 1] ^= 1;
  CHECK_EQ(png::zlib_decompress(bad, sizeof(kZlibMixed), got, kZlibMixedPlainSize, &n), png::kErrAdler);
  // Неверный заголовок zlib.
  memcpy(bad, kZlibMixed, sizeof(kZlibMixed));
  bad[1] ^= 1;
  CHECK_EQ(png::zlib_decompress(bad, sizeof(kZlibMixed), got, kZlibMixedPlainSize, &n), png::kErrZlib);
  // Обрезанный поток.
  CHECK(png::zlib_decompress(kZlibMixed, sizeof(kZlibMixed) / 2, got, kZlibMixedPlainSize, &n) != png::kOk);
  free(bad);
  free(got);
  free(want);
}

TEST(png_inflate_invalid_streams) {
  uint8_t out[64];
  size_t n;
  // Тип блока 3 — зарезервирован.
  const uint8_t reserved[] = {0x07};
  CHECK_EQ(png::inflate(reserved, sizeof(reserved), out, sizeof(out), &n, nullptr), png::kErrInflate);
  // Stored: NLEN не дополнение LEN.
  const uint8_t stored_bad[] = {0x01, 0x03, 0x00, 0xFC, 0xFE, 'a', 'b', 'c'};
  CHECK_EQ(png::inflate(stored_bad, sizeof(stored_bad), out, sizeof(out), &n, nullptr), png::kErrInflate);
  const uint8_t stored_ok[] = {0x01, 0x03, 0x00, 0xFC, 0xFF, 'a', 'b', 'c'};
  CHECK_EQ(png::inflate(stored_ok, sizeof(stored_ok), out, sizeof(out), &n, nullptr), png::kOk);
  CHECK(n == 3 && memcmp(out, "abc", 3) == 0);
  // Fixed: ссылка назад дальше начала вывода (длина 3, расстояние 1 при пустом выводе).
  // Биты: BFINAL=1, BTYPE=01, код 257 (0000001, 7 бит), дистанция 0 (00000, 5 бит).
  const uint8_t far_ref[] = {0x03, 0x02, 0x00};
  CHECK_EQ(png::inflate(far_ref, sizeof(far_ref), out, sizeof(out), &n, nullptr), png::kErrInflate);
  // Dynamic: переподписанный код длин кодов (все 19 длин = 1).
  // BFINAL=1, BTYPE=10, HLIT=0, HDIST=0, HCLEN=15 (19 кодов), каждая длина 001.
  uint8_t over[16];
  memset(over, 0, sizeof(over));
  {
    uint32_t bitpos = 0;
    auto put = [&](uint32_t v, int bits) {
      for (int i = 0; i < bits; ++i, ++bitpos) {
        if ((v >> i) & 1) over[bitpos / 8] |= static_cast<uint8_t>(1u << (bitpos % 8));
      }
    };
    put(1, 1);
    put(2, 2);
    put(0, 5);
    put(0, 5);
    put(15, 4);
    for (int i = 0; i < 19; ++i) put(1, 3);
  }
  CHECK_EQ(png::inflate(over, sizeof(over), out, sizeof(out), &n, nullptr), png::kErrInflate);
  // Пустой вход.
  CHECK_EQ(png::inflate(out, 0, out, sizeof(out), &n, nullptr), png::kErrInflate);
}

TEST(png_rejects_bad_files) {
  const unsigned char* v = kPngRgbaFixed;
  const size_t sz = sizeof(kPngRgbaFixed);
  // Сигнатура.
  CHECK_EQ(decode_mut(v, sz, 1, 'Q', false), png::kErrSignature);
  // CRC IHDR (байт ширины без пересчёта CRC).
  CHECK_EQ(decode_mut(v, sz, 8 + 8 + 3, 14, false), png::kErrCrc);
  // Глубина 16 бит, тип палитры, чересстрочность — не поддерживаются.
  CHECK_EQ(decode_mut(v, sz, 8 + 8 + 8, 16, true), png::kErrUnsupported);
  CHECK_EQ(decode_mut(v, sz, 8 + 8 + 9, 3, true), png::kErrUnsupported);
  CHECK_EQ(decode_mut(v, sz, 8 + 8 + 12, 1, true), png::kErrUnsupported);
  // Размеры: 0 и больше kMaxSide.
  CHECK_EQ(decode_mut(v, sz, 8 + 8 + 3, 0, true), png::kErrDimensions);
  CHECK_EQ(decode_mut(v, sz, 8 + 8 + 2, 0x01, true), png::kErrDimensions);  // ширина 256+13
  // Несовпадение объёма данных: высота 8 вместо 9.
  CHECK_EQ(decode_mut(v, sz, 8 + 8 + 7, 8, true), png::kErrDataSize);
  // Неверный тип фильтра в первой строке (stored-вектор: байт фильтра виден как есть).
  // Поток stored: 2 байта zlib + 5 байт заголовка блока, затем первая строка.
  {
    size_t idat = 8 + 25;
    // tEXt идёт перед IDAT: длина 3.
    idat += 12 + 3;
    size_t first_filter = idat + 8 + 2 + 5;
    unsigned char* p = mutate(kPngRgbaStored, sizeof(kPngRgbaStored), first_filter, 7, false);
    // Пересчитать Adler-32 и CRC IDAT.
    uint32_t len = (static_cast<uint32_t>(p[idat]) << 24) | (p[idat + 1] << 16) | (p[idat + 2] << 8) | p[idat + 3];
    size_t raw_len = 5 * (1 + 7 * 4);
    uint32_t ad = png::adler32(p + idat + 8 + 2 + 5, raw_len);
    size_t ad_at = idat + 8 + len - 4;
    p[ad_at] = static_cast<unsigned char>(ad >> 24);
    p[ad_at + 1] = static_cast<unsigned char>(ad >> 16);
    p[ad_at + 2] = static_cast<unsigned char>(ad >> 8);
    p[ad_at + 3] = static_cast<unsigned char>(ad);
    uint32_t c = png::crc32(p + idat + 4, len + 4);
    p[idat + 8 + len] = static_cast<unsigned char>(c >> 24);
    p[idat + 9 + len] = static_cast<unsigned char>(c >> 16);
    p[idat + 10 + len] = static_cast<unsigned char>(c >> 8);
    p[idat + 11 + len] = static_cast<unsigned char>(c);
    png::Image img;
    CHECK_EQ(png::decode(p, sizeof(kPngRgbaStored), &img), png::kErrFilter);
    png::release(&img);
    free(p);
  }
  // Обрезка файла: нет IEND.
  png::Image img;
  CHECK(png::decode(v, sz - 12, &img) != png::kOk);
  CHECK(png::decode(v, 20, &img) != png::kOk);
  // Данные после IEND.
  unsigned char* tail = static_cast<unsigned char*>(malloc(sz + 1));
  memcpy(tail, v, sz);
  tail[sz] = 0;
  CHECK_EQ(png::decode(tail, sz + 1, &img), png::kErrOrder);
  free(tail);
  // Файл больше лимита.
  CHECK_EQ(png::decode(v, png::kMaxFile + 1, &img), png::kErrTooLarge);
}

TEST(png_premultiply) {
  const uint8_t rgba[8] = {255, 128, 0, 128, 10, 20, 30, 255};
  uint32_t out[2];
  png::rgba_to_argb32_premul(rgba, 2, 1, out);
  CHECK_EQ(out[0], 0x80804000u);  // 255*128/255=128, 128*128/255≈64
  CHECK_EQ(out[1], 0xFF0A141Eu);
}
