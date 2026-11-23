// SPDX-License-Identifier: MIT
// Нативный загрузчик темы и probe: cfgdark_x86.exe и cfgdark_x64.exe из одного исходника.
// Цель — только образ 1cv8.exe с видимым окном «Конфигуратор». До проверок образа, окна и
// битности процесс открывается только с PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
// память цели (снимок модулей Toolhelp, заголовок модуля при выгрузке) читается только после
// них, права на запись — только после всех проверок. Операции над одним pid сериализует
// Local\cfgdark-op-<pid>. DLL — только соседний с загрузчиком файл с фиксированным именем;
// параметра --dll нет. Коды выхода — таблица docs/architecture.md, «Загрузчик».
// probe-dump вызывает DtDump probe и печатает снимок DtSnapshot как JSON в stdout.
#include <windows.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdint.h>

#include "probe/snapshot.h"

namespace {

enum : int {
  kOk = 0,
  kUsage = 2,
  kNoTarget = 3,
  kAmbiguous = 4,
  kNotSettled = 5,
  kBitness = 6,
  kBadDll = 7,
  kAlready = 10,
  kWinApi = 11,
  kStartFailed = 12,
  kNotLoaded = 13,
  kStopFailed = 14,
  kStillLoaded = 15,
  kForeign = 16,
  kBusy = 17,
  kDumpFailed = 18,
};

#if defined(_M_X64)
const wchar_t kArch[] = L"x64";
const WORD kMachine = IMAGE_FILE_MACHINE_AMD64;
const int kArchIndex = 1;
#elif defined(_M_IX86)
const wchar_t kArch[] = L"x86";
const WORD kMachine = IMAGE_FILE_MACHINE_I386;
const int kArchIndex = 0;
#else
#error "loader: only x86 and x64"
#endif

enum Kind { kTheme = 0, kProbe = 1 };

// [вид][битность]: модули темы и probe любой битности.
const wchar_t* const kDllNames[2][2] = {
    {L"cfgdark_theme_x86.dll", L"cfgdark_theme_x64.dll"},
    {L"cfgdark_probe_x86.dll", L"cfgdark_probe_x64.dll"},
};
const wchar_t* const kKindText[2] = {L"тема", L"probe"};

const wchar_t kImageName[] = L"1cv8.exe";
const wchar_t kTitleWord[] = L"Конфигуратор";

const DWORD kSettleMs = 25000;  // окно и процесс живут не меньше; обхода нет
const DWORD kPollMs = 250;
const DWORD kLoadTimeoutMs = 15000;
const DWORD kStartTimeoutMs = 10000;
const DWORD kStopTimeoutMs = 5000;
const DWORD kFreeTimeoutMs = 10000;
const DWORD kDumpTimeoutMs = 5000;
const DWORD kMaxDllBytes = 16u << 20;

// Коды DtStart — те же, что в theme/theme.cpp и probe/probe.cpp.
const DWORD kDtStartAlready = 1;

// Права после всех проверок: удалённый поток, строка пути, чтение заголовка модуля.
const DWORD kWriteAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION |
                           PROCESS_VM_READ | PROCESS_VM_WRITE | SYNCHRONIZE;

// ---------------------------------------------------------------------------
// Вывод

void write_text(DWORD std_handle, const wchar_t* text) {
  HANDLE h = GetStdHandle(std_handle);
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

void vformat(wchar_t* buf, const wchar_t* fmt, va_list ap) {
  // wvsprintfW пишет не больше 1024 символов; буфер на 1025.
  buf[0] = 0;
  wvsprintfW(buf, fmt, ap);
  buf[1024] = 0;
}

void say(const wchar_t* fmt, ...) {
  wchar_t buf[1025];
  va_list ap;
  va_start(ap, fmt);
  vformat(buf, fmt, ap);
  va_end(ap);
  write_text(STD_OUTPUT_HANDLE, buf);
}

void warn(const wchar_t* fmt, ...) {
  wchar_t buf[1025];
  va_list ap;
  va_start(ap, fmt);
  vformat(buf, fmt, ap);
  va_end(ap);
  write_text(STD_ERROR_HANDLE, buf);
}

// Печатает причину отказа и возвращает код выхода.
int fail(int code, const wchar_t* fmt, ...) {
  wchar_t msg[1025];
  va_list ap;
  va_start(ap, fmt);
  vformat(msg, fmt, ap);
  va_end(ap);
  warn(L"отказ (%d): %s\n", code, msg);
  return code;
}

const wchar_t kUsageText[] =
    L"cfgdark — тёмная тема конфигуратора 1С\n"
    L"\n"
    L"Использование: cfgdark_<x86|x64> <команда> [--pid N]\n"
    L"  load          загрузить тему в открытый конфигуратор\n"
    L"  unload        выгрузить тему\n"
    L"  status        состояние: цель, битность, загружены ли тема и probe\n"
    L"  probe-load    загрузить диагностическую DLL\n"
    L"  probe-dump    снять данные probe: JSON в stdout, счётчики эпохи обнуляются\n"
    L"  probe-unload  выгрузить probe\n"
    L"  --pid N       выбрать процесс; все проверки цели остаются\n"
    L"\n"
    L"Цель — 1cv8.exe с видимым окном «Конфигуратор». Перед загрузкой загрузчик\n"
    L"сам наблюдает окно 25 с. DLL берётся только из каталога загрузчика.\n"
    L"Коды выхода: 0 успех, 2 использование, 3 нет цели, 4 цель неоднозначна,\n"
    L"5 окно не простояло 25 с, 6 битность, 7 DLL, 10 уже загружено, 11 WinAPI,\n"
    L"12 DtStart, 13 не загружено, 14 DtStop, 15 модуль остался, 16 чужой модуль,\n"
    L"17 над pid уже идёт операция другого загрузчика,\n"
    L"18 probe-dump: DtDump не завершился за 5 с, вернул ошибку или снимок не прошёл проверку.\n";

int usage() {
  write_text(STD_OUTPUT_HANDLE, kUsageText);
  return kUsage;
}

// ---------------------------------------------------------------------------
// Строки

bool eq(const wchar_t* a, const wchar_t* b) {
  return CompareStringOrdinal(a, -1, b, -1, FALSE) == CSTR_EQUAL;
}

bool ieq(const wchar_t* a, const wchar_t* b) {
  return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

const wchar_t* base_name(const wchar_t* path) {
  const wchar_t* b = path;
  for (const wchar_t* p = path; *p; ++p) {
    if (*p == L'\\' || *p == L'/') b = p + 1;
  }
  return b;
}

// Копия с явным нулём; не влезло — пустая строка и ложь.
bool copy_str(wchar_t* dst, size_t cap, const wchar_t* src) {
  size_t i = 0;
  for (; src[i]; ++i) {
    if (i + 1 >= cap) {
      dst[0] = 0;
      return false;
    }
    dst[i] = src[i];
  }
  dst[i] = 0;
  return true;
}

bool parse_pid(const wchar_t* s, DWORD* out) {
  ULONGLONG v = 0;
  int n = 0;
  for (; s[n]; ++n) {
    if (n >= 10 || s[n] < L'0' || s[n] > L'9') return false;
    v = v * 10 + static_cast<ULONGLONG>(s[n] - L'0');
  }
  if (n == 0 || v == 0 || v > 0xFFFFFFFFull) return false;
  *out = static_cast<DWORD>(v);
  return true;
}

const wchar_t* machine_name(WORD m) {
  switch (m) {
    case IMAGE_FILE_MACHINE_I386: return L"x86";
    case IMAGE_FILE_MACHINE_AMD64: return L"x64";
    case IMAGE_FILE_MACHINE_ARM64: return L"arm64";
    default: return L"?";
  }
}

// ---------------------------------------------------------------------------
// Окно «Конфигуратор»

// Единственное правило заголовка (решение владельца 2026-11-22): подстрока «Конфигуратор»
// в любом месте. Формат 8.3.27: «<активное окно> - Конфигуратор - <конфигурация>», часть
// до слова меняется при переключении окон внутри 1С.
bool classify_title(const wchar_t* title, int len) {
  const int n = static_cast<int>(ARRAYSIZE(kTitleWord)) - 1;
  for (int i = 0; i + n <= len; ++i) {
    if (CompareStringOrdinal(title + i, n, kTitleWord, n, FALSE) == CSTR_EQUAL) return true;
  }
  return false;
}

// Видимое окно pid без владельца (верхний уровень обеспечивает вызывающий).
bool window_candidate(HWND hwnd, DWORD pid) {
  DWORD owner_pid = 0;
  GetWindowThreadProcessId(hwnd, &owner_pid);
  return owner_pid == pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr;
}

// Заголовок окна чужого процесса GetWindowTextW берёт без WM_GETTEXT; не печатается.
bool window_title_matches(HWND hwnd) {
  wchar_t title[256];
  int len = GetWindowTextW(hwnd, title, ARRAYSIZE(title));
  if (len < 0) len = 0;
  title[ARRAYSIZE(title) - 1] = 0;
  return classify_title(title, len);
}

struct WindowScan {
  DWORD pid;
  int visible;  // видимых окон верхнего уровня без владельца
  HWND match;   // первое из них со словом в заголовке или nullptr
};

BOOL CALLBACK scan_window(HWND hwnd, LPARAM lp) {
  WindowScan* s = reinterpret_cast<WindowScan*>(lp);
  if (!window_candidate(hwnd, s->pid)) return TRUE;
  ++s->visible;
  if (s->match == nullptr && window_title_matches(hwnd)) s->match = hwnd;
  return TRUE;
}

WindowScan scan_windows(DWORD pid) {
  WindowScan s = {pid, 0, nullptr};
  EnumWindows(scan_window, reinterpret_cast<LPARAM>(&s));
  return s;
}

// Наблюдение держится за окно, а не за текст заголовка: тот же HWND, тот же pid,
// верхний уровень, видимое, без владельца, слово по-прежнему в заголовке.
bool window_still_target(HWND hwnd, DWORD pid) {
  return hwnd != nullptr && IsWindow(hwnd) && GetAncestor(hwnd, GA_PARENT) == GetDesktopWindow() &&
         window_candidate(hwnd, pid) && window_title_matches(hwnd);
}

// ---------------------------------------------------------------------------
// Процесс-цель

struct Target {
  DWORD pid;
  HANDLE query;  // PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE — не запись
  FILETIME created;
  WORD machine;
  WindowScan win;
};

bool image_is_1cv8(HANDLE h) {
  wchar_t path[1024];
  DWORD size = ARRAYSIZE(path);
  if (!QueryFullProcessImageNameW(h, 0, path, &size) || size >= ARRAYSIZE(path)) return false;
  path[size] = 0;
  return ieq(base_name(path), kImageName);
}

bool process_machine(HANDLE h, WORD* out) {
  USHORT process = 0, native = 0;
  if (!IsWow64Process2(h, &process, &native)) return false;
  *out = process != IMAGE_FILE_MACHINE_UNKNOWN ? process : native;
  return true;
}

ULONGLONG process_age_ms(const FILETIME& created) {
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER a, b;
  a.LowPart = created.dwLowDateTime;
  a.HighPart = created.dwHighDateTime;
  b.LowPart = now.dwLowDateTime;
  b.HighPart = now.dwHighDateTime;
  return b.QuadPart > a.QuadPart ? (b.QuadPart - a.QuadPart) / 10000 : 0;
}

enum Inspect { kInspOk, kInspGone, kInspDenied, kInspNotImage, kInspError };

// Только PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE.
Inspect inspect(DWORD pid, Target* t, DWORD* err) {
  ZeroMemory(t, sizeof(*t));
  t->pid = pid;
  *err = 0;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
  if (h == nullptr) {
    *err = GetLastError();
    if (*err == ERROR_INVALID_PARAMETER) return kInspGone;
    if (*err == ERROR_ACCESS_DENIED) return kInspDenied;
    return kInspError;
  }
  if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
    CloseHandle(h);
    return kInspGone;
  }
  if (!image_is_1cv8(h)) {
    CloseHandle(h);
    return kInspNotImage;
  }
  FILETIME exit_time, kernel_time, user_time;
  if (!GetProcessTimes(h, &t->created, &exit_time, &kernel_time, &user_time) ||
      !process_machine(h, &t->machine)) {
    *err = GetLastError();
    CloseHandle(h);
    return kInspError;
  }
  t->win = scan_windows(pid);
  t->query = h;
  return kInspOk;
}

void close_target(Target* t) {
  if (t->query != nullptr) CloseHandle(t->query);
  t->query = nullptr;
}

// Цель: 1cv8.exe с видимым окном «Конфигуратор». Без --pid несколько целей — отказ.
int select_target(DWORD want, Target* out) {
  DWORD err = 0;
  ZeroMemory(out, sizeof(*out));
  if (want != 0) {
    switch (inspect(want, out, &err)) {
      case kInspGone: return fail(kNoTarget, L"процесса %lu нет", want);
      case kInspDenied: return fail(kWinApi, L"нет доступа к сведениям процесса %lu (ошибка %lu)", want, err);
      case kInspError: return fail(kWinApi, L"сведения процесса %lu: ошибка %lu", want, err);
      case kInspNotImage: return fail(kNoTarget, L"процесс %lu — не %s", want, kImageName);
      case kInspOk: break;
    }
    if (out->win.match == nullptr) {
      close_target(out);
      return fail(kNoTarget, L"у процесса %lu нет видимого окна «Конфигуратор»", want);
    }
    return kOk;
  }

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return fail(kWinApi, L"снимок процессов: ошибка %lu", GetLastError());
  int found = 0;
  wchar_t pids[256];
  pids[0] = 0;
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
    if (!ieq(pe.szExeFile, kImageName)) continue;
    Target t;
    Inspect r = inspect(pe.th32ProcessID, &t, &err);
    if (r == kInspDenied || r == kInspError) {
      warn(L"pid %lu: сведения недоступны (ошибка %lu), пропущен\n", pe.th32ProcessID, err);
      continue;
    }
    if (r != kInspOk) continue;
    if (t.win.match == nullptr) {
      close_target(&t);
      continue;
    }
    if (found == 0) {
      *out = t;
    } else {
      close_target(&t);
    }
    int len = lstrlenW(pids);
    if (len < 200) wsprintfW(pids + len, found == 0 ? L"%lu" : L", %lu", pe.th32ProcessID);
    ++found;
  }
  CloseHandle(snap);
  if (found == 0) return fail(kNoTarget, L"нет %s с видимым окном «Конфигуратор»", kImageName);
  if (found > 1) {
    close_target(out);
    return fail(kAmbiguous, L"подходящих процессов %d (pid %s), укажите --pid", found, pids);
  }
  return kOk;
}

int check_bitness(const Target& t) {
  if (t.machine != kMachine) {
    return fail(kBitness, L"битность pid %lu — %s, загрузчика — %s", t.pid, machine_name(t.machine), kArch);
  }
  return kOk;
}

// Наблюдение: то же окно «Конфигуратор» подходит на каждом опросе 25 с подряд, процесс жив,
// в конце возраст процесса не меньше 25 с.
int observe(const Target& t) {
  say(L"pid %lu: наблюдаю окно «Конфигуратор» %lu с…\n", t.pid, kSettleMs / 1000);
  const ULONGLONG start = GetTickCount64();
  for (;;) {
    if (WaitForSingleObject(t.query, 0) == WAIT_OBJECT_0) {
      return fail(kNotSettled, L"pid %lu завершился во время наблюдения", t.pid);
    }
    if (!window_still_target(t.win.match, t.pid)) {
      return fail(kNotSettled, L"окно «Конфигуратор» pid %lu пропало до 25 с", t.pid);
    }
    if (GetTickCount64() - start >= kSettleMs) break;
    Sleep(kPollMs);
  }
  if (process_age_ms(t.created) < kSettleMs) {
    return fail(kNotSettled, L"возраст pid %lu меньше 25 с", t.pid);
  }
  return kOk;
}

// Повторно после OpenProcess на запись: тот же образ и то же время создания.
int open_for_write(const Target& t, HANDLE* out) {
  *out = nullptr;
  HANDLE h = OpenProcess(kWriteAccess, FALSE, t.pid);
  if (h == nullptr) return fail(kWinApi, L"OpenProcess pid %lu: ошибка %lu", t.pid, GetLastError());
  FILETIME created, exit_time, kernel_time, user_time;
  if (!image_is_1cv8(h) || !GetProcessTimes(h, &created, &exit_time, &kernel_time, &user_time) ||
      CompareFileTime(&created, &t.created) != 0 || WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
    CloseHandle(h);
    return fail(kNoTarget, L"pid %lu сменился между проверками", t.pid);
  }
  *out = h;
  return kOk;
}

// ---------------------------------------------------------------------------
// Файл DLL: соседний с загрузчиком, фиксированное имя, Machine и экспорты из файла

struct FileView {
  const BYTE* data;
  DWORD size;
};

bool view_range(const FileView& v, DWORD off, DWORD len) {
  return off <= v.size && len <= v.size - off;
}

// Заголовки PE (IMAGE_NT_HEADERS своей разрядности); Machine проверяет вызывающий.
const IMAGE_NT_HEADERS* nt_headers(const FileView& v) {
  if (!view_range(v, 0, sizeof(IMAGE_DOS_HEADER))) return nullptr;
  const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(v.data);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
  DWORD off = static_cast<DWORD>(dos->e_lfanew);
  if (!view_range(v, off, sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER))) return nullptr;
  const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(v.data + off);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
  return nt;
}

