// SPDX-License-Identifier: MIT
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/color.h"
#include "tests/test.h"

using namespace dt;
using namespace dt::color;

namespace {

Palette g_pal;
Engine g_eng;
Engine g_eng2;

void base(Palette* p) {
  memset(p, 0, sizeof(*p));
  p->schema = 1;
  p->tolerance = 6;
  p->fallback.pale_min = 0.55;
  p->fallback.bg_l_lo = 0.08;
  p->fallback.bg_l_hi = 0.2;
  p->fallback.min_contrast = 4.5;
  p->dup.max_dx = 3;
  p->dup.max_dy = 3;
  p->dup.ring = 256;
  p->ctx[kEditor].has_bg = true;
  p->ctx[kEditor].bg = 0x202028;
}

void add(Palette* p, Role r, uint32_t from, uint32_t to, uint8_t ctx) {
  Entry& e = p->map[r][p->count[r]++];
  e.from = from;
  e.to = to;
  e.ctx = ctx;
}

double hue_deg(uint32_t c) {
  double h, s, l;
  rgb_to_hsl(c, &h, &s, &l);
  return h * 360.0;
}

double light(uint32_t c) {
  double h, s, l;
  rgb_to_hsl(c, &h, &s, &l);
  return l;
}

uint32_t lcg(uint32_t* s) {
  *s = *s * 1664525u + 1013904223u;
  return *s >> 8;
}

bool init_ok(Engine* e, const Palette& p, Diag* d) { return e->init(p, d); }

// Схема владельца (requirements.md): категория -> цвет. Операторы, комментарии и
// препроцессор осветлены решением 2026-11-22 до контраста >= 4,5 к #202028;
// текущий, выбранный идентификатор и границы блока — фоны, они в kOwnerFill.
const uint32_t kOwnerText[] = {0xDC8500, 0xF8F8E8, 0xF05454, 0x4B90C7, 0xE86020};
const uint32_t kOwnerFill[] = {0x202028, 0xA01E64, 0x525278, 0x8465C5, 0xB4B4B4, 0x4133A3};

}  // namespace

TEST(color_hsl_roundtrip) {
  uint32_t s = 12345;
  for (int i = 0; i < 2000; ++i) {
    uint32_t c = lcg(&s) & 0xFFFFFF;
    double h, sat, l;
    rgb_to_hsl(c, &h, &sat, &l);
    CHECK_EQ(hsl_to_rgb(h, sat, l), c);
  }
}

TEST(color_lookup_order) {
  Diag d;
  base(&g_pal);
  add(&g_pal, kFill, 0xFFFFFF, 0x202028, kEditor);  // точная, контекст
  add(&g_pal, kFill, 0xFFFFFF, 0x2B2B2B, kCtxAny);  // точная, общая
  add(&g_pal, kFill, 0xF0F0F0, 0x303030, kCtxAny);  // для ближнего, общая
  add(&g_pal, kFill, 0xE0E0E0, 0x404040, kEditor);  // для ближнего, контекст
  add(&g_pal, kFill, 0xE6E6E6, 0x383838, kCtxAny);  // на том же расстоянии, общая
  CHECK(init_ok(&g_eng, g_pal, &d));
  Resolved r = g_eng.resolve(kFill, kEditor, 0xFFFFFF);
  CHECK(r.rgb == 0x202028 && r.how == kHowExactCtx);
  r = g_eng.resolve(kFill, kPanel, 0xFFFFFF);
  CHECK(r.rgb == 0x2B2B2B && r.how == kHowExactAny);
  r = g_eng.resolve(kFill, kPanel, 0xF2F2F2);  // до F0F0F0 — 2, до FFFFFF — 13
  CHECK(r.rgb == 0x303030 && r.how == kHowNearAny);
  r = g_eng.resolve(kFill, kEditor, 0xE3E3E3);  // E0E0E0 (ctx) и E6E6E6 (*) — по 3: контекст раньше
  CHECK(r.rgb == 0x404040 && r.how == kHowNearCtx);
  r = g_eng.resolve(kFill, kPanel, 0xE3E3E3);  // в panel записи editor не видны
  CHECK(r.rgb == 0x383838 && r.how == kHowNearAny);
  r = g_eng.resolve(kFill, kPanel, 0xD0D0D0);  // за допуском до всех from — правило
  CHECK(r.how == kHowFallback);
  // Текстовая карта не видит записей заливки.
  r = g_eng.resolve(kText, kEditor, 0xFFFFFF);
  CHECK(r.rgb == 0xFFFFFF && r.how == kHowKeep);
}

