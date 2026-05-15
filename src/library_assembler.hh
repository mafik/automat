// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>

#include "animation.hh"
#include "library_instruction.hh"
#include "machine_code.hh"
#include "object.hh"
#include "shared_or_weak.hh"
#include "status.hh"
#include "ui_small_buffer_widget.hh"

namespace automat::library {

constexpr size_t kMachineCodeSize = 1024 * 4;

struct DeleteWithMunmap {
  void operator()(void* ptr) const;
};

struct Regs;
struct Register;
struct Assembler;
struct AssemblerWidget;
struct RegisterIndexKnobWidget;

struct RegisterWidget : public ObjectToy {
  constexpr static Rect kBaseRect = Rect::MakeAtZero<CenterX, CenterY>(3_cm, 3_cm);
  constexpr static Rect kBoundingRect = [] {
    auto rect = kBaseRect;
    rect.top += 11_mm;   // space for the register icon
    rect.right += 1_cm;  // space for byte values
    return rect;
  }();
  constexpr static float kCellHeight = kBaseRect.Height() / 8;
  constexpr static float kCellWidth = kBaseRect.Width() / 8;
  constexpr static float kHexMargin = 0.5_mm;
  constexpr static float kBendR = kCellWidth / 4;

  ui::SmallBufferWidget small_buffer_widget;
  std::unique_ptr<RegisterIndexKnobWidget> register_index_knob;

  animation::SpringV2<float> wrap_position_offset;
  animation::SpringV2<float> size_offset;
  mutable SkPath cached_shape;

  RegisterWidget(Widget* parent, Object& reg);
  ~RegisterWidget();
  Ptr<Register> LockRegister() const { return LockObject<Register>(); }
  std::string_view Name() const override;
  RRect MarbleShape() const;
  RRect SpearShaft() const;
  SkPath SpearTip() const;
  SkPath Shape() const override;
  Rect Checkerboard() const;
  SkPath FlagFront() const;
  SkPath FlagBack() const;
  float HexWidth() const;
  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  void VisitOptions(const OptionsVisitor&) const override;

  void FillChildren(Vec<Widget*>& children) override;
};

struct AssemblerWidget : ObjectToy, ui::DropTarget {
  constexpr static float kWidth = 8_cm;
  constexpr static float kHeight = 8_cm;
  constexpr static float kRadius = 1_cm;
  constexpr static RRect kRRect =
      RRect::MakeSimple(Rect::MakeAtZero<CenterX, CenterY>(kWidth, kHeight), kRadius);

  Vec<RegisterWidget*> reg_widgets;

  AssemblerWidget(Widget* parent, Assembler&);
  std::string_view Name() const override;
  void FillChildren(Vec<ui::Widget*>& children) override;
  SkPath Shape() const override;
  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  void VisitOptions(const OptionsVisitor&) const override;
  void TransformUpdated() override;
  void OnChildReparentedAway(ui::Widget& child) override;

  DropTarget* AsDropTarget() override { return this; }
  bool CanDrop(Location&) const override;
  void DropLocation(Ptr<Location>&&) override;
  SkMatrix DropSnap(const Rect& bounds, Vec2 bounds_origin, Vec2* fixed_point = nullptr) override;
};

struct Register : Object, Buffer {
  using Toy = RegisterWidget;
  int register_index;  // index into kRegisters
  int wrap_position = 8;
  int size = 64;
  Buffer::Type type = Buffer::Type::Hexadecimal;

  DEF_INTERFACE(Register, ObjectArgument<Assembler>, assembler_arg, "Reg's Assembler")
  static constexpr auto kStyle = Argument::Style::Spotlight;
  static constexpr float kAutoconnectRadius = INFINITY;
  static constexpr SkColor4f kTint = "#ff0000"_color4f;
  DEF_END(assembler_arg);

  Register(WeakPtr<Assembler> assembler_weak, int register_index);

  Ptr<Object> Clone() const override;

  unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override {
    return make_unique<RegisterWidget>(parent, *this);
  }

  void BufferVisit(const BufferVisitor&) override;
  Type GetBufferType() override { return type; };
  bool IsBufferTypeMutable() override { return true; }
  void SetBufferType(Type new_type) override {
    type = new_type;
    WakeToys();
  }

  INTERFACES(assembler_arg)
  void SetText(std::string_view text) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

// Combines functions of Assembler and Thread.
// Assembler part takes care of emitting machine code to an executable memory region.
// Thread part maintains register state across executions.
struct Assembler : Object, Container {
  using PrologueFn = uintptr_t (*)(void*);
  using Toy = AssemblerWidget;

  DEF_INTERFACE(Assembler, LongRunning, running, "Running")
  void OnCancel() {
    Status status;
    obj->mc_controller->Cancel(status);
    if (!OK(status)) {
      ERROR << "Failed to cancel Assembler: " << status;
    }
  }
  DEF_END(running);

  Ptr<Object> Clone() const override;
  INTERFACES(running)

  Assembler();

  void ExitCallback(mc::CodePoint code_point);

  std::unique_ptr<mc::Controller> mc_controller;
  time::SteadyPoint last_state_refresh = {};
  mc::Controller::State state;
  std::array<SharedOrWeakPtr<Register>, kGeneralPurposeRegisterCount> reg_objects_idx;
  std::vector<WeakPtr<Instruction>> instructions_weak;

  void UpdateMachineCode();

  void RunMachineCode(library::Instruction* entry_point, std::unique_ptr<RunTask>&&);

  unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override {
    return make_unique<AssemblerWidget>(parent, *this);
  }

  Container* AsContainer() override { return this; }

  Ptr<Location> Extract(Object& descendant) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
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