struct Sections {
  const IMAGE_SECTION_HEADER* sec;
  WORD count;
  DWORD size_of_headers;
};

// RVA → смещение в файле; chars — атрибуты секции (0 для заголовков).
bool rva_to_off(const FileView& v, const Sections& s, DWORD rva, DWORD len, DWORD* off, DWORD* chars) {
  if (rva < s.size_of_headers) {
    if (len > s.size_of_headers - rva || !view_range(v, rva, len)) return false;
    *off = rva;
    if (chars) *chars = 0;
    return true;
  }
  for (WORD i = 0; i < s.count; ++i) {
    const IMAGE_SECTION_HEADER& h = s.sec[i];
    if (rva < h.VirtualAddress || rva - h.VirtualAddress >= h.SizeOfRawData) continue;
    DWORD in = rva - h.VirtualAddress;
    if (len > h.SizeOfRawData - in) return false;
    DWORD o = h.PointerToRawData + in;
    if (o < h.PointerToRawData || !view_range(v, o, len)) return false;
    *off = o;
    if (chars) *chars = h.Characteristics;
    return true;
  }
  return false;
}

// Секция, целиком содержащая [rva, rva + len) по виртуальному размеру (данные .bss
// в файле не лежат, поэтому rva_to_off для них не годится).
bool rva_section(const Sections& s, DWORD rva, DWORD len, DWORD* chars) {
  for (WORD i = 0; i < s.count; ++i) {
    const IMAGE_SECTION_HEADER& h = s.sec[i];
    DWORD vs = h.Misc.VirtualSize != 0 ? h.Misc.VirtualSize : h.SizeOfRawData;
    if (rva < h.VirtualAddress || rva - h.VirtualAddress >= vs) continue;
    if (len > vs - (rva - h.VirtualAddress)) return false;
    *chars = h.Characteristics;
    return true;
  }
  return false;
}

