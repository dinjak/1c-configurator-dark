// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/color.h"
#include "tests/test.h"

using namespace dt;
using namespace dt::color;

namespace {

Palette g_p;
Engine g_e;

const char kHead[] = "{\"schema\":1,\"tolerance\":6,\"contexts\":{\"editor\":{\"bg\":\"#202028\"}},";
const char kTail[] =
    "\"fallback\":{\"pale_min\":0.55,\"bg_l\":[0.08,0.2],\"min_contrast\":4.5},"
    "\"dup\":{\"max_dx\":3,\"max_dy\":3,\"ring\":256}}";

// Палитра из головы, середины (карты и прочее) и хвоста.
Status parse_mid(const char* mid, Diag* d) {
  char buf[2048];
  snprintf(buf, sizeof(buf), "%s%s%s", kHead, mid, kTail);
  return parse_palette(buf, strlen(buf), &g_p, d) ? kOk : d->status;
}

Status parse_full(const char* text, Diag* d) {
  return parse_palette(text, strlen(text), &g_p, d) ? kOk : d->status;
}

const char kMaps[] = "\"map_fill\":[],\"map_text\":[],";

}  // namespace

TEST(palette_default_file) {
  char* text = nullptr;
  size_t len = 0;
  CHECK(t::read_file("themes/default/palette.json", &text, &len));
  if (text == nullptr) return;
  Diag d;
  bool ok = parse_palette(text, len, &g_p, &d);
  if (!ok) printf("  palette.json: %s at %s\n", status_name(d.status), d.where);
  CHECK(ok);
  free(text);
  CHECK_EQ(g_p.schema, 1u);
  CHECK(strcmp(g_p.name, "default") == 0);
  CHECK_EQ(g_p.tolerance, 6u);
  CHECK(g_p.ctx[kEditor].has_bg && g_p.ctx[kEditor].bg == 0x202028);
  CHECK(g_p.ctx[kEditor].has_text && g_p.ctx[kEditor].text == 0xF8F8E8);
  // Палитра M3a (дизайнер, принята владельцем 2026-11-22): фоны слоёв.
  CHECK(g_p.ctx[kPanel].has_bg && g_p.ctx[kPanel].bg == 0x30303C);
  CHECK(g_p.ctx[kPopup].has_bg && g_p.ctx[kPopup].bg == 0x2B2B36);
  CHECK(g_p.ctx[kHelp].has_bg && g_p.ctx[kHelp].bg == 0x24242C);
  CHECK(g_p.ctx[kDialog].has_bg && g_p.ctx[kDialog].bg == 0x3C3C4A);
  CHECK(!g_p.ctx[kGdi].has_bg);
  CHECK_EQ(g_p.count[kFill], 60u);
  CHECK_EQ(g_p.count[kText], 39u);
  CHECK(g_p.fallback.pale_min == 0.55 && g_p.fallback.bg_l_lo == 0.14 && g_p.fallback.bg_l_hi == 0.30);
  CHECK(g_p.fallback.min_contrast == 5.5);
  CHECK(g_p.dup.max_dx == 3 && g_p.dup.max_dy == 3 && g_p.dup.ring == 256);
  // init проверяет повторы from и неподвижность каждой цели.
  bool inited = g_e.init(g_p, &d);
  if (!inited) printf("  palette.json init: %s at %s\n", status_name(d.status), d.where);
  CHECK(inited);
  // Умолчания 1С в редакторе -> схема владельца.
  CHECK_EQ(g_e.resolve(kFill, kEditor, 0xFFFFFF).rgb, 0x202028u);
  CHECK_EQ(g_e.resolve(kText, kEditor, 0xFF0000).rgb, 0xDC8500u);
  CHECK_EQ(g_e.resolve(kText, kEditor, 0x000000).rgb, 0xF8F8E8u);
  CHECK_EQ(g_e.resolve(kText, kEditor, 0x0000FF).rgb, 0xF8F8E8u);
  CHECK_EQ(g_e.resolve(kText, kEditor, 0x008000).rgb, 0x4B90C7u);
  CHECK_EQ(g_e.resolve(kText, kEditor, 0x963200).rgb, 0xE86020u);
}

