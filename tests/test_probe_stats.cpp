// SPDX-License-Identifier: MIT
// Чистая логика probe (core/probe_stats) и экспорты DLL probe вне 1С.
#include <windows.h>

#include <string.h>

#include "core/probe_stats.h"
#include "probe/snapshot.h"
#include "tests/test.h"

using namespace dt;

namespace {

probe::KeyIndex<64> g_small;
probe::KeyIndex<4096> g_big;
probe::Timeline g_tl;
probe::PairRing g_ring;
probe::PairRing g_ring_mt;
probe::OwnerSlots g_slots;
probe::FrameStats g_fs;
probe::DupHist g_dh;

probe::RunRec run(uint64_t ghash, uint32_t count, double x, double y, uint32_t tid, uint64_t target,
                  uint32_t rgba, uint64_t ticks) {
  probe::RunRec r;
  r.ghash = ghash;
  r.count = count;
  r.rgba = rgba;
  r.has_color = 1;
  r.tid = tid;
  r.target = target;
  r.x = x;
  r.y = y;
  r.ticks = ticks;
  return r;
}

}  // namespace

TEST(probe_key_index_insert_find) {
  g_small.clear();
  bool ins = false;
  uint32_t a = g_small.insert(0x1234, &ins);
  CHECK(a != probe::kNone && ins);
  CHECK_EQ(g_small.insert(0x1234, &ins), a);
  CHECK(!ins);
  CHECK_EQ(g_small.find(0x1234), a);
  CHECK_EQ(g_small.key_at(a), 0x1234ull);
  CHECK_EQ(g_small.find(0x9999), probe::kNone);
  // Ключ 0 — пустой слот, не вставляется.
  CHECK_EQ(g_small.insert(0, &ins), probe::kNone);
  CHECK(!ins);
}

TEST(probe_key_index_full) {
  // 64 слота, пробирование ≤ 32: таблица заполняется до отказа, отказ — kNone,
  // всё вставленное находится, лишнего нет.
  g_small.clear();
  uint32_t inserted = 0, refused = 0;
  for (uint64_t k = 1; k <= 500; ++k) {
    bool ins = false;
    uint32_t i = g_small.insert(k * 0x9E3779B97F4A7C15ull, &ins);
    if (i == probe::kNone) ++refused;
    else if (ins) ++inserted;
  }
  CHECK(inserted <= 64);
  CHECK(inserted >= 32);
  CHECK(refused > 0);
  uint32_t found = 0;
  for (uint64_t k = 1; k <= 500; ++k) {
    if (g_small.find(k * 0x9E3779B97F4A7C15ull) != probe::kNone) ++found;
  }
  CHECK_EQ(found, inserted);
  uint32_t used = 0;
  for (uint32_t i = 0; i < 64; ++i) used += g_small.key_at(i) != 0 ? 1u : 0u;
  CHECK_EQ(used, inserted);
}

namespace {

volatile LONG g_mt_new = 0;

DWORD WINAPI key_worker(LPVOID) {
  for (uint64_t k = 1; k <= 2000; ++k) {
    bool ins = false;
    if (g_big.insert(k, &ins) != probe::kNone && ins) InterlockedIncrement(&g_mt_new);
  }
  return 0;
}

}  // namespace

TEST(probe_key_index_multithread) {
  // Четыре потока вставляют одни и те же ключи: каждый ключ «новый» ровно один раз.
  g_big.clear();
  HANDLE th[4];
  for (int i = 0; i < 4; ++i) th[i] = CreateThread(nullptr, 0, key_worker, nullptr, 0, nullptr);
  WaitForMultipleObjects(4, th, TRUE, INFINITE);
  for (int i = 0; i < 4; ++i) CloseHandle(th[i]);
  CHECK_EQ(g_mt_new, 2000);
  for (uint64_t k = 1; k <= 2000; ++k) CHECK(g_big.find(k) != probe::kNone);
}

