// SPDX-License-Identifier: MIT
// Диагностическая DLL (только чтение, SKIP-ONLY: ничего не меняет и всегда вызывает
// исходную функцию). M3b: три хука cairo.dll через patch::Engine (EAT со стабами +
// IAT, включая delay-load) — cairo_pattern_create_for_surface, cairo_show_glyphs,
// cairo_set_source_rgba. Данные копятся в статических таблицах без блокировок;
// DtDump копирует их в экспорт DtSnapshot, загрузчик читает снимок и печатает JSON.
//
// В хуке: нет выделения памяти, блокировок, файлов, GetModuleHandle*, flush; только
// Interlocked/атомарные операции. DllMain и хуки g_lock не берут; своих потоков нет.
#include <windows.h>
#include <intrin.h>
#include <string.h>

#include "core/dup.h"
#include "core/glyph_layout.h"
#include "core/hash.h"
#include "core/probe_stats.h"
#include "patch/patch.h"
#include "probe/snapshot.h"

#pragma intrinsic(_ReturnAddress)

using namespace dt;

namespace {

// Коды DtStart; загрузчик (loader/loader.cpp) знает 0 и 1, остальные — «12 с числом».
const DWORD kStartOk = 0;
const DWORD kStartAlready = 1;  // в процессе уже активна тема или probe
const DWORD kStartError = 2;
const DWORD kStartNoTarget = 3;  // не 1cv8.exe, нет cairo.dll или ни одного хука
const DWORD kStartNoStubs = 4;   // стабы запрещены (ACG) — режима «только IAT» нет
const DWORD kStartPatch = 5;     // ошибка патча (журнал полон и прочее)

// Коды DtDump.
const DWORD kDumpOk = 0;
const DWORD kDumpInactive = 1;
const DWORD kDumpError = 2;

static_assert(patch::kErrModules + 1 == snap::kPatchStatusCount, "snapshot: patch::Status names out of sync");
static_assert(sizeof(patch::Stats) == (snap::kPatchStats - 1) * sizeof(uint32_t), "snapshot: patch::Stats out of sync");
static_assert(snap::kBuckets == probe::Timeline::kBuckets && snap::kBucketMs == probe::Timeline::kBucketMs &&
                  snap::kTimelineFields == probe::Timeline::kFields,
              "snapshot: timeline out of sync");
static_assert(snap::kDupBins == probe::DupHist::kBins && snap::kDtBins == probe::DupHist::kDtBins,
              "snapshot: dup out of sync");
static_assert(snap::kFrameBins == probe::FrameStats::kBins + 1 && snap::kPauseBins == probe::FrameStats::kPauseBins &&
                  snap::kFrameBinUs == probe::FrameStats::kBinUs,
              "snapshot: frame out of sync");

// Константы cairo (cairo.h, стабильны с 1.0).
const int kCairoSurfaceImage = 0;  // CAIRO_SURFACE_TYPE_IMAGE
const int kCairoFormatArgb32 = 0;  // CAIRO_FORMAT_ARGB32
const int kCairoPatternSolid = 0;  // CAIRO_PATTERN_TYPE_SOLID
const int kCairoStatusSuccess = 0;

const uint32_t kMinIcon = 8;
const uint32_t kMaxIcon = 48;
// Сколько глифов прогона проверяется (раскладка, самопроверка, хеш для пар).
const uint32_t kMaxCheckGlyphs = 1024;

// Сигнатуры cairo (__cdecl).
typedef void*(__cdecl* PatternCreateFn)(void* surface);
typedef void(__cdecl* ShowGlyphsFn)(void* cr, const void* glyphs, int num_glyphs);
typedef void(__cdecl* SetRgbaFn)(void* cr, double r, double g, double b, double a);
typedef int(__cdecl* IntOfPtrFn)(void* p);
typedef unsigned char*(__cdecl* DataFn)(void* surface);
typedef void*(__cdecl* PtrOfPtrFn)(void* p);
typedef int(__cdecl* GetRgbaFn)(void* pattern, double* r, double* g, double* b, double* a);

// ---------------------------------------------------------------------------
// Состояние жизненного цикла (под g_lock: DtStart, DtStop, DtDump)

HINSTANCE g_instance = nullptr;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;
// Защита от повторной загрузки: Local\cfgdark-<pid>, общий с темой — probe
// никогда не активен вместе с ней. Создаётся в DtStart, закрывается в DtStop.
HANDLE g_guard = nullptr;
HMODULE g_cairo_pin = nullptr;  // ссылка на cairo.dll на время хуков
patch::Engine g_engine;
uint32_t g_epoch = 0;
uint32_t g_prev_ms = 0;

// Читается хуками.
volatile LONG g_active = 0;
patch::Inflight g_inflight;
uint64_t g_t0 = 0;
uint64_t g_freq = 0;
uint64_t g_gap_ticks = 0;
uint32_t g_features = 0;
glyph::LayoutDesc g_layout = {0, 0, 0};

PatternCreateFn g_orig_pattern = nullptr;
ShowGlyphsFn g_orig_glyphs = nullptr;
SetRgbaFn g_orig_rgba = nullptr;

IntOfPtrFn g_surface_get_type = nullptr;
IntOfPtrFn g_image_get_format = nullptr;
IntOfPtrFn g_image_get_width = nullptr;
IntOfPtrFn g_image_get_height = nullptr;
IntOfPtrFn g_image_get_stride = nullptr;
DataFn g_image_get_data = nullptr;
PtrOfPtrFn g_get_target = nullptr;
PtrOfPtrFn g_get_source = nullptr;
IntOfPtrFn g_pattern_get_type = nullptr;
GetRgbaFn g_pattern_get_rgba = nullptr;

uint8_t g_hook_state[snap::kHooks];
uint8_t g_hook_patch[snap::kHooks];
uint32_t g_getters_missing = 0;

// ---------------------------------------------------------------------------
// Таблицы (.bss, фиксированного размера)

const uint32_t kPages = 1024;
const uint32_t kPtrs = 8192;
const uint16_t kNoPage = 0xFFFF;

std::atomic<uint32_t> g_cnt[snap::kCounters];

probe::KeyIndex<kPages> g_pages;  // ключ — страница 64 КБ вызывающего + 1

probe::KeyIndex<snap::kSurfaces> g_surf;  // ключ — хеш пикселей (0 → 1)
// meta: w | h << 8 | fmt << 16 | page << 20 (12 бит, 0xFFF — нет) | first_ms << 32
std::atomic<uint64_t> g_surf_meta[snap::kSurfaces];
std::atomic<uint32_t> g_surf_hits[snap::kSurfaces];

probe::KeyIndex<kPtrs> g_ptrs;  // указатели поверхностей: только признак «новый»

// Ключ цветов: rgba | (страница + 1) << 32.
probe::KeyIndex<snap::kColors> g_colors;
std::atomic<uint32_t> g_color_runs[snap::kColors];
std::atomic<uint32_t> g_color_glyphs[snap::kColors];
std::atomic<uint32_t> g_color_first[snap::kColors];

probe::KeyIndex<snap::kAlpha> g_alpha;
std::atomic<uint32_t> g_alpha_calls[snap::kAlpha];

probe::Timeline g_timeline;
probe::PairRing g_ring;
probe::DupHist g_dup;
probe::OwnerSlots g_frame_slots;
probe::FrameStats g_frame;
glyph::SelfCheck g_self;

}  // namespace

