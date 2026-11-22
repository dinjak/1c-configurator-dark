// SPDX-License-Identifier: MIT
// testlate.dll — обычный импорт testlib; загружается тестом после патча EAT,
// поэтому загрузчик ОС кладёт в его IAT адрес стаба.
#include "tests/lib/testlib.h"

extern "C" {
__declspec(dllexport) int __cdecl tlate_add(int a, int b) { return tl_add(a, b); }
}
