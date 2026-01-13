// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <shared_mutex>

#include "argument.hh"
#include "object.hh"
#include "ptr.hh"

namespace automat {

struct Interface;

// Gear-shaped object that can make multiple interfaces act as one.
//
// There are actually a couple potential Gear designs:
// - a type-agnostic Gear, where different interface types can connect, and at run-time, they
// dynamic_cast to check if the sinks are compatible
// - a generic but strongly typed Gear that adopts the type of the first connected
// interface then it makes sure to only interop with those type of interfaces
// - a different Gear specialization for each interface type
// TODO: figure out which would work best
struct Gear : Object {
  std::shared_mutex mutex;

  std::vector<NestedWeakPtr<Interface>> sinks;
  std::vector<NestedWeakPtr<Interface>> sources;

  ~Gear();

  Ptr<Object> Clone() const override { return MAKE_PTR(Gear); }

  // Make sure that this member will receive sync notifications from the Sources in this SyncGroup.
  //
  // Under the hood it adds the given member to the `members` list.
  void AddSink(NestedPtr<Interface>&);

  // Make sure that the sync notifications from this interface will be propagated to Sinks of this
  // Gear.
  //
  // This will set Interface.sync_block to this Gear. Any existing Gear will be merged as
  // well.
  void AddSource(NestedPtr<Interface>&);

  // AddSink & AddSource together.
  void FullSync(NestedPtr<Interface>&);

  std::unique_ptr<ObjectWidget> MakeWidget(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer& writer) const override;

  void DeserializeState(ObjectDeserializer& d) override;
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
struct Interface : InlineArgument {
  // Note GetValueUnsafe used throughout the methods here is actually safe, because
  // ~Interface will remove itself from Gear. Gear never contains dead pointers.
  ~Interface() {
    if (end) {
      ERROR << "Some Specific Abstract Interface forgot to call Unsync in its destructor";
    }
  }

  void CanConnect(Object& start, Part& end, Status&) const override;

  void Unsync();

  template <class Self>
  void ForwardDo(this Self& self, auto&& lambda) {
    if (auto sync_block = self.end.template LockAs<Gear>()) {
      auto lock = std::shared_lock(sync_block->mutex);
      for (auto& other : sync_block->sinks) {
        lambda(*other.template GetUnsafe<Self>());
      }
    } else {
      lambda(self);
    }
  }

  template <class Self>
  void ForwardNotify(this Self& self, auto&& lambda) {
    if (auto sync_block = self.end.template LockAs<Gear>()) {
      auto lock = std::shared_lock(sync_block->mutex);
      for (auto& other : sync_block->sinks) {
        if (auto* other_cast = other.template GetUnsafe<Self>(); other_cast != &self) {
          lambda(*other_cast);
        }
      }
    }
  }

 protected:
  // Called when Interface becomes a source - it should start monitoring its updates and call the
  // Notify methods.
  virtual void OnSync() {}

  // Called when Interface stops being a source - it may stop monitoring its underlying state. No
  // need to call Notify methods any more.
  virtual void OnUnsync() {}

  friend class Gear;
};

// Returns a reference to the existing or a new Gear. This interface is initialized as a sync
// source.
Ptr<Gear> Sync(NestedPtr<Interface>& source);

}  // namespace automat
