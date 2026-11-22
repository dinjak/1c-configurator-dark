// SPDX-License-Identifier: MIT
// Детектор дублей прогонов глифов (второй проход emboss-пары, двойной текст меню).
// Сигнатура прогона: хеш индексов глифов + число глифов + цвет + поверхность.
// Таблица фиксированного размера (ring) по сигнатуре; запись помнится, пока с неё
// прошло не больше ring прогонов. Совпадение потребляет запись. Коллизия слота или
// рваная запись при гонке потоков (не сошлась контрольная сумма) — без пропуска.
#pragma once
#include <atomic>
#include <stdint.h>

namespace dt {
namespace dup {

const uint32_t kMinRing = 16;
const uint32_t kMaxRing = 1024;

struct Params {
  int32_t max_dx;
  int32_t max_dy;
  uint32_t ring;  // степень двойки в [kMinRing, kMaxRing]
};

// Хеш 32-битных индексов глифов в массиве с шагом stride (index по смещению 0).
uint64_t glyph_index_hash(const void* glyphs, uint32_t n, uint32_t stride);

class Detector {
 public:
  bool init(const Params& p);
  // Истина — прогон повторяет недавний на той же поверхности со сдвигом первого
  // глифа в пределах порога: второй проход можно пропустить.
  bool check(uint64_t glyph_hash, uint32_t count, uint32_t rgb, uint64_t surface, double x, double y);
  void reset();

 private:
#pragma warning(push)
#pragma warning(disable : 4324)  // выравнивание слотов до кэш-линии — намеренно
  struct alignas(64) Slot {
    std::atomic<uint64_t> w[8];  // key, surface, meta, x, y, seq, check, —
  };
  Params p_;
  std::atomic<uint64_t> seq_;
  Slot slots_[kMaxRing];
};
#pragma warning(pop)

}  // namespace dup
}  // namespace dt
