// SPDX-License-Identifier: MIT
// Патч-движок на testlib.dll: EAT через стабы, IAT, delay-load IAT, поздний модуль,
// журнал и откат, второй обход IAT, перевод стабов, исключение своего модуля.
#include <windows.h>

#include <string.h>

#include "patch/patch.h"
#include "patch/pe.h"
#include "tests/lib/testlib.h"
#include "tests/test.h"

extern "C" {
__declspec(dllimport) int __cdecl tc_add(int a, int b);
__declspec(dllimport) int __cdecl tc_mul(int a, int b);
__declspec(dllimport) int __cdecl tc_sub(int a, int b);
}

using namespace dt;

namespace {

typedef int(__cdecl* BinFn)(int, int);

BinFn g_add = nullptr;
BinFn g_mul = nullptr;
BinFn g_sub = nullptr;

int __cdecl hook_add(int a, int b) { return g_add(a, b) + 1000; }
int __cdecl hook_mul(int a, int b) { return g_mul(a, b) + 2000; }
int __cdecl hook_sub(int a, int b) { return g_sub(a, b) + 3000; }

// Вызов через IAT tests.exe; noinline — адрес из IAT читается заново при каждом вызове.
__declspec(noinline) int call_add(int a, int b) { return tl_add(a, b); }
__declspec(noinline) int call_sub(int a, int b) { return tl_sub(a, b); }

patch::Engine g_eng;

HMODULE lib() { return GetModuleHandleW(L"testlib.dll"); }

uint32_t image_size(HMODULE m) {
  pe::Image img;
  return pe::open(m, static_cast<size_t>(-1), &img) ? img.size : 0;
}

}  // namespace

TEST(pe_exports_and_forwarder) {
  HMODULE m = lib();
  CHECK(m != nullptr);
  pe::Image img;
  CHECK(pe::open(m, static_cast<size_t>(-1), &img));
  pe::Export ex;
  CHECK(pe::find_export(img, "tl_add", &ex));
  CHECK(!ex.forwarder && ex.in_image);
  CHECK(reinterpret_cast<uint8_t*>(m) + ex.rva == reinterpret_cast<uint8_t*>(GetProcAddress(m, "tl_add")));
  CHECK(pe::find_export(img, "tl_fwd", &ex));
  CHECK(ex.forwarder);
  CHECK(!pe::find_export(img, "tl_nope", &ex));
  CHECK(!pe::find_export(img, "tl_ad", &ex));
}

TEST(pe_rejects_bad_images) {
  static uint8_t buf[8192];
  pe::Image img;
  memset(buf, 0, sizeof(buf));
  CHECK(!pe::open(buf, sizeof(buf), &img));  // нет MZ
  IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(buf);
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_lfanew = 5000;
  CHECK(!pe::open(buf, sizeof(buf), &img));  // e_lfanew за первой страницей
  dos->e_lfanew = 64;
  CHECK(!pe::open(buf, sizeof(buf), &img));  // нет PE\0\0
  // Заголовки настоящего модуля, но доступно меньше SizeOfImage.
  memcpy(buf, lib(), 4096);
  CHECK(!pe::open(buf, 4096, &img));
  CHECK(!pe::open(buf, 32, &img));
  // Чужая машина.
  IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(buf + reinterpret_cast<IMAGE_DOS_HEADER*>(buf)->e_lfanew);
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_ARM64;
  CHECK(!pe::open(buf, static_cast<size_t>(-1), &img));
}

TEST(pe_walk_iat_finds_import) {
  struct Ctx {
    void* want;
    int hits;
  };
  Ctx c = {reinterpret_cast<void*>(GetProcAddress(lib(), "tl_add")), 0};
  pe::Image img;
  CHECK(pe::open(GetModuleHandleW(nullptr), static_cast<size_t>(-1), &img));
  pe::walk_iat(
      img,
      [](void* ctx, void** slot, bool) -> bool {
        Ctx* cc = static_cast<Ctx*>(ctx);
        if (*slot == cc->want) ++cc->hits;
        return true;
      },
      &c);
  CHECK_EQ(c.hits, 1);
}

