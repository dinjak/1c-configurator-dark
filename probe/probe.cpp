// SPDX-License-Identifier: MIT
// Диагностическая DLL (только чтение). M0: заготовка — DllMain по контракту и
// пустые DtStart/DtStop. Три хука cairo на чтение появятся на M3b.
#include <windows.h>

namespace {

HINSTANCE g_instance = nullptr;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_instance = instance;
    DisableThreadLibraryCalls(instance);
    InitializeCriticalSection(&g_lock);
    g_lock_ready = true;
  } else if (reason == DLL_PROCESS_DETACH) {
    if (reserved == nullptr && g_lock_ready) {
      DeleteCriticalSection(&g_lock);
      g_lock_ready = false;
    }
  }
  return TRUE;
}

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
