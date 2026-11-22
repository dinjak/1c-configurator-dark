// SPDX-License-Identifier: MIT
// FNV-1a/64 и хеш пикселей ARGB32 ("fnv1a64-argb32-v1").
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace dt {

const uint64_t kFnv64Offset = 0xcbf29ce484222325ull;
const uint64_t kFnv64Prime = 0x100000001b3ull;

uint64_t fnv1a64(const void* data, size_t len, uint64_t seed = kFnv64Offset);

// Хеш пикселей: строки по w*4 байт в порядке памяти, хвост stride не входит.
// Ложь — неверные размеры (stride < w*4 или переполнение).
bool hash_argb32(const void* pixels, uint32_t w, uint32_t h, uint32_t stride, uint64_t* out);

// 16 шестнадцатеричных цифр, нижний регистр, с нулём в конце.
void hash_to_hex(uint64_t v, char out[17]);
// Ровно 16 шестнадцатеричных цифр, регистр любой.
bool hash_from_hex(const char* s, size_t len, uint64_t* out);

}  // namespace dt
