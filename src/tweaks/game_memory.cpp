#include "tweaks/game_memory.h"

#include <windows.h>

#include <cstring>
#include <string>

namespace ds3::game_memory {

Range CodeSection(uint8_t* base) {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if (std::memcmp(section->Name, ".text\0\0", 8) == 0)
      return {base + section->VirtualAddress,
              base + section->VirtualAddress + section->Misc.VirtualSize};
  }
  return {};
}

uint8_t* Find(Range range, const uint8_t* pattern, const char* mask, uint8_t* from) {
  const size_t length = std::strlen(mask);
  if (!range.begin || static_cast<size_t>(range.end - range.begin) < length) return nullptr;
  for (uint8_t* p = from ? from : range.begin; p + length <= range.end; ++p) {
    if (*p != pattern[0]) continue;
    size_t i = 1;
    while (i < length && (mask[i] != 'x' || p[i] == pattern[i])) ++i;
    if (i == length) return p;
  }
  return nullptr;
}

bool Write(void* address, const void* bytes, size_t size) {
  DWORD old = 0;
  if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old)) return false;
  std::memcpy(address, bytes, size);
  VirtualProtect(address, size, old, &old);
  FlushInstructionCache(GetCurrentProcess(), address, size);
  return true;
}

bool Redirect(uint8_t* disp, const void* target) {
  const intptr_t distance = static_cast<const uint8_t*>(target) - (disp + 4);
  if (distance != static_cast<int32_t>(distance)) return false;
  const int32_t value = static_cast<int32_t>(distance);
  return Write(disp, &value, sizeof(value));
}

void* AllocateNear(const void* near_address, size_t size) {
  SYSTEM_INFO info = {};
  GetSystemInfo(&info);
  const uintptr_t step = info.dwAllocationGranularity;
  const uintptr_t center = reinterpret_cast<uintptr_t>(near_address) & ~(step - 1);
  const uintptr_t reach = 0x70000000;  // stay well inside +-2 GB
  // Try above and below at growing distances.
  for (uintptr_t offset = step; offset < reach; offset += step) {
    for (const uintptr_t candidate : {center + offset, center - offset}) {
      MEMORY_BASIC_INFORMATION mbi = {};
      if (!VirtualQuery(reinterpret_cast<void*>(candidate), &mbi, sizeof(mbi)) ||
          mbi.State != MEM_FREE)
        continue;
      if (void* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
        return memory;
    }
  }
  return nullptr;
}

bool HookImport(uint8_t* base, const char* dll, const char* function, void* hook,
                void** original) {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return false;
  for (auto* import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
       import->Name; ++import) {
    if (_stricmp(reinterpret_cast<const char*>(base + import->Name), dll) != 0 ||
        !import->OriginalFirstThunk)
      continue;
    const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + import->OriginalFirstThunk);
    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + import->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (std::strcmp(by_name->Name, function) != 0) continue;
      const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(hook);
      void* const previous = reinterpret_cast<void*>(slots->u1.Function);
      if (!Write(&slots->u1.Function, &value, sizeof(value))) return false;
      *original = previous;
      return true;
    }
  }
  return false;
}

bool Ds3DebugFpsInstalled() {
  wchar_t exe[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (!length || length == MAX_PATH) return false;
  std::wstring dir(exe, length);
  dir.resize(dir.find_last_of(L'\\') + 1);
  return GetFileAttributesW((dir + L"D3DCompiler_43.dll").c_str()) != INVALID_FILE_ATTRIBUTES &&
         GetFileAttributesW((dir + L"FPSconfig.ini").c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace ds3::game_memory
