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

struct Instruction : LiveObject, Runnable {
  mc::Inst mc_inst;

  void Args(std::function<void(Argument&)> cb) override;
  Ptr<Object> ArgPrototype(const Argument&) override;

  void ConnectionAdded(Location& here, Connection&) override;
  void ConnectionRemoved(Location& here, Connection&) override;

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;

  void OnRun(Location& here) override;

  struct Widget : FallbackWidget {
    constexpr static float kWidth = 63.5_mm;
    constexpr static float kHeight = 44.5_mm;
    constexpr static float kBorderMargin = 4_mm;
    constexpr static float kDiagonal = Sqrt(kWidth * kWidth + kHeight * kHeight);
    constexpr static Rect kRect =
        Rect::MakeAtZero<LeftX, BottomY>(Instruction::Widget::kWidth, Instruction::Widget::kHeight);

    std::optional<llvm::MCInst> mc_inst = std::nullopt;

    Widget(WeakPtr<Object> object);
    Widget(const llvm::MCInst&);

    std::string_view Name() const override { return "Instruction Widget"; }
    SkPath Shape() const override;
    void Draw(SkCanvas&) const override;
    Vec2AndDir ArgStart(const Argument&) override;
  };

  maf::Str ToAsmStr() const;

  NestedWeakPtr<const mc::Inst> ToMC() {
    return NestedWeakPtr<const mc::Inst>(AcquireWeakPtr<ReferenceCounted>(), &mc_inst);
  }

  Ptr<gui::Widget> MakeWidget() override { return MakePtr<Widget>(AcquireWeakPtr<Object>()); }

  void SerializeState(Serializer& writer, const char* key = "value") const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library
