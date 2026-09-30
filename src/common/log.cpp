#include "common/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace ds3 {
namespace {

HMODULE g_module = nullptr;
std::wstring g_module_dir;
std::mutex g_log_mutex;
FILE* g_log_file = nullptr;

}  // namespace

void SetSelfModule(void* hmodule) {
  g_module = static_cast<HMODULE>(hmodule);
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(g_module, path, MAX_PATH);
  g_module_dir = path;
  g_module_dir.resize(g_module_dir.find_last_of(L"\\/") + 1);
}

const std::wstring& ModuleDir() { return g_module_dir; }

void LogInit() {
  std::lock_guard lock(g_log_mutex);
  if (g_log_file) return;
  _wfopen_s(&g_log_file, (g_module_dir + L"DS3_DLSS.log").c_str(), L"w");
}

void Log(const char* fmt, ...) {
  std::lock_guard lock(g_log_mutex);
  if (!g_log_file) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::fprintf(g_log_file, "%02u:%02u:%02u.%03u  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  va_list args;
  va_start(args, fmt);
  std::vfprintf(g_log_file, fmt, args);
  va_end(args);
  std::fputc('\n', g_log_file);
  std::fflush(g_log_file);
}

}  // namespace ds3
