// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <shared_mutex>

#include "argument.hh"
#include "object.hh"
#include "ptr.hh"

namespace automat {

struct Syncable;

// Per-instance state for a Syncable interface.
// Each Object that exposes a Syncable interface stores one SyncState per Syncable.
struct SyncState {
  NestedWeakPtr<Interface> end;
  bool source = false;

  void Unsync(Object& self, Syncable&);

  ~SyncState() {
    // TODO: call Unsync somehow...
  }
};

// Gear-shaped object that can make multiple interfaces act as one.
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
  void AddSink(Object& obj, Syncable& syncable);

  // Make sure that the sync notifications from this Syncable will be propagated to Sinks of this
  // Gear.
  //
  // This will set Syncable.sync_block to this Gear. Any existing Gear will be merged as
  // well.
  void AddSource(Object& obj, Syncable& syncable);

  // AddSink & AddSource together.
  void FullSync(Object& obj, Syncable& syncable);

  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer& writer) const override;

  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

// Some objects within Automat may provide Syncables that can be "synced". A synced Syncable
// allows several objects that follow some Syncable to act as one.
//
// Syncable should be subclassed as a specific abstract Syncable (like OnOff) before it's
// used by Objects within Automat.
//
// For each command-like method a specific abstract Syncable should provide an entry point, whose
// name starts with "on_". It's intended to be overridden by a concrete implementation.
//
// In addition to that, each specific abstract Syncable (SAS) should also provide two methods to
// call the command:
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
struct Syncable : Argument {
  static bool classof(const Interface* i) {
    return i->kind >= Interface::kSyncable && i->kind <= Interface::kLastArgument;
  }

  // Function pointer for sync behavior.
  // get_sync_state returns the SyncState for THIS Syncable on a given Object.
  SyncState& (*get_sync_state)(Object&) = nullptr;

  // Checks whether this Syncable can be synchronized with `other`.
  bool (*can_sync)(const Syncable&, const Syncable& other) = nullptr;

  // Called when Syncable becomes a source - it should start monitoring its updates and call the
  // Notify methods.
  void (*on_sync)(const Syncable&, Object&) = nullptr;

  // Called when Syncable stops being a source - it may stop monitoring its underlying state. No
  // need to call Notify methods any more.
  void (*on_unsync)(const Syncable&, Object&) = nullptr;

  Syncable(StrView name, Kind kind = Interface::kSyncable);

  template <typename T>
  Syncable(StrView name, SyncState& (*get)(T&), Kind kind = Interface::kSyncable)
      : Syncable(name, kind) {
    get_sync_state = reinterpret_cast<SyncState& (*)(Object&)>(get);
  }

  // ForwardDo distributes a command to all synced implementations.
  // Lambda receives (Object& target, const SyncableT& target_iface).
  template <typename SyncableT, typename F>
  void ForwardDo(Object& self, F&& lambda) const {
    auto& state = get_sync_state(self);
    if (!state.source) {
      lambda(self, static_cast<const SyncableT&>(*this));
    } else if (auto gear = state.end.OwnerLockAs<Gear>()) {
      auto lock = std::shared_lock(gear->mutex);
      for (auto& member : gear->members) {
        if (!member.sink) continue;
        auto locked = member.weak.Lock();
        if (!locked) continue;
        auto* other_iface = static_cast<const SyncableT*>(locked.Get());
        lambda(*locked.Owner<Object>(), *other_iface);
      }
    } else {
      lambda(self, static_cast<const SyncableT&>(*this));
    }
  }

  // ForwardNotify distributes a notification to OTHER synced implementations (not self).
  template <typename SyncableT, typename F>
  void ForwardNotify(Object& self, F&& lambda) const {
    auto& state = get_sync_state(self);
    if (!state.source) return;
    if (auto gear = state.end.OwnerLockAs<Gear>()) {
      auto lock = std::shared_lock(gear->mutex);
      for (auto& member : gear->members) {
        auto locked = member.weak.Lock();
        if (!locked) continue;
        // Skip self
        if (locked.Get() == this && locked.Owner<Object>() == &self) continue;
        auto* other_iface = static_cast<const SyncableT*>(locked.Get());
        lambda(*locked.Owner<Object>(), *other_iface);
      }
    }
  }

  void Unsync(Object& self);
};

// Returns a reference to the existing or a new Gear. This Syncable is initialized as a sync
// source.
Ptr<Gear> FindGearOrMake(Object& source_obj, Syncable& source);

Ptr<Gear> FindGearOrNull(Object& source_obj, Syncable& source);

// Widget that draws one belt connection from a Gear to a synced member.
struct SyncConnectionWidget : Toy {
  Rect bounds;
  SkPath end_shape;
  Vec2 end{};

  SyncConnectionWidget(ui::Widget* parent, Object& object, Syncable& syncable);

  SkPath Shape() const override;
  animation::Phase Tick(time::Timer& t) override;
  void Draw(SkCanvas& canvas) const override;
  Optional<Rect> TextureBounds() const override;
};

// ToyMaker for belt widgets that connect a Syncable to a Gear.
struct SyncMemberOf {
  using Toy = SyncConnectionWidget;
  Object& object;
  Syncable& syncable;
  ReferenceCounted& GetOwner() { return object; }
  Interface* GetInterface() { return &syncable; }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent);
};

}  // namespace automat
