// SPDX-License-Identifier: MIT
// DLL темы. M2: жизненный цикл — DllMain по контракту и DtStart/DtStop с защитой от
// повторной загрузки; патчей нет. Работа (JSON, PNG, патчи) появится на M4 и пойдёт
// только в DtStart/DtStop, которые загрузчик вызывает отдельным удалённым потоком,
// не под loader lock.
#include <windows.h>

namespace {

// Коды DtStart; загрузчик (loader/loader.cpp) знает их же.
const DWORD kStartOk = 0;
const DWORD kStartAlready = 1;  // в процессе уже активна тема или probe
const DWORD kStartError = 2;

HINSTANCE g_instance = nullptr;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;
// Защита от повторной загрузки: Local\cfgdark-<pid>, общий для темы и probe.
// Создаётся в DtStart, закрывается в DtStop (аварийно — в DLL_PROCESS_DETACH).
HANDLE g_guard = nullptr;

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

}  // namespace

// DllMain — под loader lock: на attach только HINSTANCE, DisableThreadLibraryCalls и
// критическая секция; на detach с reserved == NULL — закрыть объект защиты и удалить
// критическую секцию; без файлов, патчей и ожиданий.
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_instance = instance;
    DisableThreadLibraryCalls(instance);
    InitializeCriticalSection(&g_lock);
    g_lock_ready = true;
  } else if (reason == DLL_PROCESS_DETACH) {
    // reserved != NULL — процесс завершается: ничего не делать.
    // reserved == NULL без DtStop — аварийный откат журнала (с M4; патчей пока нет)
    // и освобождение защиты от повтора.
    if (reserved == nullptr && g_lock_ready) {
      if (g_guard != nullptr) {
        CloseHandle(g_guard);
        g_guard = nullptr;
      }
      DeleteCriticalSection(&g_lock);
      g_lock_ready = false;
    }
  }
  return TRUE;
}

// 0 — успех; kStartAlready — тема или probe уже активны в процессе; патчи не ставятся.
extern "C" DWORD WINAPI DtStart(LPVOID) {
  EnterCriticalSection(&g_lock);
  DWORD rc = kStartOk;
  if (g_guard != nullptr) {
    rc = kStartAlready;
  } else {
    wchar_t name[32];
    guard_name(name);
    HANDLE h = CreateMutexW(nullptr, FALSE, name);
    if (h == nullptr) {
      rc = kStartError;
    } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(h);
      rc = kStartAlready;
    } else {
      g_guard = h;
    }
  }
  LeaveCriticalSection(&g_lock);
  return rc;
}

// M2: откатывать нечего — только освободить защиту от повтора. Повторный вызов безопасен.
extern "C" DWORD WINAPI DtStop(LPVOID) {
  EnterCriticalSection(&g_lock);
  if (g_guard != nullptr) {
    CloseHandle(g_guard);
    g_guard = nullptr;
  }
  LeaveCriticalSection(&g_lock);
  return 0;
}
