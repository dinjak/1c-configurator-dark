// SPDX-License-Identifier: MIT
#include <windows.h>

#include <string.h>

#include "core/dup.h"
#include "core/glyph_layout.h"
#include "tests/test.h"

using namespace dt;

namespace {

dup::Detector g_det;
dup::Detector g_det_mt;

dup::Params params(uint32_t ring) {
  dup::Params p;
  p.max_dx = 3;
  p.max_dy = 3;
  p.ring = ring;
  return p;
}

const uint64_t kSurfA = 0x10000;
const uint64_t kSurfB = 0x20000;

}  // namespace

TEST(dup_params) {
  CHECK(!g_det.init(params(100)));
  CHECK(!g_det.init(params(8)));
  CHECK(!g_det.init(params(2048)));
  dup::Params p = params(256);
  p.max_dx = -1;
  CHECK(!g_det.init(p));
  CHECK(g_det.init(params(16)));
  CHECK(g_det.init(params(1024)));
}

TEST(dup_pair_detected_and_consumed) {
  CHECK(g_det.init(params(256)));
  const uint64_t h = 0xABCDEF;
  // Emboss-пара: тот же прогон со сдвигом (1, 1) — второй проход пропускается.
  CHECK(!g_det.check(h, 5, 0x000000, kSurfA, 10.0, 20.0));
  CHECK(g_det.check(h, 5, 0x000000, kSurfA, 11.0, 21.0));
  // Пара потреблена: следующий такой же прогон — первый проход новой пары.
  CHECK(!g_det.check(h, 5, 0x000000, kSurfA, 11.0, 21.0));
  CHECK(g_det.check(h, 5, 0x000000, kSurfA, 10.0, 20.0));
  // Сдвиг за порогом — не дубль.
  CHECK(!g_det.check(h, 5, 0x000000, kSurfA, 50.0, 20.0));
  CHECK(!g_det.check(h, 5, 0x000000, kSurfA, 54.0, 20.0));
  CHECK(!g_det.check(h, 5, 0x000000, kSurfA, 50.0, 20.0));
  CHECK(!g_det.check(h, 5, 0x000000, kSurfA, 50.0, 23.5));
}

TEST(dup_signature_fields) {
  CHECK(g_det.init(params(256)));
  CHECK(!g_det.check(1, 5, 0x000000, kSurfA, 0, 0));
  CHECK(!g_det.check(1, 5, 0x000000, kSurfB, 1, 1));  // другая поверхность
  CHECK(!g_det.check(1, 5, 0x111111, kSurfA, 1, 1));  // другой цвет
  CHECK(!g_det.check(1, 6, 0x000000, kSurfA, 1, 1));  // другое число глифов
  CHECK(!g_det.check(2, 5, 0x000000, kSurfA, 1, 1));  // другие глифы
  // Исходная запись цела, если слоты не совпали; при совпадении слотов —
  // перезаписана, и это тоже «без пропуска».
  bool again = g_det.check(1, 5, 0x000000, kSurfA, 1, 1);
  (void)again;
  // NaN в координатах — без пропуска.
  CHECK(g_det.init(params(256)));
  volatile double z = 0.0;
  double nan = z / z;
  CHECK(!g_det.check(7, 3, 0, kSurfA, nan, 0));
  CHECK(!g_det.check(7, 3, 0, kSurfA, nan, 0));
}