// Снимок для загрузчика (экспорт DATA, .bss): пишет только DtDump под g_lock.
extern "C" {
snap::ProbeSnapshot DtSnapshot;
}

namespace {

inline void bump(uint32_t c) { g_cnt[c].fetch_add(1, std::memory_order_relaxed); }
inline void add(uint32_t c, uint32_t v) { g_cnt[c].fetch_add(v, std::memory_order_relaxed); }

uint64_t qpc() {
  LARGE_INTEGER li;
  QueryPerformanceCounter(&li);
  return static_cast<uint64_t>(li.QuadPart);
}

uint32_t ms_since_start(uint64_t q) { return probe::sat32(probe::ticks_to_ms(q - g_t0, g_freq)); }

void timeline_add(uint32_t ms, probe::Timeline::Field f) {
  if (!g_timeline.add(ms, f)) bump(snap::kTimelineOverflow);
}

uint16_t page_index(void* ret) {
  uint64_t page = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ret)) >> 16;
  bool inserted = false;
  uint32_t i = g_pages.insert(page + 1, &inserted);
  if (i == probe::kNone) {
    bump(snap::kPagesFull);
    return kNoPage;
  }
  return static_cast<uint16_t>(i);
}

uint64_t color_key(uint32_t rgba, uint16_t page) {
  return static_cast<uint64_t>(rgba) | (static_cast<uint64_t>(static_cast<uint32_t>(page) + 1) << 32);
}

