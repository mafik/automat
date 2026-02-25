// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <shared_mutex>

#include "argument.hh"
#include "object.hh"
#include "ptr.hh"

namespace automat {

struct Gear;
struct SyncConnectionWidget;

struct Syncable : Argument {
  struct Table : Argument::Table {
    static bool classof(const Interface::Table* i) {
      return i->kind >= Interface::kSyncable && i->kind <= Interface::kLastArgument;
    }

    bool (*can_sync)(Syncable, Syncable other) = nullptr;
    void (*on_sync)(Syncable) = nullptr;
    void (*on_unsync)(Syncable) = nullptr;

    static void DefaultCanConnect(Argument self, Interface end, Status& status);
    static void DefaultOnConnect(Argument self, Interface end);
    static NestedPtr<Interface::Table> DefaultFind(Argument self);

    constexpr Table(StrView name, Kind kind = Interface::kSyncable) : Argument::Table(name, kind) {
      style = Style::Invisible;
      can_connect = &DefaultCanConnect;
      on_connect = &DefaultOnConnect;
      find = &DefaultFind;
    }

    template <typename ImplT>
    constexpr void FillFrom() {
      Argument::Table::FillFrom<ImplT>();
      if constexpr (requires(ImplT& i, Syncable s) { i.CanSync(s); })
        can_sync = [](Syncable a, Syncable b) { return static_cast<ImplT&>(a).CanSync(b); };
      if constexpr (requires(ImplT& i) { i.OnSync(); })
        on_sync = [](Syncable self) { static_cast<ImplT&>(self).OnSync(); };
      if constexpr (requires(ImplT& i) { i.OnUnsync(); })
        on_unsync = [](Syncable self) { static_cast<ImplT&>(self).OnUnsync(); };
    }
  };

  struct State {
    NestedWeakPtr<Interface::Table> end;
    bool source = false;

    // Remember to call this before this state is destroyed!
    //
    // Note: DO NOT REMOVE - interfaces that don't go through Def-based table definition must call
    // this directly in object destructors.
    void Unsync(Object& self, Syncable::Table& syncable);
  };

  INTERFACE_BOUND(Syncable, Argument)

  // ForwardDo distributes a command to all synced implementations.
  template <typename T, typename F>
  void ForwardDo(this T self, F&& lambda);

  // ForwardNotify distributes a notification to OTHER synced implementations (not self).
  template <typename T, typename F>
  void ForwardNotify(this T self, F&& lambda);

  void Unsync();

  // Impl may provide:
  //   bool CanSync(Syncable other);
  //   void OnSync();
  //   void OnUnsync();
  template <typename ImplT>
  struct Def : State, DefBase {
    using Impl = ImplT;
    using Bound = Syncable;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {
      if (source || !end.IsExpired()) {
        Bind().Unsync();
      }
    }
  };

  using Toy = SyncConnectionWidget;
  ReferenceCounted& GetOwner() { return *object_ptr; }
  Interface::Table* GetInterface() { return table_ptr; }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent);
};

// Gear-shaped object that can make multiple interfaces act as one.
struct Gear : Object {
  std::shared_mutex mutex;

  struct Member {
    NestedWeakPtr<Syncable::Table> weak;
    bool sink;
  };

  std::vector<Member> members;

  ~Gear();

  Ptr<Object> Clone() const override { return MAKE_PTR(Gear); }

  // Make sure that this member will receive sync notifications from the Sources in this SyncGroup.
  void AddSink(Object& obj, Syncable::Table& syncable);

  // Make sure that the sync notifications from this Syncable will be propagated to Sinks of this
  // Gear.
  void AddSource(Object& obj, Syncable::Table& syncable);

  // AddSink & AddSource together.
  void FullSync(Object& obj, Syncable::Table& syncable);

  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer& writer) const override;

  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

template <typename T, typename F>
void Syncable::ForwardDo(this T self, F&& lambda) {
  auto& st = *self.state;
  if (!st.source) {
    lambda(self);
  } else if (auto gear = st.end.template OwnerLockAs<Gear>()) {
    auto lock = std::shared_lock(gear->mutex);
    for (auto& member : gear->members) {
      if (!member.sink) continue;
      auto locked = member.weak.Lock();
      if (!locked) continue;
      T other_iface(*locked.template Owner<Object>(),
                    *static_cast<typename T::Table*>(locked.Get()));
      lambda(other_iface);
    }
  } else {
    lambda(self);
  }
}

template <typename T, typename F>
void Syncable::ForwardNotify(this T self, F&& lambda) {
  auto& st = *self.state;
  if (!st.source) return;
  if (auto gear = st.end.template OwnerLockAs<Gear>()) {
    auto lock = std::shared_lock(gear->mutex);
    for (auto& member : gear->members) {
      auto locked = member.weak.Lock();
      if (!locked) continue;
      // Skip self
      if (locked.Get() == self.table_ptr && locked.template Owner<Object>() == self.object_ptr)
        continue;
      T other_iface(*locked.template Owner<Object>(),
                    *static_cast<typename T::Table*>(locked.Get()));
      lambda(other_iface);
    }
  }
}

// Returns a reference to the existing or a new Gear.
Ptr<Gear> FindGearOrMake(Object& source_obj, Syncable::Table& source);
Ptr<Gear> FindGearOrNull(Object& source_obj, Syncable::Table& source);

// Widget that draws one belt connection from a Gear to a synced member.
struct SyncConnectionWidget : Toy {
  Rect bounds;
  SkPath end_shape;
  Vec2 end{};

  SyncConnectionWidget(ui::Widget* parent, Object& object, Syncable::Table& syncable);

  SkPath Shape() const override;
  animation::Phase Tick(time::Timer& t) override;
  void Draw(SkCanvas& canvas) const override;
  Optional<Rect> TextureBounds() const override;
};

static_assert(ToyMaker<Syncable>);

}  // namespace automat
