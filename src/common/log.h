#pragma once
#include <string>

namespace ds3 {

// Directory containing our d3d11.dll (the Game folder), with trailing backslash.
const std::wstring& ModuleDir();
void SetSelfModule(void* hmodule);

void LogInit();
void Log(const char* fmt, ...);

}  // namespace ds3