bool ascii_eq(const FileView& v, DWORD off, const char* want) {
  for (DWORD i = 0;; ++i) {
    if (off + i < off || off + i >= v.size) return false;
    char c = static_cast<char>(v.data[off + i]);
    if (c != want[i]) return false;
    if (c == 0) return true;
  }
}

enum DllStatus {
  kDllOk,
  kDllExePath,
  kDllMissing,
  kDllOpen,
  kDllPath,
  kDllSize,
  kDllRead,
  kDllMachine,
  kDllBad,
  kDllNoExport,
  kDllNoProbeExport,
};

const wchar_t* dll_status_text(DllStatus s) {
  switch (s) {
    case kDllOk: return L"готова";
    case kDllExePath: return L"путь загрузчика не разобран (не локальный диск или длиннее MAX_PATH)";
    case kDllMissing: return L"нет файла рядом с загрузчиком";
    case kDllOpen: return L"файл не открыт на чтение (занят записью?)";
    case kDllPath: return L"путь после разрешения ссылок не совпал с каталогом загрузчика";
    case kDllSize: return L"неверный размер файла";
    case kDllRead: return L"ошибка чтения файла";
    case kDllMachine: return L"Machine не совпадает с битностью загрузчика";
    case kDllBad: return L"файл не PE-DLL или повреждён";
    case kDllNoExport: return L"нет экспортов DtStart/DtStop";
    case kDllNoProbeExport: return L"нет экспортов probe DtDump/DtSnapshot";
  }
  return L"?";
}

struct DllImage {
  wchar_t path[MAX_PATH];  // нормализованный полный путь «X:\…»
  HANDLE file;             // открыт без общего доступа на запись до конца операции
  DWORD rva_start;
  DWORD rva_stop;
  DWORD rva_dump;      // только probe
  DWORD rva_snapshot;  // только probe: DtSnapshot (DATA)
  DWORD time_stamp;
  DWORD size_of_image;
};

DllStatus parse_dll(Kind kind, const FileView& v, DllImage* out) {
  const IMAGE_NT_HEADERS* nt = nt_headers(v);
  if (nt == nullptr) return kDllBad;
  const IMAGE_FILE_HEADER& fh = nt->FileHeader;
  if (fh.Machine != kMachine) return kDllMachine;
  if (!(fh.Characteristics & IMAGE_FILE_DLL)) return kDllBad;
  DWORD opt_off = static_cast<DWORD>(reinterpret_cast<const BYTE*>(&nt->OptionalHeader) - v.data);
  if (fh.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER) || !view_range(v, opt_off, fh.SizeOfOptionalHeader)) {
    return kDllBad;
  }
  const IMAGE_OPTIONAL_HEADER& oh = nt->OptionalHeader;
  if (oh.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) return kDllMachine;
  if (oh.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return kDllNoExport;

  DWORD sec_off = opt_off + fh.SizeOfOptionalHeader;
  DWORD sec_len = static_cast<DWORD>(fh.NumberOfSections) * static_cast<DWORD>(sizeof(IMAGE_SECTION_HEADER));
  if (fh.NumberOfSections == 0 || !view_range(v, sec_off, sec_len)) return kDllBad;
  Sections s = {reinterpret_cast<const IMAGE_SECTION_HEADER*>(v.data + sec_off), fh.NumberOfSections,
                oh.SizeOfHeaders};

  const IMAGE_DATA_DIRECTORY& ed = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (ed.VirtualAddress == 0 || ed.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) return kDllNoExport;
  DWORD off = 0;
  if (!rva_to_off(v, s, ed.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY), &off, nullptr)) return kDllBad;
  const IMAGE_EXPORT_DIRECTORY* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(v.data + off);
  DWORD nfun = ex->NumberOfFunctions;
  DWORD nname = ex->NumberOfNames;
  if (nfun == 0 || nfun > 65536 || nname > nfun) return kDllBad;
  DWORD off_fun = 0, off_name = 0, off_ord = 0;
  if (!rva_to_off(v, s, ex->AddressOfFunctions, nfun * 4, &off_fun, nullptr)) return kDllBad;
  if (nname != 0 && (!rva_to_off(v, s, ex->AddressOfNames, nname * 4, &off_name, nullptr) ||
                     !rva_to_off(v, s, ex->AddressOfNameOrdinals, nname * 2, &off_ord, nullptr))) {
    return kDllBad;
  }
  const DWORD* funs = reinterpret_cast<const DWORD*>(v.data + off_fun);
  const DWORD* names = reinterpret_cast<const DWORD*>(v.data + off_name);
  const WORD* ords = reinterpret_cast<const WORD*>(v.data + off_ord);

  DWORD rva_start = 0, rva_stop = 0, rva_dump = 0, rva_snapshot = 0;
  for (DWORD i = 0; i < nname; ++i) {
    DWORD name_off = 0;
    if (!rva_to_off(v, s, names[i], 1, &name_off, nullptr)) return kDllBad;
    DWORD* slot = nullptr;
    bool data = false;
    if (ascii_eq(v, name_off, "DtStart")) {
      slot = &rva_start;
    } else if (ascii_eq(v, name_off, "DtStop")) {
      slot = &rva_stop;
    } else if (kind == kProbe && ascii_eq(v, name_off, "DtDump")) {
      slot = &rva_dump;
    } else if (kind == kProbe && ascii_eq(v, name_off, "DtSnapshot")) {
      slot = &rva_snapshot;
      data = true;
    } else {
      continue;
    }
    if (ords[i] >= nfun) return kDllBad;
    DWORD fr = funs[ords[i]];
    if (fr == 0 || fr >= oh.SizeOfImage) return kDllBad;
    if (fr >= ed.VirtualAddress && fr - ed.VirtualAddress < ed.Size) return kDllBad;  // форвардер
    if (data) {
      // Снимок: целиком в образе, в неисполняемой секции с записью.
      DWORD chars = 0;
      const DWORD need = static_cast<DWORD>(sizeof(dt::snap::ProbeSnapshot));
      if (need > oh.SizeOfImage - fr || !rva_section(s, fr, need, &chars) || (chars & IMAGE_SCN_MEM_EXECUTE) ||
          !(chars & IMAGE_SCN_MEM_WRITE)) {
        return kDllBad;
      }
    } else {
      DWORD fo = 0, chars = 0;
      if (!rva_to_off(v, s, fr, 1, &fo, &chars) || !(chars & IMAGE_SCN_MEM_EXECUTE)) return kDllBad;
    }
    *slot = fr;
  }
  if (rva_start == 0 || rva_stop == 0) return kDllNoExport;
  if (kind == kProbe && (rva_dump == 0 || rva_snapshot == 0)) return kDllNoProbeExport;
  out->rva_start = rva_start;
  out->rva_stop = rva_stop;
  out->rva_dump = rva_dump;
  out->rva_snapshot = rva_snapshot;
  out->time_stamp = fh.TimeDateStamp;
  out->size_of_image = oh.SizeOfImage;
  return kDllOk;
}

