// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <memory>

#include "action.hh"
#include "ptr.hh"
#include "vec.hh"

namespace automat {

// Option represents a potential action. It's the core of the menu system.
struct Option {
  Ptr<gui::Widget> icon;
  Option(Ptr<gui::Widget>&& icon);
  Option(maf::Str name);
  virtual ~Option() = default;
  virtual std::unique_ptr<Option> Clone() const = 0;
  virtual std::unique_ptr<Action> Activate(gui::Pointer& pointer) const = 0;
};

using OptionsVisitor = std::function<void(Option&)>;

struct OptionsProvider {
  virtual void VisitOptions(const OptionsVisitor&) const = 0;

  maf::Vec<std::unique_ptr<Option>> CloneOptions() const;
  std::unique_ptr<Action> OpenMenu(gui::Pointer& pointer) const;
};

}  // namespace automat