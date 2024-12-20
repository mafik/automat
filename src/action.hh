// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

namespace automat {

namespace gui {
struct Pointer;
struct Widget;
}  // namespace gui

// TODO: maybe merge Begin & End into constructor & destructor
struct Action {
  gui::Pointer& pointer;
  Action(gui::Pointer& pointer) : pointer(pointer) {}
  virtual ~Action() = default;
  virtual void Begin() = 0;
  virtual void Update() = 0;
  virtual void End() = 0;
  virtual gui::Widget* Widget() { return nullptr; }
};

}  // namespace automat
