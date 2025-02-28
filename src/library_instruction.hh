// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <llvm/MC/MCInst.h>

#include "base.hh"
#include "machine_code.hh"
#include "textures.hh"

namespace automat::library {

constexpr float kRegisterIconWidth = 12_mm;

struct RegisterPresentation {
  PersistentImage image;
  unsigned llvm_reg;
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

struct Instruction : LiveObject, Runnable {
  mc::Inst mc_inst;

  void Args(std::function<void(Argument&)> cb) override;
  std::shared_ptr<Object> ArgPrototype(const Argument&) override;

  void ConnectionAdded(Location& here, Connection&) override;
  void ConnectionRemoved(Location& here, Connection&) override;

  std::string_view Name() const override;
  std::shared_ptr<Object> Clone() const override;

  void OnRun(Location& here) override;

  struct Widget : gui::Widget {
    constexpr static float kWidth = 63.5_mm;
    constexpr static float kHeight = 44.5_mm;
    constexpr static float kBorderMargin = 4_mm;
    constexpr static float kDiagonal = Sqrt(kWidth * kWidth + kHeight * kHeight);
    constexpr static Rect kRect =
        Rect::MakeAtZero<LeftX, BottomY>(Instruction::Widget::kWidth, Instruction::Widget::kHeight);

    std::weak_ptr<Object> object;
    Widget(std::weak_ptr<Object> object);

    std::string_view Name() const override { return "Instruction Widget"; }
    SkPath Shape() const override;
    void Draw(SkCanvas&) const override;
    std::unique_ptr<Action> FindAction(gui::Pointer& p, gui::ActionTrigger btn) override;
    Vec2AndDir ArgStart(const Argument&) override;
  };

  static void DrawInstruction(SkCanvas& canvas, const mc::Inst& inst);

  maf::Str ToAsmStr() const;

  std::weak_ptr<mc::Inst> ToMC() { return std::shared_ptr<mc::Inst>(SharedPtr(), &mc_inst); }

  std::shared_ptr<gui::Widget> MakeWidget() override { return std::make_shared<Widget>(WeakPtr()); }

  void SerializeState(Serializer& writer, const char* key = "value") const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library
