// SPDX-License-Identifier: MIT
// DLL темы. M0: заготовка — DllMain по контракту и пустые DtStart/DtStop.
// Работа (JSON, PNG, патчи) появится на M4 и пойдёт только в DtStart/DtStop,
// которые загрузчик вызывает отдельным удалённым потоком, не под loader lock.
#include <windows.h>

namespace {

HINSTANCE g_instance = nullptr;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;

}  // namespace

// Под loader lock: только HINSTANCE, DisableThreadLibraryCalls и критическая секция.
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_instance = instance;
    DisableThreadLibraryCalls(instance);
    InitializeCriticalSection(&g_lock);
    g_lock_ready = true;
  } else if (reason == DLL_PROCESS_DETACH) {
    // reserved != NULL — процесс завершается: ничего не делать.
    // reserved == NULL без DtStop — аварийный откат журнала (с M4; патчей пока нет).
    if (reserved == nullptr && g_lock_ready) {
      DeleteCriticalSection(&g_lock);
      g_lock_ready = false;
    }
  }
  return TRUE;
}

// Код возврата 0 — успех. M0: патчей нет, читать нечего.
extern "C" DWORD WINAPI DtStart(LPVOID) {
  EnterCriticalSection(&g_lock);
  LeaveCriticalSection(&g_lock);
  return 0;
}

extern "C" DWORD WINAPI DtStop(LPVOID) {
  EnterCriticalSection(&g_lock);
  LeaveCriticalSection(&g_lock);
  return 0;
}
