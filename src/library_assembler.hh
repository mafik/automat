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

  RegisterWidget(Widget& parent, WeakPtr<Object> register_weak) : FallbackWidget(parent) {
    object = std::move(register_weak);
  }
  Ptr<Register> LockRegister() const { return LockObject<Register>(); }
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

  WeakPtr<Assembler> assembler_weak;
  Vec<RegisterWidget*> reg_widgets;

  AssemblerWidget(Widget& parent, WeakPtr<Assembler>);
  std::string_view Name() const override;
  void FillChildren(Vec<gui::Widget*>& children) override;
  SkPath Shape() const override;
  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  void VisitOptions(const OptionsVisitor&) const override;
  void TransformUpdated() override;

  DropTarget* AsDropTarget() override { return this; }
  bool CanDrop(Location&) const override;
  void DropLocation(Ptr<Location>&&) override;
  void SnapPosition(Vec2& position, float& scale, Location&, Vec2* fixed_point = nullptr) override;
};

struct Register : LiveObject {
  WeakPtr<Assembler> assembler_weak;
  int register_index;

  Register(WeakPtr<Assembler> assembler_weak, int register_index);

  Ptr<Object> Clone() const override;

  unique_ptr<gui::Widget> MakeWidget(gui::Widget& parent) override {
    return make_unique<RegisterWidget>(parent, AcquireWeakPtr<Object>());
  }

  void Args(std::function<void(Argument&)> cb) override;
  void SetText(Location& error_context, std::string_view text) override;

  void ConnectionAdded(Location& here, Connection& connection) override;
  void ConnectionRemoved(Location& here, Connection& connection) override;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

// Combines functions of Assembler and Thread.
// Assembler part takes care of emitting machine code to an executable memory region.
// Thread part maintains register state across executions.
struct Assembler : LiveObject, LongRunning, Container {
  using PrologueFn = uintptr_t (*)(void*);

  Ptr<Object> Clone() const override;

  Assembler();
  ~Assembler();

  void ExitCallback(mc::CodePoint code_point);

  std::unique_ptr<mc::Controller> mc_controller;
  time::SteadyPoint last_state_refresh = {};
  mc::Controller::State state;
  std::array<SharedOrWeakPtr<Register>, kGeneralPurposeRegisterCount> reg_objects_idx;

  void UpdateMachineCode();

  void RunMachineCode(library::Instruction* entry_point);

  void OnCancel() override;

  unique_ptr<gui::Widget> MakeWidget(gui::Widget& parent) override {
    return make_unique<AssemblerWidget>(parent, AcquireWeakPtr());
  }

  Container* AsContainer() override { return this; }

  Ptr<Location> Extract(Object& descendant) override;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

// Convenience function for updating the code with a vector of automat::library::Instruction.
//
// The main purpose is to allow Controller to store the instruction pointers without
// having to know about the automat::Instruction type. This is achieved using the _aliasing_
// feature of Ptr. The Ptr used by the controller points to mc_inst field of
// automat::Instruction, while owning the whole object.
void UpdateCode(mc::Controller& controller, std::vector<Ptr<Instruction>>&& instructions,
                Status& status);

}  // namespace automat::library