// ---------------------------------------------------------------------------
// Сбор (только при g_active)

void collect_pattern(void* surface, void* ret, uint64_t q) {
  uint32_t ms = ms_since_start(q);
  bump(snap::kPatternCalls);
  timeline_add(ms, probe::Timeline::kPatternCalls);
  if (surface == nullptr) {
    bump(snap::kSurfNull);
    return;
  }
  bool inserted = false;
  if (g_ptrs.insert(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(surface)), &inserted) == probe::kNone) {
    bump(snap::kPtrFull);
  } else if (inserted) {
    bump(snap::kNewPtr);
    timeline_add(ms, probe::Timeline::kNewPtr);
  }
  if (!(g_features & snap::kFeatSurfHash)) return;
  // Геттеры image-поверхности — строго после проверки типа.
  if (g_surface_get_type(surface) != kCairoSurfaceImage) {
    bump(snap::kSurfNotImage);
    return;
  }
  int fmt = g_image_get_format(surface);
  if (fmt != kCairoFormatArgb32) {
    bump(snap::kSurfNotArgb32);
    return;
  }
  int w = g_image_get_width(surface);
  int h = g_image_get_height(surface);
  if (w < static_cast<int>(kMinIcon) || w > static_cast<int>(kMaxIcon) || h < static_cast<int>(kMinIcon) ||
      h > static_cast<int>(kMaxIcon)) {
    bump(snap::kSurfSize);
    return;
  }
  const unsigned char* data = g_image_get_data(surface);
  if (data == nullptr) {
    bump(snap::kSurfNoData);
    return;
  }
  int stride = g_image_get_stride(surface);
  uint64_t hv = 0;
  if (stride < 4 * w ||
      !hash_argb32(data, static_cast<uint32_t>(w), static_cast<uint32_t>(h), static_cast<uint32_t>(stride), &hv)) {
    bump(snap::kSurfStride);
    return;
  }
  bump(snap::kSurfHashed);
  uint32_t i = g_surf.insert(hv != 0 ? hv : 1, &inserted);
  if (i == probe::kNone) {
    bump(snap::kSurfFull);
    return;
  }
  if (inserted) {
    uint16_t page = page_index(ret);
    uint64_t p12 = page == kNoPage ? 0xFFFu : (page & 0xFFFu);
    uint64_t meta = static_cast<uint64_t>(w) | (static_cast<uint64_t>(h) << 8) |
                    (static_cast<uint64_t>(fmt & 0xF) << 16) | (p12 << 20) | (static_cast<uint64_t>(ms) << 32);
    g_surf_meta[i].store(meta, std::memory_order_release);
    bump(snap::kNewHash);
    timeline_add(ms, probe::Timeline::kNewHash);
  }
  g_surf_hits[i].fetch_add(1, std::memory_order_relaxed);
}

// Цвет источника cr: истина — SOLID и get_rgba успешен.
bool source_color(void* cr, uint32_t* rgba) {
  void* src = g_get_source(cr);
  if (src == nullptr || g_pattern_get_type(src) != kCairoPatternSolid) {
    bump(snap::kSourceNonsolid);
    return false;
  }
  double r = 0, g = 0, b = 0, a = 0;
  if (g_pattern_get_rgba(src, &r, &g, &b, &a) != kCairoStatusSuccess) {
    bump(snap::kColorError);
    return false;
  }
  bool inexact = false;
  *rgba = probe::rgba8(r, g, b, a, &inexact);
  if (inexact) bump(snap::kColorInexact);
  return true;
}

