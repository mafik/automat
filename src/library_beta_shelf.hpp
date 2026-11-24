#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "object.hpp"

namespace automat::library {

extern constinit Interface::Table kFfmpegMenu;
extern constinit Interface::Table kTensorFlowMenu;
extern constinit Interface::Table kPipelinesMenu;

struct BetaShelf : Object {
  StrView Name() const override { return "Beta"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(BetaShelf, *this); }
  INTERFACES(kFfmpegMenu, kTensorFlowMenu, kPipelinesMenu);
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
};

}  // namespace automat::library
