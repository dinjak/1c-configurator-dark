// SPDX-License-Identifier: MIT
// Нативный загрузчик. M0: заготовка — печатает использование и выходит с кодом 2.
// Процессы не открываются, DLL не загружаются: команды появятся на M2.
#include <windows.h>

namespace {

const DWORD kExitUsage = 2;

void write_out(const wchar_t* text) {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h == nullptr || h == INVALID_HANDLE_VALUE) return;
  DWORD len = static_cast<DWORD>(lstrlenW(text));
  DWORD mode, done;
  if (GetConsoleMode(h, &mode)) {
    WriteConsoleW(h, text, len, &done, nullptr);
    return;
  }
  // Перенаправленный вывод — UTF-8.
  char buf[4096];
  int n = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(len), buf, sizeof(buf), nullptr, nullptr);
  if (n > 0) WriteFile(h, buf, static_cast<DWORD>(n), &done, nullptr);
}

const wchar_t kUsage[] =
    L"cfgdark — тёмная тема конфигуратора 1С\n"
    L"\n"
    L"Использование:\n"
    L"  cfgdark load          загрузить тему в открытый конфигуратор\n"
    L"  cfgdark unload        выгрузить тему\n"
    L"  cfgdark status        состояние\n"
    L"  cfgdark probe-load    загрузить диагностическую DLL\n"
    L"  cfgdark probe-dump    снять данные probe\n"
    L"  cfgdark probe-unload  выгрузить probe\n"
    L"  --pid N               только вместе со всеми проверками цели\n"
    L"\n"
    L"Сборка M0: команды ещё не реализованы, процессы не открываются.\n";

}  // namespace

int wmain(int, wchar_t**) {
  write_out(kUsage);
  return static_cast<int>(kExitUsage);
}