TEST(probe_time_conversion) {
  const uint64_t f = 10000000;  // 10 МГц
  CHECK_EQ(probe::ticks_to_us(10, f), 1ull);
  CHECK_EQ(probe::ticks_to_us(f, f), 1000000ull);
  CHECK_EQ(probe::ticks_to_ms(f * 3 + f / 2, f), 3500ull);
  CHECK_EQ(probe::ticks_to_ms(5, 0), 0ull);
  // Большой интервал: сутки при 3 ГГц без переполнения.
  const uint64_t g = 3000000000ull;
  CHECK_EQ(probe::ticks_to_ms(g * 86400, g), 86400000ull);
  CHECK_EQ(probe::sat32(0x100000000ull), 0xFFFFFFFFu);
  CHECK_EQ(probe::sat32(7), 7u);
}

TEST(probe_timeline_buckets) {
  CHECK(g_tl.add(0, probe::Timeline::kNewPtr));
  CHECK(g_tl.add(249, probe::Timeline::kNewPtr));
  CHECK(g_tl.add(250, probe::Timeline::kNewHash));
  CHECK(g_tl.add(4096 * 250 - 1, probe::Timeline::kGlyphRuns));
  CHECK(!g_tl.add(4096 * 250, probe::Timeline::kGlyphRuns));  // переполнение
  CHECK_EQ(g_tl.get(0, probe::Timeline::kNewPtr), 2u);
  CHECK_EQ(g_tl.get(1, probe::Timeline::kNewHash), 1u);
  CHECK_EQ(g_tl.get(1, probe::Timeline::kNewPtr), 0u);
  CHECK_EQ(g_tl.get(4095, probe::Timeline::kGlyphRuns), 1u);
}

TEST(probe_log2_bins) {
  CHECK_EQ(probe::log2_bin(0, 16), 0u);
  CHECK_EQ(probe::log2_bin(1, 16), 1u);
  CHECK_EQ(probe::log2_bin(2, 16), 2u);
  CHECK_EQ(probe::log2_bin(3, 16), 2u);
  CHECK_EQ(probe::log2_bin(4, 16), 3u);
  CHECK_EQ(probe::log2_bin(16383, 16), 14u);
  CHECK_EQ(probe::log2_bin(16384, 16), 15u);
  CHECK_EQ(probe::log2_bin(~0ull, 16), 15u);
}

TEST(probe_dxdy_bins) {
  int32_t dx = 0, dy = 0;
  int32_t b = probe::dxdy_bin(0.0, 0.0);
  CHECK(b >= 0);
  probe::dxdy_from_bin(static_cast<uint32_t>(b), &dx, &dy);
  CHECK(dx == 0 && dy == 0);
  // Округление половины от нуля.
  b = probe::dxdy_bin(1.5, -1.5);
  probe::dxdy_from_bin(static_cast<uint32_t>(b), &dx, &dy);
  CHECK(dx == 2 && dy == -2);
  b = probe::dxdy_bin(0.49, -0.49);
  probe::dxdy_from_bin(static_cast<uint32_t>(b), &dx, &dy);
  CHECK(dx == 0 && dy == 0);
  // Края ±7 и «далеко».
  b = probe::dxdy_bin(-7.0, 7.4);
  CHECK(b >= 0 && b < static_cast<int32_t>(probe::kDxDyBins));
  probe::dxdy_from_bin(static_cast<uint32_t>(b), &dx, &dy);
  CHECK(dx == -7 && dy == 7);
  CHECK_EQ(probe::dxdy_bin(7.5, 0), -1);
  CHECK_EQ(probe::dxdy_bin(0, -8), -1);
  CHECK_EQ(probe::dxdy_bin(1e300, 0), -1);
  volatile double z = 0.0;
  CHECK_EQ(probe::dxdy_bin(z / z, 0), -1);
  CHECK_EQ(probe::dxdy_bin(-7.0, -7.0), 0);
  CHECK_EQ(probe::dxdy_bin(7.0, 7.0), static_cast<int32_t>(probe::kDxDyBins - 1));
}

