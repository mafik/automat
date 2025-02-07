// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <llvm/MC/MCInst.h>

#include "base.hh"

namespace automat::library {

struct Instruction : LiveObject, Runnable {
  llvm::MCInst mc_inst;
  void* address = nullptr;
  uint8_t size = 0;

  void Args(std::function<void(Argument&)> cb) override;
  std::shared_ptr<Object> ArgPrototype(const Argument&) override;

  void ConnectionAdded(Location& here, Connection&) override;
  void ConnectionRemoved(Location& here, Connection&) override;

  std::string_view Name() const override;
  std::shared_ptr<Object> Clone() const override;

  LongRunning* OnRun(Location& here) override;

  static void SetupDevelopmentScenario();

  struct Widget : gui::Widget {
    constexpr static float kWidth = 63.5_mm;
    constexpr static float kHeight = 44.5_mm;
    constexpr static float kBorderMargin = 4_mm;
    constexpr static float kDiagonal = Sqrt(kWidth * kWidth + kHeight * kHeight);

    std::weak_ptr<Object> object;
    Widget(std::weak_ptr<Object> object);

    std::string_view Name() const override { return "Instruction Widget"; }
    SkPath Shape() const override;
    void Draw(SkCanvas&) const override;
    std::unique_ptr<Action> FindAction(gui::Pointer& p, gui::ActionTrigger btn) override;
  };

  static void DrawInstruction(SkCanvas& canvas, const llvm::MCInst& inst);

  std::shared_ptr<gui::Widget> MakeWidget() override { return std::make_shared<Widget>(WeakPtr()); }
};

}  // namespace automat::library