TEST(patch_full_cycle) {
  HMODULE m = lib();
  HMODULE cons = GetModuleHandleW(L"testcons.dll");
  CHECK(m != nullptr && cons != nullptr);
  void* orig_add = reinterpret_cast<void*>(GetProcAddress(m, "tl_add"));
  void* orig_mul = reinterpret_cast<void*>(GetProcAddress(m, "tl_mul"));
  // Слот delay-load tl_add в testcons связывается до патча, tl_mul — после.
  CHECK_EQ(tc_add(2, 3), 5);
  CHECK_EQ(call_add(2, 3), 5);

  g_eng.init(nullptr);
  void* stub_add = nullptr;
  void* stub_mul = nullptr;
  CHECK_EQ(g_eng.hook_export(m, "tl_add", reinterpret_cast<void*>(&hook_add), reinterpret_cast<void**>(&g_add),
                             &stub_add),
           patch::kOk);
  CHECK(reinterpret_cast<void*>(g_add) == orig_add);
  CHECK_EQ(g_eng.hook_export(m, "tl_mul", reinterpret_cast<void*>(&hook_mul), reinterpret_cast<void**>(&g_mul),
                             &stub_mul),
           patch::kOk);
  CHECK(reinterpret_cast<void*>(g_mul) == orig_mul);
  CHECK_EQ(g_eng.journal_size(), 2u);

  // Окно 4 ГБ: стаб выше образа, RVA помещается в 32 бита и вне каталога экспорта.
  uintptr_t base = reinterpret_cast<uintptr_t>(m);
  uintptr_t s = reinterpret_cast<uintptr_t>(stub_add);
  CHECK(s > base);
  CHECK(static_cast<uint64_t>(s - base) <= 0xFFFFFFFFull);
  CHECK(s - base >= image_size(m));
  CHECK_EQ(g_eng.hook(0).stub_rva, static_cast<uint32_t>(s - base));

  // Повтор, форвардер, отсутствующий экспорт.
  void* dummy = nullptr;
  CHECK_EQ(g_eng.hook_export(m, "tl_add", reinterpret_cast<void*>(&hook_add), &dummy), patch::kErrAlready);
  CHECK_EQ(g_eng.hook_export(m, "tl_fwd", reinterpret_cast<void*>(&hook_add), &dummy), patch::kErrForwarder);
  CHECK_EQ(g_eng.hook_export(m, "tl_nope", reinterpret_cast<void*>(&hook_add), &dummy), patch::kErrNotFound);
  // Другой движок видит EAT за образом — чужой перехват, не ставит свой поверх.
  {
    static patch::Engine other;
    other.init(nullptr);
    CHECK_EQ(other.hook_export(m, "tl_add", reinterpret_cast<void*>(&hook_add), &dummy), patch::kErrRva);
    pe::Image img;
    pe::Export ex;
    memset(&ex, 0, sizeof(ex));
    CHECK(pe::open(m, static_cast<size_t>(-1), &img) && pe::find_export(img, "tl_add", &ex));
    CHECK(!ex.in_image && ex.rva == static_cast<uint32_t>(s - base));
  }

  // После EAT: GetProcAddress отдаёт стаб, стаб ведёт в хук; IAT ещё исходный.
  CHECK(reinterpret_cast<void*>(GetProcAddress(m, "tl_add")) == stub_add);
  CHECK_EQ(reinterpret_cast<BinFn>(stub_add)(2, 3), 1005);
  CHECK_EQ(call_add(2, 3), 5);

  CHECK_EQ(g_eng.patch_iats(), patch::kOk);
  CHECK_EQ(call_add(2, 3), 1005);  // IAT tests.exe
  CHECK_EQ(tc_add(2, 3), 1005);    // delay-load, связан до патча -> пропатчен обходом
  CHECK_EQ(tc_mul(2, 3), 2006);    // delay-load, связывается сейчас через EAT -> стаб
  CHECK(g_eng.stats().iat_patched >= 1);
  CHECK(g_eng.stats().iat_delay_patched >= 1);

  // Поздний модуль: загрузчик ОС кладёт в его IAT адрес стаба.
  HMODULE late = LoadLibraryW(L"testlate.dll");
  CHECK(late != nullptr);
  BinFn tlate_add = late ? reinterpret_cast<BinFn>(GetProcAddress(late, "tlate_add")) : nullptr;
  CHECK(tlate_add != nullptr);
  if (tlate_add) CHECK_EQ(tlate_add(2, 3), 1005);
  CHECK(g_eng.count_iat_slots(stub_add) >= 1);
  CHECK(g_eng.count_iat_slots(stub_mul) >= 1);
  // Адрес вне IAT (сохранённый GetProcAddress) — переживёт откат.
  BinFn saved = reinterpret_cast<BinFn>(GetProcAddress(m, "tl_add"));

  g_eng.unpatch();
  const patch::Stats& st = g_eng.stats();
  CHECK_EQ(st.eat_restored, 2u);
  CHECK(st.iat_restored >= 2);
  CHECK(st.second_pass >= 2);  // testlate (add) и testcons (mul) держали стабы
  CHECK_EQ(st.mismatched, 0u);
  CHECK_EQ(st.module_gone, 0u);

  CHECK_EQ(call_add(2, 3), 5);
  CHECK_EQ(tc_add(2, 3), 5);
  CHECK_EQ(tc_mul(2, 3), 6);
  if (tlate_add) CHECK_EQ(tlate_add(2, 3), 5);
  CHECK(reinterpret_cast<void*>(GetProcAddress(m, "tl_add")) == orig_add);
  CHECK(reinterpret_cast<void*>(GetProcAddress(m, "tl_mul")) == orig_mul);
  CHECK(reinterpret_cast<void*>(saved) == stub_add);
  CHECK_EQ(saved(2, 3), 5);  // стаб переведён на исходную функцию
  CHECK_EQ(g_eng.count_iat_slots(stub_add), 0u);
  CHECK_EQ(g_eng.count_iat_slots(stub_mul), 0u);
  CHECK_EQ(g_eng.count_iat_slots(reinterpret_cast<void*>(&hook_add)), 0u);
  CHECK_EQ(g_eng.count_iat_slots(reinterpret_cast<void*>(&hook_mul)), 0u);
  CHECK(g_eng.count_iat_slots(orig_add) >= 3);  // tests.exe, testcons, testlate
  // Повторный откат безвреден.
  g_eng.unpatch();
  CHECK_EQ(call_add(2, 3), 5);
  if (late) FreeLibrary(late);
}

