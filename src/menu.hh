// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <memory>

#include "action.hh"
#include "animation.hh"
#include "vec.hh"

namespace automat {

namespace ui {
struct Widget;
}

// Option represents a potential action. It's the core of the menu system.
struct Option {
  enum Dir : uint8_t { E = 0, NE, N, NW, W, SW, S, SE, DIR_COUNT, DIR_NONE = 255 };
  constexpr static Dir ShiftDir(Dir d, int delta) {
    return static_cast<Dir>((static_cast<int>(d) + delta + DIR_COUNT) % DIR_COUNT);
  }

  struct Animation {
    animation::SpringV2<Vec2> offset;  // used for springy snap of option to mouse pointer
  };

  Animation animation;
  std::unique_ptr<ui::Widget> icon;

  Option() = default;
  Option(Option&&) = default;
  Option& operator=(Option&&) = default;
  virtual ~Option();
  virtual std::unique_ptr<ui::Widget> MakeIcon(ui::Widget* parent) = 0;
  virtual std::unique_ptr<Option> Clone() const = 0;
  virtual std::unique_ptr<Action> Activate(ui::Pointer& pointer) const = 0;
  virtual Dir PreferredDir() const { return DIR_NONE; }
};

struct TextOption : Option {
  Str text;
  TextOption(Str text);
  std::unique_ptr<ui::Widget> MakeIcon(ui::Widget* parent) override;
};

using OptionsVisitor = std::function<void(Option&)>;

struct OptionsProvider {
  virtual void VisitOptions(const OptionsVisitor&) const = 0;

  Vec<std::unique_ptr<Option>> CloneOptions() const;
  std::unique_ptr<Action> OpenMenu(ui::Pointer& pointer) const;
};

}  // namespace automat
