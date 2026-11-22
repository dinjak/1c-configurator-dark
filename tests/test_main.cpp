// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tests/test.h"

namespace t {
namespace {

struct Item {
  const char* name;
  Fn fn;
};

Item g_items[256];
int g_count = 0;
int g_failures = 0;
const char* g_current = "";

}  // namespace

int g_checks = 0;
const char* g_root = ".";

Reg::Reg(const char* name, Fn fn) {
  if (g_count < 256) {
    g_items[g_count].name = name;
    g_items[g_count].fn = fn;
    ++g_count;
  }
}

void fail(const char* file, int line, const char* expr) {
  ++g_failures;
  printf("  FAIL %s: %s:%d: %s\n", g_current, file, line, expr);
}

void fail_eq(const char* file, int line, const char* a, const char* b, unsigned long long va,
             unsigned long long vb) {
  ++g_failures;
  printf("  FAIL %s: %s:%d: %s == %s (0x%llx != 0x%llx)\n", g_current, file, line, a, b, va, vb);
}

bool read_file(const char* rel, char** data, size_t* len) {
  char path[1024];
  if (snprintf(path, sizeof(path), "%s/%s", g_root, rel) >= static_cast<int>(sizeof(path))) return false;
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || f == nullptr) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) {
    fclose(f);
    return false;
  }
  char* buf = static_cast<char*>(malloc(static_cast<size_t>(n) + 1));
  if (buf == nullptr) {
    fclose(f);
    return false;
  }
  size_t got = fread(buf, 1, static_cast<size_t>(n), f);
  fclose(f);
  if (got != static_cast<size_t>(n)) {
    free(buf);
    return false;
  }
  buf[n] = 0;
  *data = buf;
  *len = got;
  return true;
}

}  // namespace t

int main(int argc, char** argv) {
  if (argc > 1) t::g_root = argv[1];
#if defined(_WIN64)
  const char* arch = "x64";
#else
  const char* arch = "x86";
#endif
  int failed_tests = 0;
  for (int i = 0; i < t::g_count; ++i) {
    t::g_current = t::g_items[i].name;
    int before = t::g_failures;
    t::g_items[i].fn();
    if (t::g_failures != before) ++failed_tests;
  }
  printf("[tests] %s: %d tests, %d checks, %d failed checks, %d failed tests\n", arch, t::g_count, t::g_checks,
         t::g_failures, failed_tests);
  return t::g_failures == 0 ? 0 : 1;
}
