// SPDX-License-Identifier: MIT
#include "patch/patch.h"

#include <psapi.h>
#include <string.h>

#include "patch/pe.h"

namespace dt {
namespace patch {
namespace {

bool is_writable(DWORD prot) {
  prot &= 0xFF;
  return prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE ||
         prot == PAGE_EXECUTE_WRITECOPY;
}

bool is_exec(DWORD prot) {
  prot &= 0xFF;
  return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
         prot == PAGE_EXECUTE_WRITECOPY;
}

// Временная смена защиты вокруг одной ячейки. Исполняемая страница остаётся
// исполняемой: соседний код может выполняться в другом потоке.
struct Unprotect {
  void* addr;
  size_t len;
  DWORD old;
  bool changed;
  bool ok;
  Unprotect(void* a, size_t n) : addr(a), len(n), old(0), changed(false), ok(false) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(a, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT) return;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return;
    if (is_writable(mbi.Protect)) {
      ok = true;
      return;
    }
    DWORD want = is_exec(mbi.Protect) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    if (!VirtualProtect(a, n, want, &old)) return;
    changed = true;
    ok = true;
  }
  ~Unprotect() {
    if (changed) {
      DWORD tmp;
      VirtualProtect(addr, len, old, &tmp);
    }
  }
};

// Регион стабов: 2 страницы подряд в окне [lo, hi]. Первая — код (RX после
// заполнения), вторая — ячейки адресов (RW).
bool alloc_region(uint64_t lo, uint64_t hi, Region* r, Status* why) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  const uint64_t gran = si.dwAllocationGranularity;
  const uint64_t page = si.dwPageSize;
  const uint64_t need = 2 * page;
  uint64_t maxapp = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
  uint64_t minapp = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
  if (hi > maxapp) hi = maxapp;
  if (lo < minapp) lo = minapp;
  lo = (lo + gran - 1) & ~(gran - 1);
  *why = kErrNoStubSpace;
  for (uint64_t a = lo; a + need - 1 <= hi;) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(a)), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
    uint64_t rbase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uint64_t rend = rbase + mbi.RegionSize;
    if (mbi.State == MEM_FREE && a + need <= rend) {
      void* p = VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(a)), static_cast<SIZE_T>(need),
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
      if (p != nullptr) {
        uint8_t* code = static_cast<uint8_t*>(p);
        void** slots = reinterpret_cast<void**>(code + page);
        uint32_t cap = static_cast<uint32_t>(page / kStubSize);
        for (uint32_t i = 0; i < cap; ++i) {
          uint8_t* s = code + static_cast<size_t>(i) * kStubSize;
          memset(s, 0xCC, kStubSize);
          s[0] = 0xFF;  // jmp qword/dword ptr [...]
          s[1] = 0x25;
#if defined(_WIN64)
          int32_t disp = static_cast<int32_t>(reinterpret_cast<uint8_t*>(&slots[i]) - (s + 6));  // rip-relative
          memcpy(s + 2, &disp, 4);
#else
          uint32_t abs = reinterpret_cast<uint32_t>(&slots[i]);
          memcpy(s + 2, &abs, 4);
#endif
          slots[i] = nullptr;
        }
        DWORD old;
        if (!VirtualProtect(code, static_cast<SIZE_T>(page), PAGE_EXECUTE_READ, &old)) {
          // Запрет динамического кода (ACG): стабы невозможны.
          VirtualFree(p, 0, MEM_RELEASE);
          *why = kErrProtect;
          return false;
        }
        FlushInstructionCache(GetCurrentProcess(), code, static_cast<SIZE_T>(page));
        r->code = code;
        r->slots = slots;
        r->used = 0;
        r->cap = cap;
        return true;
      }
    }
    uint64_t next = (rend + gran - 1) & ~(gran - 1);
    if (next <= a) next = a + gran;
    a = next;
  }
  return false;
}

struct WalkCtx {
  Engine* self;
  HMODULE module;
  Status st;
  const void* value;
  uint32_t count;
};

}  // namespace

// ---------------------------------------------------------------------------

Status write_ptr(void** slot, void* expected, void* value) {
  Unprotect u(slot, sizeof(void*));
  if (!u.ok) return kErrProtect;
  void* prev = InterlockedCompareExchangePointer(slot, value, expected);
  return prev == expected ? kOk : kErrRace;
}

