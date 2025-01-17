// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <llvm/MC/MCInst.h>

#include "base.hh"

namespace automat::library {

struct Instruction : LiveObject, Runnable {
  llvm::MCInst mc_inst;
  void* address = nullptr;

  void Relocate(Location* new_here) override;

  std::string_view Name() const override;
  std::shared_ptr<Object> Clone() const override;

  LongRunning* OnRun(Location& here) override;

  static void SetupDevelopmentScenario();

  // Green box with a name of the object.
  struct Widget : FallbackWidget {
    Widget(std::weak_ptr<Object> object);
    std::string Text() const override;
  };

  std::shared_ptr<gui::Widget> MakeWidget() override { return std::make_shared<Widget>(WeakPtr()); }
};

}  // namespace automat::library