void collect_glyphs(void* cr, const void* glyphs, int num, void* ret, uint64_t q) {
  uint32_t ms = ms_since_start(q);
  bump(snap::kGlyphCalls);
  timeline_add(ms, probe::Timeline::kGlyphRuns);
  if (glyphs == nullptr || num <= 0) {
    bump(snap::kGlyphEmpty);
    return;
  }
  const uint32_t n = static_cast<uint32_t>(num);
  add(snap::kGlyphs, n);
  const uint32_t nn = n > kMaxCheckGlyphs ? kMaxCheckGlyphs : n;

  // Раскладка: обе гипотезы, читается не больше 20·nn байт.
  if (nn >= 2) {
    bump(snap::kLayoutRuns);
    const size_t readable = static_cast<size_t>(nn) * 20;
    glyph::Verdict v24 = glyph::check_run(glyphs, nn, readable, glyph::kStride24);
    glyph::Verdict v20 = glyph::check_run(glyphs, nn, readable, glyph::kStride20);
    if (v24 == glyph::kPlausible) bump(snap::kS24Plausible);
    if (v24 == glyph::kImplausible) bump(snap::kS24Implausible);
    if (v20 == glyph::kPlausible) bump(snap::kS20Plausible);
    if (v20 == glyph::kImplausible) bump(snap::kS20Implausible);
    if (v24 == glyph::kPlausible && v20 == glyph::kImplausible) bump(snap::kDecisive24);
    if (v20 == glyph::kPlausible && v24 == glyph::kImplausible) bump(snap::kDecisive20);
  }
  glyph::SelfCheck::State st = g_self.feed(glyphs, nn);

  uint32_t rgba = 0;
  bool has_color = false;
  if (g_features & snap::kFeatTextColor) {
    has_color = source_color(cr, &rgba);
    if (has_color) {
      bool inserted = false;
      uint32_t i = g_colors.insert(color_key(rgba, page_index(ret)), &inserted);
      if (i == probe::kNone) {
        bump(snap::kColorsFull);
      } else {
        if (inserted) g_color_first[i].store(ms, std::memory_order_relaxed);
        g_color_runs[i].fetch_add(1, std::memory_order_relaxed);
        g_color_glyphs[i].fetch_add(n, std::memory_order_relaxed);
      }
    }
  }

  // Пары дублей — только после пройденной самопроверки и с её шагом: до неё
  // шаг 24 при настоящем 20 вывел бы чтение за массив.
  if (!(g_features & snap::kFeatDup) || st != glyph::SelfCheck::kPassed) return;
  bump(snap::kDupRuns);
  probe::RunRec rec;
  rec.ghash = dup::glyph_index_hash(glyphs, nn, g_layout.stride);
  rec.count = n;
  rec.rgba = rgba;
  rec.has_color = has_color ? 1 : 0;
  rec.tid = GetCurrentThreadId();
  rec.target = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_get_target(cr)));
  const uint8_t* g0 = static_cast<const uint8_t*>(glyphs);
  memcpy(&rec.x, g0 + g_layout.x_off, sizeof(double));
  memcpy(&rec.y, g0 + g_layout.y_off, sizeof(double));
  rec.ticks = q;
  probe::RunRec prev;
  uint32_t torn = 0;
  bool paired = g_ring.push_and_match(rec, &prev, &torn);
  if (torn != 0) add(snap::kDupTorn, torn);
  if (!paired) return;
  bump(snap::kDupPairs);
  int32_t bin = probe::dxdy_bin(rec.x - prev.x, rec.y - prev.y);
  if (bin < 0) {
    bump(snap::kDupFar);
    return;
  }
  uint64_t dt = rec.ticks >= prev.ticks ? rec.ticks - prev.ticks : 0;
  g_dup.add(static_cast<uint32_t>(bin), probe::pair_flags(rec, prev), probe::ticks_to_us(dt, g_freq));
}

void collect_rgba(double r, double g, double b, double a, void* ret) {
  bump(snap::kRgbaCalls);
  if (a >= 1.0) return;
  bump(snap::kRgbaAlphaLt1);  // NaN — тоже сюда
  if (a == 0.0) bump(snap::kRgbaAlphaZero);
  bool inexact = false;
  uint32_t rgba = probe::rgba8(r, g, b, a, &inexact);
  bool inserted = false;
  uint32_t i = g_alpha.insert(color_key(rgba, page_index(ret)), &inserted);
  if (i == probe::kNone) {
    bump(snap::kAlphaFull);
    return;
  }
  g_alpha_calls[i].fetch_add(1, std::memory_order_relaxed);
}

// Пачка («кадр»): накладные хука — до и после исходной функции.
void frame_step(uint64_t q_in, uint64_t q0, uint64_t q1) {
  uint64_t q_out = qpc();
  int32_t slot = g_frame_slots.claim(GetCurrentThreadId());
  if (slot < 0) {
    bump(snap::kFrameSlotBusy);
    return;
  }
  probe::BurstClosed c;
  uint64_t overhead = (q0 - q_in) + (q_out - q1);
  if (probe::burst_step(g_frame_slots.state(slot), q_in, q_out, overhead, q1 - q0, g_gap_ticks, &c)) {
    g_frame.add(probe::ticks_to_us(c.base, g_freq), probe::ticks_to_us(c.pause, g_freq), c.overhead, c.orig);
  }
}

