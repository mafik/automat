// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"

namespace automat {

struct MachineWidget;

// 2D Canvas holding objects & a spaghetti of connections.
struct Machine : Object {
  Machine();
  string name = "";
  deque<Ptr<Location>> locations;

  using Toy = MachineWidget;
  std::unique_ptr<Object::Toy> MakeToy(ui::Widget* parent) override;

  Ptr<Location> Extract(Location& location);

  // Create a new location on top of all the others.
  Location& CreateEmpty();

  Location& Create(const Object& prototype) {
    auto& h = CreateEmpty();
    h.Create(prototype);
    return h;
  }

  // Adds the given object to the Machine. Returns a pointer to the Location that stores the object.
  // Existing Location is returned, if the object was already part of the Machine.
  Location& Insert(Ptr<Object>&& obj) {
    for (auto& loc : locations) {
      if (loc->object == obj) {
        return *loc;
      }
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

  string_view Name() const override { return name; }
  Ptr<Object> Clone() const override {
    auto m = MAKE_PTR(Machine);
    for (auto& my_it : locations) {
      auto& other_h = m->CreateEmpty();
      other_h.Create(*my_it->object);
    }
    return m;
  }

  void Relocate(Location* parent) override;

  string ToStr() const { return f("Machine({})", name); }

  // Report all errors that occured within this machine.
  //
  // This function will return all errors held by locations of this machine &
  // recurse into submachines.
  void Diagnostics(function<void(Location*, Error&)> error_callback) {
    for (auto& location : locations) {
      ManipulateError(*location->object, [&](Error& err) {
        if (!err.IsPresent()) {
          return;
        }
        error_callback(location.get(), err);
      });
      if (auto submachine = dynamic_cast<Machine*>(location->object.get())) {
        submachine->Diagnostics(error_callback);
      }
    }
  }
};

// UI widget for Machine. Handles drawing, drop target, and spatial queries.
struct MachineWidget : Object::Toy, ui::DropTarget {
  MachineWidget(ui::Widget* parent, Machine& machine);

  Ptr<Machine> LockMachine() const { return LockOwner<Machine>(); }

  std::string_view Name() const override { return "MachineWidget"; }

  // Widget overrides
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  Compositor GetCompositor() const override { return Compositor::QUANTUM_REALM; }

  // DropTarget overrides
  ui::DropTarget* AsDropTarget() override { return this; }
  bool CanDrop(Location&) const override { return true; }
  SkMatrix DropSnap(const Rect& bounds_local, Vec2 bounds_origin,
                    Vec2* fixed_point = nullptr) override;
  void DropLocation(Ptr<Location>&&) override;

  // Spatial queries (these use widget shape data)
  void ConnectAtPoint(Object& start, Argument&, Vec2);
  void* Nearby(Vec2 center, float radius, std::function<void*(Location&)> callback);
  void NearbyCandidates(Location& here, const Argument& arg, float radius,
                        std::function<void(Location&, Vec<Vec2AndDir>&)> callback);
  void ForStack(Location& base, std::function<void(Location&, int index)> callback);
  SkPath StackShape(Location& base);
  Vec<Ptr<Location>> ExtractStack(Location& base);
  void RaiseStack(Location& base);
};

static_assert(ToyMaker<Machine>);

}  // namespace automat
