// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>

#include "library_instruction.hh"
#include "object.hh"
#include "status.hh"

namespace automat::library {

constexpr size_t kMachineCodeSize = 1024 * 4;

struct DeleteWithMunmap {
  void operator()(void* ptr) const;
};

struct Regs;

struct RegisterWidget : gui::Widget {
  std::weak_ptr<Object> object;  // assembler
  int register_index;
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

  RegisterWidget(std::weak_ptr<Object> object, int register_index)
      : object(object), register_index(register_index) {}
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

  std::weak_ptr<Object> object;
  maf::Vec<std::shared_ptr<gui::Widget>> children;

  AssemblerWidget(std::weak_ptr<Object> object);
  std::string_view Name() const override;
  void FillChildren(maf::Vec<std::shared_ptr<gui::Widget>>& children) override;
  SkPath Shape() const override;
  void Draw(SkCanvas&) const override;
  std::unique_ptr<Action> FindAction(gui::Pointer& p, gui::ActionTrigger btn) override;
  void TransformUpdated() override;
};

// Combines functions of Assembler and Thread.
// Assembler part takes care of emitting machine code to an executable memory region.
// Thread part maintains register state across executions.
struct Assembler : LiveObject {
  using PrologueFn = uintptr_t (*)(void*);

  std::unique_ptr<char, DeleteWithMunmap> machine_code;
  std::unique_ptr<Regs> regs;
  PrologueFn prologue_fn = nullptr;
#if defined __linux__

  std::atomic<bool> is_running = false;
  pid_t pid = 0;  // int actually

#endif  // __linux__

  std::shared_ptr<Object> Clone() const override;

  Assembler(maf::Status&);
  ~Assembler();

  void UpdateMachineCode();

  void RunMachineCode(library::Instruction* entry_point);

  std::shared_ptr<gui::Widget> MakeWidget() override {
    return std::make_shared<AssemblerWidget>(WeakPtr());
  }
};

}  // namespace automat::library
