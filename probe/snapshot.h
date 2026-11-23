// SPDX-License-Identifier: MIT
// Снимок probe (M3b): POD-структура, общая для DLL probe и загрузчика. DtDump под
// g_lock копирует живые таблицы в экспорт DtSnapshot (DATA); загрузчик читает его
// ReadProcessMemory по RVA из файла и печатает JSON. Только типы фиксированной
// ширины; загрузчик и DLL одной битности, поэтому раскладка совпадает. Наружу не
// попадают текст, индексы глифов и их хеши, пиксели, адреса и пути (только basename).
#pragma once
#include <stdint.h>

namespace dt {
namespace snap {

const uint32_t kMagic = 0x53504443;  // "CDPS"
const uint32_t kVersion = 1;

const uint32_t kSurfaces = 2048;
const uint32_t kColors = 512;
const uint32_t kAlpha = 256;
const uint32_t kNames = 64;
const uint32_t kNameLen = 64;
const uint32_t kBuckets = 4096;
const uint32_t kBucketMs = 250;
const uint32_t kTimelineFields = 4;  // new_ptr, new_hash, pattern_calls, glyph_runs
const uint32_t kDupBins = 15 * 15 * 8;
const uint32_t kDtBins = 16;
const uint32_t kFrameBins = 257;  // 256 по 250 мкс + переполнение
const uint32_t kFrameBinUs = 250;
const uint32_t kPauseBins = 24;
const uint32_t kBurstGapMs = 4;

// Вызывающий: индекс в names или особые значения.
const uint8_t kCallerOther = 253;     // таблица имён полна
const uint8_t kCallerUnloaded = 254;  // по адресу нет модуля (выгружен или не образ)
const uint8_t kCallerUnknown = 255;   // таблица страниц вызывающих полна

enum Hook : uint32_t { kHookPattern = 0, kHookGlyphs, kHookRgba, kHooks };
static const char* const kHookNames[kHooks] = {
    "cairo_pattern_create_for_surface",
    "cairo_show_glyphs",
    "cairo_set_source_rgba",
};

enum HookState : uint8_t { kHookOff = 0, kHookOk, kHookAbsent, kHookFailed };
static const char* const kHookStateNames[4] = {"off", "ok", "absent", "failed"};

// Имена patch::Status по значению (probe.cpp сверяет число значений static_assert).
const uint32_t kPatchStatusCount = 12;
static const char* const kPatchStatusNames[kPatchStatusCount] = {
    "ok", "bad_image", "not_found", "forwarder", "already", "too_many_hooks",
    "journal_full", "no_stub_space", "rva", "protect", "race", "modules",
};

// Поля patch::Stats в том же порядке + размер журнала.
const uint32_t kPatchStats = 11;
static const char* const kPatchStatNames[kPatchStats] = {
    "eat_patched", "iat_patched", "iat_delay_patched", "iat_restored", "eat_restored", "second_pass",
    "mismatched",  "module_gone", "journal_full",      "regions",      "journal",
};

// Функции cairo только на чтение (GetProcAddress в DtStart); бит — функции нет.
enum Getter : uint32_t {
  kGetSurfaceType = 0,
  kGetFormat,
  kGetWidth,
  kGetHeight,
  kGetStride,
  kGetData,
  kGetTarget,
  kGetSource,
  kGetPatternType,
  kGetRgba,
  kGetters
};
static const char* const kGetterNames[kGetters] = {
    "cairo_surface_get_type",      "cairo_image_surface_get_format", "cairo_image_surface_get_width",
    "cairo_image_surface_get_height", "cairo_image_surface_get_stride", "cairo_image_surface_get_data",
    "cairo_get_target",            "cairo_get_source",               "cairo_pattern_get_type",
    "cairo_pattern_get_rgba",
};

// Части сбора, включённые при наличии нужных функций.
enum Feature : uint32_t { kFeatSurfHash = 1, kFeatTextColor = 2, kFeatDup = 4 };

// Счётчики. [0, kEpochCounters) — эпоха (DtDump обнуляет), дальше — постоянные.
enum Counter : uint32_t {
  kPatternCalls = 0,
  kSurfNull,
  kSurfNotImage,
  kSurfNotArgb32,
  kSurfSize,
  kSurfNoData,
  kSurfStride,
  kSurfHashed,
  kNewPtr,
  kNewHash,
  kGlyphCalls,
  kGlyphEmpty,
  kGlyphs,
  kSourceNonsolid,
  kColorInexact,
  kColorError,
  kRgbaCalls,
  kRgbaAlphaLt1,
  kRgbaAlphaZero,
  kDupRuns,
  kDupPairs,
  kDupFar,
  kDupTorn,
  kEpochCounters,
  // переполнения (не сбрасываются)
  kSurfFull = kEpochCounters,
  kPtrFull,
  kColorsFull,
  kAlphaFull,
  kPagesFull,
  kTimelineOverflow,
  kFrameSlotBusy,
  kNamesFull,  // заполняет DtDump: страниц, отнесённых к "other"
  kOverflowEnd,
  // раскладка глифа (не сбрасывается)
  kLayoutRuns = kOverflowEnd,
  kS24Plausible,
  kS24Implausible,
  kS20Plausible,
  kS20Implausible,
  kDecisive24,
  kDecisive20,
  kCounters
};
static const char* const kCounterNames[kCounters] = {
    "pattern_calls", "surf_null", "surf_not_image", "surf_not_argb32", "surf_size_out", "surf_no_data",
    "surf_bad_stride", "surf_hashed", "new_ptr", "new_hash", "glyph_calls", "glyph_empty", "glyphs",
    "source_nonsolid", "color_inexact", "color_error", "rgba_calls", "rgba_alpha_lt1", "rgba_alpha_zero",
    "dup_runs", "dup_pairs", "dup_far", "dup_torn",
    "surf_full", "ptr_full", "colors_full", "alpha_full", "pages_full", "timeline_overflow", "frame_slot_busy",
    "names_full",
    "runs", "s24_plausible", "s24_implausible", "s20_plausible", "s20_implausible", "decisive24", "decisive20",
};

enum SelfCheckState : uint8_t { kSelfPending = 0, kSelfPassed, kSelfFailed };
static const char* const kSelfCheckNames[3] = {"pending", "passed", "failed"};

struct SnapSurface {
  uint64_t hash;  // fnv1a64-argb32-v1
  uint32_t first_ms;
  uint32_t hits;  // за эпоху
  uint8_t w;
  uint8_t h;
  uint8_t fmt;
  uint8_t caller;
  uint32_t reserved;
};

struct SnapColor {
  uint32_t rgba;  // 0xRRGGBBAA
  uint32_t runs;  // за эпоху
  uint32_t glyphs;  // за эпоху
  uint32_t first_ms;
  uint8_t caller;
  uint8_t reserved[3];
};

struct SnapAlpha {
  uint32_t rgba;
  uint32_t calls;  // за эпоху
  uint8_t caller;
  uint8_t reserved[3];
};

struct ProbeSnapshot {
  uint32_t magic;
  uint32_t version;
  uint32_t size;  // sizeof(ProbeSnapshot)
  uint16_t machine;  // IMAGE_FILE_MACHINE_*
  uint16_t reserved0;
  uint32_t epoch;      // номер дампа, с 1
  uint32_t t_ms;       // мс от DtStart
  uint32_t t_prev_ms;  // t_ms предыдущего дампа (0 — первый)
  uint32_t reserved1;
  uint64_t qpc_freq;

