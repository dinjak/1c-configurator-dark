// SPDX-License-Identifier: MIT
#include "core/probe_stats.h"

#include <string.h>

namespace dt {
namespace probe {
namespace {

uint64_t dbits(double v) {
  uint64_t u;
  memcpy(&u, &v, sizeof(u));
  return u;
}

double bitsd(uint64_t u) {
  double v;
  memcpy(&v, &u, sizeof(v));
  return v;
}

// Округление половины от нуля; вызывающий гарантирует |v| ≤ 2^30.
int32_t round_half_away(double v) {
  return static_cast<int32_t>(v < 0 ? v - 0.5 : v + 0.5);
}

// Компонента цвета → 0..255; *inexact — v·255 не целое с точностью 1e-6.
uint32_t channel8(double v, bool* inexact) {
  if (!(v >= 0.0 && v <= 1.0)) {  // NaN и вне диапазона
    *inexact = true;
    if (v > 1.0) return 255;
    return 0;
  }
  double s = v * 255.0;
  int32_t r = round_half_away(s);
  double d = s - static_cast<double>(r);
  if (d < 0) d = -d;
  if (d > 1e-6) *inexact = true;
  return static_cast<uint32_t>(r);
}

}  // namespace

uint64_t mix64(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
}

uint64_t ticks_to_us(uint64_t ticks, uint64_t freq) {
  if (freq == 0) return 0;
  return (ticks / freq) * 1000000ull + (ticks % freq) * 1000000ull / freq;
}

uint64_t ticks_to_ms(uint64_t ticks, uint64_t freq) {
  if (freq == 0) return 0;
  return (ticks / freq) * 1000ull + (ticks % freq) * 1000ull / freq;
}

uint32_t sat32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(v); }

uint32_t log2_bin(uint64_t v, uint32_t nbins) {
  if (nbins == 0) return 0;
  uint32_t b = 0;
  while (v != 0) {
    ++b;
    v >>= 1;
  }
  return b < nbins ? b : nbins - 1;
}

int32_t dxdy_bin(double dx, double dy) {
  const double lim = kDxDyMax + 0.5;
  if (!(dx > -lim && dx < lim && dy > -lim && dy < lim)) return -1;  // NaN — тоже сюда
  int32_t rx = round_half_away(dx);
  int32_t ry = round_half_away(dy);
  if (rx < -kDxDyMax || rx > kDxDyMax || ry < -kDxDyMax || ry > kDxDyMax) return -1;
  return (ry + kDxDyMax) * static_cast<int32_t>(kDxDySide) + (rx + kDxDyMax);
}

void dxdy_from_bin(uint32_t bin, int32_t* dx, int32_t* dy) {
  *dx = static_cast<int32_t>(bin % kDxDySide) - kDxDyMax;
  *dy = static_cast<int32_t>(bin / kDxDySide) - kDxDyMax;
}

uint32_t rgba8(double r, double g, double b, double a, bool* inexact) {
  bool ix = false;
  uint32_t v = (channel8(r, &ix) << 24) | (channel8(g, &ix) << 16) | (channel8(b, &ix) << 8) | channel8(a, &ix);
  *inexact = ix;
  return v;
}

bool burst_step(Burst* b, uint64_t t_entry, uint64_t t_exit, uint64_t overhead, uint64_t orig, uint64_t gap,
                BurstClosed* out) {
  bool closed = false;
  if (b->open && t_entry >= b->last_end && t_entry - b->last_end >= gap) {
    uint64_t span = b->last_end - b->start;
    out->base = span > b->overhead ? span - b->overhead : 0;
    out->pause = t_entry - b->last_end;
    out->overhead = b->overhead;
    out->orig = b->orig;
    closed = true;
    b->open = 0;
  }
  if (!b->open) {
    b->start = t_entry;
    b->overhead = 0;
    b->orig = 0;
    b->open = 1;
  }
  b->overhead += overhead;
  b->orig += orig;
  if (t_exit > b->last_end) b->last_end = t_exit;  // вложенный вызов не сдвигает конец назад
  return closed;
}

void FrameStats::add(uint64_t base_us, uint64_t pause_us, uint64_t overhead_ticks, uint64_t orig_ticks) {
  uint64_t bin = base_us / kBinUs;
  hist_[bin < kBins ? bin : kBins].fetch_add(1, std::memory_order_relaxed);
  pause_[log2_bin(pause_us, kPauseBins)].fetch_add(1, std::memory_order_relaxed);
  count_.fetch_add(1, std::memory_order_relaxed);
  sum_us_.fetch_add(base_us, std::memory_order_relaxed);
  overhead_.fetch_add(overhead_ticks, std::memory_order_relaxed);
  orig_.fetch_add(orig_ticks, std::memory_order_relaxed);
  uint32_t v = sat32(base_us);
  uint32_t cur = max_us_.load(std::memory_order_relaxed);
  while (v > cur && !max_us_.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

void FrameStats::take(Copy* out) {
  for (uint32_t i = 0; i <= kBins; ++i) out->hist[i] = hist_[i].exchange(0, std::memory_order_relaxed);
  for (uint32_t i = 0; i < kPauseBins; ++i) out->pause[i] = pause_[i].exchange(0, std::memory_order_relaxed);
  out->count = count_.exchange(0, std::memory_order_relaxed);
  out->max_us = max_us_.exchange(0, std::memory_order_relaxed);
  out->sum_us = sum_us_.exchange(0, std::memory_order_relaxed);
  out->overhead_ticks = overhead_.exchange(0, std::memory_order_relaxed);
  out->orig_ticks = orig_.exchange(0, std::memory_order_relaxed);
}

uint32_t pair_flags(const RunRec& a, const RunRec& b) {
  uint32_t f = 0;
  if (a.tid == b.tid) f |= kSameThread;
  if (a.target == b.target) f |= kSameTarget;
  if (a.has_color && b.has_color && a.rgba == b.rgba) f |= kSameColor;
  return f;
}

bool PairRing::push_and_match(const RunRec& rec, RunRec* prev, uint32_t* torn) {
  // Сначала поиск по уже выданным позициям, потом своя позиция и сразу запись:
  // окно, в котором два писателя делят слот (p и p + kSize), остаётся коротким.
  const uint64_t cur = pos_.load(std::memory_order_acquire);
  const uint64_t meta = static_cast<uint64_t>(rec.count) | (static_cast<uint64_t>(rec.has_color ? 1 : 0) << 32);
  bool found = false;
  for (uint32_t back = 1; back <= kSize && back <= cur && !found; ++back) {
    const uint64_t q = cur - back;
    const Entry& e = e_[q & (kSize - 1)];
    const uint64_t want = 2 * q + 2;
    uint64_t s1 = e.seq.load(std::memory_order_acquire);
    if (s1 != want) {
      if (s1 == want - 1) ++*torn;  // позиция q ещё пишется
      continue;                     // иначе уже перезаписана или не записана
    }
    if (e.ghash.load(std::memory_order_relaxed) != rec.ghash) continue;
    uint64_t m = e.meta.load(std::memory_order_relaxed);
    if (static_cast<uint32_t>(m) != rec.count) continue;
    RunRec r;
    r.ghash = rec.ghash;
    r.count = rec.count;
    r.has_color = static_cast<uint32_t>(m >> 32) & 1;
    uint64_t ct = e.color_tid.load(std::memory_order_relaxed);
    r.rgba = static_cast<uint32_t>(ct);
    r.tid = static_cast<uint32_t>(ct >> 32);
    r.target = e.target.load(std::memory_order_relaxed);
    r.x = bitsd(e.x.load(std::memory_order_relaxed));
    r.y = bitsd(e.y.load(std::memory_order_relaxed));
    r.ticks = e.ticks.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (e.seq.load(std::memory_order_relaxed) != s1) {
      ++*torn;
      continue;
    }
    *prev = r;
    found = true;
  }
  const uint64_t p = pos_.fetch_add(1, std::memory_order_relaxed);
  Entry& w = e_[p & (kSize - 1)];
  w.seq.store(2 * p + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  w.ghash.store(rec.ghash, std::memory_order_relaxed);
  w.meta.store(meta, std::memory_order_relaxed);
  w.color_tid.store(static_cast<uint64_t>(rec.rgba) | (static_cast<uint64_t>(rec.tid) << 32),
                    std::memory_order_relaxed);
  w.target.store(rec.target, std::memory_order_relaxed);
  w.x.store(dbits(rec.x), std::memory_order_relaxed);
  w.y.store(dbits(rec.y), std::memory_order_relaxed);
  w.ticks.store(rec.ticks, std::memory_order_relaxed);
  w.seq.store(2 * p + 2, std::memory_order_release);
  return found;
}

void DupHist::take(uint32_t* bins, uint32_t* dt) {
  for (uint32_t i = 0; i < kBins; ++i) bins[i] = bins_[i].exchange(0, std::memory_order_relaxed);
  for (uint32_t i = 0; i < kDtBins; ++i) dt[i] = dt_[i].exchange(0, std::memory_order_relaxed);
}

}  // namespace probe
}  // namespace dt
