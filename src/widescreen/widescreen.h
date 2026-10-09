#pragma once
// [Widescreen]: lets the game render at any resolution / aspect ratio.
// The game only offers twelve 16:9 resolutions from a fixed table in
// DarkSoulsIII.exe's .data section. When the wanted resolution is not one of
// them, the first entry (800x450) is overwritten with it before the game runs,
// so it shows up in the in-game resolution list. That one 8-byte data write is
// the only change to game memory, and it is skipped when the resolution is
// already in the table (e.g. a 1920x1080 monitor).
#include <cstddef>
#include <cstdint>
#include <string>

namespace ds3::widescreen {

// Called from DllMain, before any game code runs. Does nothing when
// [Widescreen] Enabled=0.
void Apply();

// --- exposed for tests ---

// The game's stock list, in table order (verified on v1.15.2).
inline constexpr uint32_t kStockResolutions[12][2] = {
    {800, 450},   {1024, 576},  {1152, 648},  {1280, 720},  {1440, 810},  {1600, 900},
    {1680, 945},  {1920, 1080}, {2048, 1152}, {2560, 1440}, {3200, 1800}, {3840, 2160}};

bool IsStock(uint32_t width, uint32_t height);

// Start of the resolution table inside [data, data + size), or nullptr when
// it is not found exactly once.
uint32_t* FindTable(uint8_t* data, size_t size);

// GraphicsConfig.xml text: sets the saved resolutions (windowed and full
// screen) to width x height, so the game starts at the current display size
// without a trip to the menu. Returns true when the text changed.
bool FollowDisplay(std::wstring& xml, uint32_t width, uint32_t height);

// Same on the file at path (UTF-16 LE, with or without a BOM). Returns false
// when the file is missing, unreadable or could not be written.
bool FollowDisplayInFile(const std::wstring& path, uint32_t width, uint32_t height);

}  // namespace ds3::widescreen