TEST(color_fallback_fill) {
  Diag d;
  base(&g_pal);
  CHECK(init_ok(&g_eng, g_pal, &d));
  // Белый -> светлота bg_l_lo.
  Resolved r = g_eng.resolve(kFill, kPanel, 0xFFFFFF);
  CHECK(r.how == kHowFallback);
  CHECK(fabs(light(r.rgb) - 0.08) < 0.003);
  // Бледно-жёлтый фон подсказки темнеет, оттенок сохраняется.
  r = g_eng.resolve(kFill, kPopup, 0xFFFFE1);
  CHECK(r.how == kHowFallback);
  CHECK(light(r.rgb) <= 0.2 + 0.003 && light(r.rgb) >= 0.08 - 0.003);
  CHECK(fabs(hue_deg(r.rgb) - 60.0) < 3.0);
  // Насыщенные и тёмные — без изменений.
  CHECK(g_eng.resolve(kFill, kPanel, 0xFF0000).how == kHowKeep);
  CHECK(g_eng.resolve(kFill, kPanel, 0x0078D7).how == kHowKeep);
  CHECK(g_eng.resolve(kFill, kPanel, 0x202028).how == kHowKeep);
  // Чем светлее исходный, тем темнее результат.
  CHECK(light(g_eng.resolve(kFill, kPanel, 0xF0F0F0).rgb) < light(g_eng.resolve(kFill, kPanel, 0xC0C0C0).rgb));
}

TEST(color_fallback_text) {
  Diag d;
  base(&g_pal);
  CHECK(init_ok(&g_eng, g_pal, &d));
  // Чёрный текст на фоне редактора светлеет до min_contrast.
  Resolved r = g_eng.resolve(kText, kEditor, 0x000000);
  CHECK(r.how == kHowFallback);
  CHECK(g_eng.contrast(r.rgb, 0x202028) >= 4.5);
  CHECK(g_eng.contrast(r.rgb, 0x202028) < 5.2);  // минимально достаточный, не белый
  // Синяя ссылка: оттенок сохраняется.
  r = g_eng.resolve(kText, kEditor, 0x0000FF);
  CHECK(r.how == kHowFallback);
  CHECK(g_eng.contrast(r.rgb, 0x202028) >= 4.5);
  CHECK(fabs(hue_deg(r.rgb) - 240.0) < 3.0);
  // Светлый текст не трогается — исходный цвет текста не путается с бледным фоном.
  CHECK(g_eng.resolve(kText, kEditor, 0xF0F0F0).how == kHowKeep);
  CHECK(g_eng.resolve(kText, kEditor, 0xFFFFFF).how == kHowKeep);
  // Нет фона контекста: серый со светлотой bg_l_hi.
  r = g_eng.resolve(kText, kHelp, 0x000000);
  uint32_t gray = hsl_to_rgb(0.0, 0.0, 0.2);
  CHECK(g_eng.contrast(r.rgb, gray) >= 4.5);
}

TEST(color_fallback_idempotent) {
  Diag d;
  base(&g_pal);
  g_pal.ctx[kPanel].has_bg = true;
  g_pal.ctx[kPanel].bg = 0x2B2B2B;
  CHECK(init_ok(&g_eng, g_pal, &d));
  uint32_t s = 777;
  int bad = 0;
  for (int i = 0; i < 3000; ++i) {
    uint32_t c = lcg(&s) & 0xFFFFFF;
    for (int role = 0; role < 2; ++role) {
      for (int ctx = 0; ctx < kCtxCount; ++ctx) {
        uint32_t once = g_eng.resolve(static_cast<Role>(role), static_cast<Ctx>(ctx), c).rgb;
        uint32_t twice = g_eng.resolve(static_cast<Role>(role), static_cast<Ctx>(ctx), once).rgb;
        if (once != twice) ++bad;
      }
    }
  }
  CHECK_EQ(bad, 0);
}

