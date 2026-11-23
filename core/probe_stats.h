// SPDX-License-Identifier: MIT
// Чистая логика probe (M3b), без WinAPI: таблица ключей с открытой адресацией,
// шкала времени, пачки вызовов («кадр»), бины dx/dy и log2, кольцо прогонов для
// пар дублей. Всё — фиксированного размера, без выделения памяти и блокировок:
// только атомарные операции (на x86 64-битные — через CAS8B).
#pragma once
#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace dt {
namespace probe {

static_assert(std::atomic<uint64_t>::is_always_lock_free, "probe: 64-bit atomics must be lock-free");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "probe: 32-bit atomics must be lock-free");

uint64_t mix64(uint64_t x);

// ---------------------------------------------------------------------------
// Таблица ключей: открытая адресация, линейное пробирование не дальше kMaxProbe
// шагов. Ключ 0 — пустой слот (вызывающий отображает 0 в 1). Слот занимает тот,
// чей CAS 0 → key прошёл; удаления нет.

const uint32_t kNone = 0xFFFFFFFFu;
const uint32_t kMaxProbe = 32;

template <uint32_t N>
class KeyIndex {
  static_assert(N >= 2 && (N & (N - 1)) == 0, "KeyIndex: N — степень двойки");

 public:
  // Индекс слота ключа или kNone (пробирование исчерпано). *inserted — ключ занял
  // слот этим вызовом.
  uint32_t insert(uint64_t key, bool* inserted) {
    *inserted = false;
    if (key == 0) return kNone;
    uint32_t i = static_cast<uint32_t>(mix64(key)) & (N - 1);
    for (uint32_t step = 0; step < kMaxProbe; ++step) {
      uint64_t cur = keys_[i].load(std::memory_order_acquire);
      if (cur == key) return i;
      if (cur == 0) {
        uint64_t expect = 0;
        if (keys_[i].compare_exchange_strong(expect, key, std::memory_order_acq_rel)) {
          *inserted = true;
          return i;
        }
        if (expect == key) return i;
      }
      i = (i + 1) & (N - 1);
    }
    return kNone;
  }
  uint32_t find(uint64_t key) const {
    if (key == 0) return kNone;
    uint32_t i = static_cast<uint32_t>(mix64(key)) & (N - 1);
    for (uint32_t step = 0; step < kMaxProbe; ++step) {
      uint64_t cur = keys_[i].load(std::memory_order_acquire);
      if (cur == key) return i;
      if (cur == 0) return kNone;
      i = (i + 1) & (N - 1);
    }
    return kNone;
  }
  uint64_t key_at(uint32_t i) const { return keys_[i].load(std::memory_order_acquire); }
  static uint32_t capacity() { return N; }
  // Только когда писателей нет (тесты).
  void clear() {
    for (uint32_t i = 0; i < N; ++i) keys_[i].store(0, std::memory_order_relaxed);
  }

 private:
  std::atomic<uint64_t> keys_[N];
};

// ---------------------------------------------------------------------------
// Время

// Тики QPC → мкс / мс без переполнения на разумных интервалах; freq == 0 → 0.
uint64_t ticks_to_us(uint64_t ticks, uint64_t freq);
uint64_t ticks_to_ms(uint64_t ticks, uint64_t freq);
// Насыщение до 32 бит.
uint32_t sat32(uint64_t v);

// Шкала: корзины по kBucketMs от старта; за kBuckets — переполнение.
class Timeline {
 public:
  static const uint32_t kBuckets = 4096;
  static const uint32_t kBucketMs = 250;
  enum Field : uint32_t { kNewPtr = 0, kNewHash, kPatternCalls, kGlyphRuns, kFields };
  // Ложь — ms за последней корзиной (вызывающий считает переполнение).
  bool add(uint32_t ms, Field f) {
    uint32_t b = ms / kBucketMs;
    if (b >= kBuckets) return false;
    c_[b][f].fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  uint32_t get(uint32_t bucket, uint32_t f) const { return c_[bucket][f].load(std::memory_order_relaxed); }

 private:
  std::atomic<uint32_t> c_[kBuckets][kFields];
};

// ---------------------------------------------------------------------------
// Бины

// log2-бин: 0 → 0; v ≥ 1 → floor(log2 v) + 1; не больше nbins − 1.
uint32_t log2_bin(uint64_t v, uint32_t nbins);

const int32_t kDxDyMax = 7;                      // |d| ≤ 7 после округления
const uint32_t kDxDySide = 2 * kDxDyMax + 1;     // 15
const uint32_t kDxDyBins = kDxDySide * kDxDySide;  // 225
// Бин пары (dx, dy): округление половины от нуля; за пределами ±7 или NaN — -1 («далеко»).
int32_t dxdy_bin(double dx, double dy);
void dxdy_from_bin(uint32_t bin, int32_t* dx, int32_t* dy);

// RGBA8 0xRRGGBBAA из компонент 0..1 с округлением; *inexact — хоть одна v·255 не
// целая с точностью 1e-6 (или NaN / вне 0..1).
uint32_t rgba8(double r, double g, double b, double a, bool* inexact);

// ---------------------------------------------------------------------------
// Пачки («кадр»): вызовы хуков в одном потоке, разделённые паузой ≥ gap.

struct Burst {
  uint64_t start;     // вход первого вызова пачки
  uint64_t last_end;  // выход последнего вызова
  uint64_t overhead;  // накладные хука за пачку
  uint64_t orig;      // время в исходных функциях за пачку
  uint32_t open;
};

struct BurstClosed {
  uint64_t base;   // (last_end − start) − overhead, не меньше 0
  uint64_t pause;  // пауза до следующей пачки
  uint64_t overhead;
  uint64_t orig;
};

// Шаг одного потока (пишет только владелец слота). Истина — предыдущая пачка
// закрыта этим вызовом, *out заполнен; текущий вызов открывает новую.
bool burst_step(Burst* b, uint64_t t_entry, uint64_t t_exit, uint64_t overhead, uint64_t orig, uint64_t gap,
                BurstClosed* out);

// Слоты по tid: захват CAS 0 → tid; занят другим tid — -1 (коллизия).
class OwnerSlots {
 public:
  static const uint32_t kSlots = 64;
  int32_t claim(uint32_t tid) {
    if (tid == 0) return -1;
    uint32_t i = (tid >> 2) & (kSlots - 1);
    uint32_t cur = owner_[i].load(std::memory_order_acquire);
    if (cur == tid) return static_cast<int32_t>(i);
    if (cur != 0) return -1;
    uint32_t expect = 0;
    if (owner_[i].compare_exchange_strong(expect, tid, std::memory_order_acq_rel)) return static_cast<int32_t>(i);
    return expect == tid ? static_cast<int32_t>(i) : -1;
  }
  Burst* state(int32_t i) { return &st_[i]; }

