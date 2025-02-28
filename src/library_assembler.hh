// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>

#include "library_instruction.hh"
#include "machine_code.hh"
#include "object.hh"
#include "status.hh"

namespace automat::library {

constexpr size_t kMachineCodeSize = 1024 * 4;

struct DeleteWithMunmap {
  void operator()(void* ptr) const;
};

struct Regs;

struct Assembler;
struct AssemblerWidget;

struct RegisterWidget : gui::Widget {
  int register_index;
  uint64_t reg_value;
  constexpr static Rect kBaseRect = Rect::MakeAtZero<CenterX, CenterY>(3_cm, 3_cm);
  constexpr static Rect kBoundingRect = []() {
    auto rect = kBaseRect;
    rect.top += 11_mm;   // space for the register icon
    rect.right += 1_cm;  // space for byte values
    return rect;
  }();
  constexpr static Rect kInnerRect = kBaseRect.Outset(-1_mm);
  constexpr static float kCellHeight = kInnerRect.Height() / 8;
  constexpr static float kCellWidth = kInnerRect.Width() / 8;

  RegisterWidget(int register_index) : register_index(register_index) {}
  std::string_view Name() const override;
  SkPath Shape() const override;
  void Draw(SkCanvas&) const override;
};

struct AssemblerWidget : gui::Widget {
  constexpr static float kWidth = 8_cm;
  constexpr static float kHeight = 8_cm;
  constexpr static float kRadius = 1_cm;
  constexpr static RRect kRRect =
      RRect::MakeSimple(Rect::MakeAtZero<CenterX, CenterY>(kWidth, kHeight), kRadius);

  std::weak_ptr<Assembler> assembler_weak;
  maf::Vec<std::shared_ptr<gui::Widget>> children;
  mc::Controller::State state;

  AssemblerWidget(std::weak_ptr<Assembler>);
  std::string_view Name() const override;
  void FillChildren(maf::Vec<std::shared_ptr<gui::Widget>>& children) override;
  SkPath Shape() const override;
  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  std::unique_ptr<Action> FindAction(gui::Pointer& p, gui::ActionTrigger btn) override;
  void TransformUpdated() override;
};

// Combines functions of Assembler and Thread.
// Assembler part takes care of emitting machine code to an executable memory region.
// Thread part maintains register state across executions.
struct Assembler : LiveObject, LongRunning {
  using PrologueFn = uintptr_t (*)(void*);

  std::shared_ptr<Object> Clone() const override;

  Assembler();
  ~Assembler();

  void ExitCallback(mc::CodePoint code_point);

  std::unique_ptr<mc::Controller> mc_controller;

  void UpdateMachineCode();

  void RunMachineCode(library::Instruction* entry_point);

  void Cancel() override;

  std::shared_ptr<gui::Widget> MakeWidget() override {
    return std::make_shared<AssemblerWidget>(WeakPtr());
  }
};

// Convenience function for updating the code with a vector of automat::library::Instruction.
//
// The main purpose is to allow Controller to store the instruction pointers without
// having to know about the automat::Instruction type. This is achieved using the _aliasing_
// feature of std::shared_ptr. The shared_ptr used by the controller points to mc_inst field of
// automat::Instruction, while owning the whole object.
void UpdateCode(mc::Controller& controller,
                std::vector<std::shared_ptr<Instruction>>&& instructions, maf::Status& status);

}  // namespace automat::library