// Полный путь после разрешения ссылок; только локальный «X:\…» короче MAX_PATH.
bool final_path(HANDLE h, wchar_t* out) {
  wchar_t buf[MAX_PATH + 8];
  DWORD n = GetFinalPathNameByHandleW(h, buf, ARRAYSIZE(buf), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n == 0 || n >= ARRAYSIZE(buf)) return false;
  buf[n] = 0;
  const wchar_t* p = buf;
  if (p[0] == L'\\' && p[1] == L'\\' && p[2] == L'?' && p[3] == L'\\') p += 4;
  bool letter = (p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z');
  if (!letter || p[1] != L':' || p[2] != L'\\') return false;  // UNC и прочие формы — отказ
  return copy_str(out, MAX_PATH, p);
}

void close_dll(DllImage* d) {
  if (d->file != INVALID_HANDLE_VALUE) CloseHandle(d->file);
  d->file = INVALID_HANDLE_VALUE;
}

DllStatus open_dll(Kind kind, DllImage* out) {
  ZeroMemory(out, sizeof(*out));
  out->file = INVALID_HANDLE_VALUE;

  wchar_t exe[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return kDllExePath;
  HANDLE eh = CreateFileW(exe, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (eh == INVALID_HANDLE_VALUE) return kDllExePath;
  wchar_t expect[MAX_PATH];
  bool ok = final_path(eh, expect);
  CloseHandle(eh);
  if (!ok) return kDllExePath;
  wchar_t* dir_end = const_cast<wchar_t*>(base_name(expect));
  *dir_end = 0;
  size_t dir_len = static_cast<size_t>(dir_end - expect);
  if (!copy_str(dir_end, MAX_PATH - dir_len, kDllNames[kind][kArchIndex])) return kDllExePath;

  HANDLE fh = CreateFileW(expect, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (fh == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? kDllMissing : kDllOpen;
  }
  out->file = fh;
  // Файл-ссылка ведёт в другой каталог — отказ.
  if (!final_path(fh, out->path) || !ieq(out->path, expect)) {
    close_dll(out);
    return kDllPath;
  }

  LARGE_INTEGER size;
  if (!GetFileSizeEx(fh, &size) || size.QuadPart < 512 || size.QuadPart > kMaxDllBytes) {
    close_dll(out);
    return kDllSize;
  }
  DWORD bytes = static_cast<DWORD>(size.QuadPart);
  BYTE* data = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), 0, bytes));
  if (data == nullptr) {
    close_dll(out);
    return kDllRead;
  }
  DWORD got = 0;
  while (got < bytes) {
    DWORD chunk = 0;
    if (!ReadFile(fh, data + got, bytes - got, &chunk, nullptr) || chunk == 0) break;
    got += chunk;
  }
  DllStatus st = kDllRead;
  if (got == bytes) {
    FileView v = {data, bytes};
    st = parse_dll(kind, v, out);
  }
  HeapFree(GetProcessHeap(), 0, data);
  if (st != kDllOk) close_dll(out);
  return st;
}

// ---------------------------------------------------------------------------
// Модули цели (снимок Toolhelp — только чтение)

typedef bool (*ModuleFn)(void* ctx, const MODULEENTRY32W& me);  // ложь — прервать обход

bool walk_modules(DWORD pid, ModuleFn fn, void* ctx, DWORD* err) {
  HANDLE snap = INVALID_HANDLE_VALUE;
  DWORD e = 0;
  for (int i = 0; i < 8; ++i) {
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap != INVALID_HANDLE_VALUE) break;
    e = GetLastError();
    if (e != ERROR_BAD_LENGTH) break;
  }
  if (snap == INVALID_HANDLE_VALUE) {
    *err = e;
    return false;
  }
  MODULEENTRY32W me;
  me.dwSize = sizeof(me);
  for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
    me.szModule[ARRAYSIZE(me.szModule) - 1] = 0;
    me.szExePath[ARRAYSIZE(me.szExePath) - 1] = 0;
    if (!fn(ctx, me)) break;
  }
  CloseHandle(snap);
  return true;
}

struct OurModules {
  const wchar_t* expect[2];  // ожидаемый полный путь по виду (nullptr — не сверять)
  bool any[2];               // модуль с нашим именем любой битности
  bool ours[2];              // полный путь совпал с ожидаемым
  bool foreign[2];           // имя наше, путь другой
  BYTE* base[2];
};

bool collect_ours(void* ctx, const MODULEENTRY32W& me) {
  OurModules* m = static_cast<OurModules*>(ctx);
  for (int k = 0; k < 2; ++k) {
    for (int a = 0; a < 2; ++a) {
      if (!ieq(me.szModule, kDllNames[k][a])) continue;
      m->any[k] = true;
      if (m->expect[k] != nullptr && ieq(me.szExePath, m->expect[k])) {
        m->ours[k] = true;
        m->base[k] = me.modBaseAddr;
      } else {
        m->foreign[k] = true;
      }
    }
  }
  return true;
}

bool scan_our_modules(DWORD pid, const wchar_t* expect_theme, const wchar_t* expect_probe, OurModules* m,
                      DWORD* err) {
  ZeroMemory(m, sizeof(*m));
  m->expect[kTheme] = expect_theme;
  m->expect[kProbe] = expect_probe;
  return walk_modules(pid, collect_ours, m, err);
}

struct SysLookup {
  const wchar_t* name;
  BYTE* base;
};

bool find_sys(void* ctx, const MODULEENTRY32W& me) {
  SysLookup* s = static_cast<SysLookup*>(ctx);
  if (!ieq(me.szModule, s->name)) return true;
  s->base = me.modBaseAddr;
  return false;
}

// Адрес функции kernel32 (LoadLibraryW, FreeLibrary) в цели той же битности:
// смещение в модуле-владельце у себя + база того же модуля в цели по снимку.
bool remote_kernel_proc(DWORD pid, const char* proc, void** out, DWORD* err) {
  *err = 0;
  HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
  FARPROC fp = k32 != nullptr ? GetProcAddress(k32, proc) : nullptr;
  HMODULE owner = nullptr;
  wchar_t path[MAX_PATH];
  if (fp == nullptr ||
      !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(fp), &owner) ||
      GetModuleFileNameW(owner, path, MAX_PATH) == 0) {
    *err = GetLastError();
    return false;
  }
  path[MAX_PATH - 1] = 0;
  SysLookup s = {base_name(path), nullptr};
  if (!walk_modules(pid, find_sys, &s, err)) return false;
  if (s.base == nullptr) {
    *err = ERROR_MOD_NOT_FOUND;
    return false;
  }
  *out = s.base + (reinterpret_cast<BYTE*>(fp) - reinterpret_cast<BYTE*>(owner));
  return true;
}

// ---------------------------------------------------------------------------
// Защита от повторной загрузки: Local\cfgdark-<pid>, общий для темы и probe

enum Guard { kGuardAbsent, kGuardPresent, kGuardError };

Guard guard_state(DWORD pid, DWORD* err) {
  wchar_t name[64];
  wsprintfW(name, L"Local\\cfgdark-%lu", pid);
  HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, name);
  if (h != nullptr) {
    CloseHandle(h);
    return kGuardPresent;
  }
  DWORD e = GetLastError();
  if (e == ERROR_FILE_NOT_FOUND) return kGuardAbsent;
  // Нет доступа или имя занято объектом другого типа — считаем, что занято.
  if (e == ERROR_ACCESS_DENIED || e == ERROR_INVALID_HANDLE) return kGuardPresent;
  *err = e;
  return kGuardError;
}

const wchar_t* guard_text(Guard g) {
  switch (g) {
    case kGuardPresent: return L"есть";
    case kGuardAbsent: return L"нет";
    default: return L"не прочитан";
  }
}

// ---------------------------------------------------------------------------
// Блокировка операции: Local\cfgdark-op-<pid>. Создаёт и держит только загрузчик —
// load/unload/probe-load/probe-unload над одним pid не идут одновременно. Берётся после
// проверки битности и до первой проверки модулей, держится всю команду. Решение — по
// захвату, не по ERROR_ALREADY_EXISTS. status её не открывает.

int acquire_op_lock(DWORD pid, HANDLE* out) {
  *out = nullptr;
  wchar_t name[64];
  wsprintfW(name, L"Local\\cfgdark-op-%lu", pid);
  HANDLE h = CreateMutexW(nullptr, FALSE, name);
  if (h == nullptr) {
    DWORD e = GetLastError();
    // Нет доступа или имя занято объектом другого типа — считаем, что занято.
    if (e == ERROR_ACCESS_DENIED || e == ERROR_INVALID_HANDLE) {
      return fail(kBusy, L"блокировка операции над pid %lu недоступна (ошибка %lu)", pid, e);
    }
    return fail(kWinApi, L"блокировка операции над pid %lu: ошибка %lu", pid, e);
  }
  DWORD w = WaitForSingleObject(h, 0);
  if (w == WAIT_TIMEOUT) {
    CloseHandle(h);
    return fail(kBusy, L"над pid %lu уже идёт операция другого загрузчика", pid);
  }
  if (w == WAIT_ABANDONED) {
    // Захвачена: прежний владелец завершился, не освободив. Все проверки идут дальше под ней.
    warn(L"предупреждение: предыдущая операция над pid %lu прервана; состояние проверяется заново\n", pid);
  } else if (w != WAIT_OBJECT_0) {
    DWORD e = GetLastError();
    CloseHandle(h);
    return fail(kWinApi, L"блокировка операции над pid %lu: ошибка ожидания %lu", pid, e);
  }
  *out = h;
  return kOk;
}

