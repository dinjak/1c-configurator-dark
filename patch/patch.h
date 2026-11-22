// SPDX-License-Identifier: MIT
// Движок патчей: EAT через стабы и IAT (включая delay-load) с журналом отката.
//
// Стаб — `jmp [slot]` в странице VirtualAlloc поставщика: на x64 и x86 страница
// ищется выше образа в окне [base, base + 4 ГБ), в EAT пишется RVA стаба (он вне
// образа, значит вне каталога экспорта). Страница кода стабов заполняется один раз
// и сразу становится PAGE_EXECUTE_READ; адреса переходов — в соседней странице
// данных (PAGE_READWRITE), их смена — атомарная запись указателя.
// Страницы стабов не освобождаются: адреса, полученные через GetProcAddress, после
// отката ведут в исходные функции.
//
// Откат (unpatch): IAT по журналу -> EAT по журналу -> второй обход IAT (адреса
// стаба или хука, полученные поздними модулями) -> стабы на исходные функции.
// Флаг активности хуков и ожидание счётчиков входов — у вызывающего (DtStop).
#pragma once
#include <windows.h>
#include <stdint.h>

namespace dt {
namespace patch {

enum Status : uint8_t {
  kOk = 0,
  kErrBadImage,
  kErrNotFound,
  kErrForwarder,
  kErrAlready,
  kErrTooManyHooks,
  kErrJournalFull,
  kErrNoStubSpace,
  kErrRva,
  kErrProtect,
  kErrRace,
  kErrModules,
};

const uint32_t kMaxHooks = 64;
const uint32_t kMaxJournal = 4096;
const uint32_t kMaxRegions = 8;
const uint32_t kMaxModules = 1024;
const uint32_t kStubSize = 16;

enum Kind : uint8_t { kEat = 1, kIat = 2, kIatDelay = 3 };

struct JournalEntry {
  void* slot;
  uintptr_t old_value;
  uintptr_t new_value;
  HMODULE module;  // владелец слота: перед откатом проверяется, что он ещё загружен
  uint8_t width;   // 4 — EAT (RVA), sizeof(void*) — IAT
  uint8_t kind;
};

struct Hook {
  HMODULE provider;
  uint32_t* eat_slot;
  uint32_t orig_rva;
  uint32_t stub_rva;
  void* orig;
  void* hook;
  void* stub;
  void** jump_slot;  // ячейка, через которую прыгает стаб
};

struct Region {
  uint8_t* code;   // страница кода стабов (RX)
  void** slots;    // страница адресов (RW)
  uint32_t used;
  uint32_t cap;
};

struct Stats {
  uint32_t eat_patched;
  uint32_t iat_patched;
  uint32_t iat_delay_patched;
  uint32_t iat_restored;
  uint32_t eat_restored;
  uint32_t second_pass;      // слоты, исправленные вторым обходом
  uint32_t mismatched;       // слот к откату держит чужое значение — не трогали
  uint32_t module_gone;      // владелец слота выгружен — не трогали
  uint32_t journal_full;     // патчей, не поставленных из-за переполнения
  uint32_t regions;
};

class Engine {
 public:
  // exclude — модуль, чей IAT не трогается (своя DLL); journal_limit ≤ kMaxJournal.
  void init(HMODULE exclude, uint32_t journal_limit = kMaxJournal);

  // EAT поставщика -> стаб -> hook. *orig_out — исходная функция.
  Status hook_export(HMODULE provider, const char* name, void* hook, void** orig_out, void** stub_out = nullptr);
  // Все загруженные модули, кроме exclude: слот IAT с исходным адресом -> hook.
  // Журнал полон — слот не меняется, возвращается kErrJournalFull.
  Status patch_iats();
  // Полный откат в порядке из заголовка. Повторный вызов безвреден.
  void unpatch();

  const Stats& stats() const { return stats_; }
  uint32_t hook_count() const { return nhooks_; }
  const Hook& hook(uint32_t i) const { return hooks_[i]; }
  uint32_t journal_size() const { return njournal_; }

  // Сколько слотов IAT во всех модулях (без exclude) содержат value.
  uint32_t count_iat_slots(const void* value) const;

 private:
  Region* region_for(HMODULE provider, uint32_t image_size, Status* why);
  bool journal_push(const JournalEntry& e);
  void restore(const JournalEntry& e);
  static bool patch_cb(void* ctx, void** slot, bool delay);
  static bool second_cb(void* ctx, void** slot, bool delay);
  static bool count_cb(void* ctx, void** slot, bool delay);
  uint32_t enum_modules(HMODULE* mods, uint32_t cap) const;

  HMODULE exclude_;
  uint32_t journal_limit_;
  uint32_t njournal_;
  uint32_t nhooks_;
  uint32_t nregions_;
  Stats stats_;
  Hook hooks_[kMaxHooks];
  Region regions_[kMaxRegions];
  uintptr_t region_base_[kMaxRegions];  // база поставщика, для которого выделен регион
  JournalEntry journal_[kMaxJournal];
};

// Счётчики входов в хуки: слот по tid, каждый в своей кэш-линии, без блокировок.
class Inflight {
 public:
  static const uint32_t kSlots = 64;
  void reset();
  uint32_t enter();
  void leave(uint32_t slot);
  bool idle() const;
  // Ждать нуля всех счётчиков не дольше timeout_ms. Истина — дождались.
  bool wait_idle(DWORD timeout_ms) const;

 private:
#pragma warning(push)
#pragma warning(disable : 4324)  // выравнивание до кэш-линии — намеренно
  struct alignas(64) Counter {
    volatile LONG n;
  };
#pragma warning(pop)
  Counter c_[kSlots];
};

const char* status_name(Status s);

// Запись в слот с временной сменой защиты и сравнением: kOk — в слоте было
// expected и он заменён; kErrRace — там другое значение; kErrProtect — нет доступа.
Status write_ptr(void** slot, void* expected, void* value);
Status write_u32(uint32_t* slot, uint32_t expected, uint32_t value);

}  // namespace patch
}  // namespace dt