 private:
  std::atomic<uint32_t> owner_[kSlots];
  Burst st_[kSlots];
};

// Счётчики кадра за эпоху.
class FrameStats {
 public:
  static const uint32_t kBinUs = 250;
  static const uint32_t kBins = 256;  // до 64 мс; kBins — переполнение
  static const uint32_t kPauseBins = 24;
  void add(uint64_t base_us, uint64_t pause_us, uint64_t overhead_ticks, uint64_t orig_ticks);
  struct Copy {
    uint32_t hist[kBins + 1];
    uint32_t pause[kPauseBins];
    uint32_t count;
    uint32_t max_us;
    uint64_t sum_us;
    uint64_t overhead_ticks;
    uint64_t orig_ticks;
  };
  // Копия с обнулением (эпоха).
  void take(Copy* out);

 private:
  std::atomic<uint32_t> hist_[kBins + 1];
  std::atomic<uint32_t> pause_[kPauseBins];
  std::atomic<uint32_t> count_;
  std::atomic<uint32_t> max_us_;
  std::atomic<uint64_t> sum_us_;
  std::atomic<uint64_t> overhead_;
  std::atomic<uint64_t> orig_;
};

// ---------------------------------------------------------------------------
// Пары дублей: кольцо последних прогонов (seqlock по номеру позиции) и гистограммы.

struct RunRec {
  uint64_t ghash;   // хеш индексов глифов (только в памяти, наружу не выдаётся)
  uint32_t count;   // число глифов
  uint32_t rgba;    // цвет текста RGBA8
  uint32_t has_color;
  uint32_t tid;
  uint64_t target;  // cairo_get_target, только для сравнения
  double x;         // первый глиф
  double y;
  uint64_t ticks;
};

class PairRing {
 public:
  static const uint32_t kSize = 256;
  // Ищет самый свежий из последних kSize прогонов с тем же ghash и count, затем
  // записывает rec. Истина — пара найдена, *prev заполнен. *torn += рваных чтений.
  bool push_and_match(const RunRec& rec, RunRec* prev, uint32_t* torn);

 private:
  struct Entry {
    std::atomic<uint64_t> seq;  // 2p+1 — пишется позиция p, 2p+2 — записана
    std::atomic<uint64_t> ghash;
    std::atomic<uint64_t> meta;  // count | has_color << 32
    std::atomic<uint64_t> color_tid;  // rgba | tid << 32
    std::atomic<uint64_t> target;
    std::atomic<uint64_t> x;
    std::atomic<uint64_t> y;
    std::atomic<uint64_t> ticks;
  };
  std::atomic<uint64_t> pos_;
  Entry e_[kSize];
};

// Признаки пары.
enum PairFlag : uint32_t { kSameThread = 1, kSameTarget = 2, kSameColor = 4, kPairFlags = 8 };
uint32_t pair_flags(const RunRec& a, const RunRec& b);

class DupHist {
 public:
  static const uint32_t kBins = kDxDyBins * kPairFlags;  // 1800
  static const uint32_t kDtBins = 16;
  void add(uint32_t bin, uint32_t flags, uint64_t dt_us) {
    bins_[bin * kPairFlags + (flags & (kPairFlags - 1))].fetch_add(1, std::memory_order_relaxed);
    dt_[log2_bin(dt_us, kDtBins)].fetch_add(1, std::memory_order_relaxed);
  }
  // Копия с обнулением (эпоха).
  void take(uint32_t* bins, uint32_t* dt);

 private:
  std::atomic<uint32_t> bins_[kBins];
  std::atomic<uint32_t> dt_[kDtBins];
};

}  // namespace probe
}  // namespace dt