void release_op_lock(HANDLE* h) {
  if (*h == nullptr) return;
  ReleaseMutex(*h);
  CloseHandle(*h);
  *h = nullptr;
}

// Проверка 5: в процессе нет темы и probe любой битности.
int check_not_loaded(const Target& t) {
  DWORD err = 0;
  Guard g = guard_state(t.pid, &err);
  if (g == kGuardError) return fail(kWinApi, L"объект активности pid %lu: ошибка %lu", t.pid, err);
  if (g == kGuardPresent) return fail(kAlready, L"в pid %lu уже активна тема или probe", t.pid);
  OurModules m;
  if (!scan_our_modules(t.pid, nullptr, nullptr, &m, &err)) {
    return fail(kWinApi, L"снимок модулей pid %lu: ошибка %lu", t.pid, err);
  }
  if (m.any[kTheme]) return fail(kAlready, L"в pid %lu уже есть модуль темы", t.pid);
  if (m.any[kProbe]) return fail(kAlready, L"в pid %lu уже есть модуль probe", t.pid);
  return kOk;
}

// ---------------------------------------------------------------------------
// Удалённые вызовы

enum CallResult { kCallOk, kCallTimeout, kCallError };

CallResult remote_call(HANDLE proc, void* fn, void* arg, DWORD timeout, DWORD* code, DWORD* err) {
  *code = 0;
  *err = 0;
  HANDLE th = CreateRemoteThread(proc, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(fn), arg, 0, nullptr);
  if (th == nullptr) {
    *err = GetLastError();
    return kCallError;
  }
  DWORD w = WaitForSingleObject(th, timeout);
  if (w != WAIT_OBJECT_0) {
    // Поток мог остаться внутри модуля — вызывающий ничего не освобождает.
    *err = w == WAIT_TIMEOUT ? static_cast<DWORD>(WAIT_TIMEOUT) : GetLastError();
    CloseHandle(th);
    return kCallTimeout;
  }
  if (!GetExitCodeThread(th, code)) {
    *err = GetLastError();
    CloseHandle(th);
    return kCallError;
  }
  CloseHandle(th);
  return kCallOk;
}

// Заголовок модуля в памяти совпадает с файлом: Machine, TimeDateStamp, SizeOfImage.
bool headers_match(HANDLE proc, BYTE* base, const DllImage& dll) {
  BYTE buf[4096];
  SIZE_T got = 0;
  if (!ReadProcessMemory(proc, base, buf, sizeof(buf), &got) || got != sizeof(buf)) return false;
  FileView v = {buf, static_cast<DWORD>(got)};
  const IMAGE_NT_HEADERS* nt = nt_headers(v);
  if (nt == nullptr ||
      !view_range(v, static_cast<DWORD>(reinterpret_cast<const BYTE*>(nt) - buf), sizeof(IMAGE_NT_HEADERS))) {
    return false;
  }
  return nt->FileHeader.Machine == kMachine && nt->FileHeader.TimeDateStamp == dll.time_stamp &&
         nt->OptionalHeader.SizeOfImage == dll.size_of_image;
}

bool find_own_module(DWORD pid, Kind kind, const DllImage& dll, BYTE** base, DWORD* err) {
  OurModules m;
  if (!scan_our_modules(pid, kind == kTheme ? dll.path : nullptr, kind == kProbe ? dll.path : nullptr, &m, err)) {
    return false;
  }
  *base = m.ours[kind] ? m.base[kind] : nullptr;
  return true;
}

// FreeLibrary удалённым потоком и проверка, что модуль ушёл.
int free_module(HANDLE proc, DWORD pid, Kind kind, const DllImage& dll, BYTE* base) {
  void* free_fn = nullptr;
  DWORD err = 0, code = 0;
  if (!remote_kernel_proc(pid, "FreeLibrary", &free_fn, &err)) {
    return fail(kWinApi, L"адрес FreeLibrary в pid %lu: ошибка %lu", pid, err);
  }
  CallResult r = remote_call(proc, free_fn, base, kFreeTimeoutMs, &code, &err);
  if (r == kCallTimeout) {
    return fail(kWinApi, L"FreeLibrary в pid %lu не завершился за %lu с", pid, kFreeTimeoutMs / 1000);
  }
  if (r == kCallError) return fail(kWinApi, L"поток FreeLibrary в pid %lu: ошибка %lu", pid, err);
  BYTE* left = nullptr;
  if (!find_own_module(pid, kind, dll, &left, &err)) {
    return fail(kWinApi, L"снимок модулей pid %lu после FreeLibrary: ошибка %lu", pid, err);
  }
  if (left != nullptr) return fail(kStillLoaded, L"%s осталась в pid %lu после FreeLibrary", kKindText[kind], pid);
  return kOk;
}

// ---------------------------------------------------------------------------
// Перерисовка R1: InvalidateRect + UpdateWindow видимым окнам целевого pid и их
// дочерним, без широковещания. Механизм выбирается на M5.

struct RedrawCtx {
  DWORD pid;
  int count;
};

void redraw_one(HWND hwnd, RedrawCtx* c) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != c->pid || !IsWindowVisible(hwnd)) return;
  InvalidateRect(hwnd, nullptr, TRUE);
  UpdateWindow(hwnd);
  ++c->count;
}

BOOL CALLBACK redraw_child(HWND hwnd, LPARAM lp) {
  redraw_one(hwnd, reinterpret_cast<RedrawCtx*>(lp));
  return TRUE;
}

BOOL CALLBACK redraw_top(HWND hwnd, LPARAM lp) {
  RedrawCtx* c = reinterpret_cast<RedrawCtx*>(lp);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != c->pid || !IsWindowVisible(hwnd)) return TRUE;
  redraw_one(hwnd, c);
  EnumChildWindows(hwnd, redraw_child, lp);
  return TRUE;
}

int redraw(DWORD pid) {
  RedrawCtx c = {pid, 0};
  EnumWindows(redraw_top, reinterpret_cast<LPARAM>(&c));
  return c.count;
}

// ---------------------------------------------------------------------------
// Загрузка

int start_in(HANDLE proc, const Target& t, Kind kind, const DllImage& dll) {
  DWORD err = 0, code = 0;
  void* load_fn = nullptr;
  if (!remote_kernel_proc(t.pid, "LoadLibraryW", &load_fn, &err)) {
    return fail(kWinApi, L"адрес LoadLibraryW в pid %lu: ошибка %lu", t.pid, err);
  }
  SIZE_T bytes = (static_cast<SIZE_T>(lstrlenW(dll.path)) + 1) * sizeof(wchar_t);
  void* mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (mem == nullptr) return fail(kWinApi, L"VirtualAllocEx в pid %lu: ошибка %lu", t.pid, GetLastError());
  SIZE_T done = 0;
  if (!WriteProcessMemory(proc, mem, dll.path, bytes, &done) || done != bytes) {
    err = GetLastError();
    VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    return fail(kWinApi, L"WriteProcessMemory в pid %lu: ошибка %lu", t.pid, err);
  }
  CallResult r = remote_call(proc, load_fn, mem, kLoadTimeoutMs, &code, &err);
  if (r == kCallTimeout) {
    return fail(kWinApi, L"LoadLibraryW в pid %lu не завершился за %lu с; проверьте status", t.pid,
                kLoadTimeoutMs / 1000);
  }
  VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
  if (r == kCallError) return fail(kWinApi, L"поток LoadLibraryW в pid %lu: ошибка %lu", t.pid, err);

  // Код выхода потока — усечённый HMODULE: модуль ищется снимком по полному пути.
  BYTE* base = nullptr;
  if (!find_own_module(t.pid, kind, dll, &base, &err)) {
    return fail(kWinApi, L"снимок модулей pid %lu: ошибка %lu", t.pid, err);
  }
  if (base == nullptr) return fail(kWinApi, L"LoadLibraryW в pid %lu не загрузил DLL", t.pid);
  if (!headers_match(proc, base, dll)) {
    free_module(proc, t.pid, kind, dll, base);
    return fail(kWinApi, L"модуль в pid %lu не совпал с файлом; DLL выгружена", t.pid);
  }

  // DtStart по RVA из файла, не GetProcAddress у себя.
  r = remote_call(proc, base + dll.rva_start, nullptr, kStartTimeoutMs, &code, &err);
  if (r == kCallTimeout) {
    return fail(kStartFailed, L"DtStart в pid %lu не завершился за %lu с; модуль оставлен, выгрузка — unload",
                t.pid, kStartTimeoutMs / 1000);
  }
  if (r == kCallError) {
    free_module(proc, t.pid, kind, dll, base);
    return fail(kWinApi, L"поток DtStart в pid %lu: ошибка %lu", t.pid, err);
  }
  if (code != 0) {
    int fr = free_module(proc, t.pid, kind, dll, base);
    if (code == kDtStartAlready) return fail(kAlready, L"DtStart: в pid %lu уже активна тема или probe", t.pid);
    return fail(kStartFailed, L"DtStart в pid %lu вернул %lu; DLL %s", t.pid, code,
                fr == kOk ? L"выгружена" : L"выгружена не полностью");
  }
  int n = redraw(t.pid);
  say(L"%s загружена в pid %lu, перерисовано окон: %d\n", kKindText[kind], t.pid, n);
  return kOk;
}

