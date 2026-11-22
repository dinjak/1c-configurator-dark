// SPDX-License-Identifier: MIT
#include "patch/pe.h"

#include <string.h>

namespace dt {
namespace pe {
namespace {

#if defined(_WIN64)
const WORD kMachine = IMAGE_FILE_MACHINE_AMD64;
const WORD kMagic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
#else
const WORD kMachine = IMAGE_FILE_MACHINE_I386;
const WORD kMagic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
#endif

IMAGE_DATA_DIRECTORY dir(const IMAGE_NT_HEADERS* nt, uint32_t i) {
  IMAGE_DATA_DIRECTORY d = {0, 0};
  if (i < nt->OptionalHeader.NumberOfRvaAndSizes) d = nt->OptionalHeader.DataDirectory[i];
  return d;
}

// Длина строки с нулём, не выходя за образ; 0 — нет нуля в пределах.
size_t bounded_strlen(const Image& img, uint32_t rva, size_t max) {
  if (rva >= img.size) return 0;
  size_t room = img.size - rva;
  if (room > max) room = max;
  const char* s = reinterpret_cast<const char*>(img.base + rva);
  for (size_t i = 0; i < room; ++i) {
    if (s[i] == 0) return i + 1;
  }
  return 0;
}

}  // namespace

bool open(const void* base, size_t limit, Image* out) {
  memset(out, 0, sizeof(*out));
  if (base == nullptr || limit < sizeof(IMAGE_DOS_HEADER)) return false;
  const uint8_t* b = static_cast<const uint8_t*>(base);
  const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  LONG lfanew = dos->e_lfanew;
  // Заголовки в первой странице: дальше без SizeOfImage не читаем.
  if (lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) || lfanew > 4096 - static_cast<LONG>(sizeof(IMAGE_NT_HEADERS))) {
    return false;
  }
  if (static_cast<size_t>(lfanew) + sizeof(IMAGE_NT_HEADERS) > limit) return false;
  const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
  if (nt->FileHeader.Machine != kMachine || nt->OptionalHeader.Magic != kMagic) return false;
  uint32_t size = nt->OptionalHeader.SizeOfImage;
  if (size < static_cast<uint32_t>(lfanew) + sizeof(IMAGE_NT_HEADERS) || size > limit) return false;
  out->base = const_cast<uint8_t*>(b);
  out->size = size;
  out->nt = nt;
  out->exports = dir(nt, IMAGE_DIRECTORY_ENTRY_EXPORT);
  out->imports = dir(nt, IMAGE_DIRECTORY_ENTRY_IMPORT);
  out->delay_imports = dir(nt, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
  if (out->exports.Size != 0 && !rva_ok(*out, out->exports.VirtualAddress, out->exports.Size)) return false;
  return true;
}

bool in_export_dir(const Image& img, uint32_t rva) {
  uint32_t lo = img.exports.VirtualAddress;
  return img.exports.Size != 0 && rva >= lo && rva - lo < img.exports.Size;
}

bool find_export(const Image& img, const char* name, Export* out) {
  if (img.exports.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) return false;
  const IMAGE_EXPORT_DIRECTORY* ed =
      reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(img.base + img.exports.VirtualAddress);
  uint32_t nfun = ed->NumberOfFunctions, nnames = ed->NumberOfNames;
  if (nfun > 0x10000 || nnames > nfun) return false;
  if (!rva_ok(img, ed->AddressOfFunctions, nfun * 4) || !rva_ok(img, ed->AddressOfNames, nnames * 4) ||
      !rva_ok(img, ed->AddressOfNameOrdinals, nnames * 2)) {
    return false;
  }
  uint32_t* funcs = reinterpret_cast<uint32_t*>(img.base + ed->AddressOfFunctions);
  const uint32_t* names = reinterpret_cast<const uint32_t*>(img.base + ed->AddressOfNames);
  const uint16_t* ords = reinterpret_cast<const uint16_t*>(img.base + ed->AddressOfNameOrdinals);
  size_t want = strlen(name) + 1;
  for (uint32_t i = 0; i < nnames; ++i) {
    size_t len = bounded_strlen(img, names[i], want);
    if (len != want || memcmp(img.base + names[i], name, want) != 0) continue;
    uint32_t idx = ords[i];
    if (idx >= nfun) return false;
    out->ordinal_index = idx;
    out->eat_slot = &funcs[idx];
    out->rva = funcs[idx];
    out->forwarder = in_export_dir(img, out->rva);
    out->in_image = out->rva != 0 && out->rva < img.size;
    return true;
  }
  return false;
}

bool walk_iat(const Image& img, SlotFn fn, void* ctx) {
  // Обычный импорт: массив дескрипторов до нулевого.
  if (img.imports.Size != 0 && img.imports.VirtualAddress != 0) {
    uint32_t rva = img.imports.VirtualAddress;
    for (uint32_t d = 0; d < kMaxDescriptors; ++d, rva += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
      if (!rva_ok(img, rva, sizeof(IMAGE_IMPORT_DESCRIPTOR))) break;
      const IMAGE_IMPORT_DESCRIPTOR* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(img.base + rva);
      if (desc->Name == 0 && desc->FirstThunk == 0) break;
      uint32_t iat = desc->FirstThunk;
      for (uint32_t i = 0; i < kMaxThunks; ++i, iat += sizeof(void*)) {
        if (!rva_ok(img, iat, sizeof(void*))) break;
        void** slot = reinterpret_cast<void**>(img.base + iat);
        if (*reinterpret_cast<void* volatile*>(slot) == nullptr) break;
        if (!fn(ctx, slot, false)) return false;
      }
    }
  }
  // Delay-load: длина IAT — по таблице имён (INT), до связывания слоты указывают
  // на заглушки внутри самого модуля и исходный адрес не содержат.
  if (img.delay_imports.Size != 0 && img.delay_imports.VirtualAddress != 0) {
    uint32_t rva = img.delay_imports.VirtualAddress;
    for (uint32_t d = 0; d < kMaxDescriptors; ++d, rva += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)) {
      if (!rva_ok(img, rva, sizeof(IMAGE_DELAYLOAD_DESCRIPTOR))) break;
      const IMAGE_DELAYLOAD_DESCRIPTOR* desc =
          reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(img.base + rva);
      if (desc->DllNameRVA == 0) break;
      if (!desc->Attributes.RvaBased) continue;  // старый формат с VA — пропуск
      uint32_t iat = desc->ImportAddressTableRVA;
      uint32_t intr = desc->ImportNameTableRVA;
      bool have_int = intr != 0 && rva_ok(img, intr, sizeof(void*));
      for (uint32_t i = 0; i < kMaxThunks; ++i, iat += sizeof(void*), intr += sizeof(void*)) {
        if (!rva_ok(img, iat, sizeof(void*))) break;
        if (have_int) {
          if (!rva_ok(img, intr, sizeof(void*))) break;
          if (*reinterpret_cast<const uintptr_t*>(img.base + intr) == 0) break;
        }
        void** slot = reinterpret_cast<void**>(img.base + iat);
        if (!have_int && *reinterpret_cast<void* volatile*>(slot) == nullptr) break;
        if (!fn(ctx, slot, true)) return false;
      }
    }
  }
  return true;
}

}  // namespace pe
}  // namespace dt
