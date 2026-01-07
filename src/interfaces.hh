// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <shared_mutex>

#include "object.hh"
#include "ptr.hh"

namespace automat {

struct Interface;

// Gear-shaped object that can make multiple interfaces act as one.
struct SyncBlock : Object {
  std::shared_mutex mutex;
  std::vector<NestedWeakPtr<Interface>> members;

  Ptr<Object> Clone() const override { return MAKE_PTR(SyncBlock); }

  // Make sure that this member will receive sync notifications from the Sources in this SyncGroup.
  //
  // Under the hood it adds the given member to the `members` list.
  void AddSink(NestedPtr<Interface>& member);

  // Make sure that the sync notifications from this interface will be propagated to Sinks in this
  // SyncGroup.
  //
  // This will set Interface.sync_block to this SyncBlock. Any existing SyncBlock will be merged as
  // well.
  void AddSource(Interface&);

  // AddSink & AddSource together.
  void FullSync(NestedPtr<Interface>& member);

  std::unique_ptr<WidgetInterface> MakeWidget(ui::Widget* parent) override;
};

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
struct Interface : virtual Named {
  Ptr<SyncBlock> sync_block = nullptr;

  // Note GetValueUnsafe used throughout the methods here is actually safe, because
  // ~Interface will remove itself from SyncBlock. SyncBlock never contains dead pointers.
  ~Interface() {
    if (sync_block) {
      ERROR << "Some Specific Abstract Interface forgot to call Unsync in its destructor";
    }
  }

  void Unsync();

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

  // Returns a reference to the existing or a new SyncBlock.
  Ptr<SyncBlock> Sync();

 protected:
  // Interface should start monitoring its updates and call the Notify methods.
  virtual void OnSync() {}

  // Interface may stop monitoring its underlying state. No need to call Notify methods any more.
  virtual void OnUnsync() {}
};

}  // namespace automat
