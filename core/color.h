// SPDX-License-Identifier: MIT
// Движок цвета: две карты по исходному цвету (заливка и текст), контексты,
// ближний цвет с допуском, правило для неизвестных цветов по роли, неподвижные
// точки «цель -> цель» и мемо-кэш без блокировок.
//
// Порядок для (роль, контекст, цвет): квантование до 8 бит -> точная таблица
// контекста -> общая таблица ("*") -> ближний цвет (Чебышёв ≤ tolerance) ->
// правило для неизвестных цветов. Цвет — 0x00RRGGBB, альфа не трогается.
#pragma once
#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace dt {
namespace color {

enum Role : uint8_t { kFill = 0, kText = 1, kRoleCount = 2 };
enum Ctx : uint8_t { kEditor = 0, kPanel, kPopup, kHelp, kDialog, kGdi, kCtxCount, kCtxAny = 0xFF };

const uint32_t kMaxEntries = 1024;  // на одну карту
const uint32_t kSysColorCount = 31; // индексы GetSysColor 0..30
const uint32_t kMaxTolerance = 32;

struct Entry {
  uint32_t from;
  uint32_t to;
  uint8_t ctx;  // Ctx или kCtxAny
};

struct CtxInfo {
  bool has_bg;
  bool has_text;
  uint32_t bg;
  uint32_t text;
};

struct Fallback {
  double pale_min;     // HSL-светлота, с которой заливка считается бледной
  double bg_l_lo;      // диапазон светлоты затемнённой заливки
  double bg_l_hi;
  double min_contrast; // WCAG-контраст текста к фону контекста
};

struct DupConfig {
  int32_t max_dx;
  int32_t max_dy;
  uint32_t ring;
};

struct Palette {
  uint32_t schema;
  char name[64];
  uint32_t tolerance;
  CtxInfo ctx[kCtxCount];
  Entry map[kRoleCount][kMaxEntries];
  uint32_t count[kRoleCount];
  bool has_sys[kSysColorCount];
  uint32_t sys[kSysColorCount];
  Fallback fallback;
  DupConfig dup;
};

enum Status : uint8_t {
  kOk = 0,
  kErrJson,        // текст не JSON (подробности в json_error)
  kErrSchema,      // не тот schema или корень не объект
  kErrType,        // неверный тип значения
  kErrMissing,     // нет обязательного ключа
  kErrUnknownKey,  // лишний ключ
  kErrColor,       // не "#RRGGBB"
  kErrCtx,         // неизвестный контекст
  kErrRange,       // число вне допустимого диапазона
  kErrTooMany,     // переполнение карты
  kErrConflict,    // повтор from или неподвижность цели нарушена
};

struct Diag {
  Status status;
  uint8_t json_error;    // dt::json::Error
  uint32_t json_offset;
  char where[96];        // путь к месту ошибки, ASCII
};

// Как получен результат (для диагностики и тестов).
enum How : uint8_t { kHowExactCtx, kHowExactAny, kHowNearCtx, kHowNearAny, kHowFixed, kHowFallback, kHowKeep };

struct Resolved {
  uint32_t rgb;
  How how;
};

// Разбор palette.json со строгой схемой. *out заполняется целиком.
bool parse_palette(const char* text, size_t len, Palette* out, Diag* diag);
const char* status_name(Status s);

class Engine {
 public:
  // Проверяет палитру (повторы from, устойчивость каждой цели) и сбрасывает кэш.
  bool init(const Palette& p, Diag* diag);

  // Без кэша: для проверок и тестов.
  Resolved resolve(Role role, Ctx ctx, uint32_t rgb) const;
  // С мемо-кэшем; горячий путь без блокировок и выделений памяти.
  uint32_t map(Role role, Ctx ctx, uint32_t rgb);
  // Каналы 0..1 (cairo). Ложь — цвет не меняется, вызывающий передаёт исходные
  // значения без квантования. NaN и выход за [0,1] после округления — без изменений.
  bool map_unit(Role role, Ctx ctx, double r, double g, double b, double out[3]);
  bool syscolor(uint32_t index, uint32_t* rgb) const;

  const Palette& palette() const { return pal_; }
  double contrast(uint32_t a, uint32_t b) const;
  uint32_t cache_hits() const { return hits_.load(std::memory_order_relaxed); }

  static const uint32_t kCacheSize = 4096;  // степень двойки
  static const uint32_t kCacheProbe = 4;

 private:
  struct Key {
    uint32_t key;  // (номер контекста << 24) | from, номер kCtxCount — "*"
    uint32_t to;
  };
  bool lookup_exact(Role role, uint32_t ctx_slot, uint32_t rgb, uint32_t* to) const;
  bool lookup_near(Role role, Ctx ctx, uint32_t rgb, Resolved* out) const;
  uint32_t fallback(Role role, Ctx ctx, uint32_t rgb) const;
  double luminance(uint32_t rgb) const;

  Palette pal_;
  Key sorted_[kRoleCount][kMaxEntries];
  uint32_t nsorted_[kRoleCount];
  double lin_[256];
  std::atomic<uint64_t> cache_[kCacheSize];
  std::atomic<uint32_t> hits_;
};

// HSL в [0,1]; для тестов и правила.
void rgb_to_hsl(uint32_t rgb, double* h, double* s, double* l);
uint32_t hsl_to_rgb(double h, double s, double l);
inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

}  // namespace color
}  // namespace dt