TEST(color_fixed_points) {
  Diag d;
  base(&g_pal);
  add(&g_pal, kText, 0x4585B8, 0x4585B8, kEditor);
  CHECK(init_ok(&g_eng, g_pal, &d));
  Resolved r = g_eng.resolve(kText, kEditor, 0x4585B8);
  CHECK(r.rgb == 0x4585B8 && r.how == kHowFixed);
  // Рядом с неподвижной точкой: цвет пользователя не сдвигается ни к ней, ни правилом.
  r = g_eng.resolve(kText, kEditor, 0x4787BA);
  CHECK(r.rgb == 0x4787BA && r.how == kHowFixed);
  // В другом контексте точки нет: правило осветляет (контраст 4.08 < 4.5).
  r = g_eng.resolve(kText, kHelp, 0x4585B8);
  CHECK(r.how == kHowFallback);
  // Без точки тот же цвет в редакторе перекрашивается.
  base(&g_pal);
  CHECK(init_ok(&g_eng2, g_pal, &d));
  CHECK(g_eng2.resolve(kText, kEditor, 0x4585B8).rgb != 0x4585B8);
}

TEST(color_conflicts) {
  Diag d;
  // Неподвижная точка и перекраска того же цвета в том же контексте.
  base(&g_pal);
  add(&g_pal, kText, 0xDC8500, 0xDC8500, kEditor);
  add(&g_pal, kText, 0xDC8500, 0x112233, kEditor);
  CHECK(!init_ok(&g_eng, g_pal, &d));
  CHECK_EQ(d.status, kErrConflict);
  CHECK(strstr(d.where, "map_text[") != nullptr);
  // Точный повтор записи — тоже конфликт.
  base(&g_pal);
  add(&g_pal, kFill, 0xFFFFFF, 0x202028, kEditor);
  add(&g_pal, kFill, 0xFFFFFF, 0x202028, kEditor);
  CHECK(!init_ok(&g_eng, g_pal, &d));
  CHECK_EQ(d.status, kErrConflict);
  // Цепочка в контексте: X -> T, T -> U. Ручной цвет T перекрасился бы второй раз.
  base(&g_pal);
  add(&g_pal, kText, 0x000000, 0xDC8500, kEditor);
  add(&g_pal, kText, 0xDC8500, 0xF8F8E8, kEditor);
  CHECK(!init_ok(&g_eng, g_pal, &d));
  CHECK_EQ(d.status, kErrConflict);
  CHECK(strstr(d.where, "map_text[0].to") != nullptr);
  // Цепочка через общую таблицу.
  base(&g_pal);
  add(&g_pal, kText, 0x000000, 0xDC8500, kEditor);
  add(&g_pal, kText, 0xDC8500, 0xF8F8E8, kCtxAny);
  CHECK(!init_ok(&g_eng, g_pal, &d));
  CHECK_EQ(d.status, kErrConflict);
  // Цель в допуске от чужого from.
  base(&g_pal);
  add(&g_pal, kText, 0x000000, 0xDC8500, kEditor);
  add(&g_pal, kText, 0xDA8302, 0xF8F8E8, kEditor);
  CHECK(!init_ok(&g_eng, g_pal, &d));
  CHECK_EQ(d.status, kErrConflict);
  // Цель, которую правило снова изменит (бледная заливка), — конфликт...
  base(&g_pal);
  add(&g_pal, kFill, 0x0000FF, 0xE0E0E0, kEditor);
  CHECK(!init_ok(&g_eng, g_pal, &d));
  CHECK_EQ(d.status, kErrConflict);
  // ...пока цель не объявлена неподвижной точкой.
  add(&g_pal, kFill, 0xE0E0E0, 0xE0E0E0, kEditor);
  CHECK(init_ok(&g_eng, g_pal, &d));
  CHECK_EQ(g_eng.resolve(kFill, kEditor, 0xE0E0E0).how, kHowFixed);
  // Тот же from в разных контекстах — не конфликт: контекст точнее общей таблицы.
  base(&g_pal);
  add(&g_pal, kFill, 0xFFFFFF, 0x202028, kEditor);
  add(&g_pal, kFill, 0xFFFFFF, 0x2B2B2B, kCtxAny);
  CHECK(init_ok(&g_eng, g_pal, &d));
}

