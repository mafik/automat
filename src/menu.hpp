#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <memory>

#include "action.hpp"
#include "pointer.hpp"
#include "ptr.hpp"
#include "span.hpp"
#include "str.hpp"

namespace automat {

// Option represents a potential action. It's the core of the menu system.
struct Option : ReferenceCounted {
  enum Dir : uint8_t { E = 0, NE, N, NW, W, SW, S, SE, DIR_COUNT, DIR_NONE = 255 };
  constexpr static Dir ShiftDir(Dir d, int delta) {
    return static_cast<Dir>((static_cast<int>(d) + delta + DIR_COUNT) % DIR_COUNT);
  }

  virtual ~Option();
  virtual std::unique_ptr<ui::Widget> MakeIcon(ui::Widget* parent) = 0;
  virtual Ptr<Option> Clone() const = 0;
  virtual std::unique_ptr<Action> Activate(ui::Pointer& pointer) = 0;
  virtual Dir PreferredDir() const { return DIR_NONE; }
  virtual Span<const ui::ActionTrigger> Triggers() const { return {}; }
  virtual ui::Pointer::Cursor Cursor() const { return ui::Pointer::Cursor::None; }
};

inline constexpr ui::ActionTrigger kLeftButton[] = {ui::PointerButton::Left};

struct TextOption : Option {
  Str text;
  TextOption(Str text);
  std::unique_ptr<ui::Widget> MakeIcon(ui::Widget* parent) override;
};

}  // namespace automat