Status write_u32(uint32_t* slot, uint32_t expected, uint32_t value) {
  Unprotect u(slot, sizeof(uint32_t));
  if (!u.ok) return kErrProtect;
  LONG prev = InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(slot), static_cast<LONG>(value),
                                         static_cast<LONG>(expected));
  return static_cast<uint32_t>(prev) == expected ? kOk : kErrRace;
}

void Engine::init(HMODULE exclude, uint32_t journal_limit) {
  exclude_ = exclude;
  journal_limit_ = journal_limit > kMaxJournal ? kMaxJournal : journal_limit;
  njournal_ = 0;
  nhooks_ = 0;
  nregions_ = 0;  // прежние регионы не освобождаются намеренно
  memset(&stats_, 0, sizeof(stats_));
}

uint32_t Engine::enum_modules(HMODULE* mods, uint32_t cap) const {
  DWORD needed = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), mods, cap * sizeof(HMODULE), &needed)) return 0;
  uint32_t n = needed / sizeof(HMODULE);
  return n < cap ? n : cap;
}

bool Engine::journal_push(const JournalEntry& e) {
  if (njournal_ >= journal_limit_) return false;
  journal_[njournal_++] = e;
  return true;
}

Region* Engine::region_for(HMODULE provider, uint32_t image_size, Status* why) {
  uintptr_t base = reinterpret_cast<uintptr_t>(provider);
  for (uint32_t i = 0; i < nregions_; ++i) {
    if (region_base_[i] == base && regions_[i].used < regions_[i].cap) return &regions_[i];
  }
  if (nregions_ >= kMaxRegions) {
    *why = kErrNoStubSpace;
    return nullptr;
  }
  // RVA стаба: выше образа и меньше 2^32.
  uint64_t lo = static_cast<uint64_t>(base) + image_size;
  uint64_t hi = static_cast<uint64_t>(base) + 0xFFFFFFFFull;
  Region* r = &regions_[nregions_];
  if (!alloc_region(lo, hi, r, why)) return nullptr;
  region_base_[nregions_] = base;
  ++nregions_;
  ++stats_.regions;
  return r;
}

Status Engine::hook_export(HMODULE provider, const char* name, void* hook, void** orig_out, void** stub_out) {
  if (nhooks_ >= kMaxHooks) return kErrTooManyHooks;
  pe::Image img;
  if (!pe::open(provider, static_cast<size_t>(-1), &img)) return kErrBadImage;
  pe::Export ex;
  if (!pe::find_export(img, name, &ex)) return kErrNotFound;
  for (uint32_t i = 0; i < nhooks_; ++i) {
    if (hooks_[i].eat_slot == ex.eat_slot) return kErrAlready;
  }
  if (ex.forwarder) return kErrForwarder;
  if (!ex.in_image) return kErrRva;  // EAT уже указывает за образ: чужой перехват
  if (njournal_ >= journal_limit_) {
    ++stats_.journal_full;
    return kErrJournalFull;
  }
  Status why = kOk;
  Region* r = region_for(provider, img.size, &why);
  if (r == nullptr) return why;
  uint8_t* stub = r->code + static_cast<size_t>(r->used) * kStubSize;
  uint64_t diff = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(stub)) - reinterpret_cast<uintptr_t>(provider);
  if (reinterpret_cast<uintptr_t>(stub) <= reinterpret_cast<uintptr_t>(provider) || diff > 0xFFFFFFFFull ||
      diff < img.size || pe::in_export_dir(img, static_cast<uint32_t>(diff))) {
    return kErrRva;  // усечённый адрес в EAT не пишется
  }
  void** jump = &r->slots[r->used];
  ++r->used;
  void* orig = reinterpret_cast<uint8_t*>(provider) + ex.rva;
  // Исходная функция известна вызывающему до публикации: хук может сработать
  // в другом потоке сразу после записи в EAT.
  *orig_out = orig;
  InterlockedExchangePointer(jump, hook);

  JournalEntry je;
  je.slot = ex.eat_slot;
  je.old_value = ex.rva;
  je.new_value = static_cast<uint32_t>(diff);
  je.module = provider;
  je.width = 4;
  je.kind = kEat;
  journal_push(je);
  Status w = write_u32(ex.eat_slot, ex.rva, static_cast<uint32_t>(diff));
  if (w != kOk) {
    --njournal_;
    InterlockedExchangePointer(jump, orig);
    return w;
  }
  Hook& h = hooks_[nhooks_++];
  h.provider = provider;
  h.eat_slot = ex.eat_slot;
  h.orig_rva = ex.rva;
  h.stub_rva = static_cast<uint32_t>(diff);
  h.orig = orig;
  h.hook = hook;
  h.stub = stub;
  h.jump_slot = jump;
  ++stats_.eat_patched;
  if (stub_out) *stub_out = stub;
  return kOk;
}

