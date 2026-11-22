// SPDX-License-Identifier: MIT
// testcons.dll — потребитель testlib через delay-load (/DELAYLOAD:testlib.dll).
// Слот delay-load IAT связывается при первом вызове через GetProcAddress.
#include "tests/lib/testlib.h"

extern "C" {
__declspec(dllexport) int __cdecl tc_add(int a, int b) { return tl_add(a, b); }
__declspec(dllexport) int __cdecl tc_mul(int a, int b) { return tl_mul(a, b); }
__declspec(dllexport) int __cdecl tc_sub(int a, int b) { return tl_sub(a, b); }
}
