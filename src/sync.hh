// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <shared_mutex>

#include "argument.hh"
#include "object.hh"
#include "ptr.hh"

namespace automat {

struct Gear;
struct SyncBelt;
struct GearWidget;

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

  struct State : Argument::State {
    WeakPtr<Gear> gear_weak;
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
      if (source || !gear_weak.IsExpired()) {
        Bind().Unsync();
      }
    }
  };

  using Toy = SyncBelt;
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

  using Toy = GearWidget;
};

template <typename T, typename F>
void Syncable::ForwardDo(this T self, F&& lambda) {
  auto& st = *self.state;
  if (!st.source) {
    self.WakeToys();
    lambda(self);
  } else if (auto gear = st.gear_weak.Lock()) {
    gear->WakeToys();
    auto lock = std::shared_lock(gear->mutex);
    for (auto& member : gear->members) {
      if (!member.sink) continue;
      auto locked = member.weak.Lock();
      if (!locked) continue;
      T other(*locked.template Owner<Object>(), *static_cast<typename T::Table*>(locked.Get()));
      other.WakeToys();
      lambda(other);
    }
  } else {
    self.WakeToys();
    lambda(self);
  }
}

template <typename T, typename F>
void Syncable::ForwardNotify(this T self, F&& lambda) {
  auto& st = *self.state;
  if (!st.source) return;
  self.WakeToys();
  if (auto gear = st.gear_weak.Lock()) {
    gear->WakeToys();
    auto lock = std::shared_lock(gear->mutex);
    // How to visualize sync events?
    //
    // Option 1: every sync event rotates the gear by one tooth
    // Option 2: same but also introduce a maximum speed
    for (auto& member : gear->members) {
      member.weak.template OwnerUnsafe<Object>()->WakeToys();
      auto locked = member.weak.Lock();
      if (!locked) continue;
      T other(*locked.template Owner<Object>(), *static_cast<typename T::Table*>(locked.Get()));
      // Skip self
      if (self == other) continue;
      lambda(other);
      other.WakeToys();
    }
  }
}

// Returns a reference to the existing or a new Gear.
Ptr<Gear> FindGearOrMake(Object& source_obj, Syncable::Table& source);
Ptr<Gear> FindGearOrNull(Object& source_obj, Syncable::Table& source);

// Widget that draws one belt connection from a Gear to a synced member.
struct SyncBelt : ArgumentToy {
  SkPath origin_shape;
  Vec2 origin{};                                  // where the rubber belt starts
  Vec2 pinion{};                                  // where the small gear is located
  animation::SpringV2<Vec2> pinion_deflection{};  // deflection from the ideal position
  float angle = 0;
  bool is_dragged = false;

  SyncBelt(ui::Widget* parent, Object& object, Syncable::Table& syncable);

  SkPath Shape() const override;
  std::unique_ptr<Action> FindAction(ui::Pointer&, ui::ActionTrigger) override;
  animation::Phase Tick(time::Timer& t) override;
  Compositor GetCompositor() const override { return Compositor::ANCHOR_WARP; }
  Vec<Vec2> TextureAnchors() override;
  void Draw(SkCanvas& canvas) const override;
  Optional<Rect> TextureBounds() const override;
};

static_assert(ToyMaker<Syncable>);

struct SyncAction : Action {
  NestedWeakPtr<Syncable::Table> weak;
  SyncAction(ui::Pointer& pointer, Syncable syncable);
  ~SyncAction();
  void Update() override;
  bool Highlight(Interface end) const override;
  ui::Widget* Widget() override;
};

}  // namespace automat
