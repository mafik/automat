// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <shared_mutex>

#include "argument.hh"
#include "object.hh"
#include "ptr.hh"

namespace automat {

struct Syncable;

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

  struct Member {
    NestedWeakPtr<Syncable> weak;
    bool sink;
  };

  std::vector<Member> members;

  ~Gear();

  Ptr<Object> Clone() const override { return MAKE_PTR(Gear); }

  // Make sure that this member will receive sync notifications from the Sources in this SyncGroup.
  //
  // Under the hood it adds the given member to the `members` list.
  void AddSink(NestedPtr<Syncable>&);

  // Make sure that the sync notifications from this Syncable will be propagated to Sinks of this
  // Gear.
  //
  // This will set Syncable.sync_block to this Gear. Any existing Gear will be merged as
  // well.
  void AddSource(NestedPtr<Syncable>&);

  // AddSink & AddSource together.
  void FullSync(NestedPtr<Syncable>&);

  std::unique_ptr<ObjectWidget> MakeWidget(ui::Widget* parent, Object&) override;

  void SerializeState(ObjectSerializer& writer) const override;

  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

// Some objects within Automat may provide Syncables that can be "synced". A synced Syncable
// allows several objects that follow some Syncable to act as one.
//
// Syncable should be subclassed as a specific abstract Syncable (like OnOff) before it's
// used by Objects within Automat.
//
// For each command-like method a specific abstract Syncable should provide a protected virtual
// entry point, whose name starts with "On". It's intended to be overridden by a concrete
// implementation.
//
// In addition to that, each specific abstract Syncable (SAS) should also provide two non-virtual
// ways to call the method:
// - as a command - these methods should follow verb-like names, like "TurnOn", "Increment".
//   A "Do" prefix may be used if a good verb is not available. This method should use the
//   "ForwardDo" helper to forward the call to all synced Syncable implementations.
// - as a notification - these methods should start with "Notify". This method should use the
//   "ForwardNotify" helper to forward the call to _other_ synced Syncable implementations.
//
// The distinction between "Do" commands and "Notify" notifications allows Syncables that track
// external state to interoperate with other Automat objects without sending redundant commands to
// their externally tracked objects.
//
// IMPORTANT: To actually make this work, the "On" entry points should not be used directly (only
// through the "ForwardDo" & "ForwardNotify" wrappers). Whenever the "On" entry point us used
// directly, it's not going to be propagated to the other synced implementations.
struct Syncable : InlineArgument {
  bool source;

  // Note GetValueUnsafe used throughout the methods here is actually safe, because
  // ~Syncable will remove itself from Gear. Gear never contains dead pointers.
  ~Syncable() {
    if (end) {
      ERROR << "Some specific abstract Syncable forgot to call Unsync in its destructor";
    }
  }

  void CanConnect(Object& start, Part& end, Status&) const override;

  Style GetStyle() const override { return Style::Invisible; }

  void Unsync();

  template <class Self>
  void ForwardDo(this Self& self, auto&& lambda) {
    if (!self.source) {
      lambda(self);
    } else if (auto sync_block = self.end.template LockAs<Gear>()) {
      auto lock = std::shared_lock(sync_block->mutex);
      for (auto& other : sync_block->members) {
        if (!other.sink) continue;
        lambda(*other.weak.template GetUnsafe<Self>());
      }
    } else {
      lambda(self);
    }
  }

  template <class Self>
  void ForwardNotify(this Self& self, auto&& lambda) {
    if (!self.source) return;
    if (auto sync_block = self.end.template LockAs<Gear>()) {
      auto lock = std::shared_lock(sync_block->mutex);
      for (auto& other : sync_block->members) {
        if (auto* other_cast = other.weak.template GetUnsafe<Self>(); other_cast != &self) {
          lambda(*other_cast);
        }
      }
    }
  }

 protected:
  // Called when Syncable becomes a source - it should start monitoring its updates and call the
  // Notify methods.
  virtual void OnSync() {}

  // Called when Syncable stops being a source - it may stop monitoring its underlying state. No
  // need to call Notify methods any more.
  virtual void OnUnsync() {}

  friend class Gear;
};

// Returns a reference to the existing or a new Gear. This Syncable is initialized as a sync
// source.
Ptr<Gear> FindGearOrMake(NestedPtr<Syncable>& source);

Ptr<Gear> FindGearOrNull(NestedPtr<Syncable>&);

}  // namespace automat
