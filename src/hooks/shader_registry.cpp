#include "hooks/shader_registry.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "common/config.h"
#include "common/log.h"

namespace ds3::shaders {
namespace {
std::shared_mutex g_mutex;
std::unordered_map<const void*, uint64_t> g_hashes;
std::unordered_set<uint64_t> g_dumped;

void DumpBytecode(uint64_t hash, const void* bytecode, size_t length, const char* stage) {
  const std::wstring dir = ModuleDir() + L"DS3_DLSS_shaders\\";
  CreateDirectoryW(dir.c_str(), nullptr);
  wchar_t name[64];
  swprintf_s(name, L"%016llX.%hs.cso", static_cast<unsigned long long>(hash), stage);
  FILE* f = nullptr;
  if (_wfopen_s(&f, (dir + name).c_str(), L"wb") == 0 && f) {
    std::fwrite(bytecode, 1, length, f);
    std::fclose(f);
  }
}
}  // namespace

uint64_t HashBytecode(const void* bytecode, size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(bytecode);
  // DXBC container: "DXBC" magic followed by a 16-byte checksum.
  if (length >= 20 && std::memcmp(bytes, "DXBC", 4) == 0) {
    uint64_t hash;
    std::memcpy(&hash, bytes + 4, sizeof(hash));
    return hash;
  }
  uint64_t hash = 0xcbf29ce484222325ull;  // FNV-1a fallback
  for (size_t i = 0; i < length; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
  return hash;
}

void Register(const void* shader, const void* bytecode, size_t length, const char* stage) {
  if (!shader || !bytecode) return;
  const uint64_t hash = HashBytecode(bytecode, length);
  bool first_time;
  {
    std::unique_lock lock(g_mutex);
    g_hashes[shader] = hash;
    first_time = GetConfig().dump_shaders && g_dumped.insert(hash).second;
  }
  if (first_time) DumpBytecode(hash, bytecode, length, stage);
}

uint64_t Lookup(const void* shader) {
  if (!shader) return 0;
  std::shared_lock lock(g_mutex);
  const auto it = g_hashes.find(shader);
  return it == g_hashes.end() ? 0 : it->second;
}

}  // namespace ds3::shaders
