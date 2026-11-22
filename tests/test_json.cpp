// SPDX-License-Identifier: MIT
#include <stdlib.h>
#include <string.h>

#include "core/json.h"
#include "tests/test.h"

using namespace dt;

namespace {

json::Error parse_str(const char* s, json::Document* d) { return json::parse(s, strlen(s), d); }

json::Error err_of(const char* s) {
  json::Document d;
  json::Error e = parse_str(s, &d);
  json::release(&d);
  return e;
}

}  // namespace

TEST(json_valid_document) {
  json::Document d;
  const char* text =
      "\xEF\xBB\xBF { \"a\": 1, \"b\": [true, false, null, -0.5e1, 1E+2, 0], \"c\": {\"d\": \"x\\ty\"},"
      " \"u\": \"\\u0416\\ud83d\\ude00\\/\", \"ru\": \"Фон\" }\n";
  CHECK_EQ(parse_str(text, &d), json::kOk);
  const json::Value* r = d.root;
  CHECK(r != nullptr && r->type == json::kObject && r->count == 5);
  const json::Value* a = json::find(r, "a");
  CHECK(a && a->type == json::kNumber && a->number == 1.0);
  const json::Value* b = json::find(r, "b");
  CHECK(b && b->type == json::kArray && b->count == 6);
  const json::Value* e = b->child;
  CHECK(e->type == json::kBool && e->boolean);
  e = e->next;
  CHECK(e->type == json::kBool && !e->boolean);
  e = e->next;
  CHECK(e->type == json::kNull);
  e = e->next;
  CHECK(e->type == json::kNumber && e->number == -5.0);
  e = e->next;
  CHECK(e->type == json::kNumber && e->number == 100.0);
  e = e->next;
  CHECK(e->type == json::kNumber && e->number == 0.0 && e->next == nullptr);
  const json::Value* dd = json::find(json::find(r, "c"), "d");
  CHECK(dd && dd->type == json::kString && strcmp(dd->str, "x\ty") == 0 && dd->count == 3);
  const json::Value* u = json::find(r, "u");
  // Ж = D0 96, U+1F600 = F0 9F 98 80, '/'.
  CHECK(u && u->count == 7 && memcmp(u->str, "\xD0\x96\xF0\x9F\x98\x80/", 7) == 0);
  const json::Value* ru = json::find(r, "ru");
  CHECK(ru && strcmp(ru->str, "Фон") == 0);
  CHECK(json::find(r, "zz") == nullptr);
  json::release(&d);
}

TEST(json_numbers) {
  json::Document d;
  CHECK_EQ(parse_str("[0.55, 0.08, 4.5, 123456789012345678901234, 1e-400, 2.5e-3]", &d), json::kOk);
  const json::Value* v = d.root->child;
  CHECK(v->number == 0.55);
  CHECK(v->next->number == 0.08);
  CHECK(v->next->next->number == 4.5);
  double big = v->next->next->next->number;
  CHECK(big > 1.2345678901e23 && big < 1.2345678902e23);
  CHECK(v->next->next->next->next->number == 0.0);
  double small = v->next->next->next->next->next->number;
  CHECK(small > 0.00249999 && small < 0.00250001);
  json::release(&d);
}

