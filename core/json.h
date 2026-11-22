// SPDX-License-Identifier: MIT
// Строгий JSON (RFC 8259): без комментариев и хвостовых запятых, UTF-8 проверяется,
// повторные ключи — ошибка. Допускается только BOM UTF-8 в начале.
// Лимиты: вход ≤ 256 КБ, глубина ≤ 32, узлов ≤ 32768, элементов в контейнере ≤ 4096.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace dt {
namespace json {

const size_t kMaxInput = 256 * 1024;
const uint32_t kMaxDepth = 32;
const uint32_t kMaxNodes = 32768;
const uint32_t kMaxMembers = 4096;

enum Type : uint8_t { kNull, kBool, kNumber, kString, kArray, kObject };

struct Value {
  Type type;
  bool boolean;
  uint32_t count;     // байт строки или число элементов контейнера
  uint32_t key_len;   // длина ключа, если узел — член объекта
  double number;
  const char* str;    // строка UTF-8 с нулём в конце
  const char* key;    // ключ члена объекта или nullptr
  const Value* child; // первый элемент контейнера
  const Value* next;  // следующий элемент того же контейнера
};

enum Error : uint8_t {
  kOk = 0,
  kErrTooLarge,
  kErrNoMemory,
  kErrSyntax,
  kErrDepth,
  kErrTooMany,
  kErrString,
  kErrUtf8,
  kErrNumber,
  kErrDuplicateKey,
  kErrTrailing,
};

struct Document {
  const Value* root;
  Error error;
  uint32_t error_offset;  // смещение байта в исходном тексте
  void* mem;
};

// Память выделяется один раз (malloc) и освобождается release().
Error parse(const char* text, size_t len, Document* doc);
void release(Document* doc);

const Value* find(const Value* object, const char* key);
bool key_is(const Value* member, const char* key);
const char* error_name(Error e);

}  // namespace json
}  // namespace dt
