// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <llvm/MC/MCInst.h>

#include "base.hh"
#include "gui_small_buffer_widget.hh"
#include "machine_code.hh"
#include "textures.hh"

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

struct JumpArgument : Argument {
  JumpArgument();

  PaintDrawable& Icon() override;
};

extern Argument assembler_arg;
extern JumpArgument jump_arg;

struct Instruction : LiveObject, Runnable, Buffer {
  mc::Inst mc_inst;

  void Args(std::function<void(Argument&)> cb) override;
  Ptr<Object> ArgPrototype(const Argument&) override;

  void ConnectionAdded(Location& here, Connection&) override;
  void ConnectionRemoved(Location& here, Connection&) override;

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;

  LongRunning* AsLongRunning() override;

  void OnRun(Location& here, RunTask&) override;

  Buffer::Type imm_type = Buffer::Type::Unsigned;

  void BufferVisit(const BufferVisitor&) override;
  Type GetBufferType() override { return imm_type; };
  bool IsBufferTypeMutable() override { return true; }
  void SetBufferType(Type new_type) override { imm_type = new_type; }

  struct Widget : FallbackWidget {
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

    std::unique_ptr<gui::SmallBufferWidget> imm_widget;
    std::unique_ptr<gui::Widget> condition_code_widget;
    float scale = 1;
    llvm::SmallVector<Vec2, 10> token_position;
    llvm::SmallVector<float, 10> string_width_scale;
    struct Token;

    std::span<const Token> tokens;

    Widget(gui::Widget& parent, WeakPtr<Object> object);

    std::string_view Name() const override { return "Instruction Widget"; }
    SkPath Shape() const override;
    animation::Phase Tick(time::Timer&) override;
    void Draw(SkCanvas&) const override;
    Vec2AndDir ArgStart(const Argument&) override;
    void FillChildren(Vec<gui::Widget*>& children) override;
  };

  Str ToAsmStr() const;

  NestedWeakPtr<const mc::Inst> ToMC() {
    return NestedWeakPtr<const mc::Inst>(AcquireWeakPtr<ReferenceCounted>(), &mc_inst);
  }

  std::unique_ptr<gui::Widget> MakeWidget(gui::Widget& parent) override {
    return std::make_unique<Widget>(parent, AcquireWeakPtr<Object>());
  }

  void SerializeState(Serializer& writer, const char* key = "value") const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library
