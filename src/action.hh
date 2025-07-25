// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

namespace automat {

namespace gui {
struct Pointer;
struct Widget;
}  // namespace gui

// Action represents an action/gesture that user can perform by pressing a key / button / touching
// the screen and then moving the pointer around before releasing it.
//
// Actions are the main mechanism for user to interact with the UI.
struct Action {
  gui::Pointer& pointer;

  // Each action must be bound to a pointer. A reference to the pointer is stored internally to keep
  // track of its position.
  Action(gui::Pointer& pointer);

  // Action is destroyed when the pointer is released. This typically corresponds to the button
  // release or key up.
  virtual ~Action();

  // Update is called when the pointer moves (although spurious calls are also possible). This
  // function may be called hundreds of times per second.
  virtual void Update() = 0;

  virtual gui::Widget* Widget() { return nullptr; }
};

struct EmptyAction : Action {
  EmptyAction(gui::Pointer& pointer) : Action(pointer) {}
  void Update() override {}
};

}  // namespace automat
