// SPDX-License-Identifier: MIT
// Разбор PE-образа, отображённого в память (RVA = смещение от базы).
// Все RVA проверяются по SizeOfImage; принимается только образ своей битности.
#pragma once
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

namespace dt {
namespace pe {

struct Image {
  uint8_t* base;
  uint32_t size;  // SizeOfImage
  const IMAGE_NT_HEADERS* nt;
  IMAGE_DATA_DIRECTORY exports;
  IMAGE_DATA_DIRECTORY imports;
  IMAGE_DATA_DIRECTORY delay_imports;
};

// limit — сколько байт от base гарантированно доступно для чтения заголовков
// (для загруженного модуля — SIZE_MAX: тогда граница берётся из SizeOfImage).
bool open(const void* base, size_t limit, Image* out);

inline bool rva_ok(const Image& img, uint32_t rva, uint32_t len) {
  return rva < img.size && len <= img.size - rva;
}

struct Export {
  uint32_t ordinal_index;  // индекс в AddressOfFunctions
  uint32_t* eat_slot;      // адрес элемента EAT (4 байта, RVA)
  uint32_t rva;            // текущее значение элемента
  bool forwarder;          // RVA внутри каталога экспорта — строка-форвардер
  bool in_image;           // RVA внутри образа (иначе EAT уже кем-то перенаправлен)
};

bool find_export(const Image& img, const char* name, Export* out);
bool in_export_dir(const Image& img, uint32_t rva);

// Обход слотов IAT: обычный импорт и delay-load (только RVA-дескрипторы).
// Колбэк возвращает ложь, чтобы прервать обход. Результат — ложь, если прерван.
typedef bool (*SlotFn)(void* ctx, void** slot, bool delay);
bool walk_iat(const Image& img, SlotFn fn, void* ctx);

const uint32_t kMaxDescriptors = 4096;
const uint32_t kMaxThunks = 65536;

}  // namespace pe
}  // namespace dt
