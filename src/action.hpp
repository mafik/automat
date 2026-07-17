#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <functional>

#include "interface.hpp"
#include "mortal.hpp"

namespace automat {

struct Object;

namespace time {
struct Timer;
}  // namespace time

namespace ui {
struct Pointer;
struct Widget;
}  // namespace ui

// Action represents an action/gesture that user can perform by pressing a key / button / touching
// the screen and then moving the pointer around before releasing it.
//
// Actions are the main mechanism for user to interact with the UI.
struct Action {
  MortalCoil mortal_coil;
  ui::Pointer& pointer;

  // Each action must be bound to a pointer. A reference to the pointer is stored internally to keep
  // track of its position.
  Action(ui::Pointer& pointer);

  // Action is destroyed when the pointer is released. This typically corresponds to the button
  // release or key up.
  virtual ~Action() = default;

  // Update is called when the pointer moves (although spurious calls are also possible). This
  // function may be called hundreds of times per second.
  virtual void Update() = 0;

  // Return true to highlight the given part of some object.
  virtual bool Highlight(Interface highlighted) const { return false; }

  // Check the wake counters of referenced objects here.
  virtual void Poll(time::Timer&) {}

  virtual ui::Widget* Widget() { return nullptr; }

  virtual void VisitObjects(std::function<void(Object&)> visitor) {}
};

struct EmptyAction : Action {
  EmptyAction(ui::Pointer& pointer) : Action(pointer) {}
  void Update() override {}
};

}  // namespace automat
