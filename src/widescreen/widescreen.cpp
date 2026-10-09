#include "widescreen/widescreen.h"

#include <windows.h>

#include <cstring>
#include <vector>

#include "common/config.h"
#include "core.h"

namespace ds3::widescreen {
namespace {

// Main module's .data section (where the resolution table lives).
bool FindDataSection(uint8_t** data, size_t* size) {
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if (std::memcmp(section->Name, ".data\0\0", 8) == 0) {
      *data = base + section->VirtualAddress;
      *size = section->Misc.VirtualSize;
      return true;
    }
  }
  return false;
}

bool SetTag(std::wstring& xml, const wchar_t* tag, uint32_t value) {
  const std::wstring open = L"<" + std::wstring(tag) + L">";
  const std::wstring close = L"</" + std::wstring(tag) + L">";
  size_t begin = xml.find(open);
  if (begin == std::wstring::npos) return false;
  begin += open.size();
  const size_t end = xml.find(close, begin);
  if (end == std::wstring::npos) return false;
  const std::wstring text = std::to_wstring(value);
  if (xml.compare(begin, end - begin, text) == 0) return false;
  xml.replace(begin, end - begin, text);
  return true;
}

}  // namespace

// GraphicsConfig.xml is UTF-16 LE; the game writes it without a BOM. A BOM,
// if present, is kept.
bool FollowDisplayInFile(const std::wstring& path, uint32_t width, uint32_t height) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    Log("widescreen: no %ls yet, the game will create it", path.c_str());
    return false;
  }
  std::vector<uint8_t> bytes(GetFileSize(file, nullptr));
  DWORD read = 0;
  const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
  CloseHandle(file);
  const size_t bom = bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE ? 2 : 0;
  if (!ok || read != bytes.size() || bytes.size() < bom + 2 || bytes[bom] != '<' ||
      bytes[bom + 1] != 0) {
    Log("widescreen: %ls is not the expected UTF-16 file, left alone", path.c_str());
    return false;
  }
  std::wstring xml(reinterpret_cast<const wchar_t*>(bytes.data() + bom), (bytes.size() - bom) / 2);
  if (!FollowDisplay(xml, width, height)) return true;

  // Write a copy, then swap it in, so a failure never leaves a half-written file.
  const std::wstring temp = path + L".ds3dlss.tmp";
  file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const DWORD xml_bytes = static_cast<DWORD>(xml.size() * sizeof(wchar_t));
  bool done = WriteFile(file, bytes.data(), static_cast<DWORD>(bom), &written, nullptr) &&
              WriteFile(file, xml.data(), xml_bytes, &written, nullptr) && written == xml_bytes;
  CloseHandle(file);
  done = done && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
  if (!done) DeleteFileW(temp.c_str());
  Log("widescreen: GraphicsConfig.xml resolution set to %ux%u: %s", width, height,
      done ? "ok" : "FAILED");
  return done;
}

bool IsStock(uint32_t width, uint32_t height) {
  for (const auto& r : kStockResolutions)
    if (r[0] == width && r[1] == height) return true;
  return false;
}

uint32_t* FindTable(uint8_t* data, size_t size) {
  // Match entries 1..11 so a table whose first entry was already changed is
  // still found; entry 0 is the one we overwrite.
  const auto* tail = reinterpret_cast<const uint8_t*>(&kStockResolutions[1]);
  const size_t tail_size = sizeof(kStockResolutions) - sizeof(kStockResolutions[0]);
  const size_t entry = sizeof(kStockResolutions[0]);
  uint32_t* found = nullptr;
  int hits = 0;
  for (size_t i = 0; i + entry + tail_size <= size; i += 4) {
    if (std::memcmp(data + i + entry, tail, tail_size) == 0) {
      found = reinterpret_cast<uint32_t*>(data + i);
      ++hits;
    }
  }
  return hits == 1 ? found : nullptr;
}

bool FollowDisplay(std::wstring& xml, uint32_t width, uint32_t height) {
  bool changed = false;
  changed |= SetTag(xml, L"Resolution-WindowScreenWidth", width);
  changed |= SetTag(xml, L"Resolution-WindowScreenHeight", height);
  changed |= SetTag(xml, L"Resolution-FullScreenWidth", width);
  changed |= SetTag(xml, L"Resolution-FullScreenHeight", height);
  return changed;
}

void Apply() {
  const Config& config = GetConfig();
  if (!config.widescreen) return;

  uint32_t width = static_cast<uint32_t>(config.widescreen_width);
  uint32_t height = static_cast<uint32_t>(config.widescreen_height);
  if (!width || !height) {
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
      Log("widescreen: could not read the display resolution, nothing changed");
      return;
    }
    width = mode.dmPelsWidth;
    height = mode.dmPelsHeight;
  }
  Log("widescreen: target %ux%u (aspect %.3f, %s)", width, height,
      static_cast<double>(width) / height,
      config.widescreen_width && config.widescreen_height ? "from the ini" : "display resolution");

  if (IsStock(width, height)) {
    Log("widescreen: already in the game's list, game memory untouched");
  } else {
    uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t* table = FindDataSection(&data, &size) ? FindTable(data, size) : nullptr;
    if (!table) {
      Log("widescreen: resolution table not found (unknown game version?), nothing changed");
      return;
    }
    Log("widescreen: table at exe+0x%llX, entry %ux%u replaced by %ux%u",
        static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(table) -
                                        reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr))),
        table[0], table[1], width, height);
    table[0] = width;
    table[1] = height;
  }

  wchar_t appdata[MAX_PATH] = {};
  if (config.widescreen_follow_display && GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH))
    FollowDisplayInFile(std::wstring(appdata) + L"\\DarkSoulsIII\\GraphicsConfig.xml", width,
                        height);
}

}  // namespace ds3::widescreen