TEST(probe_rgba8) {
  bool ix = true;
  CHECK_EQ(probe::rgba8(1.0, 0.0, 0.0, 1.0, &ix), 0xFF0000FFu);
  CHECK(!ix);
  CHECK_EQ(probe::rgba8(128 / 255.0, 64 / 255.0, 32 / 255.0, 0.0, &ix), 0x80402000u);
  CHECK(!ix);
  CHECK_EQ(probe::rgba8(0.5, 0.0, 0.0, 1.0, &ix), 0x800000FFu);  // 127.5 → 128
  CHECK(ix);
  CHECK_EQ(probe::rgba8(1.5, -0.1, 0.0, 1.0, &ix), 0xFF0000FFu);
  CHECK(ix);
}

TEST(probe_burst_detection) {
  // Пачки разделены паузой ≥ gap: базовое время = длительность − накладные.
  probe::Burst b;
  memset(&b, 0, sizeof(b));
  probe::BurstClosed c;
  const uint64_t gap = 40;
  CHECK(!probe::burst_step(&b, 100, 110, 2, 5, gap, &c));
  CHECK(!probe::burst_step(&b, 120, 130, 3, 4, gap, &c));  // пауза 10 < 40
  CHECK(!probe::burst_step(&b, 160, 170, 1, 6, gap, &c));  // пауза 30 < 40
  CHECK(probe::burst_step(&b, 210, 215, 1, 1, gap, &c));   // пауза 40 — закрыть
  CHECK_EQ(c.base, 70ull - 6ull);
  CHECK_EQ(c.pause, 40ull);
  CHECK_EQ(c.overhead, 6ull);
  CHECK_EQ(c.orig, 15ull);
  // Новая пачка открыта вызовом 210..215.
  CHECK(probe::burst_step(&b, 1000, 1001, 0, 0, gap, &c));
  CHECK_EQ(c.base, 4ull);
  CHECK_EQ(c.pause, 785ull);
  // Накладные больше длительности — base 0, не переполнение.
  memset(&b, 0, sizeof(b));
  probe::burst_step(&b, 0, 1, 50, 0, gap, &c);
  CHECK(probe::burst_step(&b, 100, 101, 0, 0, gap, &c));
  CHECK_EQ(c.base, 0ull);
}

TEST(probe_owner_slots) {
  int32_t a = g_slots.claim(0x104);
  CHECK(a >= 0);
  CHECK_EQ(g_slots.claim(0x104), a);
  // Тот же слот ((tid >> 2) & 63), другой tid — коллизия.
  CHECK_EQ(g_slots.claim(0x104 + (64 << 2)), -1);
  CHECK(g_slots.claim(0x108) >= 0);
  CHECK_EQ(g_slots.claim(0), -1);
}

TEST(probe_frame_stats) {
  probe::FrameStats::Copy c;
  g_fs.take(&c);
  g_fs.add(0, 0, 1, 2);
  g_fs.add(249, 4000, 1, 2);
  g_fs.add(250, 4000, 1, 2);
  g_fs.add(64000, 10, 1, 2);  // за 64 мс — переполнение
  g_fs.take(&c);
  CHECK_EQ(c.count, 4u);
  CHECK_EQ(c.hist[0], 2u);
  CHECK_EQ(c.hist[1], 1u);
  CHECK_EQ(c.hist[probe::FrameStats::kBins], 1u);
  CHECK_EQ(c.max_us, 64000u);
  CHECK_EQ(c.sum_us, 64499ull);
  CHECK_EQ(c.overhead_ticks, 4ull);
  CHECK_EQ(c.orig_ticks, 8ull);
  CHECK_EQ(c.pause[probe::log2_bin(4000, probe::FrameStats::kPauseBins)], 2u);
  // Эпоха: после take всё обнулено.
  g_fs.take(&c);
  CHECK_EQ(c.count, 0u);
  CHECK_EQ(c.hist[0], 0u);
  CHECK_EQ(c.sum_us, 0ull);
}