bool Engine::patch_cb(void* ctx, void** slot, bool delay) {
  WalkCtx* c = static_cast<WalkCtx*>(ctx);
  Engine* e = c->self;
  void* v = *reinterpret_cast<void* volatile*>(slot);
  for (uint32_t k = 0; k < e->nhooks_; ++k) {
    const Hook& h = e->hooks_[k];
    if (v != h.orig) continue;
    JournalEntry je;
    je.slot = slot;
    je.old_value = reinterpret_cast<uintptr_t>(h.orig);
    je.new_value = reinterpret_cast<uintptr_t>(h.hook);
    je.module = c->module;
    je.width = sizeof(void*);
    je.kind = delay ? kIatDelay : kIat;
    if (!e->journal_push(je)) {
      ++e->stats_.journal_full;
      c->st = kErrJournalFull;
      return false;  // журнал полон: слот не трогаем и обход прекращаем
    }
    if (write_ptr(slot, h.orig, h.hook) == kOk) {
      if (delay) ++e->stats_.iat_delay_patched;
      else ++e->stats_.iat_patched;
    } else {
      --e->njournal_;
    }
    break;
  }
  return true;
}

Status Engine::patch_iats() {
  HMODULE mods[kMaxModules];
  uint32_t n = enum_modules(mods, kMaxModules);
  if (n == 0) return kErrModules;
  WalkCtx c;
  memset(&c, 0, sizeof(c));
  c.self = this;
  c.st = kOk;
  for (uint32_t i = 0; i < n && c.st == kOk; ++i) {
    if (mods[i] == exclude_) continue;
    HMODULE pin = nullptr;
    // Ссылка на модуль на время обхода: его не выгрузят посреди записи.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(mods[i]), &pin)) {
      continue;
    }
    if (pin == mods[i]) {
      pe::Image img;
      if (pe::open(mods[i], static_cast<size_t>(-1), &img)) {
        c.module = mods[i];
        pe::walk_iat(img, &Engine::patch_cb, &c);
      }
    }
    FreeLibrary(pin);
  }
  return c.st;
}

void Engine::restore(const JournalEntry& e) {
  HMODULE pin = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, static_cast<LPCWSTR>(e.slot), &pin)) {
    ++stats_.module_gone;
    return;
  }
  if (pin != e.module) {
    ++stats_.module_gone;
    FreeLibrary(pin);
    return;
  }
  Status w;
  if (e.width == 4) {
    w = write_u32(static_cast<uint32_t*>(e.slot), static_cast<uint32_t>(e.new_value),
                  static_cast<uint32_t>(e.old_value));
  } else {
    w = write_ptr(static_cast<void**>(e.slot), reinterpret_cast<void*>(e.new_value),
                  reinterpret_cast<void*>(e.old_value));
  }
  if (w == kOk) {
    if (e.kind == kEat) ++stats_.eat_restored;
    else ++stats_.iat_restored;
  } else {
    ++stats_.mismatched;
  }
  FreeLibrary(pin);
}

bool Engine::second_cb(void* ctx, void** slot, bool) {
  WalkCtx* c = static_cast<WalkCtx*>(ctx);
  Engine* e = c->self;
  void* v = *reinterpret_cast<void* volatile*>(slot);
  for (uint32_t k = 0; k < e->nhooks_; ++k) {
    const Hook& h = e->hooks_[k];
    if (v != h.stub && v != h.hook) continue;
    if (write_ptr(slot, v, h.orig) == kOk) ++e->stats_.second_pass;
    break;
  }
  return true;
}