int load_with_dll(const Target& t, Kind kind, const DllImage& dll) {
  int rc = check_not_loaded(t);
  if (rc != kOk) return rc;
  rc = observe(t);
  if (rc != kOk) return rc;
  rc = check_not_loaded(t);
  if (rc != kOk) return rc;
  HANDLE proc = nullptr;
  rc = open_for_write(t, &proc);
  if (rc != kOk) return rc;
  rc = start_in(proc, t, kind, dll);
  CloseHandle(proc);
  return rc;
}

int cmd_load(Kind kind, DWORD want) {
  Target t;
  int rc = select_target(want, &t);
  if (rc != kOk) return rc;
  rc = check_bitness(t);
  HANDLE op = nullptr;
  if (rc == kOk) rc = acquire_op_lock(t.pid, &op);
  if (rc == kOk) {
    DllImage dll;
    DllStatus ds = open_dll(kind, &dll);
    if (ds != kDllOk) {
      rc = fail(kBadDll, L"%s: %s", kDllNames[kind][kArchIndex], dll_status_text(ds));
    } else {
      rc = load_with_dll(t, kind, dll);
      close_dll(&dll);
    }
  }
  release_op_lock(&op);
  close_target(&t);
  return rc;
}

// ---------------------------------------------------------------------------
// Выгрузка: только свой модуль. DtStop (5 с) → FreeLibrary → проверка → перерисовка.

int stop_in(HANDLE proc, const Target& t, Kind kind, const DllImage& dll, BYTE* base) {
  DWORD code = 0, err = 0;
  CallResult r = remote_call(proc, base + dll.rva_stop, nullptr, kStopTimeoutMs, &code, &err);
  if (r == kCallTimeout) {
    return fail(kStopFailed, L"DtStop в pid %lu не завершился за %lu с; FreeLibrary не вызывался", t.pid,
                kStopTimeoutMs / 1000);
  }
  if (r == kCallError) return fail(kWinApi, L"поток DtStop в pid %lu: ошибка %lu", t.pid, err);
  if (code != 0) return fail(kStopFailed, L"DtStop в pid %lu вернул %lu; FreeLibrary не вызывался", t.pid, code);
  int rc = free_module(proc, t.pid, kind, dll, base);
  if (rc != kOk) return rc;
  int n = redraw(t.pid);
  say(L"%s выгружена из pid %lu, перерисовано окон: %d\n", kKindText[kind], t.pid, n);
  return kOk;
}

int unload_with_dll(const Target& t, Kind kind, const DllImage& dll) {
  DWORD err = 0;
  OurModules m;
  if (!scan_our_modules(t.pid, kind == kTheme ? dll.path : nullptr, kind == kProbe ? dll.path : nullptr, &m, &err)) {
    return fail(kWinApi, L"снимок модулей pid %lu: ошибка %lu", t.pid, err);
  }
  if (!m.any[kind]) return fail(kNotLoaded, L"%s не загружена в pid %lu", kKindText[kind], t.pid);
  if (!m.ours[kind]) {
    return fail(kForeign, L"в pid %lu модуль с именем %s из другого каталога — не выгружаю", t.pid,
                kDllNames[kind][kArchIndex]);
  }
  // До открытия на запись: модуль в памяти — тот же файл (RVA DtStop берётся из файла).
  HANDLE rd = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, t.pid);
  if (rd == nullptr) return fail(kWinApi, L"OpenProcess (чтение) pid %lu: ошибка %lu", t.pid, GetLastError());
  bool same = headers_match(rd, m.base[kind], dll);
  CloseHandle(rd);
  if (!same) {
    return fail(kForeign, L"модуль в pid %lu не совпадает с файлом %s (пересобран?) — не выгружаю", t.pid,
                kDllNames[kind][kArchIndex]);
  }
  HANDLE proc = nullptr;
  int rc = open_for_write(t, &proc);
  if (rc != kOk) return rc;
  rc = stop_in(proc, t, kind, dll, m.base[kind]);
  CloseHandle(proc);
  return rc;
}

int cmd_unload(Kind kind, DWORD want) {
  Target t;
  int rc = select_target(want, &t);
  if (rc != kOk) return rc;
  rc = check_bitness(t);
  HANDLE op = nullptr;
  if (rc == kOk) rc = acquire_op_lock(t.pid, &op);
  if (rc == kOk) {
    DllImage dll;
    DllStatus ds = open_dll(kind, &dll);
    if (ds != kDllOk) {
      rc = fail(kBadDll, L"%s: %s (нужен для RVA DtStop)", kDllNames[kind][kArchIndex], dll_status_text(ds));
    } else {
      rc = unload_with_dll(t, kind, dll);
      close_dll(&dll);
    }
  }
  release_op_lock(&op);
  close_target(&t);
  return rc;
}

// ---------------------------------------------------------------------------
// probe-dump: DtDump (5 с) → ReadProcessMemory(DtSnapshot) → проверка → JSON в stdout.
// Только целые числа; хеши и цвета — hex-строки, вызывающий — basename модуля. Файлов
// не пишет, перерисовки нет. Текста, глифов, заголовков, пикселей, адресов в снимке нет.

wchar_t g_json[1025];
int g_json_len = 0;

void jflush() {
  if (g_json_len == 0) return;
  g_json[g_json_len] = 0;
  write_text(STD_OUTPUT_HANDLE, g_json);
  g_json_len = 0;
}

void jput(wchar_t c) {
  if (g_json_len >= 1024) jflush();
  g_json[g_json_len++] = c;
}

void jraw(const wchar_t* s) {
  for (; *s; ++s) jput(*s);
}

void jascii(const char* s) {
  for (; *s; ++s) jput(static_cast<wchar_t>(static_cast<unsigned char>(*s)));
}

// wsprintfW не умеет 64 бита — десятичное вручную.
void ju64(uint64_t v) {
  wchar_t d[21];
  int n = 0;
  do {
    d[n++] = static_cast<wchar_t>(L'0' + v % 10);
    v /= 10;
  } while (v != 0);
  while (n > 0) jput(d[--n]);
}

// ,"key":
void jkey(bool* first, const char* key) {
  if (!*first) jput(L',');
  *first = false;
  jput(L'"');
  jascii(key);
  jraw(L"\":");
}

void jkv(bool* first, const char* key, uint64_t v) {
  jkey(first, key);
  ju64(v);
}

void jstr(const char* s) {
  jput(L'"');
  jascii(s);
  jput(L'"');
}

void jhex(uint32_t v) {
  wchar_t b[16];
  wsprintfW(b, L"\"%08lx\"", static_cast<unsigned long>(v));
  jraw(b);
}

void jhex64(uint64_t v) {
  wchar_t b[24];
  wsprintfW(b, L"\"%08lx%08lx\"", static_cast<unsigned long>(v >> 32), static_cast<unsigned long>(v & 0xFFFFFFFFu));
  jraw(b);
}

void jcaller(const dt::snap::ProbeSnapshot& s, uint8_t c) {
  if (c < s.nnames) {
    jput(L'"');
    const uint16_t* n = s.names[c];
    for (uint32_t i = 0; i < dt::snap::kNameLen && n[i]; ++i) {
      wchar_t ch = static_cast<wchar_t>(n[i]);
      if (ch == L'"' || ch == L'\\') {
        jput(L'\\');
        jput(ch);
      } else if (ch < 0x20) {
        jput(L'_');
      } else {
        jput(ch);
      }
    }
    jput(L'"');
  } else if (c == dt::snap::kCallerOther) {
    jstr("other");
  } else if (c == dt::snap::kCallerUnloaded) {
    jstr("unloaded");
  } else {
    jstr("unknown");
  }
}

const char* patch_status_name(uint8_t v) {
  return v < dt::snap::kPatchStatusCount ? dt::snap::kPatchStatusNames[v] : "unknown";
}

bool snapshot_valid(dt::snap::ProbeSnapshot* s) {
  using namespace dt::snap;
  if (s->magic != kMagic || s->version != kVersion || s->size != sizeof(ProbeSnapshot) || s->machine != kMachine) {
    return false;
  }
  if (s->nsurfaces > kSurfaces || s->ncolors > kColors || s->nalpha > kAlpha || s->nnames > kNames) return false;
  for (uint32_t i = 0; i < kNames; ++i) s->names[i][kNameLen - 1] = 0;
  return true;
}