TEST(dup_collision_no_skip) {
  // Подобрать прогон B, попадающий в тот же слот, что и A (кольцо 16).
  CHECK(g_det.init(params(16)));
  uint64_t a = 0x1234;
  CHECK(!g_det.check(a, 4, 0, kSurfA, 0, 0));
  uint64_t b = 0;
  bool found = false;
  for (uint64_t cand = 1; cand < 100000 && !found; ++cand) {
    if (cand == a) continue;
    // Проверка, что B вытесняет A: после B второй проход A не пропускается.
    CHECK(g_det.init(params(16)));
    g_det.check(a, 4, 0, kSurfA, 0, 0);
    g_det.check(cand, 4, 0, kSurfA, 100, 100);
    if (!g_det.check(a, 4, 0, kSurfA, 1, 1)) {
      b = cand;
      found = true;
    }
  }
  CHECK(found);
  // Коллизия: A записан, B занял слот, второй проход A — без пропуска,
  // а второй проход B — пропуск.
  CHECK(g_det.init(params(16)));
  CHECK(!g_det.check(a, 4, 0, kSurfA, 0, 0));
  CHECK(!g_det.check(b, 4, 0, kSurfA, 100, 100));
  CHECK(!g_det.check(a, 4, 0, kSurfA, 1, 1));
  CHECK(g_det.init(params(16)));
  CHECK(!g_det.check(a, 4, 0, kSurfA, 0, 0));
  CHECK(!g_det.check(b, 4, 0, kSurfA, 100, 100));
  CHECK(g_det.check(b, 4, 0, kSurfA, 101, 101));
}

TEST(dup_ring_recency) {
  // Запись старше ring прогонов не даёт пропуска, даже если слот не вытеснен.
  CHECK(g_det.init(params(16)));
  CHECK(!g_det.check(0x55, 2, 0, kSurfA, 0, 0));
  int skipped = 0;
  for (int i = 0; i < 16; ++i) {
    if (g_det.check(0x55, 2, 0, kSurfB, 1000.0 * i, 0)) ++skipped;  // другая поверхность
  }
  CHECK_EQ(skipped, 0);
  CHECK(!g_det.check(0x55, 2, 0, kSurfA, 1, 1));
}

TEST(dup_glyph_index_hash_stride) {
  // Индексы одинаковые, шаг разный: хеш один и тот же.
  uint8_t a24[3 * 24];
  uint8_t a20[3 * 20];
  memset(a24, 0xEE, sizeof(a24));
  memset(a20, 0x11, sizeof(a20));
  const uint32_t idx[3] = {36, 72, 80};
  for (int i = 0; i < 3; ++i) {
    memcpy(a24 + i * 24, &idx[i], 4);
    memcpy(a20 + i * 20, &idx[i], 4);
  }
  CHECK_EQ(dup::glyph_index_hash(a24, 3, 24), dup::glyph_index_hash(a20, 3, 20));
  uint32_t other = 37;
  memcpy(a24, &other, 4);
  CHECK(dup::glyph_index_hash(a24, 3, 24) != dup::glyph_index_hash(a20, 3, 20));
}

namespace {

volatile LONG g_mt_skips = 0;

DWORD WINAPI dup_worker(LPVOID arg) {
  uint32_t s = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
  for (int i = 0; i < 20000; ++i) {
    s = s * 1664525u + 1013904223u;
    uint64_t h = s % 64;  // много повторов, чтобы потоки делили слоты
    if (g_det_mt.check(h, 3, 0, kSurfA, (s >> 8) % 4, (s >> 12) % 4)) InterlockedIncrement(&g_mt_skips);
  }
  return 0;
}

}  // namespace

TEST(dup_multithread_smoke) {
  // Гонки без блокировок: не падаем, пропуски есть, но не на каждом вызове.
  CHECK(g_det_mt.init(params(64)));
  HANDLE th[4];
  for (int i = 0; i < 4; ++i) {
    th[i] = CreateThread(nullptr, 0, dup_worker, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(i + 1)), 0, nullptr);
    CHECK(th[i] != nullptr);
  }
  WaitForMultipleObjects(4, th, TRUE, INFINITE);
  for (int i = 0; i < 4; ++i) CloseHandle(th[i]);
  CHECK(g_mt_skips > 0);
  CHECK(g_mt_skips < 4 * 20000);
}

// ---------------------------------------------------------------------------
// Раскладка глифа.