void Engine::unpatch() {
  // 1. IAT по журналу, в обратном порядке.
  for (uint32_t i = njournal_; i-- > 0;) {
    if (journal_[i].kind != kEat) restore(journal_[i]);
  }
  // 2. EAT по журналу.
  for (uint32_t i = njournal_; i-- > 0;) {
    if (journal_[i].kind == kEat) restore(journal_[i]);
  }
  njournal_ = 0;
  // 3. Второй обход: адреса стаба или хука, которые модули получили после
  //    патча (поздняя загрузка, delay-load через GetProcAddress).
  if (nhooks_ != 0) {
    HMODULE mods[kMaxModules];
    uint32_t n = enum_modules(mods, kMaxModules);
    WalkCtx c;
    memset(&c, 0, sizeof(c));
    c.self = this;
    for (uint32_t i = 0; i < n; ++i) {
      if (mods[i] == exclude_) continue;
      HMODULE pin = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(mods[i]), &pin)) {
        continue;
      }
      if (pin == mods[i]) {
        pe::Image img;
        if (pe::open(mods[i], static_cast<size_t>(-1), &img)) {
          c.module = mods[i];
          pe::walk_iat(img, &Engine::second_cb, &c);
        }
      }
      FreeLibrary(pin);
    }
  }
  // 4. Стабы — на исходные функции: сохранённые вне IAT адреса стабов безопасны.
  for (uint32_t k = 0; k < nhooks_; ++k) InterlockedExchangePointer(hooks_[k].jump_slot, hooks_[k].orig);
  nhooks_ = 0;
}

bool Engine::count_cb(void* ctx, void** slot, bool) {
  WalkCtx* c = static_cast<WalkCtx*>(ctx);
  if (*reinterpret_cast<void* volatile*>(slot) == c->value) ++c->count;
  return true;
}

uint32_t Engine::count_iat_slots(const void* value) const {
  HMODULE mods[kMaxModules];
  uint32_t n = enum_modules(mods, kMaxModules);
  WalkCtx c;
  memset(&c, 0, sizeof(c));
  c.value = value;
  for (uint32_t i = 0; i < n; ++i) {
    if (mods[i] == exclude_) continue;
    HMODULE pin = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(mods[i]), &pin)) {
      continue;
    }
    pe::Image img;
    if (pin == mods[i] && pe::open(mods[i], static_cast<size_t>(-1), &img)) {
      pe::walk_iat(img, &Engine::count_cb, &c);
    }
    FreeLibrary(pin);
  }
  return c.count;
}

// ---------------------------------------------------------------------------

void Inflight::reset() {
  for (uint32_t i = 0; i < kSlots; ++i) InterlockedExchange(&c_[i].n, 0);
}

uint32_t Inflight::enter() {
  // tid кратны 4: младшие биты не различают потоки.
  uint32_t slot = (GetCurrentThreadId() >> 2) & (kSlots - 1);
  InterlockedIncrement(&c_[slot].n);
  return slot;
}

void Inflight::leave(uint32_t slot) { InterlockedDecrement(&c_[slot & (kSlots - 1)].n); }

bool Inflight::idle() const {
  for (uint32_t i = 0; i < kSlots; ++i) {
    if (c_[i].n != 0) return false;
  }
  return true;
}

bool Inflight::wait_idle(DWORD timeout_ms) const {
  ULONGLONG start = GetTickCount64();
  for (;;) {
    if (idle()) return true;
    if (GetTickCount64() - start >= timeout_ms) return false;
    Sleep(1);
  }
}

const char* status_name(Status s) {
  switch (s) {
    case kOk: return "ok";
    case kErrBadImage: return "bad_image";
    case kErrNotFound: return "not_found";
    case kErrForwarder: return "forwarder";
    case kErrAlready: return "already";
    case kErrTooManyHooks: return "too_many_hooks";
    case kErrJournalFull: return "journal_full";
    case kErrNoStubSpace: return "no_stub_space";
    case kErrRva: return "rva";
    case kErrProtect: return "protect";
    case kErrRace: return "race";
    case kErrModules: return "modules";
  }
  return "unknown";
}

}  // namespace patch
}  // namespace dt