// ---------------------------------------------------------------------------
// Хуки: inflight.enter → при g_active снять данные → исходная функция с теми же
// аргументами → leave. Возвращаемое значение не меняется.

__declspec(noinline) void* __cdecl hook_pattern_create_for_surface(void* surface) {
  uint32_t slot = g_inflight.enter();
  void* ret = _ReturnAddress();
  void* r;
  if (g_active) {
    uint64_t q_in = qpc();
    collect_pattern(surface, ret, q_in);
    uint64_t q0 = qpc();
    r = g_orig_pattern(surface);
    uint64_t q1 = qpc();
    frame_step(q_in, q0, q1);
  } else {
    r = g_orig_pattern(surface);
  }
  g_inflight.leave(slot);
  return r;
}

__declspec(noinline) void __cdecl hook_show_glyphs(void* cr, const void* glyphs, int num_glyphs) {
  uint32_t slot = g_inflight.enter();
  void* ret = _ReturnAddress();
  if (g_active) {
    uint64_t q_in = qpc();
    collect_glyphs(cr, glyphs, num_glyphs, ret, q_in);
    uint64_t q0 = qpc();
    g_orig_glyphs(cr, glyphs, num_glyphs);
    uint64_t q1 = qpc();
    frame_step(q_in, q0, q1);
  } else {
    g_orig_glyphs(cr, glyphs, num_glyphs);
  }
  g_inflight.leave(slot);
}

__declspec(noinline) void __cdecl hook_set_source_rgba(void* cr, double r, double g, double b, double a) {
  uint32_t slot = g_inflight.enter();
  void* ret = _ReturnAddress();
  if (g_active) {
    uint64_t q_in = qpc();
    collect_rgba(r, g, b, a, ret);
    uint64_t q0 = qpc();
    g_orig_rgba(cr, r, g, b, a);
    uint64_t q1 = qpc();
    frame_step(q_in, q0, q1);
  } else {
    g_orig_rgba(cr, r, g, b, a);
  }
  g_inflight.leave(slot);
}

// ---------------------------------------------------------------------------
// Жизненный цикл

// out — не меньше 32 символов: префикс 14 + до 10 цифр + нуль.
void guard_name(wchar_t* out) {
  const wchar_t prefix[] = L"Local\\cfgdark-";
  int n = 0;
  for (; prefix[n]; ++n) out[n] = prefix[n];
  wchar_t digits[10];
  int d = 0;
  DWORD pid = GetCurrentProcessId();
  do {
    digits[d++] = static_cast<wchar_t>(L'0' + pid % 10);
    pid /= 10;
  } while (pid != 0 && d < 10);
  while (d > 0) out[n++] = digits[--d];
  out[n] = 0;
}

const wchar_t* base_name(const wchar_t* path) {
  const wchar_t* b = path;
  for (const wchar_t* p = path; *p; ++p) {
    if (*p == L'\\' || *p == L'/') b = p + 1;
  }
  return b;
}

bool process_is_1cv8() {
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;
  path[n] = 0;
  return CompareStringOrdinal(base_name(path), -1, L"1cv8.exe", -1, TRUE) == CSTR_EQUAL;
}

template <typename Fn>
void resolve(HMODULE m, const char* name, snap::Getter bit, Fn* out) {
  *out = reinterpret_cast<Fn>(GetProcAddress(m, name));
  if (*out == nullptr) g_getters_missing |= 1u << bit;
}

void close_guard() {
  if (g_guard != nullptr) {
    CloseHandle(g_guard);
    g_guard = nullptr;
  }
}

// Откат после неудачного DtStart: хуки не активны, но поток мог войти в хук и
// вызвать исходную функцию — ждём его, прежде чем загрузчик выгрузит DLL.
DWORD abort_start(DWORD code) {
  g_engine.unpatch();
  if (g_inflight.wait_idle(3000)) Sleep(100);
  if (g_cairo_pin != nullptr) {
    FreeLibrary(g_cairo_pin);
    g_cairo_pin = nullptr;
  }
  close_guard();
  return code;
}