TEST(json_rejects_non_strict) {
  CHECK_EQ(err_of(""), json::kErrSyntax);
  CHECK_EQ(err_of("   "), json::kErrSyntax);
  CHECK_EQ(err_of("{\"a\":1,}"), json::kErrSyntax);   // хвостовая запятая
  CHECK_EQ(err_of("[1,2,]"), json::kErrSyntax);
  CHECK_EQ(err_of("{\"a\":1 // c\n}"), json::kErrSyntax);  // комментарий
  CHECK_EQ(err_of("{'a':1}"), json::kErrSyntax);
  CHECK_EQ(err_of("{a:1}"), json::kErrSyntax);
  CHECK_EQ(err_of("[01]"), json::kErrNumber);
  CHECK_EQ(err_of("[1.]"), json::kErrNumber);
  CHECK_EQ(err_of("[.5]"), json::kErrSyntax);
  CHECK_EQ(err_of("[+1]"), json::kErrSyntax);
  CHECK_EQ(err_of("[1e]"), json::kErrNumber);
  CHECK_EQ(err_of("[-]"), json::kErrNumber);
  CHECK_EQ(err_of("[1e400]"), json::kErrNumber);
  CHECK_EQ(err_of("[NaN]"), json::kErrSyntax);
  CHECK_EQ(err_of("[tru]"), json::kErrSyntax);
  CHECK_EQ(err_of("{\"a\":1}x"), json::kErrTrailing);
  CHECK_EQ(err_of("{\"a\":1}{}"), json::kErrTrailing);
  CHECK_EQ(err_of("{\"a\":1,\"a\":2}"), json::kErrDuplicateKey);
  CHECK_EQ(err_of("[\"a\tb\"]"), json::kErrString);   // управляющий символ
  CHECK_EQ(err_of("[\"\\x\"]"), json::kErrString);
  CHECK_EQ(err_of("[\"\\u12\"]"), json::kErrString);
  CHECK_EQ(err_of("[\"\\ud800\"]"), json::kErrString);  // одинокий суррогат
  CHECK_EQ(err_of("[\"\\udc00\"]"), json::kErrString);
  CHECK_EQ(err_of("[\"\\u0000\"]"), json::kErrString);
  CHECK_EQ(err_of("[\"abc"), json::kErrString);
  CHECK_EQ(err_of("[\"\xC0\xAF\"]"), json::kErrUtf8);   // overlong
  CHECK_EQ(err_of("[\"\xED\xA0\x80\"]"), json::kErrUtf8); // суррогат в UTF-8
  CHECK_EQ(err_of("[\"\xF4\x90\x80\x80\"]"), json::kErrUtf8);
  CHECK_EQ(err_of("[\"\xE2\x82\"]"), json::kErrUtf8);
  CHECK_EQ(err_of("[\xD0\x96]"), json::kErrSyntax);  // не-ASCII вне строки
}

TEST(json_error_offset) {
  json::Document d;
  CHECK_EQ(parse_str("{\"a\": [1, 2,, 3]}", &d), json::kErrSyntax);
  CHECK_EQ(d.error_offset, 12u);
  CHECK(d.root == nullptr && d.mem == nullptr);
}

TEST(json_limits) {
  // Глубина: 32 уровня — можно, 33 — нет.
  char deep[80];
  memset(deep, 0, sizeof(deep));
  for (int i = 0; i < 32; ++i) deep[i] = '[';
  for (int i = 0; i < 32; ++i) deep[32 + i] = ']';
  CHECK_EQ(err_of(deep), json::kOk);
  memset(deep, 0, sizeof(deep));
  for (int i = 0; i < 33; ++i) deep[i] = '[';
  for (int i = 0; i < 33; ++i) deep[33 + i] = ']';
  CHECK_EQ(err_of(deep), json::kErrDepth);

  // Размер: больше 256 КБ — отказ до разбора.
  size_t n = json::kMaxInput + 1;
  char* big = static_cast<char*>(malloc(n));
  memset(big, ' ', n);
  big[0] = '0';
  json::Document d;
  CHECK_EQ(json::parse(big, n, &d), json::kErrTooLarge);
  // Ровно 256 КБ допустимы.
  CHECK_EQ(json::parse(big, n - 1, &d), json::kOk);
  json::release(&d);

  // Элементов в контейнере больше 4096 — отказ.
  size_t m = 2 + 4097 * 2;
  char* arr = static_cast<char*>(malloc(m + 1));
  arr[0] = '[';
  for (size_t i = 0; i < 4097; ++i) {
    arr[1 + i * 2] = '0';
    arr[2 + i * 2] = ',';
  }
  arr[m - 2] = ']';  // заменяет последнюю запятую
  arr[m - 1] = 0;
  CHECK_EQ(json::parse(arr, m - 1, &d), json::kErrTooMany);
  free(arr);
  free(big);
}
