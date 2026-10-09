// [Widescreen] test (no game needed): table search on a copy of the real
// exe's .data bytes (when the game is installed) and on synthetic data, plus
// the GraphicsConfig.xml rewrite.
// Usage: widescreen_test [path\to\DarkSoulsIII.exe]
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "widescreen/widescreen.h"

using namespace ds3::widescreen;

static int g_failures = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failures;
}

static void SyntheticTable() {
  std::vector<uint8_t> data(4096, 0xCD);
  std::memcpy(data.data() + 1000, kStockResolutions, sizeof(kStockResolutions));
  uint32_t* table = FindTable(data.data(), data.size());
  Check(table == reinterpret_cast<uint32_t*>(data.data() + 1000), "finds the table");

  // Already-changed first entry (e.g. a second Apply, or another mod).
  table[0] = 2376;
  table[1] = 1080;
  Check(FindTable(data.data(), data.size()) == table, "finds it with entry 0 replaced");

  // Two copies: ambiguous, must refuse.
  std::memcpy(data.data() + 3000, kStockResolutions, sizeof(kStockResolutions));
  Check(FindTable(data.data(), data.size()) == nullptr, "refuses two matches");

  std::vector<uint8_t> none(4096, 0);
  Check(FindTable(none.data(), none.size()) == nullptr, "nothing in empty data");
}

// Maps the exe's .data section from the file (raw part only) and searches it.
static void RealExe(const char* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::printf("skip  real exe (%s not found)\n", path);
    return;
  }
  std::vector<uint8_t> exe((std::istreambuf_iterator<char>(file)), {});
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe.data());
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe.data() + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if (std::memcmp(section->Name, ".data\0\0", 8) != 0) continue;
    uint8_t* data = exe.data() + section->PointerToRawData;
    uint32_t* table = FindTable(data, section->SizeOfRawData);
    Check(table != nullptr, "finds the table in the real exe");
    if (table) {
      const size_t rva = section->VirtualAddress + (reinterpret_cast<uint8_t*>(table) - data);
      std::printf("      at exe+0x%zX, first entry %ux%u\n", rva, table[0], table[1]);
      Check(table[0] == 800 && table[1] == 450, "first entry is 800x450");
    }
    return;
  }
  Check(false, "real exe has a .data section");
}

static void ConfigRewrite() {
  const std::wstring original =
      L"<?xml version=\"1.0\" encoding=\"UTF-16\" ?>\r\n<config><ScreenMode>WINDOW</ScreenMode>\r\n"
      L"<Resolution-WindowScreenWidth>1920</Resolution-WindowScreenWidth>\r\n"
      L"<Resolution-WindowScreenHeight>1080</Resolution-WindowScreenHeight>\r\n"
      L"<Resolution-FullScreenWidth>1920</Resolution-FullScreenWidth>\r\n"
      L"<Resolution-FullScreenHeight>1080</Resolution-FullScreenHeight>\r\n"
      L"<SSAO>HIGH</SSAO>\r\n</config>\r\n";
  std::wstring xml = original;
  Check(FollowDisplay(xml, 2376, 1080), "rewrite reports a change");
  Check(xml.find(L"<Resolution-WindowScreenWidth>2376</") != std::wstring::npos &&
            xml.find(L"<Resolution-FullScreenWidth>2376</") != std::wstring::npos &&
            xml.find(L"<Resolution-WindowScreenHeight>1080</") != std::wstring::npos,
        "windowed and full-screen width set");
  Check(xml.find(L"<SSAO>HIGH</SSAO>") != std::wstring::npos &&
            xml.size() == original.size(),
        "everything else unchanged");
  Check(!FollowDisplay(xml, 2376, 1080), "second rewrite is a no-op");
  Check(FollowDisplay(xml, 1920, 1080) && xml == original, "switching back restores 1920x1080");

  std::wstring other = L"<config><SSAO>HIGH</SSAO></config>";
  Check(!FollowDisplay(other, 2376, 1080), "file without the tags is left alone");
}

// Round trip on a temp copy of the real GraphicsConfig.xml (BOM-less UTF-16).
static void RealConfigFile() {
  wchar_t appdata[MAX_PATH] = {}, temp[MAX_PATH] = {};
  GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
  GetTempPathW(MAX_PATH, temp);
  const std::wstring real = std::wstring(appdata) + L"\\DarkSoulsIII\\GraphicsConfig.xml";
  const std::wstring copy = std::wstring(temp) + L"widescreen_test_GraphicsConfig.xml";
  if (!CopyFileW(real.c_str(), copy.c_str(), FALSE)) {
    std::printf("skip  real config file (not found)\n");
    return;
  }
  auto read = [&] {
    std::ifstream f(copy, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
  };
  const std::string before = read();
  Check(FollowDisplayInFile(copy, 2376, 1080), "real file: rewrite succeeds");
  const std::string after = read();
  const size_t bom = after[0] == '\xFF' ? 2 : 0;
  const std::wstring text(reinterpret_cast<const wchar_t*>(after.data() + bom),
                          (after.size() - bom) / 2);
  Check(text.find(L"<Resolution-WindowScreenWidth>2376</") != std::wstring::npos,
        "real file: windowed width is 2376");
  Check((before[0] == '\xFF') == (after[0] == '\xFF'), "real file: BOM kept as it was");
  DeleteFileW(copy.c_str());
}

int main(int argc, char** argv) {
  SyntheticTable();
  RealExe(argc > 1 ? argv[1]
                   : "G:\\SteamLibrary\\steamapps\\common\\DARK SOULS III\\Game\\DarkSoulsIII.exe");
  ConfigRewrite();
  RealConfigFile();
  Check(IsStock(1920, 1080) && IsStock(800, 450) && !IsStock(2376, 1080) && !IsStock(2560, 1080),
        "stock list");
  std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
  return g_failures ? 1 : 0;
}