DWORD start_locked() {
  wchar_t name[32];
  guard_name(name);
  HANDLE h = CreateMutexW(nullptr, FALSE, name);
  if (h == nullptr) return kStartError;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(h);
    return kStartAlready;
  }
  g_guard = h;

  if (!process_is_1cv8()) {
    close_guard();
    return kStartNoTarget;
  }
  if (!GetModuleHandleExW(0, L"cairo.dll", &g_cairo_pin)) {
    g_cairo_pin = nullptr;
    close_guard();
    return kStartNoTarget;
  }

  // Функции только на чтение — до патча EAT, поэтому это исходные адреса.
  g_getters_missing = 0;
  resolve(g_cairo_pin, "cairo_surface_get_type", snap::kGetSurfaceType, &g_surface_get_type);
  resolve(g_cairo_pin, "cairo_image_surface_get_format", snap::kGetFormat, &g_image_get_format);
  resolve(g_cairo_pin, "cairo_image_surface_get_width", snap::kGetWidth, &g_image_get_width);
  resolve(g_cairo_pin, "cairo_image_surface_get_height", snap::kGetHeight, &g_image_get_height);
  resolve(g_cairo_pin, "cairo_image_surface_get_stride", snap::kGetStride, &g_image_get_stride);
  resolve(g_cairo_pin, "cairo_image_surface_get_data", snap::kGetData, &g_image_get_data);
  resolve(g_cairo_pin, "cairo_get_target", snap::kGetTarget, &g_get_target);
  resolve(g_cairo_pin, "cairo_get_source", snap::kGetSource, &g_get_source);
  resolve(g_cairo_pin, "cairo_pattern_get_type", snap::kGetPatternType, &g_pattern_get_type);
  resolve(g_cairo_pin, "cairo_pattern_get_rgba", snap::kGetRgba, &g_pattern_get_rgba);
  const uint32_t surf_bits = (1u << snap::kGetSurfaceType) | (1u << snap::kGetFormat) | (1u << snap::kGetWidth) |
                             (1u << snap::kGetHeight) | (1u << snap::kGetStride) | (1u << snap::kGetData);
  const uint32_t color_bits = (1u << snap::kGetSource) | (1u << snap::kGetPatternType) | (1u << snap::kGetRgba);
  uint32_t features = 0;
  if (!(g_getters_missing & surf_bits)) features |= snap::kFeatSurfHash;
  if (!(g_getters_missing & color_bits)) features |= snap::kFeatTextColor;
  if ((features & snap::kFeatTextColor) && !(g_getters_missing & (1u << snap::kGetTarget))) {
    features |= snap::kFeatDup;
  }
  g_features = features;

  g_engine.init(g_instance);
  struct Spec {
    void* hook;
    void** orig;
  };
  const Spec specs[snap::kHooks] = {
      {reinterpret_cast<void*>(&hook_pattern_create_for_surface), reinterpret_cast<void**>(&g_orig_pattern)},
      {reinterpret_cast<void*>(&hook_show_glyphs), reinterpret_cast<void**>(&g_orig_glyphs)},
      {reinterpret_cast<void*>(&hook_set_source_rgba), reinterpret_cast<void**>(&g_orig_rgba)},
  };
  uint32_t installed = 0;
  for (uint32_t k = 0; k < snap::kHooks; ++k) {
    patch::Status s = g_engine.hook_export(g_cairo_pin, snap::kHookNames[k], specs[k].hook, specs[k].orig);
    g_hook_patch[k] = static_cast<uint8_t>(s);
    if (s == patch::kOk) {
      g_hook_state[k] = snap::kHookOk;
      ++installed;
    } else if (s == patch::kErrNotFound || s == patch::kErrForwarder) {
      g_hook_state[k] = snap::kHookAbsent;
    } else {
      g_hook_state[k] = snap::kHookFailed;
      return abort_start((s == patch::kErrProtect || s == patch::kErrNoStubSpace) ? kStartNoStubs : kStartPatch);
    }
  }
  if (installed == 0) return abort_start(kStartNoTarget);
  if (g_engine.patch_iats() != patch::kOk) return abort_start(kStartPatch);

  g_self.init(glyph::compiled_layout(), 1000, 20);
  g_layout = glyph::describe(glyph::compiled_layout());
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_freq = static_cast<uint64_t>(f.QuadPart);
  g_gap_ticks = g_freq * snap::kBurstGapMs / 1000;
  g_t0 = qpc();
  g_epoch = 0;
  g_prev_ms = 0;
  InterlockedExchange(&g_active, 1);
  return kStartOk;
}

// ---------------------------------------------------------------------------
// Снимок