TEST(palette_valid_full) {
  Diag d;
  const char* text =
      "{\"schema\":1,\"name\":\"t\",\"tolerance\":0,"
      "\"contexts\":{\"editor\":{\"bg\":\"#202028\",\"text\":\"#f8f8e8\"},\"panel\":{},\"gdi\":{\"bg\":\"#1E1E1E\"}},"
      "\"map_fill\":[{\"from\":\"#FFFFFF\",\"to\":\"#202028\",\"ctx\":\"editor\"},"
      "{\"ctx\":\"*\",\"to\":\"#2b2b2b\",\"from\":\"#ffffff\"}],"
      "\"map_text\":[{\"from\":\"#000000\",\"to\":\"#F8F8E8\",\"ctx\":\"gdi\"}],"
      "\"syscolors\":{\"5\":\"#202028\",\"30\":\"#101010\",\"0\":\"#000001\"},"
      "\"fallback\":{\"pale_min\":0.6,\"bg_l\":[0.1,0.1],\"min_contrast\":3},"
      "\"dup\":{\"max_dx\":0,\"max_dy\":64,\"ring\":1024}}";
  CHECK_EQ(parse_full(text, &d), kOk);
  CHECK_EQ(g_p.count[kFill], 2u);
  CHECK(g_p.map[kFill][1].ctx == kCtxAny && g_p.map[kFill][1].to == 0x2B2B2B);
  CHECK(g_p.map[kText][0].ctx == kGdi);
  CHECK(g_p.ctx[kGdi].has_bg && g_p.ctx[kGdi].bg == 0x1E1E1E);
  CHECK(g_e.init(g_p, &d));
  uint32_t c = 0;
  CHECK(g_e.syscolor(5, &c) && c == 0x202028);
  CHECK(g_e.syscolor(30, &c) && c == 0x101010);
  CHECK(g_e.syscolor(0, &c) && c == 0x000001);
  CHECK(!g_e.syscolor(6, &c));
  CHECK(!g_e.syscolor(31, &c));
}

TEST(palette_schema_errors) {
  Diag d;
  CHECK_EQ(parse_mid(kMaps, &d), kOk);
  // JSON-ошибка с позицией.
  CHECK_EQ(parse_full("{\"schema\":1,}", &d), kErrJson);
  CHECK_EQ(d.json_offset, 12u);
  CHECK_EQ(parse_full("[]", &d), kErrSchema);
  CHECK_EQ(parse_full("{\"schema\":2,\"tolerance\":6,\"contexts\":{},\"map_fill\":[],\"map_text\":[],"
                      "\"fallback\":{\"pale_min\":0.55,\"bg_l\":[0.08,0.2],\"min_contrast\":4.5},"
                      "\"dup\":{\"max_dx\":3,\"max_dy\":3,\"ring\":256}}",
                      &d),
           kErrSchema);
  // Лишний и отсутствующий ключ.
  CHECK_EQ(parse_mid("\"map_fill\":[],\"map_text\":[],\"extra\":1,", &d), kErrUnknownKey);
  CHECK_EQ(parse_mid("\"map_fill\":[],", &d), kErrMissing);
  CHECK(strstr(d.where, "map_text") != nullptr);
  // Запись карты: лишний ключ, нет ctx, неверный цвет, неизвестный контекст.
  CHECK_EQ(parse_mid("\"map_fill\":[{\"from\":\"#FFFFFF\",\"to\":\"#000000\",\"ctx\":\"*\",\"x\":1}],\"map_text\":[],", &d),
           kErrUnknownKey);
  CHECK_EQ(parse_mid("\"map_fill\":[{\"from\":\"#FFFFFF\",\"to\":\"#000000\"}],\"map_text\":[],", &d), kErrMissing);
  CHECK_EQ(parse_mid("\"map_fill\":[{\"from\":\"#FFFFF\",\"to\":\"#000000\",\"ctx\":\"*\"}],\"map_text\":[],", &d),
           kErrColor);
  CHECK(strstr(d.where, "map_fill[0].from") != nullptr);
  CHECK_EQ(parse_mid("\"map_fill\":[{\"from\":\"FFFFFF0\",\"to\":\"#000000\",\"ctx\":\"*\"}],\"map_text\":[],", &d),
           kErrColor);
  CHECK_EQ(parse_mid("\"map_fill\":[{\"from\":\"#FFFFFG\",\"to\":\"#000000\",\"ctx\":\"*\"}],\"map_text\":[],", &d),
           kErrColor);
  CHECK_EQ(parse_mid("\"map_fill\":[],\"map_text\":[{\"from\":\"#FFFFFF\",\"to\":\"#000000\",\"ctx\":\"menu\"}],", &d),
           kErrCtx);
  CHECK_EQ(parse_mid("\"map_fill\":{},\"map_text\":[],", &d), kErrType);
  CHECK_EQ(parse_mid("\"map_fill\":[1],\"map_text\":[],", &d), kErrType);
  // Контексты: неизвестный, "*" как контекст, лишний ключ.
  CHECK_EQ(parse_full("{\"schema\":1,\"tolerance\":6,\"contexts\":{\"menu\":{}},\"map_fill\":[],\"map_text\":[],"
                      "\"fallback\":{\"pale_min\":0.55,\"bg_l\":[0.08,0.2],\"min_contrast\":4.5},"
                      "\"dup\":{\"max_dx\":3,\"max_dy\":3,\"ring\":256}}",
                      &d),
           kErrCtx);
  CHECK_EQ(parse_full("{\"schema\":1,\"tolerance\":6,\"contexts\":{\"*\":{}},\"map_fill\":[],\"map_text\":[],"
                      "\"fallback\":{\"pale_min\":0.55,\"bg_l\":[0.08,0.2],\"min_contrast\":4.5},"
                      "\"dup\":{\"max_dx\":3,\"max_dy\":3,\"ring\":256}}",
                      &d),
           kErrCtx);
  CHECK_EQ(parse_full("{\"schema\":1,\"tolerance\":6,\"contexts\":{\"help\":{\"fg\":\"#000000\"}},\"map_fill\":[],"
                      "\"map_text\":[],\"fallback\":{\"pale_min\":0.55,\"bg_l\":[0.08,0.2],\"min_contrast\":4.5},"
                      "\"dup\":{\"max_dx\":3,\"max_dy\":3,\"ring\":256}}",
                      &d),
           kErrUnknownKey);
}