TEST(patch_exclude_own_module) {
  HMODULE m = lib();
  HMODULE cons = GetModuleHandleW(L"testcons.dll");
  CHECK_EQ(tc_sub(5, 3), 2);  // связать delay-слот до патча
  g_eng.init(cons);           // testcons играет роль «своего» модуля
  CHECK_EQ(g_eng.hook_export(m, "tl_sub", reinterpret_cast<void*>(&hook_sub), reinterpret_cast<void**>(&g_sub)),
           patch::kOk);
  CHECK_EQ(g_eng.patch_iats(), patch::kOk);
  CHECK_EQ(call_sub(5, 3), 3002);
  CHECK_EQ(tc_sub(5, 3), 2);  // IAT исключённого модуля не тронут
  g_eng.unpatch();
  CHECK_EQ(call_sub(5, 3), 2);
  CHECK(reinterpret_cast<void*>(GetProcAddress(m, "tl_sub")) == reinterpret_cast<void*>(g_sub));
}

TEST(patch_journal_full) {
  HMODULE m = lib();
  void* orig_mul = reinterpret_cast<void*>(GetProcAddress(m, "tl_mul"));
  g_eng.init(nullptr, 1);  // место только под одну запись
  CHECK_EQ(g_eng.hook_export(m, "tl_add", reinterpret_cast<void*>(&hook_add), reinterpret_cast<void**>(&g_add)),
           patch::kOk);
  void* dummy = nullptr;
  // Журнал полон: EAT tl_mul не меняется.
  CHECK_EQ(g_eng.hook_export(m, "tl_mul", reinterpret_cast<void*>(&hook_mul), &dummy), patch::kErrJournalFull);
  CHECK(reinterpret_cast<void*>(GetProcAddress(m, "tl_mul")) == orig_mul);
  // Журнал полон: слоты IAT не меняются.
  CHECK_EQ(g_eng.patch_iats(), patch::kErrJournalFull);
  CHECK_EQ(call_add(2, 3), 5);
  CHECK_EQ(g_eng.stats().iat_patched, 0u);
  CHECK(g_eng.stats().journal_full >= 2);
  g_eng.unpatch();
  CHECK(reinterpret_cast<void*>(GetProcAddress(m, "tl_add")) == reinterpret_cast<void*>(g_add));
}

TEST(patch_repeat_cycles) {
  // Несколько циклов подряд: каждый — новый регион стабов, старые не освобождаются
  // и продолжают вести в исходную функцию.
  HMODULE m = lib();
  void* orig_add = reinterpret_cast<void*>(GetProcAddress(m, "tl_add"));
  void* stubs[3] = {nullptr, nullptr, nullptr};
  for (int i = 0; i < 3; ++i) {
    g_eng.init(nullptr);
    CHECK_EQ(g_eng.hook_export(m, "tl_add", reinterpret_cast<void*>(&hook_add), reinterpret_cast<void**>(&g_add),
                               &stubs[i]),
             patch::kOk);
    CHECK_EQ(g_eng.patch_iats(), patch::kOk);
    CHECK_EQ(call_add(1, 1), 1002);
    CHECK_EQ(g_eng.stats().regions, 1u);
    g_eng.unpatch();
    CHECK_EQ(call_add(1, 1), 2);
  }
  CHECK(stubs[0] != stubs[1] && stubs[1] != stubs[2]);
  for (int i = 0; i < 3; ++i) CHECK_EQ(reinterpret_cast<BinFn>(stubs[i])(1, 1), 2);
  CHECK(reinterpret_cast<void*>(GetProcAddress(m, "tl_add")) == orig_add);
}

namespace {

patch::Inflight g_inflight;

DWORD WINAPI inflight_worker(LPVOID) {
  uint32_t slot = g_inflight.enter();
  Sleep(60);
  g_inflight.leave(slot);
  return 0;
}

}  // namespace

TEST(patch_inflight_counters) {
  g_inflight.reset();
  CHECK(g_inflight.idle());
  uint32_t slot = g_inflight.enter();
  CHECK(!g_inflight.idle());
  CHECK(!g_inflight.wait_idle(5));
  g_inflight.leave(slot);
  CHECK(g_inflight.wait_idle(5));
  HANDLE th = CreateThread(nullptr, 0, inflight_worker, nullptr, 0, nullptr);
  CHECK(th != nullptr);
  Sleep(10);
  CHECK(g_inflight.wait_idle(2000));
  WaitForSingleObject(th, INFINITE);
  CloseHandle(th);
  CHECK(g_inflight.idle());
}
