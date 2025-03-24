// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>

#include "str.hh"

namespace automat {

namespace gui {
struct Pointer;
struct Widget;
}  // namespace gui

struct Action;

struct Option {
  virtual ~Option() = default;
  virtual maf::StrView Name() const = 0;
  virtual std::unique_ptr<Action> Activate(gui::Pointer& pointer) const = 0;
};

struct Action {
  gui::Pointer& pointer;
  Action(gui::Pointer& pointer) : pointer(pointer) {}
  virtual ~Action() = default;
  virtual void Update() = 0;
  virtual gui::Widget* Widget() { return nullptr; }
};

/* Keeping this around for copy-pasting
struct MinimalActionTemplate : Action {
  MinimalActionTemplate(gui::Pointer& pointer) : Action(pointer) {}
  void Update() override {}
};
*/

}  // namespace automat