TEST(probe_pair_ring) {
  probe::RunRec prev;
  memset(&prev, 0, sizeof(prev));
  uint32_t torn = 0;
  CHECK(!g_ring.push_and_match(run(0xAA, 5, 10, 20, 1, 100, 0x000000FF, 1000), &prev, &torn));
  CHECK(!g_ring.push_and_match(run(0xBB, 5, 10, 20, 1, 100, 0x000000FF, 1001), &prev, &torn));
  // Тот же прогон: пара с самым свежим совпадением.
  CHECK(g_ring.push_and_match(run(0xAA, 5, 11, 21, 2, 100, 0x111111FF, 1010), &prev, &torn));
  CHECK(prev.x == 10 && prev.y == 20 && prev.tid == 1 && prev.ticks == 1000);
  CHECK_EQ(probe::pair_flags(run(0xAA, 5, 11, 21, 2, 100, 0x111111FF, 1010), prev),
           static_cast<uint32_t>(probe::kSameTarget));
  CHECK(g_ring.push_and_match(run(0xAA, 5, 12, 22, 2, 100, 0x111111FF, 1020), &prev, &torn));
  CHECK(prev.x == 11 && prev.tid == 2);
  CHECK_EQ(probe::pair_flags(run(0xAA, 5, 12, 22, 2, 100, 0x111111FF, 1020), prev),
           static_cast<uint32_t>(probe::kSameThread | probe::kSameTarget | probe::kSameColor));
  // Другое число глифов — не пара.
  CHECK(!g_ring.push_and_match(run(0xAA, 6, 12, 22, 2, 100, 0x111111FF, 1030), &prev, &torn));
  // Старше kSize прогонов — забыт.
  CHECK(!g_ring.push_and_match(run(0xCC, 3, 0, 0, 1, 1, 0, 1), &prev, &torn));
  for (uint32_t i = 0; i < probe::PairRing::kSize; ++i) {
    g_ring.push_and_match(run(0x1000 + i, 3, 0, 0, 1, 1, 0, 2), &prev, &torn);
  }
  CHECK(!g_ring.push_and_match(run(0xCC, 3, 0, 0, 1, 1, 0, 3), &prev, &torn));
  CHECK_EQ(torn, 0u);
  // Без цвета — «тот же цвет» не ставится.
  probe::RunRec a = run(1, 1, 0, 0, 1, 1, 0, 0);
  probe::RunRec b = a;
  a.has_color = 0;
  b.has_color = 0;
  CHECK_EQ(probe::pair_flags(a, b), static_cast<uint32_t>(probe::kSameThread | probe::kSameTarget));
}

namespace {

volatile LONG g_ring_pairs = 0;
volatile LONG g_ring_bad = 0;

DWORD WINAPI ring_worker(LPVOID arg) {
  uint32_t s = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
  for (int i = 0; i < 20000; ++i) {
    s = s * 1664525u + 1013904223u;
    uint64_t h = s % 32;
    // Координаты выводятся из ключа: у любой пары, прочитанной целиком, они сходятся.
    probe::RunRec r = run(h, 3, static_cast<double>(h), static_cast<double>(h) * 2, 1, h * 7, 0, i);
    probe::RunRec prev;
    uint32_t torn = 0;
    if (g_ring_mt.push_and_match(r, &prev, &torn)) {
      InterlockedIncrement(&g_ring_pairs);
      if (prev.x != r.x || prev.y != r.y || prev.target != r.target) InterlockedIncrement(&g_ring_bad);
    }
  }
  return 0;
}

}  // namespace

TEST(probe_pair_ring_multithread) {
  HANDLE th[4];
  for (int i = 0; i < 4; ++i) {
    th[i] = CreateThread(nullptr, 0, ring_worker, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(i + 1)), 0, nullptr);
  }
  WaitForMultipleObjects(4, th, TRUE, INFINITE);
  for (int i = 0; i < 4; ++i) CloseHandle(th[i]);
  CHECK(g_ring_pairs > 0);
  CHECK_EQ(g_ring_bad, 0);
}

