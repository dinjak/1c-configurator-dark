// SPDX-License-Identifier: MIT
#define TESTLIB_BUILD
#include "tests/lib/testlib.h"

extern "C" {
int __cdecl tl_add(int a, int b) { return a + b; }
int __cdecl tl_mul(int a, int b) { return a * b; }
int __cdecl tl_sub(int a, int b) { return a - b; }
}
