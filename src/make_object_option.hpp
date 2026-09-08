#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "menu.hpp"
#include "mortal.hpp"

namespace automat {

struct MakeObjectOption : Option {
  Ptr<Object> proto;
  Dir dir;
  MortalPtr<ui::Widget> icon;
  MakeObjectOption(Ptr<Object> proto, Dir dir = DIR_NONE);
  std::unique_ptr<ui::Widget> MakeIcon(ui::Widget* parent) override;
  Ptr<Option> Clone() const override;
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) override;
  Dir PreferredDir() const override { return dir; }
};

}  // namespace automat