void print_snapshot(const dt::snap::ProbeSnapshot& s) {
  using namespace dt::snap;
  bool top = true;
  jput(L'{');
  jkv(&top, "schema", 1);
  jkey(&top, "arch");
  jput(L'"');
  jraw(kArch);
  jput(L'"');
  jkv(&top, "epoch", s.epoch);
  jkv(&top, "t_ms", s.t_ms);
  jkv(&top, "t_prev_ms", s.t_prev_ms);
  jkv(&top, "qpc_freq", s.qpc_freq);
  jraw(L"\n");

  jkey(&top, "hooks");
  bool f = true;
  jput(L'{');
  for (uint32_t k = 0; k < kHooks; ++k) {
    jkey(&f, kHookNames[k]);
    bool g = true;
    jput(L'{');
    jkey(&g, "state");
    jstr(s.hook_state[k] < 4 ? kHookStateNames[s.hook_state[k]] : "unknown");
    jkey(&g, "patch");
    jstr(patch_status_name(s.hook_patch[k]));
    jput(L'}');
  }
  jput(L'}');
  jkey(&top, "getters_missing");
  jput(L'[');
  f = true;
  for (uint32_t k = 0; k < kGetters; ++k) {
    if (!(s.getters_missing & (1u << k))) continue;
    if (!f) jput(L',');
    f = false;
    jstr(kGetterNames[k]);
  }
  jput(L']');
  jkey(&top, "features");
  f = true;
  jput(L'{');
  jkv(&f, "surf_hash", (s.features & kFeatSurfHash) ? 1 : 0);
  jkv(&f, "text_color", (s.features & kFeatTextColor) ? 1 : 0);
  jkv(&f, "dup", (s.features & kFeatDup) ? 1 : 0);
  jput(L'}');
  jkey(&top, "patch");
  f = true;
  jput(L'{');
  for (uint32_t k = 0; k < kPatchStats; ++k) jkv(&f, kPatchStatNames[k], s.patch[k]);
  jput(L'}');
  jraw(L"\n");

  jkey(&top, "calls");
  f = true;
  jput(L'{');
  for (uint32_t c = 0; c < kEpochCounters; ++c) jkv(&f, kCounterNames[c], s.counters[c]);
  jput(L'}');
  jkey(&top, "overflow");
  f = true;
  jput(L'{');
  for (uint32_t c = kEpochCounters; c < kOverflowEnd; ++c) jkv(&f, kCounterNames[c], s.counters[c]);
  jput(L'}');
  jraw(L"\n");

  jkey(&top, "surfaces");
  jput(L'[');
  for (uint32_t i = 0; i < s.nsurfaces; ++i) {
    const SnapSurface& o = s.surfaces[i];
    if (i != 0) jraw(L",\n");
    f = true;
    jput(L'{');
    jkey(&f, "hash");
    jhex64(o.hash);
    jkv(&f, "w", o.w);
    jkv(&f, "h", o.h);
    jkv(&f, "fmt", o.fmt);
    jkey(&f, "caller");
    jcaller(s, o.caller);
    jkv(&f, "first_ms", o.first_ms);
    jkv(&f, "hits", o.hits);
    jput(L'}');
  }
  jput(L']');
  jraw(L"\n");

  jkey(&top, "text_colors");
  jput(L'[');
  for (uint32_t i = 0; i < s.ncolors; ++i) {
    const SnapColor& o = s.colors[i];
    if (i != 0) jraw(L",\n");
    f = true;
    jput(L'{');
    jkey(&f, "rgba");
    jhex(o.rgba);
    jkey(&f, "caller");
    jcaller(s, o.caller);
    jkv(&f, "runs", o.runs);
    jkv(&f, "glyphs", o.glyphs);
    jkv(&f, "first_ms", o.first_ms);
    jput(L'}');
  }
  jput(L']');
  jkv(&top, "color_inexact", s.counters[kColorInexact]);
  jraw(L"\n");

  jkey(&top, "alpha_lt1");
  jput(L'[');
  for (uint32_t i = 0; i < s.nalpha; ++i) {
    const SnapAlpha& o = s.alpha[i];
    if (i != 0) jraw(L",\n");
    f = true;
    jput(L'{');
    jkey(&f, "rgba");
    jhex(o.rgba);
    jkey(&f, "caller");
    jcaller(s, o.caller);
    jkv(&f, "calls", o.calls);
    jput(L'}');
  }
  jput(L']');
  jraw(L"\n");

  jkey(&top, "layout");
  f = true;
  jput(L'{');
  for (uint32_t c = kOverflowEnd; c < kCounters; ++c) jkv(&f, kCounterNames[c], s.counters[c]);
  jkey(&f, "self_check");
  jstr(s.self_check < 3 ? kSelfCheckNames[s.self_check] : "unknown");
  jkv(&f, "self_check_seen", s.self_check_seen);
  jkv(&f, "compiled_stride", s.compiled_stride);
  jput(L'}');
  jraw(L"\n");

  jkey(&top, "dup");
  f = true;
  jput(L'{');
  jkv(&f, "stride", s.self_check == kSelfPassed ? s.compiled_stride : 0);
  jkey(&f, "bins");
  jput(L'[');
  bool fb = true;
  for (uint32_t i = 0; i < kDupBins; ++i) {
    if (s.dup_bins[i] == 0) continue;
    uint32_t pos = i / 8, flags = i % 8;
    int dx = static_cast<int>(pos % 15) - 7;
    int dy = static_cast<int>(pos / 15) - 7;
    if (!fb) jraw(L",\n");
    fb = false;
    wchar_t b[160];
    wsprintfW(b, L"{\"dx\":%d,\"dy\":%d,\"same_thread\":%u,\"same_target\":%u,\"same_color\":%u,\"n\":%lu}", dx, dy,
              flags & 1u, (flags >> 1) & 1u, (flags >> 2) & 1u, static_cast<unsigned long>(s.dup_bins[i]));
    jraw(b);
  }
  jput(L']');
  jkv(&f, "far", s.counters[kDupFar]);
  jkv(&f, "pairs", s.counters[kDupPairs]);
  jkv(&f, "runs", s.counters[kDupRuns]);
  jkv(&f, "torn", s.counters[kDupTorn]);
  jkey(&f, "dt_log2");
  jput(L'[');
  for (uint32_t i = 0; i < kDtBins; ++i) {
    if (i != 0) jput(L',');
    ju64(s.dup_dt[i]);
  }
  jraw(L"]}");
  jraw(L"\n");

  jkey(&top, "frame");
  f = true;
  jput(L'{');
  jkv(&f, "gap_ms", kBurstGapMs);
  jkv(&f, "count", s.frame_count);
  jkv(&f, "sum_us", s.frame_sum_us);
  jkv(&f, "max_us", s.frame_max_us);
  jkv(&f, "bin_us", kFrameBinUs);
  jkey(&f, "hist");
  jput(L'[');
  for (uint32_t i = 0; i < kFrameBins; ++i) {
    if (i != 0) jput(L',');
    ju64(s.frame_hist[i]);
  }
  jput(L']');
  jkey(&f, "pause_log2");
  jput(L'[');
  for (uint32_t i = 0; i < kPauseBins; ++i) {
    if (i != 0) jput(L',');
    ju64(s.frame_pause[i]);
  }
  jput(L']');
  jkv(&f, "overhead_ticks", s.frame_overhead_ticks);
  jkv(&f, "orig_ticks", s.frame_orig_ticks);
  jkv(&f, "qpc_freq", s.qpc_freq);
  jput(L'}');
  jraw(L"\n");

  jkey(&top, "timeline");
  f = true;
  jput(L'{');
  jkv(&f, "bucket_ms", kBucketMs);
  jkey(&f, "buckets");
  jput(L'[');
  bool fl = true;
  for (uint32_t i = 0; i < kBuckets; ++i) {
    const uint32_t* c = s.timeline[i];
    if ((c[0] | c[1] | c[2] | c[3]) == 0) continue;
    if (!fl) jraw(L",\n");
    fl = false;
    wchar_t b[96];
    wsprintfW(b, L"[%lu,%lu,%lu,%lu,%lu]", static_cast<unsigned long>(i), static_cast<unsigned long>(c[0]),
              static_cast<unsigned long>(c[1]), static_cast<unsigned long>(c[2]), static_cast<unsigned long>(c[3]));
    jraw(b);
  }
  jraw(L"]}}\n");
  jflush();
}