TEST(color_owner_scheme_reapply) {
  // Повторное применение к уже тёмной схеме владельца: с палитрой по умолчанию
  // (неподвижные точки) каждый цвет схемы остаётся собой.
  char* text = nullptr;
  size_t len = 0;
  CHECK(t::read_file("themes/default/palette.json", &text, &len));
  if (text == nullptr) return;
  Diag d;
  CHECK(parse_palette(text, len, &g_pal, &d));
  free(text);
  CHECK(init_ok(&g_eng, g_pal, &d));
  for (uint32_t c : kOwnerText) {
    Resolved r = g_eng.resolve(kText, kEditor, c);
    CHECK_EQ(r.rgb, c);
    CHECK_EQ(g_eng.map(kText, kEditor, c), c);
    double out[3];
    CHECK(!g_eng.map_unit(kText, kEditor, ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0,
                          (c & 0xFF) / 255.0, out));
  }
  for (uint32_t c : kOwnerFill) {
    CHECK_EQ(g_eng.resolve(kFill, kEditor, c).rgb, c);
    CHECK_EQ(g_eng.map(kFill, kEditor, c), c);
  }
  // Повторно (схема уже применена) — то же самое.
  for (uint32_t c : kOwnerText) CHECK_EQ(g_eng.map(kText, kEditor, g_eng.map(kText, kEditor, c)), c);
  // Прежние цели владельца (ручная схема) и умолчание 1С для препроцессора
  // переходят в осветлённые.
  CHECK_EQ(g_eng.resolve(kText, kEditor, 0xC82626).rgb, 0xF05454u);
  CHECK_EQ(g_eng.resolve(kText, kEditor, 0xBA3E00).rgb, 0xE86020u);
  CHECK_EQ(g_eng.resolve(kText, kEditor, 0x963200).rgb, 0xE86020u);
  CHECK_EQ(g_eng.resolve(kText, kEditor, 0x008000).rgb, 0x4B90C7u);
  // Цели первого осветления (2026-11-22) переходят в текущие.
  CHECK_EQ(g_eng.resolve(kText, kEditor, 0xE04848).rgb, 0xF05454u);
  CHECK_EQ(g_eng.resolve(kText, kEditor, 0xD95A1A).rgb, 0xE86020u);
  CHECK_EQ(g_eng.resolve(kText, kEditor, 0x4585B8).rgb, 0x4B90C7u);

  // Без неподвижных точек одно правило испортило бы часть схемы: у операторов,
  // комментариев и препроцессора контраст к #202028 ниже min_contrast палитры;
  // фоны «текущего» (#8465C5) и «выбранного» (#B4B4B4) идентификатора правило
  // сочло бы бледными. #8465C5 и #4133A3 ниже как текст — только пример сдвига.
  Palette* bare = &g_pal;
  bare->count[kFill] = 0;
  bare->count[kText] = 0;
  CHECK(init_ok(&g_eng2, *bare, &d));
  CHECK_EQ(g_eng2.resolve(kText, kEditor, 0xDC8500).rgb, 0xDC8500u);
  CHECK_EQ(g_eng2.resolve(kText, kEditor, 0xF8F8E8).rgb, 0xF8F8E8u);
  CHECK_EQ(g_eng2.resolve(kText, kEditor, 0xB4B4B4).rgb, 0xB4B4B4u);
  CHECK(g_eng2.resolve(kText, kEditor, 0xF05454).rgb != 0xF05454u);
  CHECK(g_eng2.resolve(kText, kEditor, 0x4B90C7).rgb != 0x4B90C7u);
  CHECK(g_eng2.resolve(kText, kEditor, 0xE86020).rgb != 0xE86020u);
  CHECK(g_eng2.resolve(kText, kEditor, 0x8465C5).rgb != 0x8465C5u);
  CHECK(g_eng2.resolve(kText, kEditor, 0x4133A3).rgb != 0x4133A3u);
  CHECK_EQ(g_eng2.resolve(kFill, kEditor, 0x202028).rgb, 0x202028u);
  CHECK_EQ(g_eng2.resolve(kFill, kEditor, 0xA01E64).rgb, 0xA01E64u);
  CHECK_EQ(g_eng2.resolve(kFill, kEditor, 0x525278).rgb, 0x525278u);
  CHECK(g_eng2.resolve(kFill, kEditor, 0xB4B4B4).rgb != 0xB4B4B4u);
  CHECK(g_eng2.resolve(kFill, kEditor, 0x8465C5).rgb != 0x8465C5u);
}

