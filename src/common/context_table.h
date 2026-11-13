#pragma once
// State kept per device context, for hooks that track what a context has
// bound. Contexts record on worker threads, but each is used by one thread at
// a time, so a context's own state needs no further locking. Slots are never
// freed: the game keeps its contexts for the whole session.
#include <d3d11.h>

#include <atomic>
#include <cstddef>

#include "core.h"

namespace ds3 {

template <typename State, size_t kSize = 64>
class ContextTable {
 public:
  // `owner` names the feature in the log line written if the table fills up.
  explicit ContextTable(const char* owner) : owner_(owner) {}

  // The context's state, or null when it has none yet.
  State* Find(ID3D11DeviceContext* ctx) { return Lookup(ctx, false); }

  // The context's state, added on first use. Null when the table is full: that
  // context is then ignored by the feature, which is logged once.
  State* FindOrAdd(ID3D11DeviceContext* ctx) {
    State* state = Lookup(ctx, true);
    if (!state && !warned_.exchange(true))
      Log("%s: more than %zu device contexts, the extra ones are ignored (logged once)", owner_,
          kSize);
    return state;
  }

 private:
  struct Slot {
    std::atomic<ID3D11DeviceContext*> ctx{nullptr};
    State state;
  };

  State* Lookup(ID3D11DeviceContext* ctx, bool add) {
    for (Slot& s : slots_) {
      ID3D11DeviceContext* owner = s.ctx.load(std::memory_order_acquire);
      if (owner == ctx) return &s.state;
      if (owner) continue;
      if (!add) return nullptr;
      // On failure `owner` holds whoever took the slot first.
      if (s.ctx.compare_exchange_strong(owner, ctx) || owner == ctx) return &s.state;
    }
    return nullptr;
  }

  const char* owner_;
  Slot slots_[kSize];
  std::atomic<bool> warned_{false};
};

}  // namespace ds3