// Имя модуля по странице вызывающего → индекс в DtSnapshot.names.
uint8_t caller_name(uint64_t page) {
  snap::ProbeSnapshot& s = DtSnapshot;
  HMODULE m = nullptr;
  const void* addr = reinterpret_cast<const void*>(static_cast<uintptr_t>(page << 16));
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(addr), &m)) {
    return snap::kCallerUnloaded;
  }
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return snap::kCallerUnloaded;
  path[n] = 0;
  const wchar_t* b = base_name(path);
  wchar_t name[snap::kNameLen];
  uint32_t len = 0;
  for (; b[len] && len + 1 < snap::kNameLen; ++len) name[len] = b[len];
  name[len] = 0;
  for (uint32_t i = 0; i < s.nnames; ++i) {
    if (CompareStringOrdinal(name, -1, reinterpret_cast<const wchar_t*>(s.names[i]), -1, TRUE) == CSTR_EQUAL) {
      return static_cast<uint8_t>(i);
    }
  }
  if (s.nnames >= snap::kNames) {
    ++s.counters[snap::kNamesFull];
    return snap::kCallerOther;
  }
  uint32_t i = s.nnames++;
  for (uint32_t k = 0; k <= len; ++k) s.names[i][k] = static_cast<uint16_t>(name[k]);
  return static_cast<uint8_t>(i);
}

uint8_t g_page_caller[kPages];

uint8_t caller_of(uint32_t page_idx) {
  return page_idx < kPages ? g_page_caller[page_idx] : snap::kCallerUnknown;
}

void dump_locked() {
  snap::ProbeSnapshot& s = DtSnapshot;
  const uint64_t now = qpc();
  memset(&s, 0, sizeof(s));

  for (uint32_t c = 0; c < snap::kCounters; ++c) {
    s.counters[c] = c < snap::kEpochCounters ? g_cnt[c].exchange(0, std::memory_order_relaxed)
                                             : g_cnt[c].load(std::memory_order_relaxed);
  }
  s.counters[snap::kNamesFull] = 0;

  for (uint32_t i = 0; i < kPages; ++i) {
    uint64_t key = g_pages.key_at(i);
    g_page_caller[i] = key == 0 ? snap::kCallerUnknown : caller_name(key - 1);
  }

  for (uint32_t i = 0; i < snap::kSurfaces; ++i) {
    uint64_t key = g_surf.key_at(i);
    if (key == 0) continue;
    uint64_t meta = g_surf_meta[i].load(std::memory_order_acquire);
    snap::SnapSurface& o = s.surfaces[s.nsurfaces++];
    o.hash = key;
    o.w = static_cast<uint8_t>(meta);
    o.h = static_cast<uint8_t>(meta >> 8);
    o.fmt = static_cast<uint8_t>((meta >> 16) & 0xF);
    uint32_t p12 = static_cast<uint32_t>((meta >> 20) & 0xFFF);
    o.caller = meta == 0 ? snap::kCallerUnknown : caller_of(p12 == 0xFFF ? kPages : p12);
    o.first_ms = static_cast<uint32_t>(meta >> 32);
    o.hits = g_surf_hits[i].exchange(0, std::memory_order_relaxed);
  }
  for (uint32_t i = 0; i < snap::kColors; ++i) {
    uint64_t key = g_colors.key_at(i);
    if (key == 0) continue;
    snap::SnapColor& o = s.colors[s.ncolors++];
    o.rgba = static_cast<uint32_t>(key);
    o.caller = caller_of(static_cast<uint32_t>(key >> 32) - 1);
    o.first_ms = g_color_first[i].load(std::memory_order_relaxed);
    o.runs = g_color_runs[i].exchange(0, std::memory_order_relaxed);
    o.glyphs = g_color_glyphs[i].exchange(0, std::memory_order_relaxed);
  }
  for (uint32_t i = 0; i < snap::kAlpha; ++i) {
    uint64_t key = g_alpha.key_at(i);
    if (key == 0) continue;
    snap::SnapAlpha& o = s.alpha[s.nalpha++];
    o.rgba = static_cast<uint32_t>(key);
    o.caller = caller_of(static_cast<uint32_t>(key >> 32) - 1);
    o.calls = g_alpha_calls[i].exchange(0, std::memory_order_relaxed);
  }

  g_dup.take(s.dup_bins, s.dup_dt);
  probe::FrameStats::Copy fc;
  g_frame.take(&fc);
  memcpy(s.frame_hist, fc.hist, sizeof(s.frame_hist));
  memcpy(s.frame_pause, fc.pause, sizeof(s.frame_pause));
  s.frame_count = fc.count;
  s.frame_max_us = fc.max_us;
  s.frame_sum_us = fc.sum_us;
  s.frame_overhead_ticks = fc.overhead_ticks;
  s.frame_orig_ticks = fc.orig_ticks;

  for (uint32_t b = 0; b < snap::kBuckets; ++b) {
    for (uint32_t f = 0; f < snap::kTimelineFields; ++f) s.timeline[b][f] = g_timeline.get(b, f);
  }

  for (uint32_t k = 0; k < snap::kHooks; ++k) {
    s.hook_state[k] = g_hook_state[k];
    s.hook_patch[k] = g_hook_patch[k];
  }
  s.self_check = static_cast<uint8_t>(g_self.state());
  s.self_check_seen = g_self.seen();
  s.compiled_stride = static_cast<uint8_t>(g_layout.stride);
  s.features = g_features;
  s.getters_missing = g_getters_missing;
  const patch::Stats& ps = g_engine.stats();
  memcpy(s.patch, &ps, sizeof(ps));
  s.patch[snap::kPatchStats - 1] = g_engine.journal_size();

  s.epoch = ++g_epoch;
  s.t_ms = ms_since_start(now);
  s.t_prev_ms = g_prev_ms;
  g_prev_ms = s.t_ms;
  s.qpc_freq = g_freq;
#if defined(_WIN64)
  s.machine = IMAGE_FILE_MACHINE_AMD64;
#else
  s.machine = IMAGE_FILE_MACHINE_I386;
#endif
  s.size = sizeof(s);
  s.version = snap::kVersion;
  s.magic = snap::kMagic;
}

}  // namespace

