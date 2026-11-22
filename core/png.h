// SPDX-License-Identifier: MIT
// Декодер PNG: свой inflate (stored, fixed, dynamic), 8 бит RGB/RGBA, без чересстрочности.
// Проверяются CRC всех чанков, Adler-32, порядок чанков и точный объём данных.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace dt {
namespace png {

const size_t kMaxFile = 1u << 20;
const uint32_t kMaxSide = 256;

enum Error : uint8_t {
  kOk = 0,
  kErrTooLarge,
  kErrSignature,
  kErrChunk,
  kErrCrc,
  kErrHeader,
  kErrUnsupported,
  kErrDimensions,
  kErrOrder,
  kErrNoMemory,
  kErrZlib,
  kErrInflate,
  kErrAdler,
  kErrDataSize,
  kErrFilter,
};

struct Image {
  uint32_t w;
  uint32_t h;
  uint8_t* rgba;  // w*h*4, прямая (не умноженная) альфа; malloc
};

Error decode(const uint8_t* data, size_t size, Image* out);
void release(Image* img);

// cairo ARGB32: 32-битное значение 0xAARRGGBB, цвет умножен на альфу.
void rgba_to_argb32_premul(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t* out);

// Внутренние части, открыты для тестов.
uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0);
uint32_t adler32(const uint8_t* data, size_t len, uint32_t adler = 1);
// Сырой DEFLATE: dst заполняется ровно, *out_len — сколько записано.
Error inflate(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap, size_t* out_len,
              size_t* src_used);
// zlib-обёртка: заголовок, DEFLATE, Adler-32; хвост после Adler-32 — ошибка.
Error zlib_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap,
                      size_t* out_len);

const char* error_name(Error e);

}  // namespace png
}  // namespace dt
