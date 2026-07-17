#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "base.hpp"
#include "vm.hpp"

namespace automat {

struct BoardWidget;

// 2D Canvas holding objects & a spaghetti of connections.
struct Board : Object {
  Board();

  // Center of the board in RootWidget's coordinates.
  Vec2 position = {0, 0};

  deque<Ptr<Location>> locations;

  using Toy = BoardWidget;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  Ptr<Location> Extract(Location& location);

  void MoveToTop(Location& location);

  Location* LocationOrNull(Object& object);

  // Create a new location on top of all the others.
  Location& CreateEmpty();

  Location& Create(const Object& prototype) {
    auto& h = CreateEmpty();
    h.Create(prototype);
    return h;
  }

  // Adds the given object to the Board. Returns a pointer to the Location that stores the object.
  // Existing Location is returned, if the object was already part of the Board.
  Location& Insert(Ptr<Object>&& obj) {
    auto lock = std::lock_guard(vm.mutex);
    if (auto* loc = LocationOrNull(*obj)) {
      return *loc;
    }
    auto& h = CreateEmpty();
    h.InsertHere(std::move(obj));
    return h;
  }

  // Create an instance of T and return its location.
  //
  // The new instance is created from a prototype instance found in `prototypes`.
  template <typename T>
  Location& Create() {
    return Create(*prototypes->Find<T>());
  }

  void SerializeState(ObjectSerializer& writer) const override;

  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  Ptr<Object> Clone() const override {
    auto m = MAKE_PTR(Board);
    for (auto& my_it : locations) {
      auto& other_h = m->CreateEmpty();
      other_h.Create(*my_it->object);
    }
    return m;
  }

  string ToStr() const { return "Board"; }
};

// UI widget for Board. Handles drawing, drop target, and spatial queries.
struct BoardWidget : ObjectToy, ui::DropTarget {
  struct ToyStore toys;

  BoardWidget(ui::Widget* parent, Board& board);

  Ptr<Board> LockBoard() const { return LockOwner<Board>(); }

  std::string_view Name() const override { return "BoardWidget"; }

  void Poll(time::Timer& timer) { toys.Poll(timer); }

  // Widget overrides
  Tock Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  Compositor GetCompositor() const override { return Compositor::QUANTUM_REALM; }
  void VisitOptions(const OptionsVisitor&) const override;
  std::unique_ptr<Action> FindAction(ui::Pointer&, ui::ActionTrigger) override;

  // DropTarget overrides
  ui::DropTarget* AsDropTarget() override { return this; }
  bool CanDrop(Location&) const override { return true; }
  SkMatrix DropSnap(const Rect& bounds_local, Vec2 bounds_origin,
                    Vec2* fixed_point = nullptr) override;
  void DropLocation(Ptr<Location>&&) override;

  void RebuildOverlaps(Board&);

  // Spatial queries (these use widget shape data)
  void ConnectAtPoint(Argument, Vec2);
  void* Nearby(Vec2 center, float radius, std::function<void*(Location&)> callback);
  void NearbyCandidates(
      Location& here, Argument::Table& arg, float radius,
      std::function<void(ObjectToy&, Interface::Table*, Vec<Vec2AndDir>&)> callback);
  void ForStack(Location& base, std::function<void(Location&, int index)> callback);
  SkPath StackShape(Location& base);
  Vec<Ptr<Location>> DragStack(Location& base);
  Vec<Ptr<Location>> CloneStack(Location& base);
  void RaiseStack(Location& base);
};

BoardWidget* BoardOrNull(const ui::Widget& widget);

static_assert(ToyMaker<Board>);

}  // namespace automat
