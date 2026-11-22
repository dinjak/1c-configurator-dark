// SPDX-License-Identifier: MIT
#include "core/color.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/json.h"

namespace dt {
namespace color {
namespace {

const char* const kCtxNames[kCtxCount] = {"editor", "panel", "popup", "help", "dialog", "gdi"};

void set_where(Diag* d, Status s, const char* fmt, const char* a = "", int i = -1, const char* b = "") {
  if (d == nullptr) return;
  d->status = s;
  if (i >= 0) snprintf(d->where, sizeof(d->where), fmt, a, i, b);
  else snprintf(d->where, sizeof(d->where), fmt, a, b);
}

bool parse_hex_color(const json::Value* v, uint32_t* out) {
  if (v == nullptr || v->type != json::kString || v->count != 7 || v->str[0] != '#') return false;
  uint32_t c = 0;
  for (int i = 1; i < 7; ++i) {
    char ch = v->str[i];
    uint32_t d;
    if (ch >= '0' && ch <= '9') d = static_cast<uint32_t>(ch - '0');
    else if (ch >= 'a' && ch <= 'f') d = static_cast<uint32_t>(ch - 'a' + 10);
    else if (ch >= 'A' && ch <= 'F') d = static_cast<uint32_t>(ch - 'A' + 10);
    else return false;
    c = (c << 4) | d;
  }
  *out = c;
  return true;
}

bool parse_ctx_name(const char* s, uint32_t len, uint8_t* out) {
  if (len == 1 && s[0] == '*') {
    *out = kCtxAny;
    return true;
  }
  for (uint8_t i = 0; i < kCtxCount; ++i) {
    if (strlen(kCtxNames[i]) == len && memcmp(kCtxNames[i], s, len) == 0) {
      *out = i;
      return true;
    }
  }
  return false;
}

bool as_int(const json::Value* v, long lo, long hi, long* out) {
  if (v == nullptr || v->type != json::kNumber) return false;
  double d = v->number;
  if (!(d >= static_cast<double>(lo) && d <= static_cast<double>(hi))) return false;
  long i = static_cast<long>(d);
  if (static_cast<double>(i) != d) return false;
  *out = i;
  return true;
}

bool as_real(const json::Value* v, double lo, double hi, double* out) {
  if (v == nullptr || v->type != json::kNumber) return false;
  if (!(v->number >= lo && v->number <= hi)) return false;
  *out = v->number;
  return true;
}

// Все ключи объекта из списка; обязательные помечены '!' в начале имени.
bool check_keys(const json::Value* obj, const char* const* keys, size_t n, const char* path, Diag* d) {
  for (const json::Value* m = obj->child; m != nullptr; m = m->next) {
    bool known = false;
    for (size_t i = 0; i < n; ++i) {
      const char* k = keys[i][0] == '!' ? keys[i] + 1 : keys[i];
      if (json::key_is(m, k)) known = true;
    }
    if (!known) {
      set_where(d, kErrUnknownKey, "%s: unknown key", path);
      return false;
    }
  }
  for (size_t i = 0; i < n; ++i) {
    if (keys[i][0] == '!' && json::find(obj, keys[i] + 1) == nullptr) {
      set_where(d, kErrMissing, "%s.%s", path, -1, keys[i] + 1);
      return false;
    }
  }
  return true;
}

bool parse_map(const json::Value* arr, const char* name, Palette* p, Role role, Diag* d) {
  if (arr->type != json::kArray) {
    set_where(d, kErrType, "%s: array expected", name);
    return false;
  }
  if (arr->count > kMaxEntries) {
    set_where(d, kErrTooMany, "%s: too many entries", name);
    return false;
  }
  static const char* const kKeys[] = {"!from", "!to", "!ctx"};
  int i = 0;
  for (const json::Value* e = arr->child; e != nullptr; e = e->next, ++i) {
    char path[48];
    snprintf(path, sizeof(path), "%s[%d]", name, i);
    if (e->type != json::kObject) {
      set_where(d, kErrType, "%s: object expected", path);
      return false;
    }
    if (!check_keys(e, kKeys, 3, path, d)) return false;
    Entry& out = p->map[role][p->count[role]];
    if (!parse_hex_color(json::find(e, "from"), &out.from)) {
      set_where(d, kErrColor, "%s.from", path);
      return false;
    }
    if (!parse_hex_color(json::find(e, "to"), &out.to)) {
      set_where(d, kErrColor, "%s.to", path);
      return false;
    }
    const json::Value* c = json::find(e, "ctx");
    if (c->type != json::kString || !parse_ctx_name(c->str, c->count, &out.ctx)) {
      set_where(d, kErrCtx, "%s.ctx", path);
      return false;
    }
    ++p->count[role];
  }
  return true;
}

bool parse_contexts(const json::Value* obj, Palette* p, Diag* d) {
  if (obj->type != json::kObject) {
    set_where(d, kErrType, "contexts: object expected");
    return false;
  }
  for (const json::Value* m = obj->child; m != nullptr; m = m->next) {
    uint8_t ci;
    if (!parse_ctx_name(m->key, m->key_len, &ci) || ci == kCtxAny) {
      set_where(d, kErrCtx, "contexts: unknown context");
      return false;
    }
    char path[48];
    snprintf(path, sizeof(path), "contexts.%s", kCtxNames[ci]);
    if (m->type != json::kObject) {
      set_where(d, kErrType, "%s: object expected", path);
      return false;
    }
    static const char* const kKeys[] = {"bg", "text"};
    if (!check_keys(m, kKeys, 2, path, d)) return false;
    CtxInfo& ctx = p->ctx[ci];
    if (const json::Value* v = json::find(m, "bg")) {
      if (!parse_hex_color(v, &ctx.bg)) {
        set_where(d, kErrColor, "%s.bg", path);
        return false;
      }
      ctx.has_bg = true;
    }
    if (const json::Value* v = json::find(m, "text")) {
      if (!parse_hex_color(v, &ctx.text)) {
        set_where(d, kErrColor, "%s.text", path);
        return false;
      }
      ctx.has_text = true;
    }
  }
  return true;
}

bool parse_syscolors(const json::Value* obj, Palette* p, Diag* d) {
  if (obj->type != json::kObject) {
    set_where(d, kErrType, "syscolors: object expected");
    return false;
  }
  for (const json::Value* m = obj->child; m != nullptr; m = m->next) {
    // Ключ — десятичный индекс без ведущих нулей.
    uint32_t idx = 0;
    bool ok = m->key_len >= 1 && m->key_len <= 2 && !(m->key_len == 2 && m->key[0] == '0');
    for (uint32_t i = 0; ok && i < m->key_len; ++i) {
      if (m->key[i] < '0' || m->key[i] > '9') ok = false;
      else idx = idx * 10 + static_cast<uint32_t>(m->key[i] - '0');
    }
    if (!ok || idx >= kSysColorCount) {
      set_where(d, kErrRange, "syscolors: bad index");
      return false;
    }
    if (!parse_hex_color(m, &p->sys[idx])) {
      set_where(d, kErrColor, "syscolors.%s", m->key);
      return false;
    }
    p->has_sys[idx] = true;
  }
  return true;
}

bool parse_fallback(const json::Value* obj, Palette* p, Diag* d) {
  if (obj->type != json::kObject) {
    set_where(d, kErrType, "fallback: object expected");
    return false;
  }
  static const char* const kKeys[] = {"!pale_min", "!bg_l", "!min_contrast"};
  if (!check_keys(obj, kKeys, 3, "fallback", d)) return false;
  Fallback& f = p->fallback;
  if (!as_real(json::find(obj, "pale_min"), 0.01, 1.0, &f.pale_min)) {
    set_where(d, kErrRange, "fallback.pale_min");
    return false;
  }
  const json::Value* bl = json::find(obj, "bg_l");
  if (bl->type != json::kArray || bl->count != 2 || !as_real(bl->child, 0.0, 1.0, &f.bg_l_lo) ||
      !as_real(bl->child->next, 0.0, 1.0, &f.bg_l_hi) || f.bg_l_lo > f.bg_l_hi ||
      f.bg_l_hi > f.pale_min - 0.01) {
    // Верх диапазона ниже pale_min с запасом на округление до 8 бит: затемнённая
    // заливка не должна снова считаться бледной.
    set_where(d, kErrRange, "fallback.bg_l");
    return false;
  }
  if (!as_real(json::find(obj, "min_contrast"), 1.0, 21.0, &f.min_contrast)) {
    set_where(d, kErrRange, "fallback.min_contrast");
    return false;
  }
  return true;
}

bool parse_dup(const json::Value* obj, Palette* p, Diag* d) {
  if (obj->type != json::kObject) {
    set_where(d, kErrType, "dup: object expected");
    return false;
  }
  static const char* const kKeys[] = {"!max_dx", "!max_dy", "!ring"};
  if (!check_keys(obj, kKeys, 3, "dup", d)) return false;
  long dx, dy, ring;
  if (!as_int(json::find(obj, "max_dx"), 0, 64, &dx)) {
    set_where(d, kErrRange, "dup.max_dx");
    return false;
  }
  if (!as_int(json::find(obj, "max_dy"), 0, 64, &dy)) {
    set_where(d, kErrRange, "dup.max_dy");
    return false;
  }
  if (!as_int(json::find(obj, "ring"), 16, 1024, &ring) || (ring & (ring - 1)) != 0) {
    set_where(d, kErrRange, "dup.ring");
    return false;
  }
  p->dup.max_dx = static_cast<int32_t>(dx);
  p->dup.max_dy = static_cast<int32_t>(dy);
  p->dup.ring = static_cast<uint32_t>(ring);
  return true;
}

uint32_t chebyshev(uint32_t a, uint32_t b) {
  uint32_t m = 0;
  for (int sh = 0; sh <= 16; sh += 8) {
    int x = static_cast<int>((a >> sh) & 0xFF) - static_cast<int>((b >> sh) & 0xFF);
    uint32_t v = static_cast<uint32_t>(x < 0 ? -x : x);
    if (v > m) m = v;
  }
  return m;
}

uint8_t unit_to_8(double v) {
  if (v <= 0.0) return 0;
  if (v >= 1.0) return 255;
  return static_cast<uint8_t>(v * 255.0 + 0.5);
}

double hue_to_rgb(double p, double q, double t) {
  if (t < 0.0) t += 1.0;
  if (t > 1.0) t -= 1.0;
  if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
  if (t < 0.5) return q;
  if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
  return p;
}

}  // namespace

// ---------------------------------------------------------------------------

void rgb_to_hsl(uint32_t c, double* h, double* s, double* l) {
  double r = ((c >> 16) & 0xFF) / 255.0, g = ((c >> 8) & 0xFF) / 255.0, b = (c & 0xFF) / 255.0;
  double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
  double mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
  *l = (mx + mn) / 2.0;
  if (mx == mn) {
    *h = 0.0;
    *s = 0.0;
    return;
  }
  double d = mx - mn;
  *s = *l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
  double hh;
  if (mx == r) hh = (g - b) / d + (g < b ? 6.0 : 0.0);
  else if (mx == g) hh = (b - r) / d + 2.0;
  else hh = (r - g) / d + 4.0;
  *h = hh / 6.0;
}

uint32_t hsl_to_rgb(double h, double s, double l) {
  if (l < 0.0) l = 0.0;
  if (l > 1.0) l = 1.0;
  if (s <= 0.0) {
    uint8_t v = unit_to_8(l);
    return rgb(v, v, v);
  }
  double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
  double p = 2.0 * l - q;
  return rgb(unit_to_8(hue_to_rgb(p, q, h + 1.0 / 3.0)), unit_to_8(hue_to_rgb(p, q, h)),
             unit_to_8(hue_to_rgb(p, q, h - 1.0 / 3.0)));
}

bool parse_palette(const char* text, size_t len, Palette* p, Diag* d) {
  memset(p, 0, sizeof(*p));
  if (d) memset(d, 0, sizeof(*d));
  json::Document doc;
  json::Error je = json::parse(text, len, &doc);
  if (je != json::kOk) {
    if (d) {
      d->status = kErrJson;
      d->json_error = static_cast<uint8_t>(je);
      d->json_offset = doc.error_offset;
      snprintf(d->where, sizeof(d->where), "json: %s", json::error_name(je));
    }
    return false;
  }
  bool ok = false;
  const json::Value* root = doc.root;
  do {
    if (root->type != json::kObject) {
      set_where(d, kErrSchema, "root: object expected");
      break;
    }
    static const char* const kKeys[] = {"!schema", "name", "!tolerance", "!contexts", "!map_fill",
                                        "!map_text", "syscolors", "!fallback", "!dup"};
    if (!check_keys(root, kKeys, sizeof(kKeys) / sizeof(kKeys[0]), "root", d)) break;
    long schema;
    if (!as_int(json::find(root, "schema"), 1, 1, &schema)) {
      set_where(d, kErrSchema, "schema: 1 expected");
      break;
    }
    p->schema = static_cast<uint32_t>(schema);
    if (const json::Value* n = json::find(root, "name")) {
      if (n->type != json::kString || n->count >= sizeof(p->name)) {
        set_where(d, kErrType, "name");
        break;
      }
      memcpy(p->name, n->str, n->count + 1);
    }
    long tol;
    if (!as_int(json::find(root, "tolerance"), 0, kMaxTolerance, &tol)) {
      set_where(d, kErrRange, "tolerance");
      break;
    }
    p->tolerance = static_cast<uint32_t>(tol);
    if (!parse_contexts(json::find(root, "contexts"), p, d)) break;
    if (!parse_map(json::find(root, "map_fill"), "map_fill", p, kFill, d)) break;
    if (!parse_map(json::find(root, "map_text"), "map_text", p, kText, d)) break;
    if (const json::Value* s = json::find(root, "syscolors")) {
      if (!parse_syscolors(s, p, d)) break;
    }
    if (!parse_fallback(json::find(root, "fallback"), p, d)) break;
    if (!parse_dup(json::find(root, "dup"), p, d)) break;
    ok = true;
  } while (false);
  json::release(&doc);
  if (!ok) {
    Diag keep;
    memset(&keep, 0, sizeof(keep));
    if (d) keep = *d;
    memset(p, 0, sizeof(*p));
    if (d) *d = keep;
    return false;
  }
  if (d) d->status = kOk;
  return true;
}

const char* status_name(Status s) {
  switch (s) {
    case kOk: return "ok";
    case kErrJson: return "json";
    case kErrSchema: return "schema";
    case kErrType: return "type";
    case kErrMissing: return "missing";
    case kErrUnknownKey: return "unknown_key";
    case kErrColor: return "color";
    case kErrCtx: return "ctx";
    case kErrRange: return "range";
    case kErrTooMany: return "too_many";
    case kErrConflict: return "conflict";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------

double Engine::luminance(uint32_t c) const {
  return 0.2126 * lin_[(c >> 16) & 0xFF] + 0.7152 * lin_[(c >> 8) & 0xFF] + 0.0722 * lin_[c & 0xFF];
}

double Engine::contrast(uint32_t a, uint32_t b) const {
  double la = luminance(a), lb = luminance(b);
  if (la < lb) {
    double t = la;
    la = lb;
    lb = t;
  }
  return (la + 0.05) / (lb + 0.05);
}

bool Engine::init(const Palette& p, Diag* d) {
  if (d) memset(d, 0, sizeof(*d));
  memcpy(&pal_, &p, sizeof(pal_));
  for (int i = 0; i < 256; ++i) {
    double c = i / 255.0;
    lin_[i] = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
  }
  for (uint32_t i = 0; i < kCacheSize; ++i) cache_[i].store(0, std::memory_order_relaxed);

  // Фон контекста должен позволять довести текст до min_contrast осветлением.
  const double need = pal_.fallback.min_contrast;
  if (contrast(0xFFFFFF, hsl_to_rgb(0.0, 0.0, pal_.fallback.bg_l_hi)) < need) {
    set_where(d, kErrRange, "fallback.bg_l: white text below min_contrast");
    return false;
  }
  for (uint32_t c = 0; c < kCtxCount; ++c) {
    if (pal_.ctx[c].has_bg && contrast(0xFFFFFF, pal_.ctx[c].bg) < need) {
      set_where(d, kErrRange, "contexts.%s.bg: too light for min_contrast", kCtxNames[c]);
      return false;
    }
  }

  static const char* const kMapNames[kRoleCount] = {"map_fill", "map_text"};
  for (uint32_t r = 0; r < kRoleCount; ++r) {
    uint32_t n = pal_.count[r];
    for (uint32_t i = 0; i < n; ++i) {
      const Entry& e = pal_.map[r][i];
      uint32_t slot = e.ctx == kCtxAny ? kCtxCount : e.ctx;
      Key k;
      k.key = (slot << 24) | (e.from & 0xFFFFFF);
      k.to = e.to;
      // Вставка с сортировкой: карты маленькие, init вне горячего пути.
      uint32_t j = i;
      while (j > 0 && sorted_[r][j - 1].key > k.key) {
        sorted_[r][j] = sorted_[r][j - 1];
        --j;
      }
      sorted_[r][j] = k;
    }
    nsorted_[r] = n;
    for (uint32_t i = 1; i < n; ++i) {
      if (sorted_[r][i].key == sorted_[r][i - 1].key) {
        // Один from в одном контексте дважды: либо повтор, либо две разные цели.
        for (uint32_t k = 0; k < n; ++k) {
          const Entry& e = pal_.map[r][k];
          uint32_t slot = e.ctx == kCtxAny ? kCtxCount : e.ctx;
          if (((slot << 24) | e.from) == sorted_[r][i].key) {
            set_where(d, kErrConflict, "%s[%d]: duplicate from", kMapNames[r], static_cast<int>(k));
            break;
          }
        }
        return false;
      }
    }
  }

  // Неподвижность целей: цвет, в который палитра переводит, при повторном
  // применении (ручная настройка пользователя уже в схеме) не меняется.
  for (uint32_t r = 0; r < kRoleCount; ++r) {
    for (uint32_t i = 0; i < pal_.count[r]; ++i) {
      const Entry& e = pal_.map[r][i];
      for (uint32_t c = 0; c < kCtxCount; ++c) {
        if (e.ctx != kCtxAny && e.ctx != c) continue;
        Resolved res = resolve(static_cast<Role>(r), static_cast<Ctx>(c), e.to);
        if (res.rgb != e.to) {
          if (d) {
            d->status = kErrConflict;
            snprintf(d->where, sizeof(d->where), "%s[%d].to recolored again in %s", kMapNames[r],
                     static_cast<int>(i), kCtxNames[c]);
          }
          return false;
        }
      }
    }
  }
  return true;
}

bool Engine::lookup_exact(Role role, uint32_t slot, uint32_t c, uint32_t* to) const {
  uint32_t key = (slot << 24) | (c & 0xFFFFFF);
  uint32_t lo = 0, hi = nsorted_[role];
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (sorted_[role][mid].key < key) lo = mid + 1;
    else hi = mid;
  }
  if (lo < nsorted_[role] && sorted_[role][lo].key == key) {
    *to = sorted_[role][lo].to;
    return true;
  }
  return false;
}

bool Engine::lookup_near(Role role, Ctx ctx, uint32_t c, Resolved* out) const {
  // Ранг: расстояние, затем запись контекста раньше общей, затем неподвижная
  // точка раньше обычной, затем порядок в файле.
  uint32_t best = 0xFFFFFFFFu;
  const Entry* hit = nullptr;
  bool hit_ctx = false;
  for (uint32_t i = 0; i < pal_.count[role]; ++i) {
    const Entry& e = pal_.map[role][i];
    bool is_ctx = e.ctx != kCtxAny;
    if (is_ctx && e.ctx != ctx) continue;
    uint32_t dist = chebyshev(e.from, c);
    if (dist > pal_.tolerance) continue;
    uint32_t rank = (dist << 2) | (is_ctx ? 0u : 2u) | (e.from == e.to ? 0u : 1u);
    if (rank < best) {
      best = rank;
      hit = &e;
      hit_ctx = is_ctx;
    }
  }
  if (hit == nullptr) return false;
  if (hit->from == hit->to) {
    out->rgb = c;  // рядом с неподвижной точкой: цвет пользователя не трогаем
    out->how = kHowFixed;
  } else {
    out->rgb = hit->to;
    out->how = hit_ctx ? kHowNearCtx : kHowNearAny;
  }
  return true;
}

uint32_t Engine::fallback(Role role, Ctx ctx, uint32_t c) const {
  const Fallback& f = pal_.fallback;
  double h, s, l;
  rgb_to_hsl(c, &h, &s, &l);
  if (role == kFill) {
    // Бледная заливка темнеет, оттенок и насыщенность HSL сохраняются:
    // светлота [pale_min, 1] линейно и обратно ложится в [bg_l_hi, bg_l_lo]
    // (белый -> bg_l_lo, самый тёмный из бледных -> bg_l_hi).
    if (l < f.pale_min) return c;
    double t = f.pale_min < 1.0 ? (1.0 - l) / (1.0 - f.pale_min) : 0.0;
    double nl = f.bg_l_lo + t * (f.bg_l_hi - f.bg_l_lo);
    return hsl_to_rgb(h, s, nl);
  }
  // Текст светлеет до min_contrast к фону контекста, оттенок сохраняется.
  // Нет фона контекста — серый со светлотой bg_l_hi (худший случай заливки).
  uint32_t bg = (ctx < kCtxCount && pal_.ctx[ctx].has_bg) ? pal_.ctx[ctx].bg : hsl_to_rgb(0.0, 0.0, f.bg_l_hi);
  if (contrast(c, bg) >= f.min_contrast) return c;
  // Яркость каналов HSL не убывает по светлоте: предикат «контраст достаточен»
  // монотонен на [l, 1] (провал у фона лежит слева от первого успеха) — бисекция.
  double lo = l, hi = 1.0;
  uint32_t best = 0xFFFFFF;
  for (int it = 0; it < 40; ++it) {
    double mid = (lo + hi) / 2.0;
    uint32_t cand = hsl_to_rgb(h, s, mid);
    if (contrast(cand, bg) >= f.min_contrast) {
      hi = mid;
      best = cand;
    } else {
      lo = mid;
    }
  }
  return best;
}

Resolved Engine::resolve(Role role, Ctx ctx, uint32_t c) const {
  c &= 0xFFFFFF;
  Resolved r;
  uint32_t to;
  if (ctx < kCtxCount && lookup_exact(role, ctx, c, &to)) {
    r.rgb = to;
    r.how = to == c ? kHowFixed : kHowExactCtx;
    return r;
  }
  if (lookup_exact(role, kCtxCount, c, &to)) {
    r.rgb = to;
    r.how = to == c ? kHowFixed : kHowExactAny;
    return r;
  }
  if (pal_.tolerance > 0 && lookup_near(role, ctx, c, &r)) return r;
  r.rgb = fallback(role, ctx, c);
  r.how = r.rgb == c ? kHowKeep : kHowFallback;
  return r;
}

uint32_t Engine::map(Role role, Ctx ctx, uint32_t c) {
  c &= 0xFFFFFF;
  uint32_t cslot = ctx < kCtxCount ? ctx : 7u;
  uint32_t key = (static_cast<uint32_t>(role & 1) << 27) | (cslot << 24) | c;
  uint32_t h = (key * 2654435761u) >> 20;  // 12 бит индекса
  const uint32_t mask = kCacheSize - 1;
  uint32_t empty = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < kCacheProbe; ++i) {
    uint32_t idx = (h + i) & mask;
    uint64_t e = cache_[idx].load(std::memory_order_relaxed);
    if ((e >> 63) == 0) {
      empty = idx;
      break;
    }
    if (static_cast<uint32_t>((e >> 32) & 0x0FFFFFFF) == key) return static_cast<uint32_t>(e & 0xFFFFFF);
  }
  uint32_t v = resolve(role, ctx, c).rgb;
  uint64_t e = (1ull << 63) | (static_cast<uint64_t>(key) << 32) | v;
  // Одна атомарная 64-битная запись: читатель видит либо старую, либо новую пару.
  cache_[empty != 0xFFFFFFFFu ? empty : (h & mask)].store(e, std::memory_order_relaxed);
  return v;
}

bool Engine::map_unit(Role role, Ctx ctx, double r, double g, double b, double out[3]) {
  if (!(r >= 0.0 && r <= 1.0) || !(g >= 0.0 && g <= 1.0) || !(b >= 0.0 && b <= 1.0)) return false;
  uint32_t q = rgb(unit_to_8(r), unit_to_8(g), unit_to_8(b));
  uint32_t m = map(role, ctx, q);
  if (m == q) return false;
  out[0] = ((m >> 16) & 0xFF) / 255.0;
  out[1] = ((m >> 8) & 0xFF) / 255.0;
  out[2] = (m & 0xFF) / 255.0;
  return true;
}

bool Engine::syscolor(uint32_t index, uint32_t* out) const {
  if (index >= kSysColorCount || !pal_.has_sys[index]) return false;
  *out = pal_.sys[index];
  return true;
}

}  // namespace color
}  // namespace dt