TEST(probe_dup_hist) {
  uint32_t bins[probe::DupHist::kBins];
  uint32_t dt[probe::DupHist::kDtBins];
  g_dh.take(bins, dt);
  int32_t b = probe::dxdy_bin(1, 1);
  g_dh.add(static_cast<uint32_t>(b), probe::kSameThread | probe::kSameColor, 100);
  g_dh.add(static_cast<uint32_t>(b), probe::kSameThread | probe::kSameColor, 100);
  g_dh.take(bins, dt);
  CHECK_EQ(bins[static_cast<uint32_t>(b) * probe::kPairFlags + (probe::kSameThread | probe::kSameColor)], 2u);
  CHECK_EQ(dt[probe::log2_bin(100, probe::DupHist::kDtBins)], 2u);
  g_dh.take(bins, dt);
  CHECK_EQ(bins[static_cast<uint32_t>(b) * probe::kPairFlags + (probe::kSameThread | probe::kSameColor)], 0u);
}

TEST(probe_snapshot_tables) {
  // Имена счётчиков — по одному на каждый, и группы идут подряд.
  for (uint32_t i = 0; i < snap::kCounters; ++i) CHECK(snap::kCounterNames[i] != nullptr);
  static_assert(snap::kEpochCounters < snap::kOverflowEnd && snap::kOverflowEnd < snap::kCounters,
                "snapshot: counter groups");
  CHECK(strcmp(snap::kCounterNames[snap::kEpochCounters], "surf_full") == 0);
  CHECK(strcmp(snap::kCounterNames[snap::kLayoutRuns], "runs") == 0);
  CHECK(strcmp(snap::kCounterNames[snap::kCounters - 1], "decisive20") == 0);
  CHECK(strcmp(snap::kCounterNames[snap::kColorInexact], "color_inexact") == 0);
}

TEST(probe_dll_outside_1c) {
  // Вне 1cv8.exe probe не ставит хуков: DtStart — 3, DtDump — «не активен», DtStop — 0.
#if defined(_WIN64)
  const wchar_t* name = L"cfgdark_probe_x64.dll";
#else
  const wchar_t* name = L"cfgdark_probe_x86.dll";
#endif
  HMODULE m = LoadLibraryW(name);
  CHECK(m != nullptr);
  if (m == nullptr) return;
  typedef DWORD(WINAPI * Fn)(LPVOID);
  Fn start = reinterpret_cast<Fn>(GetProcAddress(m, "DtStart"));
  Fn stop = reinterpret_cast<Fn>(GetProcAddress(m, "DtStop"));
  Fn dump = reinterpret_cast<Fn>(GetProcAddress(m, "DtDump"));
  const snap::ProbeSnapshot* s = reinterpret_cast<const snap::ProbeSnapshot*>(GetProcAddress(m, "DtSnapshot"));
  CHECK(start != nullptr && stop != nullptr && dump != nullptr && s != nullptr);
  if (start && stop && dump && s) {
    CHECK_EQ(dump(nullptr), 1u);
    CHECK_EQ(start(nullptr), 3u);
    CHECK_EQ(dump(nullptr), 1u);
    CHECK_EQ(s->magic, 0u);  // снимок не тронут
    CHECK_EQ(stop(nullptr), 0u);
    CHECK_EQ(start(nullptr), 3u);  // защита освобождена — повтор снова доходит до проверки образа
    CHECK_EQ(stop(nullptr), 0u);
    // DtSnapshot — в доступной на запись неисполняемой памяти.
    MEMORY_BASIC_INFORMATION mbi;
    CHECK(VirtualQuery(s, &mbi, sizeof(mbi)) == sizeof(mbi));
    DWORD p = mbi.Protect & 0xFF;
    CHECK(p == PAGE_READWRITE || p == PAGE_WRITECOPY);
  }
  FreeLibrary(m);
}