// DllMain — под loader lock: на attach только HINSTANCE, DisableThreadLibraryCalls и
// критическая секция; на detach с reserved == NULL — аварийный откат патчей без
// ожидания (если DtStop не вызывали), закрыть объект защиты, удалить критическую
// секцию. g_lock не берётся; файлов и ожиданий нет.
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_instance = instance;
    DisableThreadLibraryCalls(instance);
    InitializeCriticalSection(&g_lock);
    g_lock_ready = true;
  } else if (reason == DLL_PROCESS_DETACH) {
    // reserved != NULL — процесс завершается: ничего не делать.
    if (reserved == nullptr && g_lock_ready) {
      if (g_active) {
        InterlockedExchange(&g_active, 0);
        g_engine.unpatch();
      }
      close_guard();
      DeleteCriticalSection(&g_lock);
      g_lock_ready = false;
    }
  }
  return TRUE;
}

// 0 — хуки стоят; 1 — тема или probe уже активны; 2 — общая ошибка; 3 — не 1cv8.exe,
// нет cairo.dll или ни одного хука; 4 — стабы запрещены; 5 — ошибка патча.
extern "C" DWORD WINAPI DtStart(LPVOID) {
  EnterCriticalSection(&g_lock);
  DWORD rc = g_guard != nullptr ? kStartAlready : start_locked();
  LeaveCriticalSection(&g_lock);
  return rc;
}

// 0 — хуки сняты, защита освобождена; 1 — не дождались выхода потоков из хуков
// (защита и ссылка на cairo остаются, повторный вызов безопасен).
extern "C" DWORD WINAPI DtStop(LPVOID) {
  EnterCriticalSection(&g_lock);
  DWORD rc = 0;
  if (g_guard != nullptr) {
    InterlockedExchange(&g_active, 0);
    g_engine.unpatch();
    if (g_inflight.wait_idle(3000)) {
      Sleep(100);  // поток мог пройти стаб, но ещё не войти в счётчик
      if (g_cairo_pin != nullptr) {
        FreeLibrary(g_cairo_pin);
        g_cairo_pin = nullptr;
      }
      close_guard();
    } else {
      rc = 1;
    }
  }
  LeaveCriticalSection(&g_lock);
  return rc;
}

// Копирует таблицы в DtSnapshot и обнуляет счётчики эпохи. 0 — готово; 1 — probe не
// активен (снимок не тронут); 2 — ошибка.
extern "C" DWORD WINAPI DtDump(LPVOID) {
  if (!g_lock_ready) return kDumpError;
  EnterCriticalSection(&g_lock);
  DWORD rc = kDumpInactive;
  if (g_active) {
    dump_locked();
    rc = kDumpOk;
  }
  LeaveCriticalSection(&g_lock);
  return rc;
}
