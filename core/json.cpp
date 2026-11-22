// SPDX-License-Identifier: MIT
#include "core/json.h"

#include <stdlib.h>
#include <string.h>

namespace dt {
namespace json {
namespace {

struct Parser {
  const uint8_t* begin;
  const uint8_t* p;
  const uint8_t* end;
  Value* nodes;
  uint32_t node_cap;
  uint32_t node_used;
  char* sbuf;
  size_t scap;
  size_t sused;
  Error err;
  const uint8_t* err_pos;
};

bool fail(Parser* ps, Error e, const uint8_t* at) {
  if (ps->err == kOk) {
    ps->err = e;
    ps->err_pos = at;
  }
  return false;
}

void skip_ws(Parser* ps) {
  while (ps->p < ps->end) {
    uint8_t c = *ps->p;
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
    ++ps->p;
  }
}

Value* new_node(Parser* ps) {
  if (ps->node_used >= ps->node_cap) {
    fail(ps, kErrTooMany, ps->p);
    return nullptr;
  }
  Value* v = &ps->nodes[ps->node_used++];
  memset(v, 0, sizeof(*v));
  return v;
}

int hex_digit(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool read_hex4(Parser* ps, uint32_t* out) {
  if (ps->end - ps->p < 4) return fail(ps, kErrString, ps->p);
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    int d = hex_digit(ps->p[i]);
    if (d < 0) return fail(ps, kErrString, ps->p + i);
    v = (v << 4) | static_cast<uint32_t>(d);
  }
  ps->p += 4;
  *out = v;
  return true;
}

// Длина корректной последовательности UTF-8 с первого байта (≥ 0x80), 0 — ошибка.
size_t utf8_seq(const uint8_t* p, const uint8_t* end) {
  uint8_t c = p[0];
  size_t n;
  uint8_t lo = 0x80, hi = 0xBF;
  if (c >= 0xC2 && c <= 0xDF) {
    n = 2;
  } else if (c >= 0xE0 && c <= 0xEF) {
    n = 3;
    if (c == 0xE0) lo = 0xA0;
    if (c == 0xED) hi = 0x9F;  // суррогаты запрещены
  } else if (c >= 0xF0 && c <= 0xF4) {
    n = 4;
    if (c == 0xF0) lo = 0x90;
    if (c == 0xF4) hi = 0x8F;
  } else {
    return 0;
  }
  if (static_cast<size_t>(end - p) < n) return 0;
  if (p[1] < lo || p[1] > hi) return 0;
  for (size_t i = 2; i < n; ++i) {
    if (p[i] < 0x80 || p[i] > 0xBF) return 0;
  }
  return n;
}

void put_utf8(char* out, size_t* pos, uint32_t cp) {
  size_t i = *pos;
  if (cp < 0x80) {
    out[i++] = static_cast<char>(cp);
  } else if (cp < 0x800) {
    out[i++] = static_cast<char>(0xC0 | (cp >> 6));
    out[i++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out[i++] = static_cast<char>(0xE0 | (cp >> 12));
    out[i++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[i++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out[i++] = static_cast<char>(0xF0 | (cp >> 18));
    out[i++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[i++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[i++] = static_cast<char>(0x80 | (cp & 0x3F));
  }
  *pos = i;
}

// Строка: ps->p на открывающей кавычке. Результат — в sbuf с нулём в конце.
bool parse_string(Parser* ps, const char** out, uint32_t* out_len) {
  ++ps->p;
  // Декодированная строка не длиннее исходной: \uXXXX (6 байт) -> ≤ 3 байта,
  // пара суррогатов (12 байт) -> 4 байта. Запас sbuf рассчитан в parse().
  char* dst = ps->sbuf + ps->sused;
  size_t n = 0;
  for (;;) {
    if (ps->p >= ps->end) return fail(ps, kErrString, ps->p);
    uint8_t c = *ps->p;
    if (c == '"') {
      ++ps->p;
      break;
    }
    if (c < 0x20) return fail(ps, kErrString, ps->p);
    if (c == '\\') {
      const uint8_t* esc = ps->p;
      ++ps->p;
      if (ps->p >= ps->end) return fail(ps, kErrString, esc);
      uint8_t e = *ps->p++;
      switch (e) {
        case '"': dst[n++] = '"'; break;
        case '\\': dst[n++] = '\\'; break;
        case '/': dst[n++] = '/'; break;
        case 'b': dst[n++] = '\b'; break;
        case 'f': dst[n++] = '\f'; break;
        case 'n': dst[n++] = '\n'; break;
        case 'r': dst[n++] = '\r'; break;
        case 't': dst[n++] = '\t'; break;
        case 'u': {
          uint32_t cp;
          if (!read_hex4(ps, &cp)) return false;
          if (cp >= 0xDC00 && cp <= 0xDFFF) return fail(ps, kErrString, esc);
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (ps->end - ps->p < 6 || ps->p[0] != '\\' || ps->p[1] != 'u') {
              return fail(ps, kErrString, esc);
            }
            ps->p += 2;
            uint32_t lo;
            if (!read_hex4(ps, &lo)) return false;
            if (lo < 0xDC00 || lo > 0xDFFF) return fail(ps, kErrString, esc);
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          if (cp == 0) return fail(ps, kErrString, esc);  // нуль внутри строки не нужен
          put_utf8(dst, &n, cp);
          break;
        }
        default:
          return fail(ps, kErrString, esc);
      }
      continue;
    }
    if (c < 0x80) {
      dst[n++] = static_cast<char>(c);
      ++ps->p;
      continue;
    }
    size_t k = utf8_seq(ps->p, ps->end);
    if (k == 0) return fail(ps, kErrUtf8, ps->p);
    memcpy(dst + n, ps->p, k);
    n += k;
    ps->p += k;
  }
  if (n > 0xFFFFFFFFu) return fail(ps, kErrTooLarge, ps->p);
  dst[n] = 0;
  ps->sused += n + 1;
  *out = dst;
  *out_len = static_cast<uint32_t>(n);
  return true;
}

bool parse_number(Parser* ps, double* out) {
  const uint8_t* start = ps->p;
  bool neg = false;
  if (*ps->p == '-') {
    neg = true;
    ++ps->p;
  }
  if (ps->p >= ps->end) return fail(ps, kErrNumber, start);
  uint64_t mant = 0;
  int digits = 0;
  long exp10 = 0;
  if (*ps->p == '0') {
    ++ps->p;
    if (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') return fail(ps, kErrNumber, start);
  } else if (*ps->p >= '1' && *ps->p <= '9') {
    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
      if (digits < 19) {
        mant = mant * 10 + static_cast<uint64_t>(*ps->p - '0');
        ++digits;
      } else {
        ++exp10;
      }
      ++ps->p;
    }
  } else {
    return fail(ps, kErrNumber, start);
  }
  if (ps->p < ps->end && *ps->p == '.') {
    ++ps->p;
    if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') return fail(ps, kErrNumber, start);
    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
      uint32_t d = static_cast<uint32_t>(*ps->p - '0');
      if (mant == 0 && d == 0) {
        --exp10;  // ведущие нули дробной части значащими не считаются
      } else if (digits < 19) {
        mant = mant * 10 + d;
        ++digits;
        --exp10;
      }
      ++ps->p;
    }
  }
  if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
    ++ps->p;
    bool eneg = false;
    if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) {
      eneg = *ps->p == '-';
      ++ps->p;
    }
    if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') return fail(ps, kErrNumber, start);
    long e = 0;
    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
      if (e < 100000) e = e * 10 + (*ps->p - '0');
      ++ps->p;
    }
    exp10 += eneg ? -e : e;
  }
  double v = static_cast<double>(mant);
  if (mant != 0) {
    static const double kPow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    if (exp10 > 400) return fail(ps, kErrNumber, start);
    if (exp10 < -400) {
      v = 0.0;
    } else {
      long e = exp10 < 0 ? -exp10 : exp10;
      while (e > 0) {
        long step = e > 22 ? 22 : e;
        if (exp10 > 0) v *= kPow10[step];
        else v /= kPow10[step];
        e -= step;
      }
    }
    if (v > 1.7976931348623157e308) return fail(ps, kErrNumber, start);
  }
  *out = neg ? -v : v;
  return true;
}

bool match_literal(Parser* ps, const char* lit) {
  size_t n = strlen(lit);
  if (static_cast<size_t>(ps->end - ps->p) < n || memcmp(ps->p, lit, n) != 0) {
    return fail(ps, kErrSyntax, ps->p);
  }
  ps->p += n;
  return true;
}

bool parse_value(Parser* ps, Value* v, uint32_t depth);

bool parse_container(Parser* ps, Value* v, uint32_t depth, bool object) {
  if (depth >= kMaxDepth) return fail(ps, kErrDepth, ps->p);
  v->type = object ? kObject : kArray;
  const uint8_t close = object ? '}' : ']';
  ++ps->p;
  skip_ws(ps);
  if (ps->p < ps->end && *ps->p == close) {
    ++ps->p;
    return true;
  }
  Value* last = nullptr;
  for (;;) {
    skip_ws(ps);
    if (ps->p >= ps->end) return fail(ps, kErrSyntax, ps->p);
    if (v->count >= kMaxMembers) return fail(ps, kErrTooMany, ps->p);
    const char* key = nullptr;
    uint32_t key_len = 0;
    const uint8_t* key_pos = ps->p;
    if (object) {
      if (*ps->p != '"') return fail(ps, kErrSyntax, ps->p);
      if (!parse_string(ps, &key, &key_len)) return false;
      for (const Value* m = v->child; m != nullptr; m = m->next) {
        if (m->key_len == key_len && memcmp(m->key, key, key_len) == 0) {
          return fail(ps, kErrDuplicateKey, key_pos);
        }
      }
      skip_ws(ps);
      if (ps->p >= ps->end || *ps->p != ':') return fail(ps, kErrSyntax, ps->p);
      ++ps->p;
      skip_ws(ps);
    }
    Value* item = new_node(ps);
    if (item == nullptr) return false;
    if (!parse_value(ps, item, depth + 1)) return false;
    item->key = key;
    item->key_len = key_len;
    if (last == nullptr) v->child = item;
    else last->next = item;
    last = item;
    ++v->count;
    skip_ws(ps);
    if (ps->p >= ps->end) return fail(ps, kErrSyntax, ps->p);
    if (*ps->p == ',') {
      ++ps->p;
      continue;  // хвостовая запятая отсекается: дальше обязан быть элемент
    }
    if (*ps->p == close) {
      ++ps->p;
      return true;
    }
    return fail(ps, kErrSyntax, ps->p);
  }
}

bool parse_value(Parser* ps, Value* v, uint32_t depth) {
  skip_ws(ps);
  if (ps->p >= ps->end) return fail(ps, kErrSyntax, ps->p);
  switch (*ps->p) {
    case '{':
      return parse_container(ps, v, depth, true);
    case '[':
      return parse_container(ps, v, depth, false);
    case '"': {
      v->type = kString;
      return parse_string(ps, &v->str, &v->count);
    }
    case 't':
      v->type = kBool;
      v->boolean = true;
      return match_literal(ps, "true");
    case 'f':
      v->type = kBool;
      v->boolean = false;
      return match_literal(ps, "false");
    case 'n':
      v->type = kNull;
      return match_literal(ps, "null");
    default:
      if (*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9')) {
        v->type = kNumber;
        return parse_number(ps, &v->number);
      }
      return fail(ps, kErrSyntax, ps->p);
  }
}

}  // namespace

Error parse(const char* text, size_t len, Document* doc) {
  memset(doc, 0, sizeof(*doc));
  if (text == nullptr && len != 0) {
    doc->error = kErrSyntax;
    return doc->error;
  }
  if (len > kMaxInput) {
    doc->error = kErrTooLarge;
    return doc->error;
  }
  // Каждый узел занимает ≥ 1 байт текста; строки после декодирования не длиннее
  // исходных, плюс нуль на каждую (строк ≤ len / 2).
  uint32_t node_cap = static_cast<uint32_t>(len < kMaxNodes ? len + 1 : kMaxNodes);
  size_t scap = len + len / 2 + 16;
  size_t bytes = static_cast<size_t>(node_cap) * sizeof(Value) + scap;
  void* mem = malloc(bytes);
  if (mem == nullptr) {
    doc->error = kErrNoMemory;
    return doc->error;
  }
  Parser ps;
  memset(&ps, 0, sizeof(ps));
  ps.begin = reinterpret_cast<const uint8_t*>(text);
  ps.p = ps.begin;
  ps.end = ps.begin + len;
  ps.nodes = static_cast<Value*>(mem);
  ps.node_cap = node_cap;
  ps.sbuf = static_cast<char*>(mem) + static_cast<size_t>(node_cap) * sizeof(Value);
  ps.scap = scap;
  if (len >= 3 && ps.p[0] == 0xEF && ps.p[1] == 0xBB && ps.p[2] == 0xBF) ps.p += 3;

  Value* root = new_node(&ps);
  bool ok = root != nullptr && parse_value(&ps, root, 0);
  if (ok) {
    skip_ws(&ps);
    if (ps.p != ps.end) ok = fail(&ps, kErrTrailing, ps.p);
  }
  if (!ok) {
    free(mem);
    doc->error = ps.err == kOk ? kErrSyntax : ps.err;
    const uint8_t* at = ps.err_pos != nullptr ? ps.err_pos : ps.p;
    doc->error_offset = static_cast<uint32_t>(at - ps.begin);
    return doc->error;
  }
  doc->root = root;
  doc->mem = mem;
  doc->error = kOk;
  return kOk;
}

void release(Document* doc) {
  if (doc == nullptr) return;
  free(doc->mem);
  doc->mem = nullptr;
  doc->root = nullptr;
}

bool key_is(const Value* member, const char* key) {
  size_t n = strlen(key);
  return member != nullptr && member->key != nullptr && member->key_len == n &&
         memcmp(member->key, key, n) == 0;
}

const Value* find(const Value* object, const char* key) {
  if (object == nullptr || object->type != kObject) return nullptr;
  for (const Value* m = object->child; m != nullptr; m = m->next) {
    if (key_is(m, key)) return m;
  }
  return nullptr;
}

const char* error_name(Error e) {
  switch (e) {
    case kOk: return "ok";
    case kErrTooLarge: return "too_large";
    case kErrNoMemory: return "no_memory";
    case kErrSyntax: return "syntax";
    case kErrDepth: return "depth";
    case kErrTooMany: return "too_many";
    case kErrString: return "string";
    case kErrUtf8: return "utf8";
    case kErrNumber: return "number";
    case kErrDuplicateKey: return "duplicate_key";
    case kErrTrailing: return "trailing";
  }
  return "unknown";
}

}  // namespace json
}  // namespace dt
