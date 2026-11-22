// SPDX-License-Identifier: MIT
#include "core/png.h"

#include <stdlib.h>
#include <string.h>

namespace dt {
namespace png {
namespace {

// ---------------------------------------------------------------------------
// Чтение битов DEFLATE: младший бит первым.

struct Bits {
  const uint8_t* p;
  const uint8_t* end;
  uint32_t buf;
  int cnt;
  bool overrun;
};

bool need(Bits* b, int n) {
  while (b->cnt < n) {
    if (b->p >= b->end) {
      b->overrun = true;
      return false;
    }
    b->buf |= static_cast<uint32_t>(*b->p++) << b->cnt;
    b->cnt += 8;
  }
  return true;
}

bool getbits(Bits* b, int n, uint32_t* out) {
  if (n == 0) {
    *out = 0;
    return true;
  }
  if (!need(b, n)) return false;
  *out = b->buf & ((1u << n) - 1u);
  b->buf >>= n;
  b->cnt -= n;
  return true;
}

// Канонический код Хаффмана: число кодов каждой длины и символы по порядку кодов.
const int kMaxBits = 15;

struct Huff {
  uint16_t count[kMaxBits + 1];
  uint16_t symbol[288];
  int ncodes;
};

// Ложь — переподписанный код или неполный (кроме одного кода длиной 1 и пустого).
bool build(Huff* h, const uint8_t* lengths, int n) {
  memset(h->count, 0, sizeof(h->count));
  for (int i = 0; i < n; ++i) h->count[lengths[i]]++;
  h->ncodes = n - h->count[0];
  if (h->ncodes == 0) return true;  // пустой код; декодирование из него — ошибка
  int left = 1;
  for (int len = 1; len <= kMaxBits; ++len) {
    left <<= 1;
    left -= h->count[len];
    if (left < 0) return false;
  }
  if (left > 0 && !(h->ncodes == 1 && h->count[1] == 1)) return false;
  uint16_t offs[kMaxBits + 1];
  offs[1] = 0;
  for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + h->count[len]);
  for (int i = 0; i < n; ++i) {
    if (lengths[i] != 0) h->symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
  }
  return true;
}

// -1 — ошибка (нет данных или код вне таблицы).
int decode_sym(Bits* b, const Huff* h) {
  if (h->ncodes == 0) return -1;
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= kMaxBits; ++len) {
    uint32_t bit;
    if (!getbits(b, 1, &bit)) return -1;
    code |= static_cast<int>(bit);
    int count = h->count[len];
    if (code - first < count) return h->symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}

const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct Out {
  uint8_t* dst;
  size_t cap;
  size_t pos;
};

Error codes(Bits* b, Out* o, const Huff* lit, const Huff* dist) {
  for (;;) {
    int sym = decode_sym(b, lit);
    if (sym < 0) return kErrInflate;
    if (sym < 256) {
      if (o->pos >= o->cap) return kErrDataSize;
      o->dst[o->pos++] = static_cast<uint8_t>(sym);
      continue;
    }
    if (sym == 256) return kOk;
    sym -= 257;
    if (sym >= 29) return kErrInflate;
    uint32_t extra;
    if (!getbits(b, kLenExtra[sym], &extra)) return kErrInflate;
    size_t len = kLenBase[sym] + extra;
    int ds = decode_sym(b, dist);
    if (ds < 0 || ds >= 30) return kErrInflate;
    if (!getbits(b, kDistExtra[ds], &extra)) return kErrInflate;
    size_t d = kDistBase[ds] + extra;
    if (d > o->pos) return kErrInflate;
    if (len > o->cap - o->pos) return kErrDataSize;
    // Побайтно: источник может перекрываться с приёмником.
    for (size_t i = 0; i < len; ++i) {
      o->dst[o->pos] = o->dst[o->pos - d];
      ++o->pos;
    }
  }
}

Error block_stored(Bits* b, Out* o) {
  b->buf = 0;  // до границы байта; need() читает побайтно, остаток < 8 бит
  b->cnt = 0;
  if (b->end - b->p < 4) return kErrInflate;
  uint32_t len = static_cast<uint32_t>(b->p[0]) | (static_cast<uint32_t>(b->p[1]) << 8);
  uint32_t nlen = static_cast<uint32_t>(b->p[2]) | (static_cast<uint32_t>(b->p[3]) << 8);
  b->p += 4;
  if ((len ^ 0xFFFFu) != nlen) return kErrInflate;
  if (static_cast<size_t>(b->end - b->p) < len) return kErrInflate;
  if (len > o->cap - o->pos) return kErrDataSize;
  memcpy(o->dst + o->pos, b->p, len);
  o->pos += len;
  b->p += len;
  return kOk;
}

Error block_fixed(Bits* b, Out* o) {
  // Таблицы строятся на стеке: без общего состояния между потоками.
  uint8_t l[288];
  int i = 0;
  for (; i < 144; ++i) l[i] = 8;
  for (; i < 256; ++i) l[i] = 9;
  for (; i < 280; ++i) l[i] = 7;
  for (; i < 288; ++i) l[i] = 8;
  // 32 кода расстояния образуют полный код; символы 30 и 31 отвергает codes().
  uint8_t d[32];
  for (i = 0; i < 32; ++i) d[i] = 5;
  Huff lit, dist;
  build(&lit, l, 288);
  build(&dist, d, 32);
  return codes(b, o, &lit, &dist);
}

Error block_dynamic(Bits* b, Out* o) {
  static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  uint32_t hlit, hdist, hclen;
  if (!getbits(b, 5, &hlit) || !getbits(b, 5, &hdist) || !getbits(b, 4, &hclen)) return kErrInflate;
  hlit += 257;
  hdist += 1;
  hclen += 4;
  if (hlit > 286 || hdist > 30) return kErrInflate;
  uint8_t lengths[286 + 30];
  memset(lengths, 0, sizeof(lengths));
  uint8_t cl[19];
  memset(cl, 0, sizeof(cl));
  for (uint32_t i = 0; i < hclen; ++i) {
    uint32_t v;
    if (!getbits(b, 3, &v)) return kErrInflate;
    cl[kOrder[i]] = static_cast<uint8_t>(v);
  }
  Huff clh;
  if (!build(&clh, cl, 19) || clh.ncodes == 0) return kErrInflate;
  uint32_t total = hlit + hdist;
  uint32_t idx = 0;
  while (idx < total) {
    int sym = decode_sym(b, &clh);
    if (sym < 0) return kErrInflate;
    if (sym < 16) {
      lengths[idx++] = static_cast<uint8_t>(sym);
      continue;
    }
    uint8_t val = 0;
    uint32_t rep;
    if (sym == 16) {
      if (idx == 0) return kErrInflate;
      val = lengths[idx - 1];
      if (!getbits(b, 2, &rep)) return kErrInflate;
      rep += 3;
    } else if (sym == 17) {
      if (!getbits(b, 3, &rep)) return kErrInflate;
      rep += 3;
    } else {
      if (!getbits(b, 7, &rep)) return kErrInflate;
      rep += 11;
    }
    if (idx + rep > total) return kErrInflate;
    while (rep--) lengths[idx++] = val;
  }
  if (lengths[256] == 0) return kErrInflate;  // нет кода конца блока
  Huff lit, dist;
  if (!build(&lit, lengths, static_cast<int>(hlit))) return kErrInflate;
  if (!build(&dist, lengths + hlit, static_cast<int>(hdist))) return kErrInflate;
  return codes(b, o, &lit, &dist);
}

// ---------------------------------------------------------------------------

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

bool type_is(const uint8_t* t, const char* s) { return memcmp(t, s, 4) == 0; }

bool type_valid(const uint8_t* t) {
  for (int i = 0; i < 4; ++i) {
    uint8_t c = t[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
  }
  return true;
}

struct CrcTable {
  uint32_t v[256];
  constexpr CrcTable() : v() {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      v[n] = c;
    }
  }
};
constexpr CrcTable kCrcTable;

uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
  int p = static_cast<int>(a) + b - c;
  int pa = p > a ? p - a : a - p;
  int pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

Error unfilter(uint8_t* raw, uint32_t w, uint32_t h, uint32_t bpp) {
  size_t row = static_cast<size_t>(w) * bpp;
  uint8_t* prev = nullptr;
  for (uint32_t y = 0; y < h; ++y) {
    uint8_t* line = raw + static_cast<size_t>(y) * (row + 1);
    uint8_t ft = line[0];
    uint8_t* cur = line + 1;
    switch (ft) {
      case 0:
        break;
      case 1:
        for (size_t i = bpp; i < row; ++i) cur[i] = static_cast<uint8_t>(cur[i] + cur[i - bpp]);
        break;
      case 2:
        if (prev) for (size_t i = 0; i < row; ++i) cur[i] = static_cast<uint8_t>(cur[i] + prev[i]);
        break;
      case 3:
        for (size_t i = 0; i < row; ++i) {
          uint32_t a = i >= bpp ? cur[i - bpp] : 0;
          uint32_t b = prev ? prev[i] : 0;
          cur[i] = static_cast<uint8_t>(cur[i] + ((a + b) >> 1));
        }
        break;
      case 4:
        for (size_t i = 0; i < row; ++i) {
          uint8_t a = i >= bpp ? cur[i - bpp] : 0;
          uint8_t b = prev ? prev[i] : 0;
          uint8_t c = (prev && i >= bpp) ? prev[i - bpp] : 0;
          cur[i] = static_cast<uint8_t>(cur[i] + paeth(a, b, c));
        }
        break;
      default:
        return kErrFilter;
    }
    prev = cur;
  }
  return kOk;
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc) {
  uint32_t c = crc ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) c = kCrcTable.v[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

uint32_t adler32(const uint8_t* data, size_t len, uint32_t adler) {
  uint32_t a = adler & 0xFFFF, b = adler >> 16;
  for (size_t i = 0; i < len; ++i) {
    a = (a + data[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

Error inflate(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap, size_t* out_len,
              size_t* src_used) {
  Bits b;
  b.p = src;
  b.end = src + src_len;
  b.buf = 0;
  b.cnt = 0;
  b.overrun = false;
  Out o;
  o.dst = dst;
  o.cap = dst_cap;
  o.pos = 0;
  for (;;) {
    uint32_t last, type;
    if (!getbits(&b, 1, &last) || !getbits(&b, 2, &type)) return kErrInflate;
    Error e;
    if (type == 0) e = block_stored(&b, &o);
    else if (type == 1) e = block_fixed(&b, &o);
    else if (type == 2) e = block_dynamic(&b, &o);
    else return kErrInflate;
    if (e != kOk) return e;
    if (last) break;
  }
  // Неиспользованные биты последнего байта отбрасываются; целые байты возвращаются.
  size_t unused_bytes = static_cast<size_t>(b.cnt / 8);
  *out_len = o.pos;
  if (src_used) *src_used = static_cast<size_t>(b.p - src) - unused_bytes;
  return kOk;
}

Error zlib_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap,
                      size_t* out_len) {
  if (src_len < 6) return kErrZlib;
  uint8_t cmf = src[0], flg = src[1];
  if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7) return kErrZlib;
  if (((static_cast<uint32_t>(cmf) << 8) | flg) % 31 != 0) return kErrZlib;
  if (flg & 0x20) return kErrZlib;  // предустановленный словарь не поддерживается
  size_t used = 0;
  Error e = inflate(src + 2, src_len - 2, dst, dst_cap, out_len, &used);
  if (e != kOk) return e;
  size_t pos = 2 + used;
  if (src_len - pos != 4) return src_len - pos < 4 ? kErrAdler : kErrZlib;
  if (adler32(dst, *out_len) != be32(src + pos)) return kErrAdler;
  return kOk;
}

Error decode(const uint8_t* data, size_t size, Image* out) {
  memset(out, 0, sizeof(*out));
  static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (data == nullptr || size < 8 || memcmp(data, kSig, 8) != 0) return kErrSignature;
  if (size > kMaxFile) return kErrTooLarge;

  // Проход 1: структура чанков, CRC, IHDR, суммарный размер IDAT.
  uint32_t w = 0, h = 0, bpp = 0;
  size_t idat_total = 0;
  bool have_ihdr = false, have_iend = false, have_plte = false;
  int idat_state = 0;  // 0 — ещё не было, 1 — идут подряд, 2 — закончились
  size_t pos = 8;
  while (pos < size) {
    if (have_iend) return kErrOrder;  // данные после IEND
    if (size - pos < 12) return kErrChunk;
    uint32_t len = be32(data + pos);
    const uint8_t* type = data + pos + 4;
    if (len > 0x7FFFFFFFu || len > size - pos - 12) return kErrChunk;
    if (!type_valid(type)) return kErrChunk;
    const uint8_t* body = data + pos + 8;
    if (crc32(type, static_cast<size_t>(len) + 4) != be32(body + len)) return kErrCrc;
    bool is_idat = type_is(type, "IDAT");
    if (!have_ihdr && !type_is(type, "IHDR")) return kErrOrder;
    if (idat_state == 1 && !is_idat) idat_state = 2;
    if (type_is(type, "IHDR")) {
      if (have_ihdr) return kErrOrder;
      if (len != 13) return kErrHeader;
      w = be32(body);
      h = be32(body + 4);
      uint8_t depth = body[8], ctype = body[9], comp = body[10], filt = body[11], inter = body[12];
      if (w == 0 || h == 0 || w > kMaxSide || h > kMaxSide) return kErrDimensions;
      if (comp != 0 || filt != 0 || inter > 1) return kErrHeader;
      if (depth != 8 || (ctype != 2 && ctype != 6) || inter != 0) return kErrUnsupported;
      bpp = ctype == 6 ? 4 : 3;
      have_ihdr = true;
    } else if (is_idat) {
      if (idat_state == 2) return kErrOrder;
      idat_state = 1;
      idat_total += len;
    } else if (type_is(type, "IEND")) {
      if (len != 0) return kErrChunk;
      have_iend = true;
    } else if (type_is(type, "PLTE")) {
      // Для RGB/RGBA палитра — необязательная подсказка; допустима одна и до IDAT.
      if (have_plte || idat_state != 0 || len % 3 != 0 || len == 0 || len > 768) return kErrOrder;
      have_plte = true;
    } else if (type[0] >= 'A' && type[0] <= 'Z') {
      return kErrUnsupported;  // неизвестный критический чанк
    }
    pos += static_cast<size_t>(len) + 12;
  }
  if (!have_iend || idat_state == 0) return kErrOrder;

  // Проход 2: склейка IDAT, распаковка, фильтры.
  size_t raw_size = static_cast<size_t>(h) * (static_cast<size_t>(w) * bpp + 1);
  uint8_t* idat = static_cast<uint8_t*>(malloc(idat_total ? idat_total : 1));
  uint8_t* raw = static_cast<uint8_t*>(malloc(raw_size));
  uint8_t* rgba = static_cast<uint8_t*>(malloc(static_cast<size_t>(w) * h * 4));
  Error e = kOk;
  if (!idat || !raw || !rgba) e = kErrNoMemory;
  if (e == kOk) {
    size_t at = 0;
    pos = 8;
    while (pos < size) {
      uint32_t len = be32(data + pos);
      if (type_is(data + pos + 4, "IDAT")) {
        memcpy(idat + at, data + pos + 8, len);
        at += len;
      }
      pos += static_cast<size_t>(len) + 12;
    }
    size_t got = 0;
    e = zlib_decompress(idat, idat_total, raw, raw_size, &got);
    if (e == kOk && got != raw_size) e = kErrDataSize;
  }
  if (e == kOk) e = unfilter(raw, w, h, bpp);
  if (e == kOk) {
    size_t row = static_cast<size_t>(w) * bpp;
    for (uint32_t y = 0; y < h; ++y) {
      const uint8_t* src = raw + static_cast<size_t>(y) * (row + 1) + 1;
      uint8_t* dst = rgba + static_cast<size_t>(y) * w * 4;
      for (uint32_t x = 0; x < w; ++x) {
        dst[x * 4 + 0] = src[x * bpp + 0];
        dst[x * 4 + 1] = src[x * bpp + 1];
        dst[x * 4 + 2] = src[x * bpp + 2];
        dst[x * 4 + 3] = bpp == 4 ? src[x * bpp + 3] : 0xFF;
      }
    }
  }
  free(idat);
  free(raw);
  if (e != kOk) {
    free(rgba);
    return e;
  }
  out->w = w;
  out->h = h;
  out->rgba = rgba;
  return kOk;
}

void release(Image* img) {
  if (img == nullptr) return;
  free(img->rgba);
  img->rgba = nullptr;
  img->w = img->h = 0;
}

void rgba_to_argb32_premul(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t* out) {
  size_t n = static_cast<size_t>(w) * h;
  for (size_t i = 0; i < n; ++i) {
    uint32_t r = rgba[i * 4 + 0], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
    r = (r * a + 127) / 255;
    g = (g * a + 127) / 255;
    b = (b * a + 127) / 255;
    out[i] = (a << 24) | (r << 16) | (g << 8) | b;
  }
}

const char* error_name(Error e) {
  switch (e) {
    case kOk: return "ok";
    case kErrTooLarge: return "too_large";
    case kErrSignature: return "signature";
    case kErrChunk: return "chunk";
    case kErrCrc: return "crc";
    case kErrHeader: return "header";
    case kErrUnsupported: return "unsupported";
    case kErrDimensions: return "dimensions";
    case kErrOrder: return "order";
    case kErrNoMemory: return "no_memory";
    case kErrZlib: return "zlib";
    case kErrInflate: return "inflate";
    case kErrAdler: return "adler";
    case kErrDataSize: return "data_size";
    case kErrFilter: return "filter";
  }
  return "unknown";
}

}  // namespace png
}  // namespace dt
