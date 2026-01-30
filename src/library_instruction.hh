// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <llvm/MC/MCInst.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#include "base.hh"
#include "machine_code.hh"
#include "textures.hh"
#include "ui_small_buffer_widget.hh"

namespace automat::library {

constexpr float kRegisterIconWidth = 12_mm;

struct RegisterPresentation {
  PersistentImage image;
  unsigned llvm_reg;
  std::string name;
};

constexpr static int kGeneralPurposeRegisterCount = 6;

extern RegisterPresentation kRegisters[kGeneralPurposeRegisterCount];

enum class Flag {
  CF,
  OF,
  PF,
  ZF,
  SF,
};

struct AssemblerArgument : Argument {
  StrView Name() const override { return "assembler"sv; }
  float AutoconnectRadius() const override { return INFINITY; }
  SkColor Tint() const override { return "#ff0000"_color; }
  Style GetStyle() const override { return Style::Invisible; }
  Ptr<Object> Prototype() const override;
  void CanConnect(Object& start, Part& end, Status& status) const override;
  void Connect(Object& start, const NestedPtr<Part>& end) override;
  NestedPtr<Part> Find(const Object& start) const override;
};

struct JumpArgument : Argument {
  JumpArgument();

  StrView Name() const override { return "jump"sv; }
  PaintDrawable& Icon() override;
  void CanConnect(Object& start, Part& end, Status& status) const override;
  void Connect(Object& start, const NestedPtr<Part>& end) override;
  NestedPtr<Part> Find(const Object& start) const override;
};

// Same as NextArg - but calls UpdateMachineCode when it's reconnected
struct NextInstructionArg : NextArg {
  void Connect(Object& start, const NestedPtr<Part>& end) override;
};

extern AssemblerArgument assembler_arg;
extern JumpArgument jump_arg;
extern NextInstructionArg next_instruction_arg;

struct Instruction : Object, Runnable, Buffer {
  mc::Inst mc_inst;
  NestedWeakPtr<Runnable> jump_target;  // Connection target for jump_arg
  NestedWeakPtr<Object> assembler_weak;

  void Parts(const std::function<void(Part&)>& cb) override;

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;

  LongRunning* AsLongRunning() override;

  void OnRun(std::unique_ptr<RunTask>&) override;

  Buffer::Type imm_type = Buffer::Type::Unsigned;

  void BufferVisit(const BufferVisitor&) override;
  Type GetBufferType() override { return imm_type; };
  bool IsBufferTypeMutable() override { return true; }
  void SetBufferType(Type new_type) override { imm_type = new_type; }

  struct Widget : WidgetBase {
    constexpr static float kWidth = 63.5_mm;
    constexpr static float kHeight = 44.5_mm;
    constexpr static float kBorderMargin = 4_mm;
    constexpr static float kDiagonal = Sqrt(kWidth * kWidth + kHeight * kHeight);
    constexpr static Rect kRect =
        Rect::MakeAtZero<LeftX, BottomY>(Instruction::Widget::kWidth, Instruction::Widget::kHeight);

    constexpr static float kRegisterTokenWidth = kRegisterIconWidth;
    constexpr static float kRegisterIconScale = kRegisterTokenWidth / kRegisterIconWidth;
    constexpr static float kFixedFlagWidth = 6_mm;
    constexpr static float kMinTextScale = 0.9;
    constexpr static float kMaxTextScale = 1.1;

    constexpr static float kXMin =
        Instruction::Widget::kRect.left + Instruction::Widget::kBorderMargin * 2;
    constexpr static float kXMax =
        Instruction::Widget::kRect.right - Instruction::Widget::kBorderMargin * 2;
    constexpr static float kXRange = kXMax - kXMin;
    constexpr static float kXCenter = (kXMax + kXMin) / 2;
    constexpr static float kYMax =
        Instruction::Widget::kRect.top - Instruction::Widget::kBorderMargin * 2;
    constexpr static float kYMin =
        Instruction::Widget::kRect.bottom + Instruction::Widget::kBorderMargin * 2;
    constexpr static float kYRange = kYMax - kYMin;
    constexpr static float kYCenter = (kYMax + kYMin) / 2;
    constexpr static float kLineHeight = 11_mm;

    std::unique_ptr<ui::SmallBufferWidget> imm_widget;
    std::unique_ptr<ui::Widget> condition_code_widget;
    float scale = 1;
    llvm::SmallVector<Vec2, 10> token_position;
    llvm::SmallVector<float, 10> string_width_scale;

    struct Token {
      enum Tag : uint8_t {
        String,
        RegisterOperand,
        ImmediateOperand,
        FixedRegister,
        FixedFlag,
        ConditionCode,
        FixedCondition,
        BreakLine,
      } tag;
      union {
        const char* str;
        unsigned reg;
        unsigned imm;
        unsigned fixed_reg;
        Flag flag;
        unsigned cond_code;  // token_i of the condition immediate
        llvm::X86::CondCode fixed_cond;
      };
    };

    std::span<const Token> tokens;

    Widget(ui::Widget* parent, WeakPtr<Object> object);

    std::string_view Name() const override { return "Instruction Widget"; }
    SkPath Shape() const override;
    animation::Phase Tick(time::Timer&) override;
    void Draw(SkCanvas&) const override;
    Vec2AndDir ArgStart(const Argument&, ui::Widget* coordinate_space = nullptr) override;
    void FillChildren(Vec<ui::Widget*>& children) override;
  };

  Str ToAsmStr() const;

  NestedWeakPtr<const mc::Inst> ToMC() {
    return NestedWeakPtr<const mc::Inst>(AcquireWeakPtr<ReferenceCounted>(), &mc_inst);
  }

  std::unique_ptr<ObjectWidget> MakeWidget(ui::Widget* parent,
                                           WeakPtr<ReferenceCounted> object) override {
    return std::make_unique<Widget>(parent, std::move(object).Cast<Object>());
  }

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
