// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <mutex>
#include <shared_mutex>

#include "object.hh"
#include "ptr.hh"

namespace automat {

struct SyncBlock;

// Some objects within Automat may provide interfaces that can be "synced". A synced interface
// allows several objects that follow some interface to act as one.
//
// Interface should be subclassed as a specific abstract interface (like OnOff) before it's
// used byObjects within Automat.
//
// For each command-like method a specific abstract interface should provide a protected virtual
// entry point, whose name starts with "On". It's intended to be overridden by a concrete
// implementation.
//
// In addition to that, each SAI should also provide two non-virtual ways to call the method:
// - as a command - these methods should follow verb-like names, like "TurnOn", "Increment".
//   A "Do" prefix may be used if a good verb is not available. This method should use the
//   "ForwardDo" helper to forward the call to all syced interface implementations.
// - as a notification - these methods should start with "Notify". This method should use the
//   "ForwardNotify" helper to forward the call to _other_ synced interface implementations.
//
// The distinction between "Do" commands and "Notify" notifications allows interfaces that track
// external state to interoperate with other Automat objects without sending redundant commands to
// their externally tracked objects.
//
// IMPORTANT: To actually make this work, the "On" entry points should not be used directly (only
// through the "ForwardDo" & "ForwardNotify" wrappers). Whenever the "On" entry point us used
// directly, it's not going to be propagated to the other synced implementations.
struct Interface : virtual Nomen {
  Ptr<SyncBlock> sync_block = nullptr;

  // Note tGetValueUnsafe used throughout the methods here is actually safe, because
  // ~Interface will remove itself from SyncBlock. SyncBlock never contains dead pointers.
  ~Interface() {
    if (sync_block) {
      ERROR << "Some Specific Abstract Interface forgot to call Unsync in its destructor";
    }
  }

  template <class Self>
  void Unsync(this Self& self) {
    if (self.sync_block == nullptr) return;
    auto sync_block = std::move(self.sync_block);  // keep mutex alive
    auto lock = std::unique_lock(sync_block->mutex);
    auto& members = sync_block->members;
    if (members.size() == 2) {
      static_cast<Self*>(members[0].GetValueUnsafe())->sync_block.Reset();
      static_cast<Self*>(members[1].GetValueUnsafe())->sync_block.Reset();
    } else {
      for (int i = 0; i < members.size(); ++i) {
        auto* member = static_cast<Self*>(members[i].GetValueUnsafe());
        if (member == &self) {
          members.erase(members.begin() + i);
          return;
        }
      }
      __builtin_unreachable();
    }
  }

  template <class Self>
  void ForwardDo(this Self& self, auto&& lambda) {
    if (self.sync_block) {
      auto lock = std::shared_lock(self.sync_block->mutex);
      for (auto& other : self.sync_block->members) {
        lambda(*static_cast<Self*>(other.GetValueUnsafe()));
      }
    } else {
      lambda(self);
    }
  }

  template <class Self>
  void ForwardNotify(this Self& self, auto&& lambda) {
    if (self.sync_block) {
      auto lock = std::shared_lock(self.sync_block->mutex);
      for (auto& other : self.sync_block->members) {
        if (auto other_cast = static_cast<Self*>(other.GetValueUnsafe()); other_cast != &self) {
          lambda(*other_cast);
        }
      }
    }
  }
};

struct SyncBlock : Object {
  std::shared_mutex mutex;
  std::vector<NestedWeakPtr<Interface>> members;

  Ptr<Object> Clone() const { return MAKE_PTR(SyncBlock); }
};

template <typename I>
void Sync(Object& self_object, I& self, Object& other_object, I& other) {
  if (self.sync_block == nullptr && other.sync_block == nullptr) {
    auto block = MAKE_PTR(SyncBlock);
    auto lock = std::unique_lock(block->mutex);
    block->members.emplace_back(self_object.AcquireWeakPtr(), &self);
    block->members.emplace_back(other_object.AcquireWeakPtr(), &other);
    self.sync_block = block;
    other.sync_block = std::move(block);
  } else if (self.sync_block != nullptr && other.sync_block != nullptr) {
    if (self.sync_block == other.sync_block) {
      return;
    }
    auto lock = std::scoped_lock(self.sync_block->mutex, other.sync_block->mutex);

    // Move the members from the 'other' block to 'self' block.
    auto& self_block = self.sync_block;
    auto& other_block = other.sync_block;
    auto& self_members = self.sync_block->members;
    auto& other_members = other.sync_block->members;
    while (!other_members.empty()) {
      self_members.emplace_back(std::move(other_members.back()));
      other_members.pop_back();
      ((I*)(self_members.back().GetValueUnsafe()))->sync_block = self_block;
    }
  } else {
    if (self.sync_block) {
      auto lock = std::unique_lock(self.sync_block->mutex);
      self.sync_block->members.emplace_back(other_object.AcquireWeakPtr(), &other);
      other.sync_block = self.sync_block;
    } else {
      auto lock = std::unique_lock(other.sync_block->mutex);
      other.sync_block->members.emplace_back(self_object.AcquireWeakPtr(), &self);
      self.sync_block = other.sync_block;
    }
  }
}

}  // namespace automat