int dump_with_dll(const Target& t, const DllImage& dll) {
  DWORD err = 0;
  OurModules m;
  if (!scan_our_modules(t.pid, nullptr, dll.path, &m, &err)) {
    return fail(kWinApi, L"снимок модулей pid %lu: ошибка %lu", t.pid, err);
  }
  if (!m.any[kProbe]) return fail(kNotLoaded, L"probe не загружена в pid %lu", t.pid);
  if (!m.ours[kProbe]) {
    return fail(kForeign, L"в pid %lu модуль с именем %s из другого каталога — не трогаю", t.pid,
                kDllNames[kProbe][kArchIndex]);
  }
  // До открытия на запись: модуль в памяти — тот же файл (RVA DtDump и DtSnapshot из файла).
  HANDLE rd = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, t.pid);
  if (rd == nullptr) return fail(kWinApi, L"OpenProcess (чтение) pid %lu: ошибка %lu", t.pid, GetLastError());
  bool same = headers_match(rd, m.base[kProbe], dll);
  CloseHandle(rd);
  if (!same) {
    return fail(kForeign, L"модуль в pid %lu не совпадает с файлом %s (пересобран?) — не трогаю", t.pid,
                kDllNames[kProbe][kArchIndex]);
  }
  HANDLE proc = nullptr;
  int rc = open_for_write(t, &proc);
  if (rc != kOk) return rc;
  DWORD code = 0;
  CallResult r = remote_call(proc, m.base[kProbe] + dll.rva_dump, nullptr, kDumpTimeoutMs, &code, &err);
  if (r == kCallTimeout) {
    rc = fail(kDumpFailed, L"DtDump в pid %lu не завершился за %lu с; снимок не читаю", t.pid, kDumpTimeoutMs / 1000);
  } else if (r == kCallError) {
    rc = fail(kWinApi, L"поток DtDump в pid %lu: ошибка %lu", t.pid, err);
  } else if (code != 0) {
    rc = fail(kDumpFailed, L"DtDump в pid %lu вернул %lu (1 — probe не активна, 2 — ошибка)", t.pid, code);
  } else {
    dt::snap::ProbeSnapshot* s = static_cast<dt::snap::ProbeSnapshot*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(dt::snap::ProbeSnapshot)));
    if (s == nullptr) {
      rc = fail(kWinApi, L"нет памяти под снимок");
    } else {
      SIZE_T got = 0;
      if (!ReadProcessMemory(proc, m.base[kProbe] + dll.rva_snapshot, s, sizeof(*s), &got) || got != sizeof(*s)) {
        rc = fail(kWinApi, L"ReadProcessMemory снимка в pid %lu: ошибка %lu", t.pid, GetLastError());
      } else if (!snapshot_valid(s)) {
        rc = fail(kDumpFailed, L"снимок pid %lu не прошёл проверку (magic, версия, размер, Machine, счётчики)", t.pid);
      } else {
        print_snapshot(*s);
        rc = kOk;
      }
      HeapFree(GetProcessHeap(), 0, s);
    }
  }
  CloseHandle(proc);
  return rc;
}

// Те же проверки цели, что у выгрузки, без 25-секундного наблюдения; блокировка
// операции держится всю команду.
int cmd_dump(DWORD want) {
  Target t;
  int rc = select_target(want, &t);
  if (rc != kOk) return rc;
  rc = check_bitness(t);
  HANDLE op = nullptr;
  if (rc == kOk) rc = acquire_op_lock(t.pid, &op);
  if (rc == kOk) {
    DllImage dll;
    DllStatus ds = open_dll(kProbe, &dll);
    if (ds != kDllOk) {
      rc = fail(kBadDll, L"%s: %s (нужен для RVA DtDump/DtSnapshot)", kDllNames[kProbe][kArchIndex],
                dll_status_text(ds));
    } else {
      rc = dump_with_dll(t, dll);
      close_dll(&dll);
    }
  }
  release_op_lock(&op);
  close_target(&t);
  return rc;
}

// ---------------------------------------------------------------------------
// Состояние: без заголовков окон

const wchar_t* module_text(const OurModules& m, Kind k) {
  if (!m.any[k]) return L"нет";
  if (m.ours[k] && m.foreign[k]) return L"загружена (наш файл и модуль с тем же именем из другого каталога)";
  if (m.ours[k]) return L"загружена (наш файл)";
  if (m.expect[k] == nullptr) return L"модуль есть (путь не сверен: нет своей DLL)";
  return L"модуль с нашим именем из другого каталога";
}

struct StatusTally {
  int targets;
  DWORD target_pid;
  const wchar_t* expect[2];
};

void status_one(DWORD pid, StatusTally* st) {
  Target t;
  DWORD err = 0;
  switch (inspect(pid, &t, &err)) {
    case kInspGone: say(L"pid %lu: процесса нет\n", pid); return;
    case kInspDenied: say(L"pid %lu: нет доступа к сведениям (ошибка %lu)\n", pid, err); return;
    case kInspError: say(L"pid %lu: ошибка сведений %lu\n", pid, err); return;
    case kInspNotImage: say(L"pid %lu: не %s\n", pid, kImageName); return;
    case kInspOk: break;
  }
  say(L"pid %lu: %s %s, возраст %lu с, видимых окон верхнего уровня: %d, «Конфигуратор»: %s\n", pid, kImageName,
      machine_name(t.machine), static_cast<DWORD>(process_age_ms(t.created) / 1000), t.win.visible,
      t.win.match != nullptr ? L"есть" : L"нет");
  Guard g = guard_state(pid, &err);
  // Память цели (снимок модулей) — только после проверок образа, окна и битности.
  if (t.win.match == nullptr) {
    say(L"  модули: не проверялись (не цель); объект активности: %s\n", guard_text(g));
  } else if (t.machine != kMachine) {
    const wchar_t* tm = machine_name(t.machine);
    if (t.machine == IMAGE_FILE_MACHINE_I386 || t.machine == IMAGE_FILE_MACHINE_AMD64) {
      say(L"  модули: не проверялись (битность цели %s, загрузчика %s — запустите cfgdark_%s); "
          L"объект активности: %s\n", tm, kArch, tm, guard_text(g));
    } else {
      say(L"  модули: не проверялись (битность цели %s, загрузчика %s); объект активности: %s\n", tm, kArch,
          guard_text(g));
    }
  } else {
    OurModules m;
    if (scan_our_modules(pid, st->expect[kTheme], st->expect[kProbe], &m, &err)) {
      say(L"  тема: %s; probe: %s; объект активности: %s\n", module_text(m, kTheme), module_text(m, kProbe),
          guard_text(g));
    } else {
      say(L"  модули: снимок не снят (ошибка %lu); объект активности: %s\n", err, guard_text(g));
    }
  }
  if (t.win.match != nullptr) {
    ++st->targets;
    st->target_pid = pid;
  }
  close_target(&t);
}

int cmd_status(DWORD want) {
  say(L"загрузчик: %s\n", kArch);
  DllImage dll[2];
  StatusTally st = {0, 0, {nullptr, nullptr}};
  for (int k = 0; k < 2; ++k) {
    DllStatus ds = open_dll(static_cast<Kind>(k), &dll[k]);
    say(L"%s: %s\n", kDllNames[k][kArchIndex], dll_status_text(ds));
    if (ds == kDllOk) {
      close_dll(&dll[k]);
      st.expect[k] = dll[k].path;
    }
  }
  if (want != 0) {
    status_one(want, &st);
  } else {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return fail(kWinApi, L"снимок процессов: ошибка %lu", GetLastError());
    int seen = 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
      if (!ieq(pe.szExeFile, kImageName)) continue;
      ++seen;
      status_one(pe.th32ProcessID, &st);
    }
    CloseHandle(snap);
    if (seen == 0) say(L"%s не запущен\n", kImageName);
  }
  if (st.targets == 0) {
    say(L"цель: нет\n");
    return kNoTarget;
  }
  if (st.targets > 1) {
    say(L"цель: неоднозначна (%d), нужен --pid\n", st.targets);
    return kAmbiguous;
  }
  say(L"цель: pid %lu\n", st.target_pid);
  return kOk;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) return usage();
  DWORD pid = 0;
  for (int i = 2; i < argc; ++i) {
    if (eq(argv[i], L"--pid") && pid == 0 && i + 1 < argc && parse_pid(argv[i + 1], &pid)) {
      ++i;
      continue;
    }
    warn(L"неверный аргумент: %s\n", argv[i]);
    return usage();
  }
  const wchar_t* cmd = argv[1];
  if (eq(cmd, L"load")) return cmd_load(kTheme, pid);
  if (eq(cmd, L"unload")) return cmd_unload(kTheme, pid);
  if (eq(cmd, L"probe-load")) return cmd_load(kProbe, pid);
  if (eq(cmd, L"probe-unload")) return cmd_unload(kProbe, pid);
  if (eq(cmd, L"status")) return cmd_status(pid);
  if (eq(cmd, L"probe-dump")) return cmd_dump(pid);
  if (!eq(cmd, L"help") && !eq(cmd, L"--help") && !eq(cmd, L"-h") && !eq(cmd, L"/?")) {
    warn(L"неизвестная команда: %s\n", cmd);
  }
  return usage();
}