  uint8_t hook_state[kHooks];  // HookState
  uint8_t hook_patch[kHooks];  // patch::Status последней попытки
  uint8_t self_check;          // SelfCheckState
  uint8_t compiled_stride;     // шаг cairo_glyph_t, с которым собран probe
  uint32_t self_check_seen;
  uint32_t features;         // Feature
  uint32_t getters_missing;  // биты Getter

  uint32_t patch[kPatchStats];
  uint32_t counters[kCounters];

  uint32_t nsurfaces;
  uint32_t ncolors;
  uint32_t nalpha;
  uint32_t nnames;
  SnapSurface surfaces[kSurfaces];
  SnapColor colors[kColors];
  SnapAlpha alpha[kAlpha];
  uint16_t names[kNames][kNameLen];  // basename модуля, UTF-16, с нулём

  uint32_t dup_bins[kDupBins];  // [(dy+7)*15 + (dx+7)] * 8 + флаги (поток 1, поверхность 2, цвет 4)
  uint32_t dup_dt[kDtBins];     // Δt пары, log2 мкс

  uint32_t frame_hist[kFrameBins];
  uint32_t frame_pause[kPauseBins];  // паузы между пачками, log2 мкс
  uint32_t frame_count;
  uint32_t frame_max_us;
  uint64_t frame_sum_us;
  uint64_t frame_overhead_ticks;
  uint64_t frame_orig_ticks;

  uint32_t timeline[kBuckets][kTimelineFields];
};

}  // namespace snap
}  // namespace dt
