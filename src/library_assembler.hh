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
#include "shared_or_weak.hh"
#include "status.hh"

namespace automat::library {

constexpr size_t kMachineCodeSize = 1024 * 4;

struct DeleteWithMunmap {
  void operator()(void* ptr) const;
};

struct Regs;
struct Register;
struct Assembler;
struct AssemblerWidget;

struct RegisterWidget : public Object::FallbackWidget {
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

  RegisterWidget(std::weak_ptr<Object> register_weak) { object = std::move(register_weak); }
  std::shared_ptr<Register> LockRegister() const { return LockObject<Register>(); }
  std::string_view Name() const override;
  SkPath Shape() const override;
  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  void VisitOptions(const OptionsVisitor&) const override;
};

struct AssemblerWidget : Object::FallbackWidget, gui::DropTarget {
  constexpr static float kWidth = 8_cm;
  constexpr static float kHeight = 8_cm;
  constexpr static float kRadius = 1_cm;
  constexpr static RRect kRRect =
      RRect::MakeSimple(Rect::MakeAtZero<CenterX, CenterY>(kWidth, kHeight), kRadius);

  std::weak_ptr<Assembler> assembler_weak;
  maf::Vec<std::shared_ptr<RegisterWidget>> reg_widgets;

  AssemblerWidget(std::weak_ptr<Assembler>);
  std::string_view Name() const override;
  void FillChildren(maf::Vec<std::shared_ptr<gui::Widget>>& children) override;
  SkPath Shape() const override;
  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  void VisitOptions(const OptionsVisitor&) const override;
  void TransformUpdated() override;

  DropTarget* AsDropTarget() override { return this; }
  bool CanDrop(Location&) const override;
  void DropLocation(std::shared_ptr<Location>&&) override;
  void SnapPosition(Vec2& position, float& scale, Location&, Vec2* fixed_point = nullptr) override;
};

struct Register : LiveObject {
  std::weak_ptr<Assembler> assembler_weak;
  int register_index;

  Register(std::weak_ptr<Assembler> assembler_weak, int register_index);

  std::shared_ptr<Object> Clone() const override;

  std::shared_ptr<gui::Widget> MakeWidget() override {
    return std::make_shared<RegisterWidget>(WeakPtr());
  }
};

// Combines functions of Assembler and Thread.
// Assembler part takes care of emitting machine code to an executable memory region.
// Thread part maintains register state across executions.
struct Assembler : LiveObject, LongRunning, Container {
  using PrologueFn = uintptr_t (*)(void*);

  std::shared_ptr<Object> Clone() const override;

  Assembler();
  ~Assembler();

  void ExitCallback(mc::CodePoint code_point);

  std::mutex mutex;
  std::unique_ptr<mc::Controller> mc_controller;
  time::T last_state_refresh = 0;
  mc::Controller::State state;
  std::array<SharedOrWeakPtr<Register>, kGeneralPurposeRegisterCount> reg_objects_idx;

  void UpdateMachineCode();

  void RunMachineCode(library::Instruction* entry_point);

  void Cancel() override;

  std::shared_ptr<gui::Widget> MakeWidget() override {
    return std::make_shared<AssemblerWidget>(WeakPtr());
  }

  Container* AsContainer() override { return this; }

  std::shared_ptr<Location> Extract(Object& descendant) override;
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