TEST(palette_range_errors) {
  Diag d;
  char buf[1024];
  // tolerance: дробь, отрицательное, больше 32.
  const char* tols[] = {"1.5", "-1", "33", "\"6\""};
  for (const char* t : tols) {
    snprintf(buf, sizeof(buf),
             "{\"schema\":1,\"tolerance\":%s,\"contexts\":{},\"map_fill\":[],\"map_text\":[],"
             "\"fallback\":{\"pale_min\":0.55,\"bg_l\":[0.08,0.2],\"min_contrast\":4.5},"
             "\"dup\":{\"max_dx\":3,\"max_dy\":3,\"ring\":256}}",
             t);
    CHECK_EQ(parse_full(buf, &d), kErrRange);
  }
  // fallback: bg_l выше pale_min, перевёрнутый диапазон, не два числа, контраст вне [1, 21].
  const char* fbs[] = {
      "{\"pale_min\":0.55,\"bg_l\":[0.08,0.6],\"min_contrast\":4.5}",
      "{\"pale_min\":0.55,\"bg_l\":[0.08,0.545],\"min_contrast\":4.5}",
      "{\"pale_min\":0.55,\"bg_l\":[0.2,0.08],\"min_contrast\":4.5}",
      "{\"pale_min\":0.55,\"bg_l\":[0.08],\"min_contrast\":4.5}",
      "{\"pale_min\":0.55,\"bg_l\":[0.08,0.2],\"min_contrast\":22}",
      "{\"pale_min\":0,\"bg_l\":[0.08,0.2],\"min_contrast\":4.5}",
  };
  for (const char* fb : fbs) {
    snprintf(buf, sizeof(buf),
             "{\"schema\":1,\"tolerance\":6,\"contexts\":{},\"map_fill\":[],\"map_text\":[],"
             "\"fallback\":%s,\"dup\":{\"max_dx\":3,\"max_dy\":3,\"ring\":256}}",
             fb);
    CHECK_EQ(parse_full(buf, &d), kErrRange);
  }
  // dup: ring не степень двойки и вне [16, 1024], сдвиг больше 64.
  const char* dups[] = {"{\"max_dx\":3,\"max_dy\":3,\"ring\":100}", "{\"max_dx\":3,\"max_dy\":3,\"ring\":8}",
                        "{\"max_dx\":3,\"max_dy\":3,\"ring\":2048}", "{\"max_dx\":65,\"max_dy\":3,\"ring\":256}"};
  for (const char* dp : dups) {
    snprintf(buf, sizeof(buf),
             "{\"schema\":1,\"tolerance\":6,\"contexts\":{},\"map_fill\":[],\"map_text\":[],"
             "\"fallback\":{\"pale_min\":0.55,\"bg_l\":[0.08,0.2],\"min_contrast\":4.5},\"dup\":%s}",
             dp);
    CHECK_EQ(parse_full(buf, &d), kErrRange);
  }
  // syscolors: индекс вне 0..30, ведущий ноль, не число.
  CHECK_EQ(parse_mid("\"map_fill\":[],\"map_text\":[],\"syscolors\":{\"31\":\"#000000\"},", &d), kErrRange);
  CHECK_EQ(parse_mid("\"map_fill\":[],\"map_text\":[],\"syscolors\":{\"05\":\"#000000\"},", &d), kErrRange);
  CHECK_EQ(parse_mid("\"map_fill\":[],\"map_text\":[],\"syscolors\":{\"x\":\"#000000\"},", &d), kErrRange);
  CHECK_EQ(parse_mid("\"map_fill\":[],\"map_text\":[],\"syscolors\":{\"5\":\"#00000\"},", &d), kErrColor);
}

TEST(palette_too_many_entries) {
  // 1025 записей — больше kMaxEntries.
  size_t cap = 64 * 1100 + 512;
  char* buf = static_cast<char*>(malloc(cap));
  size_t n = static_cast<size_t>(snprintf(buf, cap, "%s\"map_fill\":[", kHead));
  for (int i = 0; i < 1025; ++i) {
    n += static_cast<size_t>(snprintf(buf + n, cap - n, "%s{\"from\":\"#%06X\",\"to\":\"#%06X\",\"ctx\":\"*\"}",
                                      i ? "," : "", i, i));
  }
  n += static_cast<size_t>(snprintf(buf + n, cap - n, "],\"map_text\":[],%s", kTail));
  Diag d;
  CHECK(!parse_palette(buf, n, &g_p, &d));
  CHECK_EQ(d.status, kErrTooMany);
  free(buf);
}
