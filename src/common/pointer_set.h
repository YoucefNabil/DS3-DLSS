#pragma once
// Small helpers for recognising the game's shaders from the device hooks.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ds3 {

// Lock-free set of pointers (shader objects), readable from any thread. The
// game creates the same shader many times, so sets can grow to thousands.
template <size_t kSize>
class PointerSet {
 public:
  void Insert(const void* p) {
    const auto v = reinterpret_cast<uintptr_t>(p);
    for (size_t n = 0, i = Slot(v); n < kSize; ++n, i = (i + 1) & (kSize - 1)) {
      uintptr_t cur = slots_[i].load(std::memory_order_acquire);
      if (cur == v) return;
      if ((cur == 0 || cur == kTombstone) && slots_[i].compare_exchange_strong(cur, v)) return;
      if (cur == v) return;
    }
  }
  void Erase(const void* p) {
    const auto v = reinterpret_cast<uintptr_t>(p);
    for (size_t n = 0, i = Slot(v); n < kSize; ++n, i = (i + 1) & (kSize - 1)) {
      uintptr_t cur = slots_[i].load(std::memory_order_acquire);
      if (cur == 0) return;
      if (cur == v) slots_[i].compare_exchange_strong(cur, kTombstone);
    }
  }
  bool Contains(const void* p) const {
    if (!p) return false;
    const auto v = reinterpret_cast<uintptr_t>(p);
    for (size_t n = 0, i = Slot(v); n < kSize; ++n, i = (i + 1) & (kSize - 1)) {
      const uintptr_t cur = slots_[i].load(std::memory_order_relaxed);
      if (cur == v) return true;
      if (cur == 0) return false;
    }
    return false;
  }

 private:
  static constexpr uintptr_t kTombstone = 1;
  static_assert((kSize & (kSize - 1)) == 0);
  static size_t Slot(uintptr_t v) { return ((v >> 4) * 0x9E3779B97F4A7C15ull >> 20) & (kSize - 1); }
  std::atomic<uintptr_t> slots_[kSize] = {};
};

// True if the shader bytecode contains `text` (names in its reflection data).
inline bool BytecodeContains(const void* code, size_t length, const char* text) {
  if (!code) return false;
  const auto* begin = static_cast<const char*>(code);
  const auto* end = begin + length;
  return std::search(begin, end, text, text + std::strlen(text)) != end;
}

}  // namespace ds3
