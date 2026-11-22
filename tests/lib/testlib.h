// SPDX-License-Identifier: MIT
// testlib.dll — поставщик экспортов для тестов патч-движка.
#pragma once

#if defined(TESTLIB_BUILD)
#define TL_API
#else
#define TL_API __declspec(dllimport)
#endif

extern "C" {
TL_API int __cdecl tl_add(int a, int b);
TL_API int __cdecl tl_mul(int a, int b);
TL_API int __cdecl tl_sub(int a, int b);
}
// tl_fwd — форвардер на kernel32.GetTickCount (только в .def): патчить нельзя.