namespace {

void make24(uint8_t* buf, uint32_t n, bool garbage_pad) {
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t* g = buf + i * 24;
    uint32_t idx = 30 + i * 7;
    memcpy(g, &idx, 4);
    uint32_t pad = garbage_pad ? 0xDEADBEEF : 0;
    memcpy(g + 4, &pad, 4);
    double x = 10.0 + 7.5 * i, y = 14.0;
    memcpy(g + 8, &x, 8);
    memcpy(g + 16, &y, 8);
  }
}

void make20(uint8_t* buf, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t* g = buf + i * 20;
    uint32_t idx = 30 + i * 7;
    memcpy(g, &idx, 4);
    double x = 10.0 + 7.5 * i, y = 14.0;
    memcpy(g + 4, &x, 8);
    memcpy(g + 12, &y, 8);
  }
}

}  // namespace

TEST(glyph_layout_compiled) {
  // x64 закреплено static_assert; MSVC x86 тоже выравнивает double на 8 —
  // но раскладку чужой сборки cairo на x86 подтверждает только самопроверка.
  CHECK_EQ(glyph::compiled_layout(), glyph::kStride24);
  glyph::LayoutDesc d = glyph::describe(glyph::kStride20);
  CHECK(d.stride == 20 && d.x_off == 4 && d.y_off == 12);
}

TEST(glyph_layout_detect) {
  uint8_t b24[8 * 24];
  uint8_t b20[8 * 20];
  make24(b24, 8, true);
  make20(b20, 8);
  CHECK_EQ(glyph::detect(b24, 8), glyph::kStride24);
  CHECK_EQ(glyph::detect(b20, 8), glyph::kStride20);
  CHECK_EQ(glyph::check_run(b24, 8, sizeof(b24), glyph::kStride24), glyph::kPlausible);
  CHECK_EQ(glyph::check_run(b24, 8, sizeof(b24), glyph::kStride20), glyph::kImplausible);
  CHECK_EQ(glyph::check_run(b20, 8, sizeof(b20), glyph::kStride24), glyph::kImplausible);
  // Читается не больше readable: один глиф по 24 в 20 байтах не помещается.
  CHECK_EQ(glyph::check_run(b24, 1, 20, glyph::kStride24), glyph::kNoData);
  // Мусор.
  uint8_t junk[8 * 24];
  uint32_t s = 5;
  for (size_t i = 0; i < sizeof(junk); ++i) {
    s = s * 1664525u + 1013904223u;
    junk[i] = static_cast<uint8_t>(s >> 24);
  }
  CHECK_EQ(glyph::detect(junk, 8), glyph::kUnknown);
  // NaN в координате.
  make24(b24, 8, false);
  volatile double z = 0.0;
  double nan = z / z;
  memcpy(b24 + 2 * 24 + 8, &nan, 8);
  CHECK_EQ(glyph::check_run(b24, 8, sizeof(b24), glyph::kStride24), glyph::kImplausible);
}

TEST(glyph_self_check) {
  static glyph::SelfCheck sc;
  uint8_t b24[8 * 24];
  make24(b24, 8, true);
  sc.init(glyph::kStride24, 4, 24);
  CHECK_EQ(sc.state(), glyph::SelfCheck::kPending);
  CHECK_EQ(sc.feed(b24, 1), glyph::SelfCheck::kPending);  // одиночный глиф не в зачёт
  for (int i = 0; i < 3; ++i) CHECK_EQ(sc.feed(b24, 8), glyph::SelfCheck::kPending);
  CHECK_EQ(sc.feed(b24, 8), glyph::SelfCheck::kPassed);
  // Провал — до конца сессии.
  uint8_t b20[8 * 20];
  make20(b20, 8);
  sc.init(glyph::kStride24, 4, 20);  // x86: шаг не подтверждён, читаем ≤ 20·n
  CHECK_EQ(sc.feed(b20, 8), glyph::SelfCheck::kFailed);
  CHECK_EQ(sc.feed(b24, 8), glyph::SelfCheck::kFailed);
  sc.init(glyph::kUnknown, 4, 24);
  CHECK_EQ(sc.state(), glyph::SelfCheck::kFailed);
}
