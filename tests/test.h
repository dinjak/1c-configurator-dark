// SPDX-License-Identifier: MIT
// Минимальный каркас юнит-тестов: без исключений и STL.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace t {

typedef void (*Fn)();

struct Reg {
  Reg(const char* name, Fn fn);
};

extern int g_checks;
extern const char* g_root;  // корень репозитория (аргумент tests.exe)

void fail(const char* file, int line, const char* expr);
void fail_eq(const char* file, int line, const char* a, const char* b, unsigned long long va,
             unsigned long long vb);
// Файл относительно корня репозитория; память — malloc, освободить free().
bool read_file(const char* rel, char** data, size_t* len);

}  // namespace t

#define TEST(name)                          \
  static void name();                       \
  static t::Reg reg_##name(#name, &name);   \
  static void name()

#define CHECK(c)                                \
  do {                                          \
    ++t::g_checks;                              \
    if (!(c)) t::fail(__FILE__, __LINE__, #c);  \
  } while (0)

#define CHECK_EQ(a, b)                                                                        \
  do {                                                                                        \
    ++t::g_checks;                                                                            \
    auto va_ = (a);                                                                           \
    auto vb_ = (b);                                                                           \
    if (!(va_ == vb_))                                                                        \
      t::fail_eq(__FILE__, __LINE__, #a, #b, (unsigned long long)(va_), (unsigned long long)(vb_)); \
  } while (0)