TEST(color_cache_consistent) {
  Diag d;
  base(&g_pal);
  add(&g_pal, kFill, 0xFFFFFF, 0x202028, kEditor);
  add(&g_pal, kText, 0x000000, 0xF8F8E8, kEditor);
  CHECK(init_ok(&g_eng, g_pal, &d));
  uint32_t s = 99;
  int bad = 0;
  // Больше цветов, чем ячеек кэша: вытеснение не ломает результат.
  for (int pass = 0; pass < 2; ++pass) {
    s = 99;
    for (int i = 0; i < 10000; ++i) {
      uint32_t c = lcg(&s) & 0xFFFFFF;
      Role role = static_cast<Role>(i & 1);
      Ctx ctx = static_cast<Ctx>(i % kCtxCount);
      if (g_eng.map(role, ctx, c) != g_eng.resolve(role, ctx, c).rgb) ++bad;
    }
  }
  CHECK_EQ(bad, 0);
  // Ключ кэша различает роль и контекст.
  CHECK_EQ(g_eng.map(kFill, kEditor, 0xFFFFFF), 0x202028u);
  CHECK(g_eng.map(kText, kEditor, 0xFFFFFF) == 0xFFFFFFu);
  CHECK(g_eng.map(kFill, kPanel, 0xFFFFFF) != 0x202028u);
  CHECK_EQ(g_eng.map(kText, kEditor, 0x000000), 0xF8F8E8u);
}

TEST(color_map_unit) {
  Diag d;
  base(&g_pal);
  add(&g_pal, kFill, 0xFFFFFF, 0x202028, kEditor);
  add(&g_pal, kFill, 0x808080, 0x808080, kEditor);
  CHECK(init_ok(&g_eng, g_pal, &d));
  double out[3] = {-1, -1, -1};
  CHECK(g_eng.map_unit(kFill, kEditor, 1.0, 1.0, 1.0, out));
  CHECK(fabs(out[0] - 0x20 / 255.0) < 1e-9 && fabs(out[2] - 0x28 / 255.0) < 1e-9);
  // Неподвижная точка и немаппленный цвет — ложь: вызывающий отдаёт исходные значения.
  CHECK(!g_eng.map_unit(kFill, kEditor, 0.5019, 0.5019, 0.5019, out));
  CHECK(!g_eng.map_unit(kFill, kEditor, 0.9, 0.1, 0.1, out));
  volatile double neg = -1.0;
  double nan = sqrt(neg);
  CHECK(!g_eng.map_unit(kFill, kEditor, nan, 0.5, 0.5, out));
  CHECK(!g_eng.map_unit(kFill, kEditor, 1.5, 0.5, 0.5, out));
}

TEST(color_bg_too_light_rejected) {
  Diag d;
  base(&g_pal);
  g_pal.ctx[kHelp].has_bg = true;
  g_pal.ctx[kHelp].bg = 0xC0C0C0;  // белый текст не даст 4.5
  CHECK(!init_ok(&g_eng, g_pal, &d));
  CHECK_EQ(d.status, kErrRange);
}
