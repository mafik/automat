// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_instruction.hh"

#include <include/core/SkBlendMode.h>
#include <include/core/SkBlurTypes.h>
#include <include/core/SkColor.h>
#include <include/core/SkFontTypes.h>
#include <include/core/SkImage.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkShader.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkBlenders.h>
#include <include/effects/SkGradientShader.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/MC/MCInstBuilder.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#include <cmath>
#include <memory>
#include <tracy/Tracy.hpp>

#include "animation.hh"
#include "argument.hh"
#include "automat.hh"
#include "color.hh"
#include "drawable.hh"
#include "embedded.hh"
#include "font.hh"
#include "hex.hh"
#include "knob.hh"
#include "library_assembler.hh"
#include "llvm_asm.hh"
#include "math.hh"
#include "ptr.hh"
#include "svg.hh"
#include "textures.hh"
#include "time.hh"
#include "wave1d.hh"
#include "widget.hh"

using namespace std;
using namespace llvm;
using namespace automat;

namespace automat::library {

RegisterPresentation kRegisters[kGeneralPurposeRegisterCount] = {
    RegisterPresentation{
        .image = PersistentImage::MakeFromAsset(
            embedded::assets_reg_ax_webp, PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
        .llvm_reg = X86::RAX,
        .name = "RAX",
    },
    RegisterPresentation{
        .image = PersistentImage::MakeFromAsset(
            embedded::assets_reg_bx_webp, PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
        .llvm_reg = X86::RBX,
        .name = "RBX",
    },
    RegisterPresentation{
        .image = PersistentImage::MakeFromAsset(
            embedded::assets_reg_cx_webp, PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
        .llvm_reg = X86::RCX,
        .name = "RCX",
    },
    RegisterPresentation{
        .image = PersistentImage::MakeFromAsset(
            embedded::assets_reg_dx_webp, PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
        .llvm_reg = X86::RDX,
        .name = "RDX",
    },
    RegisterPresentation{
        .image = PersistentImage::MakeFromAsset(
            embedded::assets_reg_si_webp, PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
        .llvm_reg = X86::RSI,
        .name = "RSI",
    },
    RegisterPresentation{
        .image = PersistentImage::MakeFromAsset(
            embedded::assets_reg_di_webp, PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
        .llvm_reg = X86::RDI,
        .name = "RDI",
    },
};

static const SkPath kJumpPath = PathFromSVG(
    "m.7-2.7a.5.5 0 000 1 .5.5 0 000-1m-2.6 2a.1.1 0 01-.1-.5l1.2-.3 1.4 0 1.1.6 1 0a.1.1 0 010 "
    ".5l-1.1 0-.7-.3-.5 1.1 1.3-0 .9 1a.1.1 0 01-.4.4l-.7-.8-1.8 0-.9.9-1.1 0a.1.1 0 010-.6l.8 0 "
    ".9-1 .5-1.3-.7-0z",
    SVGUnit_Millimeters);

struct JumpDrawable : PaintDrawable {
  void onDraw(SkCanvas* canvas) override { canvas->drawPath(kJumpPath, paint); }
};

static JumpDrawable jump_icon;

JumpArgument::JumpArgument() {
  // Jump argument configuration
}

PaintDrawable& JumpArgument::Icon() { return jump_icon; }

void JumpArgument::CanConnect(Object& start, Atom& end, Status& status) const {
  if (!dynamic_cast<Runnable*>(&end)) {
    AppendErrorMessage(status) += "Jump target must be a Runnable";
  }
}

void JumpArgument::OnConnect(Object& start, const NestedPtr<Atom>& end) {
  if (auto* inst = dynamic_cast<Instruction*>(&start)) {
    if (end) {
      if (auto* runnable = dynamic_cast<Runnable*>(end.Get())) {
        inst->jump_target = NestedWeakPtr<Runnable>(end.GetOwnerWeak(), runnable);
      }
    } else {
      inst->jump_target = {};
    }
    // Notify assembler of change
    if (auto* assembler = dynamic_cast<Assembler*>(assembler_arg.ObjectOrNull(*inst))) {
      assembler->UpdateMachineCode();
    }
  }
}

NestedPtr<Atom> JumpArgument::Find(const Object& start) const {
  if (auto* inst = dynamic_cast<const Instruction*>(&start)) {
    if (auto locked = inst->jump_target.Lock()) {
      return NestedPtr<Syncable>(locked.GetOwnerWeak().Lock(), locked.Get());
    }
  }
  return {};
}

JumpArgument jump_arg;

void NextInstructionArg::OnConnect(Object& start, const NestedPtr<Atom>& end) {
  NextArg::OnConnect(start, end);
  if (auto* inst = dynamic_cast<Instruction*>(&start)) {
    // Notify assembler of change
    if (auto* assembler = dynamic_cast<Assembler*>(assembler_arg.ObjectOrNull(*inst))) {
      assembler->UpdateMachineCode();
    }
  }
}

NextInstructionArg next_instruction_arg;

void AssemblerArgument::CanConnect(Object& start, Atom& end, Status& status) const {
  if (!dynamic_cast<Assembler*>(&end)) {
    AppendErrorMessage(status) += "Must connect to an Assembler";
  }
}

void AssemblerArgument::OnConnect(Object& start, const NestedPtr<Atom>& end) {
  auto* instruction = dynamic_cast<Instruction*>(&start);
  if (instruction == nullptr) return;

  if (auto old_assembler_obj = instruction->assembler_weak.Lock()) {
    if (auto old_assembler = dynamic_cast<Assembler*>(old_assembler_obj.Get())) {
      for (int i = 0; i < old_assembler->instructions_weak.size(); ++i) {
        auto& old_instr = old_assembler->instructions_weak[i];
        if (old_instr.GetUnsafe() == instruction) {
          old_assembler->instructions_weak.erase(old_assembler->instructions_weak.begin() + i);
          break;
        }
      }
    }
  }

  auto* assembler = dynamic_cast<Assembler*>(end.Get());
  if (assembler == nullptr) {
    instruction->assembler_weak.Reset();
  } else {
    instruction->assembler_weak = NestedPtr<Object>(assembler->AcquirePtr());
    assembler->instructions_weak.emplace_back(instruction->AcquirePtr());
    assembler->UpdateMachineCode();
  }
}

NestedPtr<Atom> AssemblerArgument::Find(const Object& start) const {
  if (auto* instruction = dynamic_cast<const Instruction*>(&start)) {
    return instruction->assembler_weak.Lock();
  }
  return NestedPtr<Atom>();
}

Ptr<Object> AssemblerArgument::Prototype() const { return MAKE_PTR(Assembler); }

AssemblerArgument assembler_arg;

static Assembler* FindAssembler(Object& start) {
  return dynamic_cast<Assembler*>(assembler_arg.ObjectOrNull(start));
}

static Assembler* FindOrCreateAssembler(Object& start) {
  return dynamic_cast<Assembler*>(&assembler_arg.ObjectOrMake(start));
}

void Instruction::Atoms(const std::function<LoopControl(Atom&)>& cb) {
  auto opcode = mc_inst.getOpcode();
  if (opcode != X86::JMP_1 && opcode != X86::JMP_4) {
    if (LoopControl::Break == cb(next_instruction_arg)) return;
  }
  if (LoopControl::Break == cb(assembler_arg)) return;
  auto& assembler = LLVM_Assembler::Get();
  auto& info = assembler.mc_instr_info->get(opcode);
  if (info.isBranch()) {
    if (LoopControl::Break == cb(jump_arg)) return;
  }
}

string_view Instruction::Name() const { return "Instruction"; }
Ptr<Object> Instruction::Clone() const { return MAKE_PTR(Instruction, *this); }

void Instruction::BufferVisit(const BufferVisitor& visitor) {
  unsigned n = mc_inst.getNumOperands();
  for (unsigned i = 0; i < n; ++i) {
    auto& operand = mc_inst.getOperand(i);
    if (operand.isImm()) {
      int64_t imm = operand.getImm();
      size_t size = mc::ImmediateSize(mc_inst);
      span<char> span = {reinterpret_cast<char*>(&imm), size};
      bool changed = visitor(span);
      if (changed) {
        operand.setImm(imm);
        if (auto assembler = FindAssembler(*this)) {
          assembler->UpdateMachineCode();
        }
      }
      return;
    }
  }
  visitor(span<char>{});
}

void Instruction::MyRunnable::OnRun(std::unique_ptr<RunTask>& run_task) {
  auto& instr = Instruction();
  ZoneScopedN("Instruction");
  auto assembler = FindOrCreateAssembler(instr);
  assembler->RunMachineCode(&instr, std::move(run_task));
}

LongRunning* Instruction::AsLongRunning() {
  if (auto* as = FindAssembler(*this)) {
    return &as->running;
  }
  return nullptr;
}

static std::string AssemblyText(const mc::Inst& mc_inst) {
  // Nicely formatted assembly:
  std::string str;
  raw_string_ostream os(str);

  auto& llvm_asm = LLVM_Assembler::Get();
  auto& mc_inst_printer = llvm_asm.mc_inst_printer;
  auto& mc_subtarget_info = llvm_asm.mc_subtarget_info;

  mc_inst_printer->printInst(&mc_inst, 0, "", *mc_subtarget_info, os);
  os.flush();
  for (auto& c : str) {
    if (c == '\t') {
      c = ' ';
    }
  }
  if (str.starts_with(' ')) {
    str.erase(0, 1);
  }
  return str;
}

Str Instruction::ToAsmStr() const { return AssemblyText(mc_inst); }

static std::string MachineText(const mc::Inst& mc_inst) {
  auto& llvm_asm = LLVM_Assembler::Get();
  SmallVector<char, 128> buffer;
  SmallVector<MCFixup, 1> fixups;
  llvm_asm.mc_code_emitter->encodeInstruction(mc_inst, buffer, fixups, *llvm_asm.mc_subtarget_info);
  return BytesToHex(buffer);
}

constexpr float kFineFontSize = 2_mm;

static ui::Font& FineFont() {
  static auto font = ui::Font::MakeV2(ui::Font::GetGrenzeThin(), kFineFontSize);
  return *font;
}

constexpr float kHeavyFontSize = 4_mm;

static ui::Font& HeavyFont() {
  static auto font = ui::Font::MakeV2(ui::Font::GetGrenzeSemiBold(), kHeavyFontSize);
  return *font;
}

constexpr float kSubscriptFontSize = 2_mm;

static ui::Font& SubscriptFont() {
  static auto font = ui::Font::MakeV2(ui::Font::GetGrenzeSemiBold(), kSubscriptFontSize);
  return *font;
}

static const SkRRect kInstructionRRect =
    SkRRect::MakeRectXY(Instruction::Widget::kRect.sk, 3_mm, 3_mm);

static const SkPath kInstructionShape = SkPath::RRect(kInstructionRRect);

SkPath Instruction::Widget::Shape() const { return kInstructionShape; }

PersistentImage paper_texture = PersistentImage::MakeFromAsset(
    embedded::assets_04_paper_C_grain_webp,
    PersistentImage::MakeArgs{.tile_x = SkTileMode::kRepeat, .tile_y = SkTileMode::kRepeat});

using Token = Instruction::Widget::Token;

std::span<const Token> PrintInstruction(const mc::Inst& inst) {
  switch (inst.getOpcode()) {
    case X86::JMP_1:
    case X86::JMP_2:
    case X86::JMP_4: {
      constexpr static Token tokens[] = {{.tag = Token::String, .str = "Jump"}};
      return tokens;
    }

    case X86::XOR64i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "xor "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::XOR32i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "xor "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::XOR16i16: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "xor "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::XOR8i8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "xor "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }

    case X86::XOR64ri32:
    case X86::XOR64ri8:
    case X86::XOR32ri:
    case X86::XOR32ri8:
    case X86::XOR16ri8:
    case X86::XOR16ri:
    case X86::XOR8ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},       {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},        {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1},  {.tag = Token::String, .str = "xor "},
          {.tag = Token::ImmediateOperand, .imm = 2},
      };
      return tokens;
    }

    case X86::XOR8rr_NOREX:
    case X86::XOR8rr_REV:
    case X86::XOR8rr:
    case X86::XOR64rr_REV:
    case X86::XOR64rr:
    case X86::XOR32rr_REV:
    case X86::XOR32rr:
    case X86::XOR16rr_REV:
    case X86::XOR16rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},      {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},       {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1}, {.tag = Token::String, .str = "xor"},
          {.tag = Token::RegisterOperand, .reg = 2},
      };
      return tokens;
    }

    case X86::NOT8r:
    case X86::NOT16r:
    case X86::NOT32r:
    case X86::NOT64r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Flip"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }
    case X86::ANDN64rr:
    case X86::ANDN32rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},  {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "¬("},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "and"}, {.tag = Token::RegisterOperand, .reg = 2},
          {.tag = Token::String, .str = ")"},
      };
      return tokens;
    }
    case X86::AND64rr_REV:
    case X86::AND64rr:
    case X86::AND32rr_REV:
    case X86::AND32rr:
    case X86::AND16rr_REV:
    case X86::AND16rr:
    case X86::AND8rr:
    case X86::AND8rr_REV: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},      {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},       {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1}, {.tag = Token::String, .str = "and"},
          {.tag = Token::RegisterOperand, .reg = 2},
      };
      return tokens;
    }
    case X86::AND8i8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "and "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::AND16i16: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "and "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::AND32i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "and "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::AND64i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "and "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }

    case X86::AND64ri8:
    case X86::AND32ri8:
    case X86::AND16ri8:
    case X86::AND8ri:
    case X86::AND64ri32:
    case X86::AND32ri:
    case X86::AND16ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},       {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},        {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1},  {.tag = Token::String, .str = "and "},
          {.tag = Token::ImmediateOperand, .imm = 2},
      };
      return tokens;
    }

    case X86::OR8i8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "or "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::OR16i16: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "or "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::OR32i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "or "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::OR64i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "or "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }

    case X86::OR64ri32:
    case X86::OR64ri8:
    case X86::OR32ri:
    case X86::OR32ri8:
    case X86::OR16ri:
    case X86::OR16ri8:
    case X86::OR8ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},       {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},        {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1},  {.tag = Token::String, .str = "or "},
          {.tag = Token::ImmediateOperand, .imm = 2},
      };
      return tokens;
    }

    case X86::OR64rr:
    case X86::OR64rr_REV:
    case X86::OR32rr:
    case X86::OR32rr_REV:
    case X86::OR16rr:
    case X86::OR16rr_REV:
    case X86::OR8rr:
    case X86::OR8rr_REV: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},      {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},       {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1}, {.tag = Token::String, .str = "or"},
          {.tag = Token::RegisterOperand, .reg = 2},
      };
      return tokens;
    }

    case X86::INC8r:
    case X86::INC64r:
    case X86::INC32r:
    case X86::INC16r: {
      constexpr static Token tokens[] = {
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "+1"},
      };
      return tokens;
    }

    case X86::DEC8r:
    case X86::DEC64r:
    case X86::DEC32r:
    case X86::DEC16r: {
      constexpr static Token tokens[] = {
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "-1"},
      };
      return tokens;
    }

    case X86::ADC64i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "+ "},
          {.tag = Token::ImmediateOperand, .imm = 0},
          {.tag = Token::String, .str = " +"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }
    case X86::ADC32i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "+ "},
          {.tag = Token::ImmediateOperand, .imm = 0},
          {.tag = Token::String, .str = " +"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }
    case X86::ADC16i16: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "+ "},
          {.tag = Token::ImmediateOperand, .imm = 0},
          {.tag = Token::String, .str = " +"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }
    case X86::ADC8i8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "+ "},
          {.tag = Token::ImmediateOperand, .imm = 0},
          {.tag = Token::String, .str = " +"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }

    case X86::ADC64ri32:
    case X86::ADC64ri8:
    case X86::ADC32ri8:
    case X86::ADC32ri:
    case X86::ADC16ri:
    case X86::ADC16ri8:
    case X86::ADC8ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},        {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},         {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1},   {.tag = Token::String, .str = "+ "},
          {.tag = Token::ImmediateOperand, .imm = 2},  {.tag = Token::String, .str = " +"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }

    case X86::ADC64rr:
    case X86::ADC64rr_REV:
    case X86::ADC32rr:
    case X86::ADC32rr_REV:
    case X86::ADC16rr:
    case X86::ADC16rr_REV:
    case X86::ADC8rr:
    case X86::ADC8rr_REV: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},        {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},         {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1},   {.tag = Token::String, .str = "+"},
          {.tag = Token::RegisterOperand, .reg = 2},   {.tag = Token::String, .str = "+"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }

    case X86::ADD64i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "+ "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::ADD32i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "+ "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::ADD16i16: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "+ "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::ADD8i8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "+ "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }

    case X86::ADD64ri32:
    case X86::ADD64ri8:
    case X86::ADD32ri:
    case X86::ADD32ri8:
    case X86::ADD16ri:
    case X86::ADD16ri8:
    case X86::ADD8ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},       {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},        {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1},  {.tag = Token::String, .str = "+ "},
          {.tag = Token::ImmediateOperand, .imm = 2},
      };
      return tokens;
    }

    case X86::ADD64rr:
    case X86::ADD64rr_REV:
    case X86::ADD32rr:
    case X86::ADD32rr_REV:
    case X86::ADD16rr:
    case X86::ADD16rr_REV:
    case X86::ADD8rr:
    case X86::ADD8rr_REV: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},      {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},       {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1}, {.tag = Token::String, .str = "+"},
          {.tag = Token::RegisterOperand, .reg = 2},
      };
      return tokens;
    }

    case X86::ADCX32rr:
    case X86::ADCX64rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},        {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},         {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1},   {.tag = Token::String, .str = "+"},
          {.tag = Token::RegisterOperand, .reg = 2},   {.tag = Token::String, .str = "+"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }

    case X86::ADOX32rr:
    case X86::ADOX64rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},        {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},         {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1},   {.tag = Token::String, .str = "+"},
          {.tag = Token::RegisterOperand, .reg = 2},   {.tag = Token::String, .str = "+"},
          {.tag = Token::FixedFlag, .flag = Flag::OF},
      };
      return tokens;
    }

    case X86::SBB64i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "- "},
          {.tag = Token::ImmediateOperand, .imm = 0},
          {.tag = Token::String, .str = " -"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }
    case X86::SBB32i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "- "},
          {.tag = Token::ImmediateOperand, .imm = 0},
          {.tag = Token::String, .str = " -"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }
    case X86::SBB16i16: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "- "},
          {.tag = Token::ImmediateOperand, .imm = 0},
          {.tag = Token::String, .str = " -"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }
    case X86::SBB8i8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "- "},
          {.tag = Token::ImmediateOperand, .imm = 0},
          {.tag = Token::String, .str = " -"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }

    case X86::SBB64ri32:
    case X86::SBB64ri8:
    case X86::SBB32ri:
    case X86::SBB32ri8:
    case X86::SBB16ri:
    case X86::SBB16ri8:
    case X86::SBB8ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},        {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},         {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1},   {.tag = Token::String, .str = "- "},
          {.tag = Token::ImmediateOperand, .imm = 2},  {.tag = Token::String, .str = " -"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }

    case X86::SBB64rr:
    case X86::SBB64rr_REV:
    case X86::SBB32rr:
    case X86::SBB32rr_REV:
    case X86::SBB16rr:
    case X86::SBB16rr_REV:
    case X86::SBB8rr:
    case X86::SBB8rr_REV: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},        {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},         {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1},   {.tag = Token::String, .str = "-"},
          {.tag = Token::RegisterOperand, .reg = 2},   {.tag = Token::String, .str = "-"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }

    case X86::SUB64i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "- "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::SUB32i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "- "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::SUB16i16: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "- "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::SUB8i8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "- "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }

    case X86::SUB64ri32:
    case X86::SUB64ri8:
    case X86::SUB32ri:
    case X86::SUB32ri8:
    case X86::SUB16ri:
    case X86::SUB16ri8:
    case X86::SUB8ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},       {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},        {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1},  {.tag = Token::String, .str = "- "},
          {.tag = Token::ImmediateOperand, .imm = 2},
      };
      return tokens;
    }

    case X86::SUB64rr:
    case X86::SUB64rr_REV:
    case X86::SUB32rr:
    case X86::SUB32rr_REV:
    case X86::SUB16rr:
    case X86::SUB16rr_REV:
    case X86::SUB8rr:
    case X86::SUB8rr_REV: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},      {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},       {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1}, {.tag = Token::String, .str = "-"},
          {.tag = Token::RegisterOperand, .reg = 2},
      };
      return tokens;
    }

    case X86::RCL64r1:
    case X86::RCL32r1:
    case X86::RCL16r1:
    case X86::RCL8r1: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Rotate"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "left"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "once"},
      };
      return tokens;
    }

    case X86::RCL64rCL:
    case X86::RCL32rCL:
    case X86::RCL16rCL:
    case X86::RCL8rCL: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Rotate"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "left"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::CL},
          {.tag = Token::String, .str = "times"},
      };
      return tokens;
    }

    case X86::RCL64ri:
    case X86::RCL32ri:
    case X86::RCL16ri:
    case X86::RCL8ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Rotate"},    {.tag = Token::FixedFlag, .flag = Flag::CF},
          {.tag = Token::RegisterOperand, .reg = 0},  {.tag = Token::String, .str = "left "},
          {.tag = Token::ImmediateOperand, .imm = 2}, {.tag = Token::String, .str = " times"},
      };
      return tokens;
    }

    case X86::RCR64r1:
    case X86::RCR32r1:
    case X86::RCR16r1:
    case X86::RCR8r1: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Rotate"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "right"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "once"},
      };
      return tokens;
    }

    case X86::RCR64rCL:
    case X86::RCR32rCL:
    case X86::RCR16rCL:
    case X86::RCR8rCL: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Rotate"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "right"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::CL},
          {.tag = Token::String, .str = "times"},
      };
      return tokens;
    }

    case X86::RCR16ri:
    case X86::RCR32ri:
    case X86::RCR64ri:
    case X86::RCR8ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Rotate"},    {.tag = Token::FixedFlag, .flag = Flag::CF},
          {.tag = Token::RegisterOperand, .reg = 0},  {.tag = Token::String, .str = "right "},
          {.tag = Token::ImmediateOperand, .imm = 2}, {.tag = Token::String, .str = " times"},
      };
      return tokens;
    }

    case X86::ROL8r1:
    case X86::ROL64r1:
    case X86::ROL32r1:
    case X86::ROL16r1: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Rotate"}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "left"},   {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "once"},
      };
      return tokens;
    }

    case X86::ROL64ri:
    case X86::ROL32ri:
    case X86::ROL16ri:
    case X86::ROL8ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Rotate"}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "left "},  {.tag = Token::ImmediateOperand, .imm = 2},
          {.tag = Token::String, .str = " times"},
      };
      return tokens;
    }

    case X86::ROL64rCL:
    case X86::ROL32rCL:
    case X86::ROL16rCL:
    case X86::ROL8rCL: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Rotate"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "left"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::CL},
          {.tag = Token::String, .str = "times"},
      };
      return tokens;
    }

    case X86::ROR16r1:
    case X86::ROR32r1:
    case X86::ROR64r1:
    case X86::ROR8r1: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Rotate"}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "right"},  {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "once"},
      };
      return tokens;
    }

    case X86::ROR16rCL:
    case X86::ROR32rCL:
    case X86::ROR64rCL:
    case X86::ROR8rCL: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Rotate"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "right"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::CL},
          {.tag = Token::String, .str = "times"},
      };
      return tokens;
    }

    case X86::ROR16ri:
    case X86::ROR32ri:
    case X86::ROR64ri:
    case X86::ROR8ri:
    case X86::RORX32ri:
    case X86::RORX64ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Rotate"}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "right "}, {.tag = Token::ImmediateOperand, .imm = 2},
          {.tag = Token::String, .str = " times"},
      };
      return tokens;
    }

    case X86::TZCNT64rr:
    case X86::TZCNT32rr:
    case X86::TZCNT16rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "# of trailing zeroes in"},
          {.tag = Token::RegisterOperand, .reg = 1},
      };
      return tokens;
    }

    case X86::POPCNT64rr:
    case X86::POPCNT32rr:
    case X86::POPCNT16rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "# of raised bits in"},
          {.tag = Token::RegisterOperand, .reg = 1},
      };
      return tokens;
    }

    case X86::LZCNT64rr:
    case X86::LZCNT32rr:
    case X86::LZCNT16rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "# of leading zeroes in"},
          {.tag = Token::RegisterOperand, .reg = 1},
      };
      return tokens;
    }

    case X86::BTC64rr:
    case X86::BTC32rr:
    case X86::BTC16rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Flip bit"},
          {.tag = Token::RegisterOperand, .reg = 2},
          {.tag = Token::String, .str = "of"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }

    case X86::BTC64ri8:
    case X86::BTC32ri8:
    case X86::BTC16ri8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Flip bit "},
          {.tag = Token::ImmediateOperand, .imm = 2},
          {.tag = Token::String, .str = " of"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }

    case X86::BTR64rr:
    case X86::BTR32rr:
    case X86::BTR16rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Lower bit"},
          {.tag = Token::RegisterOperand, .reg = 2},
          {.tag = Token::String, .str = "of"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }

    case X86::BTS64rr:
    case X86::BTS32rr:
    case X86::BTS16rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Raise bit"},
          {.tag = Token::RegisterOperand, .reg = 2},
          {.tag = Token::String, .str = "of"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }

    case X86::BT64rr:
    case X86::BT32rr:
    case X86::BT16rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Test bit"},
          {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "of"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }

    case X86::BT64ri8:
    case X86::BT32ri8:
    case X86::BT16ri8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Test bit "},
          {.tag = Token::ImmediateOperand, .imm = 1},
          {.tag = Token::String, .str = " of"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }

    case X86::BTR16ri8:
    case X86::BTR32ri8:
    case X86::BTR64ri8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Lower bit "},
          {.tag = Token::ImmediateOperand, .imm = 2},
          {.tag = Token::String, .str = " of"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }

    case X86::BTS16ri8:
    case X86::BTS32ri8:
    case X86::BTS64ri8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Raise bit "},
          {.tag = Token::ImmediateOperand, .imm = 2},
          {.tag = Token::String, .str = " of"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }

    case X86::BEXTR64rr:
    case X86::BEXTR32rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "bitfield extract of"},
          {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "using length & start from"},
          {.tag = Token::RegisterOperand, .reg = 2},
      };
      return tokens;
    }

      // Lowest set bit operations

    case X86::BLSI32rr:
    case X86::BLSI64rr: {
      // Isolate
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "(lowest raised bit) of"},
          {.tag = Token::RegisterOperand, .reg = 1},
      };
      return tokens;
    }

    case X86::BLSMSK32rr:
    case X86::BLSMSK64rr: {
      // Mask
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "(all bits up to lowest raised bit)"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "of"},
          {.tag = Token::RegisterOperand, .reg = 1},
      };
      return tokens;
    }

    case X86::BLSR32rr:
    case X86::BLSR64rr: {
      // Reset
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set "},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "(lowering lowest set bit) of"},
          {.tag = Token::RegisterOperand, .reg = 1},
      };
      return tokens;
    }

    case X86::BSF16rr:
    case X86::BSF32rr:
    case X86::BSF64rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "position of lowest raised bit"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "of"},
          {.tag = Token::RegisterOperand, .reg = 1},
      };
      return tokens;
    }

    case X86::BSR16rr:
    case X86::BSR32rr:
    case X86::BSR64rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "position of highest raised bit"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "of"},
          {.tag = Token::RegisterOperand, .reg = 1},
      };
      return tokens;
    }

    case X86::BSWAP32r:
    case X86::BSWAP64r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Swap bytes of"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }

    case X86::BZHI32rr:
    case X86::BZHI64rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "bits of"},
          {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "below position"},
          {.tag = Token::RegisterOperand, .reg = 2},
      };
      return tokens;
    }

    case X86::PDEP32rr:
    case X86::PDEP64rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "parallel deposit of"},
          {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "using mask"},
          {.tag = Token::RegisterOperand, .reg = 2},
      };
      return tokens;
    }

    case X86::PEXT32rr:
    case X86::PEXT64rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "parallel extract of"},
          {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "using mask"},
          {.tag = Token::RegisterOperand, .reg = 2},
      };
      return tokens;
    }

      // --- Move instructions ---

    case X86::MOV8rr_REV:
    case X86::MOV8rr:
    case X86::MOV8rr_NOREX:
    case X86::MOV32rr:
    case X86::MOV16rr:
    case X86::MOV64rr_REV:
    case X86::MOV64rr:
    case X86::MOV16rr_REV:
    case X86::MOV32rr_REV: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::RegisterOperand, .reg = 1},
      };
      return tokens;
    }

    case X86::MOV16ri:
    case X86::MOV16ri_alt:
    case X86::MOV64ri:
    case X86::MOV64ri32:
    case X86::MOV8ri:
    case X86::MOV8ri_alt:
    case X86::MOV32ri:
    case X86::MOV32ri_alt: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},       {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},        {.tag = Token::BreakLine},
          {.tag = Token::ImmediateOperand, .imm = 1},
      };
      return tokens;
    }

    case X86::MOVSX16rr16:
    case X86::MOVSX16rr32:
    case X86::MOVSX16rr8:
    case X86::MOVSX32rr16:
    case X86::MOVSX32rr32:
    case X86::MOVSX32rr8:
    case X86::MOVSX32rr8_NOREX:
    case X86::MOVSX64rr16:
    case X86::MOVSX64rr32:
    case X86::MOVSX64rr8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Sign-extended"},
          {.tag = Token::RegisterOperand, .reg = 1},
      };
      return tokens;
    }

    case X86::MOVZX16rr16:
    case X86::MOVZX16rr8:
    case X86::MOVZX32rr16:
    case X86::MOVZX32rr8:
    case X86::MOVZX32rr8_NOREX:
    case X86::MOVZX64rr16:
    case X86::MOVZX64rr8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Zero-extended"},
          {.tag = Token::RegisterOperand, .reg = 1},
      };
      return tokens;
    }

      // --- Exchange two general purpose registers ---

    case X86::XCHG8rr:
    case X86::XCHG16rr:
    case X86::XCHG32rr:
    case X86::XCHG64rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Swap"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "and"},
          {.tag = Token::RegisterOperand, .reg = 1},
      };
      return tokens;
    }

      // --- Exchange AX with some other register ---

    case X86::XCHG64ar: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Swap"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "and"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }
    case X86::XCHG32ar: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Swap"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "and"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }
    case X86::XCHG16ar: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Swap"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "and"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }

    case X86::CMOV64rr:
    case X86::CMOV32rr:
    case X86::CMOV16rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "If "},   {.tag = Token::ConditionCode, .cond_code = 3},
          {.tag = Token::String, .str = " then"}, {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Set"},   {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},    {.tag = Token::RegisterOperand, .reg = 2},
      };
      return tokens;
    }

    case X86::CMP64i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Compare"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "with "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::CMP32i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Compare"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "with "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::CMP16i16: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Compare"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "with "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::CMP8i8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Compare"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "with "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }

    case X86::CMP64ri32:
    case X86::CMP64ri8:
    case X86::CMP32ri8:
    case X86::CMP32ri:
    case X86::CMP16ri8:
    case X86::CMP16ri:
    case X86::CMP8ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Compare"},   {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 0},  {.tag = Token::String, .str = "with "},
          {.tag = Token::ImmediateOperand, .imm = 1},
      };
      return tokens;
    }

    case X86::CMP64rr:
    case X86::CMP64rr_REV:
    case X86::CMP32rr:
    case X86::CMP32rr_REV:
    case X86::CMP16rr:
    case X86::CMP16rr_REV:
    case X86::CMP8rr:
    case X86::CMP8rr_REV: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Compare"},  {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 0}, {.tag = Token::String, .str = "with"},
          {.tag = Token::RegisterOperand, .reg = 1},
      };
      return tokens;
    }

    // --- TEST Instructions ---
    case X86::TEST64i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Test"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "and "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::TEST32i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Test"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "and "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::TEST16i16: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Test"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "and "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }
    case X86::TEST8i8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Test"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "and "},
          {.tag = Token::ImmediateOperand, .imm = 0},
      };
      return tokens;
    }

    case X86::TEST64ri32:
    case X86::TEST32ri:
    case X86::TEST16ri:
    case X86::TEST8ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Test"},      {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 0},  {.tag = Token::String, .str = "and "},
          {.tag = Token::ImmediateOperand, .imm = 1},
      };
      return tokens;
    }

    // Register–register variants (TEST?rr):
    case X86::TEST64rr:
    case X86::TEST32rr:
    case X86::TEST16rr:
    case X86::TEST8rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Test"},     {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 0}, {.tag = Token::String, .str = "and"},
          {.tag = Token::RegisterOperand, .reg = 1},
      };
      return tokens;
    }

    case X86::LOOPNE: {
      constexpr static Token tokens[] = {
          {.tag = Token::FixedRegister, .fixed_reg = X86::RCX},
          {.tag = Token::String, .str = "-1"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "If "},
          {.tag = Token::FixedCondition, .fixed_cond = X86::CondCode::COND_NE},
          {.tag = Token::String, .str = " and"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RCX},
          {.tag = Token::String, .str = "≠0"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Then jump"},
      };
      return tokens;
    }

    case X86::LOOPE: {
      constexpr static Token tokens[] = {
          {.tag = Token::FixedRegister, .fixed_reg = X86::RCX},
          {.tag = Token::String, .str = "-1"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "If "},
          {.tag = Token::FixedCondition, .fixed_cond = X86::CondCode::COND_E},
          {.tag = Token::String, .str = " and"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RCX},
          {.tag = Token::String, .str = "≠0"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Then jump"},
      };
      return tokens;
    }

    case X86::LOOP: {
      constexpr static Token tokens[] = {
          {.tag = Token::FixedRegister, .fixed_reg = X86::RCX},
          {.tag = Token::String, .str = "-1"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "If"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RCX},
          {.tag = Token::String, .str = "≠0"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Then jump"},
      };
      return tokens;
    }

    case X86::JRCXZ: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "If"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RCX},
          {.tag = Token::String, .str = "=0"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Then jump"},
      };
      return tokens;
    }

    case X86::JECXZ: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "If"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::ECX},
          {.tag = Token::String, .str = "=0"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Then jump"},
      };
      return tokens;
    }

    case X86::JCC_1:
    case X86::JCC_4: {
      constexpr static Token tokens[] = {{.tag = Token::String, .str = "If "},
                                         {.tag = Token::ConditionCode, .cond_code = 1},
                                         {.tag = Token::BreakLine},
                                         {.tag = Token::String, .str = "Then jump"}};
      return tokens;
    }

    case X86::SETCCr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "If "},   {.tag = Token::ConditionCode, .cond_code = 1},
          {.tag = Token::String, .str = " then"}, {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Set"},   {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to 1"},
      };
      return tokens;
    }

    case X86::STC: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Raise"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }

    case X86::CLC: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Lower"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }

    case X86::CMC: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Flip"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }

    case X86::RDTSC: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EDX},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Current time"},
      };
      return tokens;
    }

    case X86::RDSEED64r:
    case X86::RDSEED32r:
    case X86::RDSEED16r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Securely"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Randomize"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }

    case X86::RDRAND64r:
    case X86::RDRAND32r:
    case X86::RDRAND16r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Randomize"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }

    case X86::SHL64r1:
    case X86::SHL32r1:
    case X86::SHL16r1:
    case X86::SHL8r1: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Multiply"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "by 2"},
      };
      return tokens;
    }

    // Register+immediate shifts (left)
    case X86::SHL64ri:
    case X86::SHL32ri:
    case X86::SHL16ri:
    case X86::SHL8ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Multiply"}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "by 2 "},    {.tag = Token::ImmediateOperand, .imm = 2},
          {.tag = Token::String, .str = " times"},
      };
      return tokens;
    }

    // Shift left by CL
    case X86::SHL64rCL:
    case X86::SHL32rCL:
    case X86::SHL16rCL:
    case X86::SHL8rCL: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Multiply"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "by 2"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::CL},
          {.tag = Token::String, .str = "times"},
      };
      return tokens;
    }

    // Shift right by 1
    case X86::SHR64r1:
    case X86::SHR32r1:
    case X86::SHR16r1:
    case X86::SHR8r1: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Divide"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "by 2"},
      };
      return tokens;
    }

    // Register+immediate shift right
    case X86::SHR64ri:
    case X86::SHR32ri:
    case X86::SHR16ri:
    case X86::SHR8ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Divide"}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "by 2 "},  {.tag = Token::ImmediateOperand, .imm = 2},
          {.tag = Token::String, .str = " times"},
      };
      return tokens;
    }

    // Shift right by CL
    case X86::SHR64rCL:
    case X86::SHR32rCL:
    case X86::SHR16rCL:
    case X86::SHR8rCL: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Divide"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "by 2"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::CL},
          {.tag = Token::String, .str = "times"},
      };
      return tokens;
    }

    case X86::SAR64r1:
    case X86::SAR32r1:
    case X86::SAR16r1:
    case X86::SAR8r1: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Divide ±"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "by 2"},
      };
      return tokens;
    }

    case X86::SAR64rCL:
    case X86::SAR32rCL:
    case X86::SAR16rCL:
    case X86::SAR8rCL: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Divide ±"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "by 2"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::CL},
          {.tag = Token::String, .str = "times"},
      };
      return tokens;
    }

    case X86::SAR64ri:
    case X86::SAR32ri:
    case X86::SAR16ri:
    case X86::SAR8ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Divide ±"}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "by 2 "},    {.tag = Token::ImmediateOperand, .imm = 2},
          {.tag = Token::String, .str = " times"},
      };
      return tokens;
    }

    case X86::NEG8r:
    case X86::NEG64r:
    case X86::NEG32r:
    case X86::NEG16r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to -"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }

    case X86::IDIV8r:
    case X86::DIV8r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "÷"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AH},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "mod"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }
    case X86::IDIV16r:
    case X86::DIV16r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::DX},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "÷"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::DX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::DX},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "mod"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }
    case X86::IDIV32r:
    case X86::DIV32r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EDX},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "÷"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EDX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EDX},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "mod"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }
    case X86::IDIV64r:
    case X86::DIV64r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RDX},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "÷"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RDX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RDX},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "mod"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }

    case X86::MUL8r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "×"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }
    case X86::MUL16r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::DX},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "×"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }
    case X86::MUL32r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EDX},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "×"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }
    case X86::MUL64r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RDX},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "×"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }

    case X86::IMUL8r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
          {.tag = Token::String, .str = "×"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }
    case X86::IMUL16r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::DX},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "×"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }
    case X86::IMUL32r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EDX},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "×"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }
    case X86::IMUL64r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RDX},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "×"},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }

    case X86::IMUL64rr:
    case X86::IMUL32rr:
    case X86::IMUL16rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},      {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},       {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1}, {.tag = Token::String, .str = "×"},
          {.tag = Token::RegisterOperand, .reg = 2},
      };
      return tokens;
    }

    case X86::IMUL64rri32:
    case X86::IMUL64rri8:
    case X86::IMUL32rri:
    case X86::IMUL32rri8:
    case X86::IMUL16rri:
    case X86::IMUL16rri8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},       {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"},        {.tag = Token::BreakLine},
          {.tag = Token::RegisterOperand, .reg = 1},  {.tag = Token::String, .str = "× "},
          {.tag = Token::ImmediateOperand, .imm = 2},
      };
      return tokens;
    }

    case X86::CQO: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RDX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Sign of"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
      };
      return tokens;
    }
    case X86::CDQ: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EDX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Sign of"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
      };
      return tokens;
    }
    case X86::CWD: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::DX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Sign of"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
      };
      return tokens;
    }
    case X86::CDQE: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Sign-extended"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
      };
      return tokens;
    }
    case X86::CWDE: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::EAX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Sign-extended"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
      };
      return tokens;
    }
    case X86::CBW: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AX},
          {.tag = Token::String, .str = "to"},
          {.tag = Token::BreakLine},
          {.tag = Token::String, .str = "Sign-extended"},
          {.tag = Token::FixedRegister, .fixed_reg = X86::AL},
      };
      return tokens;
    }

    default: {
      static std::mutex unknown_tokens_mutex;
      static std::map<unsigned, std::vector<Token>> unknown_tokens;
      std::lock_guard<std::mutex> lock(unknown_tokens_mutex);
      auto it = unknown_tokens.find(inst.getOpcode());
      if (it == unknown_tokens.end()) {
        auto name = LLVM_Assembler::Get().mc_instr_info->getName(inst.getOpcode()).data();
        it = unknown_tokens
                 .emplace(inst.getOpcode(),
                          std::vector<Token>{
                              Token{.tag = Token::String, .str = name},
                          })
                 .first;
        LOG << "Warning: PrintInstruction() is missing a case for X86::" << name;
      }
      return it->second;
    }
  }
}

void DrawFlag(SkCanvas& canvas, Flag flag) {
  static const SkPath spike = [] {
    auto base_path = PathFromSVG("M-4 0A40 40 0 000-10 40 40 0 004 0 8 8 0 01-4 0");
    auto bounds = base_path.getBounds();
    float scale = 1.5_mm / bounds.bottom();
    base_path = base_path.makeScale(scale, scale);
    base_path.offset(0, 10_mm);
    return base_path;
  }();
  SkPaint pole_paint;
  pole_paint.setStyle(SkPaint::kStroke_Style);
  pole_paint.setStrokeWidth(0.5_mm);
  SkPaint spike_paint;
  canvas.drawLine(Vec2{0, 0}, Vec2{0, 10_mm}, pole_paint);
  canvas.drawPath(spike, spike_paint);

  PersistentImage* img = nullptr;
  switch (flag) {
    case Flag::CF: {
      static PersistentImage cf_image =
          PersistentImage::MakeFromAsset(embedded::assets_flag_cf_webp, PersistentImage::MakeArgs{
                                                                            .width = 8_mm,
                                                                        });
      img = &cf_image;
      break;
    }
    default:  // fallthrough
    case Flag::OF: {
      static PersistentImage of_image =
          PersistentImage::MakeFromAsset(embedded::assets_flag_of_webp, PersistentImage::MakeArgs{
                                                                            .width = 8_mm,
                                                                        });
      img = &of_image;
      break;
    }
  }
  if (img) {
    canvas.save();
    canvas.translate(-0.4_mm, 4.5_mm);
    img->draw(canvas);
    canvas.restore();
  }
}

constexpr float kConditionCodeTokenWidth = 8_mm;
constexpr float kConditionCodeTokenHeight = 8_mm;

constexpr Rect kConditionCodeRect =
    Rect::MakeAtZero<LeftX, BottomY>(kConditionCodeTokenWidth, kConditionCodeTokenHeight);

struct EnumKnobWidget : ui::Widget {
  float last_vx = 0;
  Knob knob;

  constexpr static time::FloatDuration kClickWigglePeriod = 0.5s;
  constexpr static time::FloatDuration kClickWiggleHalfTime = 0.1s;
  animation::SpringV2<float> click_wiggle = {};
  bool is_dragging = false;
  float cond_code_float = 0;
  int n_options;

  int value = 0;  // ground-truth value, obtained from the getter in Tick

  EnumKnobWidget(ui::Widget* parent, int n_options) : ui::Widget(parent), n_options(n_options) {
    knob.unit_angle = 60_deg;
    knob.unit_distance = kGaugeRadius * 2;

    // Add some initial history to make the initial direction more stable.
    constexpr static int kInitialHistory = 40;
    for (int i = 0; i < kInitialHistory; ++i) {
      float a = i * M_PI * 2 / kInitialHistory;
      float x = knob.unit_distance * 2 * (kInitialHistory - i) / kInitialHistory;
      float amp = kGaugeRadius * 0.25;
      float perp = sinf(a * 2) * amp;
      knob.history.push_back({-x - perp, -x + perp});
    }
    knob.Update({0, 0});
    knob.value = 0;
  }
  SkPath Shape() const override { return SkPath::Circle(0, 0, kConditionCodeTokenWidth / 2); }
  void TransformUpdated() override { WakeAnimation(); }

  static constexpr float kBorderWidth = 0.5_mm;
  static constexpr float kBorderHalf = kBorderWidth / 2;
  static constexpr float kGaugeRadius = 4_mm;
  static constexpr Rect kGaugeOval = Rect::MakeAtZero(kGaugeRadius * 2, kGaugeRadius * 2);
  static constexpr float kInnerRadius = kGaugeRadius - kBorderWidth;
  static constexpr Rect kInnerOval = Rect::MakeAtZero(kInnerRadius * 2, kInnerRadius * 2);
  static constexpr float kSymbolRadius = 2_mm;
  static constexpr Rect kSymbolOval = Rect::MakeAtZero(kSymbolRadius * 2, kSymbolRadius * 2);

  static constexpr auto& kWaterOval = kInnerOval;
  static constexpr float kWaterRadius = kWaterOval.Height() / 2;

  // Constants describing the arrow around the condition symbol
  static constexpr float kMiddleR = (kInnerRadius + kSymbolRadius) / 2;
  static constexpr Rect kMiddleOval = Rect::MakeAtZero(kMiddleR * 2, kMiddleR * 2);
  static constexpr Rect kFarOval = kMiddleOval.Outset(kBorderHalf);
  static constexpr Rect kNearOval = kMiddleOval.Outset(-kBorderHalf);

  constexpr static float kRegionEndRadius = kGaugeRadius;
  constexpr static float kRegionStartRadius = kInnerRadius;
  constexpr static float kRegionWidth = kRegionEndRadius - kRegionStartRadius;
  constexpr static Rect kRegionOuter = Rect::MakeAtZero(2 * kRegionEndRadius, 2 * kRegionEndRadius);
  constexpr static Rect kRegionInner =
      Rect::MakeAtZero(2 * kRegionStartRadius, 2 * kRegionStartRadius);
  constexpr static float kRegionMargin = kBorderWidth / 2;

  virtual int KnobGet() const = 0;
  virtual void KnobSet(int value) = 0;

  animation::Phase Tick(time::Timer& timer) override {
    auto phase = animation::Finished;
    value = KnobGet();
    auto old_value = value;
    if (isnan(knob.value) || isinf(knob.value)) {
      knob.value = 0;
    }
    while (knob.value >= 0.5) {
      knob.value -= 1;
      if (value >= n_options - 1) {
        value = 0;
      } else {
        value = value + 1;
      }
    }
    while (knob.value < -0.5) {
      knob.value += 1;
      if (value <= 0) {
        value = n_options - 1;
      } else {
        value = value - 1;
      }
    }
    if (value != old_value) {
      KnobSet(value);
    }
    phase |= click_wiggle.SpringTowards(0, timer.d, time::ToSeconds(kClickWigglePeriod),
                                        time::ToSeconds(kClickWiggleHalfTime));
    cond_code_float = (float)value + knob.value + click_wiggle.value;

    return phase;
  }

  static SkPath RegionPath(float start_deg, float end_deg) {
    const static float kRegionOuterAngleAdjust =
        asin(kRegionMargin / 2 / kRegionEndRadius) * 180 / M_PI;
    const static float kRegionInnerAngleAdjust =
        asin(kRegionMargin / 2 / kRegionStartRadius) * 180 / M_PI;
    SkPath path;
    float sweep = end_deg - start_deg;
    path.arcTo(kRegionOuter.sk, start_deg + kRegionOuterAngleAdjust,
               sweep - 2 * kRegionOuterAngleAdjust, true);
    path.arcTo(kRegionInner.sk, end_deg - kRegionInnerAngleAdjust,
               -sweep + 2 * kRegionInnerAngleAdjust, false);
    path.close();
    return path;
  };

  static void DrawConditionCodeBackground(SkCanvas& canvas, X86::CondCode cond_code) {
    static constexpr float kParityRegionSweep = 360.f / 9;
    static const SkPath kEvenParityRegion = [&]() {
      SkPath path;
      for (int i = 0; i < 9; ++i) {
        if ((i & 1) == 1) continue;
        float start_deg = (i - 0.5f) * 360 / 9;
        path.arcTo(kRegionOuter.sk, start_deg, kParityRegionSweep, true);
        path.arcTo(kRegionInner.sk, start_deg + kParityRegionSweep, -kParityRegionSweep, false);
        path.lineTo(0, 0);
      }
      return path;
    }();
    static const SkPath kOddParityRegion = [&]() {
      SkPath path;
      for (int i = 0; i < 9; ++i) {
        if ((i & 1) != 1) continue;
        float start_deg = (i - 0.5f) * 360 / 9;
        path.arcTo(kRegionOuter.sk, start_deg, kParityRegionSweep, true);
        path.arcTo(kRegionInner.sk, start_deg + kParityRegionSweep, -kParityRegionSweep, false);
      }
      return path;
    }();

    static constexpr float kZeroAngle = 12;
    static const SkPath kRegion0b = RegionPath(-kZeroAngle / 2, kZeroAngle / 2);
    static const SkPath kRegion7b = RegionPath(kZeroAngle / 2, 45);
    static const SkPath kRegion8b = RegionPath(45, 67.5);
    static const SkPath kRegion15b = RegionPath(67.5, 90);
    static const SkPath kRegion16b = RegionPath(90, 112.5);
    static const SkPath kRegion31b = RegionPath(112.5, 135);
    static const SkPath kRegion32b = RegionPath(135, 157.5);
    static const SkPath kRegion63b = RegionPath(157.5, 180);
    static const SkPath kRegionSigned64b = RegionPath(180, 225);
    static const SkPath kRegionSigned32b = RegionPath(225, 270);
    static const SkPath kRegionSigned16b = RegionPath(270, 315);
    static const SkPath kRegionSigned8b = RegionPath(315, 360 - kZeroAngle / 2);

    static const SkPath kRegionsArr[] = {kRegion0b,        kRegion7b,        kRegion8b,
                                         kRegion15b,       kRegion16b,       kRegion31b,
                                         kRegion32b,       kRegion63b,       kRegionSigned64b,
                                         kRegionSigned32b, kRegionSigned16b, kRegionSigned8b};
    static const SkColor kUnsignedColors[] = {
        color::HSLuv(0, 0, 57),      // 0b
        color::HSLuv(128, 100, 60),  // 7b
        color::HSLuv(121, 100, 62),  // 8b
        color::HSLuv(99, 100, 65),   // 15b
        color::HSLuv(91, 100, 70),   // 16b
        color::HSLuv(65, 100, 69),   // 31b
        color::HSLuv(40, 100, 62),   // 32b
        color::HSLuv(21, 100, 57),   // 63b
        color::HSLuv(12, 95, 53),    // 64b
        color::HSLuv(12, 95, 53),    // 32b
        color::HSLuv(12, 95, 53),    // 16b
        color::HSLuv(12, 95, 53),    // 8b
    };
    static const SkColor kSignedColors[] = {
        kUnsignedColors[0],          // 0b
        kUnsignedColors[1],          // 7b
        kUnsignedColors[2],          // 8b
        kUnsignedColors[3],          // 15b
        kUnsignedColors[4],          // 16b
        kUnsignedColors[5],          // 31b
        kUnsignedColors[6],          // 32b
        kUnsignedColors[7],          // 63b
        color::HSLuv(250, 100, 46),  // 64b
        color::HSLuv(240, 100, 50),  // 32b
        color::HSLuv(226, 100, 73),  // 16b
        color::HSLuv(176, 100, 65),  // 8b
    };

    static const SkPaint neutral_paint = [] {
      SkPaint paint;
      paint.setColor(color::HSLuv(283, 100, 57));
      return paint;
    }();

    // Switch fill based on signed-ness of the condition
    switch (cond_code) {
      // Signed
      case X86::CondCode::COND_S:
      case X86::CondCode::COND_NS:
      case X86::CondCode::COND_L:
      case X86::CondCode::COND_LE:
      case X86::CondCode::COND_G:
      case X86::CondCode::COND_GE:
      case X86::CondCode::COND_O:
      case X86::CondCode::COND_NO: {
        for (int i = 0; i < std::size(kRegionsArr); ++i) {
          SkPaint paint;
          paint.setColor(kSignedColors[i]);
          canvas.drawPath(kRegionsArr[i], paint);
        }
        break;
      }
      // Unsigned
      case X86::CondCode::COND_A:
      case X86::CondCode::COND_AE:
      case X86::CondCode::COND_B:
      case X86::CondCode::COND_BE: {
        for (int i = 0; i < std::size(kRegionsArr); ++i) {
          SkPaint paint;
          paint.setColor(kUnsignedColors[i]);
          canvas.drawPath(kRegionsArr[i], paint);
        }
        break;
      }
      // Sign-neutral
      case X86::CondCode::COND_E:
      case X86::CondCode::COND_NE: {
        for (int i = 0; i < std::size(kRegionsArr); ++i) {
          canvas.drawPath(kRegionsArr[i], neutral_paint);
        }
        break;
      }
      case X86::CondCode::COND_P: {  // even number of bits in the lowest byte
        canvas.drawPath(kEvenParityRegion, neutral_paint);
        break;
      }
      case X86::CondCode::COND_NP: {  // odd number of bits in the lowest byte
        canvas.drawPath(kOddParityRegion, neutral_paint);
        break;
      }
      default: {
        break;
      }
    }
    static const SkPaint white_overlay = [] {
      SkPaint paint;
      SkColor mask_colors[] = {"#ffffff"_color, "#ffffff00"_color};
      float mask_pos[] = {kRegionStartRadius / kRegionEndRadius, 1};
      auto mask = SkGradientShader::MakeRadial(Vec2(), kRegionEndRadius, mask_colors, mask_pos,
                                               std::size(mask_colors), SkTileMode::kClamp);
      SkColor colors[] = {"#dddddd"_color, "#bbbbbb"_color};
      auto color = SkGradientShader::MakeRadial(Vec2(0, kMiddleR), kGaugeRadius + kMiddleR, colors,
                                                nullptr, std::size(colors), SkTileMode::kClamp);
      paint.setShader(SkShaders::Blend(SkBlendMode::kSrcIn, mask, color));
      return paint;
    }();
    canvas.drawCircle(Vec2(), kRegionEndRadius, white_overlay);
  }

  static void DrawConditionCodeSymbol(SkCanvas& canvas, X86::CondCode cond_code) {
    SkPath dial;    // black arrow
    SkPath symbol;  // symbol drawn in the center

    SkPaint symbol_fill;
    symbol_fill.setAntiAlias(true);
    SkPaint dial_fill;
    dial_fill.setColor("#00000044"_color);

    auto FillCircle = [&](float degrees) {
      auto center = Vec2::Polar(degrees / 180 * M_PI, kMiddleR);
      dial.addCircle(center.x, center.y, kBorderWidth * 0.75, SkPathDirection::kCW);
    };
    auto StrokeCircle = [&](float degrees) {
      auto center = Vec2::Polar(degrees / 180 * M_PI, kMiddleR);
      dial.addCircle(center.x, center.y, kBorderWidth, SkPathDirection::kCW);
      dial.addCircle(center.x, center.y, kBorderHalf, SkPathDirection::kCCW);
    };
    static const float kCircleAngleAdjust = asin(kBorderWidth * 0.75 / kMiddleR) * 180 / M_PI;
    auto Arc = [&](float start_deg, float sweep_deg) {
      if (sweep_deg < 0) {
        start_deg += sweep_deg;
        sweep_deg = -sweep_deg;
      }
      static const SkRect kArcOuterRect =
          kMiddleOval.sk.makeOutset(kBorderHalf / 2, kBorderHalf / 2);
      static const SkRect kArcInnerRect =
          kMiddleOval.sk.makeOutset(-kBorderHalf / 2, -kBorderHalf / 2);
      Vec2 inner_point =
          Vec2::Polar((start_deg + sweep_deg) / 180 * M_PI, kMiddleR - kBorderHalf / 2);
      dial.arcTo(kArcOuterRect, start_deg, sweep_deg, true);
      dial.lineTo(inner_point);
      dial.arcTo(kArcInnerRect, start_deg + sweep_deg, -sweep_deg, false);
      dial.close();
    };
    auto Triangle = [&](SinCos angle, bool ccw = false) {
      constexpr static float kSide = kInnerRadius - kSymbolRadius;
      const static float kHeight = kSide * sqrt(3) / 2;
      auto ab = Vec2::Polar(angle, kMiddleR);
      auto a = Vec2::Polar(angle, kInnerRadius);
      auto b = Vec2::Polar(angle, kSymbolRadius);
      auto c = ab + Vec2::Polar(angle + (ccw ? -90_deg : 90_deg), kHeight);
      auto ca = (a + c) / 2;
      auto bc = (b + c) / 2;
      dial.moveTo(ab);
      dial.arcTo(b, bc, kBorderHalf);
      dial.arcTo(c, ca, 0);
      dial.arcTo(a, ab, kBorderHalf);
      dial.close();
    };
    // Switch the symbol
    switch (cond_code) {
      case X86::CondCode::COND_O: {  // overflow
        Triangle(225_deg, false);
        Triangle(135_deg, true);
        Arc(135, 90);
        static const SkPath kOverflowSymbol = PathFromSVG(
            "M.9-.01c.1.16.15.35.15.53C1.05 1.1.58 1.57 0 1.57S-1.05 "
            "1.1-1.05.52c0-.18.05-.37.15-.53L0-1.57Z",
            SVGUnit_Millimeters);
        symbol = kOverflowSymbol;
        break;
      }
      case X86::CondCode::COND_NO: {  // no overflow
        Arc(225, 270);
        Triangle(225_deg, true);
        Triangle(135_deg, false);
        static const SkPath kNoOverflowSymbol = PathFromSVG(
            "M.92.03c.09.15.13.33.13.5C1.05 1.11.58 1.58 0 1.58c-.18 "
            "0-.35-.05-.5-.13L.92.03ZM1.14-.62-.9 1.42-1.21 "
            "1.1.83-.94ZM.39-.91-1.04.52c0-.19.05-.37.14-.53L0-1.57Z",
            SVGUnit_Millimeters);
        symbol = kNoOverflowSymbol;
        break;
      }
      case X86::CondCode::COND_L:  // fallthrough
      case X86::CondCode::COND_B: {
        static const SkPath static_path =
            PathFromSVG("M-2.7-1.1V1.2L2.4 2.5 2.7 1.2-1.8 0l4.5-1.2-.3-1.4Z", SVGUnit_Millimeters)
                .makeScale(0.5, 0.5);
        symbol = static_path;
        Arc(-90, 90 - kCircleAngleAdjust);
        Triangle(-90_deg, true);
        StrokeCircle(0);
        break;
      }
      case X86::CondCode::COND_GE:  // fallthrough
      case X86::CondCode::COND_AE: {
        static const SkPath static_path = PathFromSVG(
                                              "M-2.4-3.2-2.8-2 1.8-1-2.7 0l.3 1.1L2.7 "
                                              "0V-1.9L-2.4-3.2ZM2.7.7-2.6 1.9l.4 1.3L2.7 2V.7Z",
                                              SVGUnit_Millimeters)
                                              .makeScale(0.5, 0.5);
        symbol = static_path;
        Triangle(90_deg, false);
        FillCircle(0);
        Arc(0, 90);
        break;
      }
      case X86::CondCode::COND_E: {
        static const SkPath static_path =
            PathFromSVG(
                "m-.45-2.08c-1.13 0-2.14.03-2.21.11-.17.17-.15 1.12 0 1.26.14.17 5.26.15 5.38 0 "
                ".12-.08.05-1.2 0-1.26-.08-.06-1.72-.11-3.17-.11zm0 2.66c-1.13 "
                "0-2.14.03-2.21.1-.17.17-.15 1.12 0 1.26.14.17 5.26.15 5.38 0 .12-.08.05-1.2 "
                "0-1.26C2.64.62 1 .57-.45.58z",
                SVGUnit_Millimeters)
                .makeScale(0.5, 0.5);
        symbol = static_path;
        FillCircle(0);
        break;
      }
      case X86::CondCode::COND_NE: {
        static const SkPath static_path =
            PathFromSVG(
                "m1.08-2.74-2.89 5 .77.45 2.89-5zm-1.53.67c-1.13 0-2.14.03-2.21.1-.17.17-.15 1.12 "
                "0 "
                "1.26.05.06.78.1 1.68.11l.85-1.48c-.11 0-.22 0-.32 0zM2.51-2a.71.71 0 "
                "01-.03.06L1.71-.61c.57-.02.98-.05 1.01-.09.12-.08.05-1.2 "
                "0-1.26-.01-.01-.1-.02-.21-.03zM1.02.59.17 2.07c1.27 0 2.49-.05 "
                "2.55-.12.12-.08.05-1.2 "
                "0-1.26C2.67.65 1.93.62 1.02.6zm-2.68 0c-.56.02-.96.04-1 .09-.17.17-.15 1.12 0 "
                "1.26.01.02.09.03.21.05a.71.71 0 01.04-.09z",
                SVGUnit_Millimeters)
                .makeScale(0.5, 0.5);
        symbol = static_path;
        StrokeCircle(0);
        Arc(kCircleAngleAdjust, 360 - kCircleAngleAdjust * 2);
        break;
      }
      case X86::CondCode::COND_LE:  // fallthrough
      case X86::CondCode::COND_BE: {
        static const SkPath static_path =
            PathFromSVG(
                "M-2.7.7V2L2.2 3.2 2.6 1.9-2.7.7ZM2.4-3.2-2.7-1.9V0L2.4 1.1 2.7 0-1.8-1 2.8-2 "
                "2.4-3.2Z",
                SVGUnit_Millimeters)
                .makeScale(0.5, 0.5);
        symbol = static_path;
        Arc(0, -90);
        FillCircle(0);
        Triangle(-90_deg, true);
        break;
      }
      case X86::CondCode::COND_G:  // fallthrough
      case X86::CondCode::COND_A: {
        static const SkPath static_path =
            PathFromSVG("M2.7-1.1V1.2L-2.4 2.5-2.7 1.2 1.8 0-2.7-1.2l.3-1.4Z", SVGUnit_Millimeters)
                .makeScale(0.5, 0.5);
        symbol = static_path;
        Arc(90, -90 + kCircleAngleAdjust);
        StrokeCircle(0);
        Triangle(90_deg, false);
        break;
      }
      case X86::CondCode::COND_S: {  // sign / negative
        static const SkPath kSignSymbol =
            PathFromSVG("m-4.5-1c.1-.1 8.9-.1 9 0 .1.1.1 1.9 0 2-.1.1-8.9.1-9 0-.1-.1-.1-1.9 0-2z");
        symbol = kSignSymbol;
        StrokeCircle(0);
        FillCircle(180);
        Arc(180, 180 - kCircleAngleAdjust);
        break;
      }
      case X86::CondCode::COND_NS: {  // not sign / positive
        static const SkPath kNoSignSymbol = PathFromSVG(
            "m-4.5-1c.1-.1 8.9-.1 9 0 .1.1.1 1.9 0 2-.1.1-8.9.1-9 0-.1-.1-.1-1.9 "
            "0-2zm3.5-3.5c.1-.1 "
            "1.9-.1 2 0 .1.1.1 8.9 0 9-.1.1-1.9.1-2 0-.1-.1-.1-8.9 0-9z");
        symbol = kNoSignSymbol;
        FillCircle(0);
        StrokeCircle(180);
        Arc(0, 180 - kCircleAngleAdjust);
        break;
      }
      case X86::CondCode::COND_P:     // fallthrough
      case X86::CondCode::COND_NP: {  // number of bits in the lowest byte
        static const SkPath kFlagSymbol = PathFromSVG(
            "M-1.22-1.42c.08.14.12.23.28.34-.06 0-.11-.02-.15-.04 0 .06 0 .11 0 "
            ".18.09.02.18.05.25.04.19 0 "
            ".36-.16.54-.14.18.01.3.21.48.22.23.02.45-.16.68-.17.31-.02.93.14.93.14S1.01-.74.67-."
            "57C."
            "46-.47.35-.22.13-.16-.01-.12-.17-.24-.32-.21-.51-.16-.64.04-.82.11-.85.12-.89.13-.93."
            "14c.14.68.31 1.31.34 1.33 0 .06-.49.06-.5 0-.03-.13-.09-.7-.15-1.28-.04 "
            "0-.06-.01-.07-.02C-1.35-.23-1.39-.6-1.37-1c0-.01.01-.01.02-.02 0-.03 0-.06 "
            "0-.09-.05.02-.08.02-.16.02.14-.11.22-.2.29-.33Z",
            SVGUnit_Millimeters);
        static const SkPath kTwoFlagsSymbol = [] {
          SkPath path;
          path.addPath(kFlagSymbol.makeTransform(
              SkMatrix::RotateDeg(-5).preTranslate(0.7_mm, -0.5_mm).preScale(0.6, 0.6)));
          path.addPath(kFlagSymbol.makeTransform(SkMatrix::RotateDeg(5).preTranslate(0, 0.3_mm)));
          // path.setFillType(SkPathFillType::kEvenOdd);
          return path;
        }();
        static const SkPath kParityDial = [&]() {
          SkPath path;
          auto font = ui::Font::MakeV2(ui::Font::GetSilkscreen(), 1.4_mm);
          SkGlyphID glyphs[9];
          SkRect bounds[9];
          font->sk_font.textToGlyphs("012345678", 9, SkTextEncoding::kUTF8, glyphs);
          font->sk_font.getBounds(glyphs, bounds, nullptr);
          for (int i = 0; i < 9; ++i) {
            SkPath glyph_path;
            font->sk_font.getPath(glyphs[i], &glyph_path);
            bounds[i] = glyph_path.getBounds();
            glyph_path.transform(SkMatrix::Translate(-bounds[i].centerX(), -bounds[i].centerY()));
            glyph_path.transform(SkMatrix::Scale(font->font_scale, -font->font_scale));
            auto dir = SinCos::FromDegrees(i * 360.f / 9);
            glyph_path.transform(
                SkMatrix::Translate(Vec2::Polar(dir, (kInnerRadius + kSymbolRadius) / 2)));
            path.addPath(glyph_path);
          }
          return path;
        }();
        if (cond_code == X86::CondCode::COND_P) {
          symbol = kTwoFlagsSymbol;
        } else {
          symbol = kFlagSymbol;
        }
        dial = kParityDial;
        break;
      }
      default: {
        break;
      }
    }

    canvas.drawPath(dial, dial_fill);
    canvas.drawPath(symbol, symbol_fill);
  }

  virtual void DrawKnobBackground(SkCanvas& canvas, int cond_code) const {
    SkPaint white_paint;
    white_paint.setColor("#ffffff"_color);
    canvas.drawCircle(Vec2(), kRegionEndRadius, white_paint);
  }

  virtual void DrawKnobSymbol(SkCanvas& canvas, int cond_code) const = 0;

  // Allows derived classes to draw something under the glass
  virtual void DrawKnobBelowGlass(SkCanvas& canvas) const {}

  // Allows derived classes to draw something over the glass
  virtual void DrawKnobOverGlass(SkCanvas& canvas) const {}

  void Draw(SkCanvas& canvas) const override {
    float cond_code_floor = floorf(cond_code_float);
    float cond_code_ceil = ceilf(cond_code_float);
    float cond_code_t = cond_code_float - cond_code_floor;  // how far towards ceil we are currently
    if (cond_code_floor < 0) {
      cond_code_floor = n_options - 1;
    }
    if (cond_code_ceil >= n_options) {
      cond_code_ceil = 0;
    }

    DrawKnobBackground(canvas, (X86::CondCode)roundf(cond_code_float));

    canvas.save();
    SkRRect clip = SkRRect::MakeOval(kInnerOval.sk);
    canvas.clipRRect(clip);
    float radius = std::clamp(knob.radius, kGaugeRadius * 2, kGaugeRadius * 9);
    Vec2 delta;
    Vec2 center;
    float angle;
    if (isinf(radius)) {
      delta = Vec2::Polar(knob.tangent, kGaugeRadius * 2);
      canvas.translate(delta.x * cond_code_t, delta.y * cond_code_t);
    } else {
      center = Vec2::Polar(knob.tangent - 90_deg, radius);
      angle = asinf(kGaugeRadius / radius) * 2 * 180 / M_PI;
      canvas.rotate(-angle * cond_code_t, center.x, center.y);
    }

    DrawKnobSymbol(canvas, (X86::CondCode)cond_code_floor);
    if (cond_code_ceil != cond_code_floor) {
      if (isinf(radius)) {
        canvas.translate(-delta.x, -delta.y);
      } else {
        canvas.rotate(angle, center.x, center.y);
      }
      DrawKnobSymbol(canvas, (X86::CondCode)cond_code_ceil);
    }
    canvas.restore();

    DrawKnobBelowGlass(canvas);

    if constexpr (kDebugKnob) {
      SkPaint circle_paint;
      circle_paint.setColor("#ff0000"_color);
      circle_paint.setStyle(SkPaint::kStroke_Style);
      if (isinf(knob.radius)) {  // line
        Vec2 a = Vec2::Polar(knob.tangent, -10_mm);
        Vec2 b = Vec2::Polar(knob.tangent, 10_mm);
        canvas.drawLine(a.sk, b.sk, circle_paint);
      } else {
        canvas.drawCircle(knob.center, knob.radius, circle_paint);
      }
      SkPaint history_paint;
      history_paint.setColor("#00ff00"_color);
      for (auto& point : knob.history) {
        canvas.drawCircle(point, 0.1_mm, history_paint);
      }
    }

    {    // Glass effects
      {  // shadow
        SkPaint paint;
        paint.setColor("#00000080"_color);
        paint.setMaskFilter(SkMaskFilter::MakeBlur(SkBlurStyle::kNormal_SkBlurStyle, kBorderWidth));
        canvas.save();
        RRect clip = RRect::MakeSimple(kGaugeOval, kGaugeRadius);
        canvas.clipRRect(clip.sk);
        SkPath path;
        path.addCircle(0, -kBorderWidth * 2, kGaugeRadius);
        path.toggleInverseFillType();
        canvas.drawPath(path, paint);
        canvas.restore();
      }

      {  // sky reflection
        SkPaint paint;
        SkColor colors[] = {"#ffffffaa"_color, "#ffffff30"_color, "#ffffff00"_color};
        paint.setShader(SkGradientShader::MakeRadial(Vec2(0, kMiddleR), kGaugeRadius * 1.5, colors,
                                                     nullptr, std::size(colors),
                                                     SkTileMode::kClamp));
        canvas.save();
        RRect clip = RRect::MakeSimple(kInnerOval, kInnerRadius);
        canvas.clipRRect(clip.sk);
        canvas.drawCircle(Vec2(0, kGaugeRadius * 2), hypotf(kGaugeRadius * 2, kGaugeRadius), paint);
        canvas.restore();
      }

      {  // light edge
        SkPaint paint;
        SkPoint pts[] = {Vec2(-kGaugeRadius, 0), Vec2(kGaugeRadius, 0)};
        SkColor colors[] = {"#ffffff20"_color, "#ffffffaa"_color, "#ffffff20"_color};
        paint.setShader(SkGradientShader::MakeLinear(pts, colors, nullptr, std::size(colors),
                                                     SkTileMode::kClamp));
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(kBorderWidth);
        paint.setMaskFilter(
            SkMaskFilter::MakeBlur(SkBlurStyle::kNormal_SkBlurStyle, kBorderHalf / 3));
        canvas.drawCircle(0, 0, kGaugeRadius - kBorderHalf, paint);
      }
    }

    DrawKnobOverGlass(canvas);
  }

  Optional<Rect> TextureBounds() const override {
    if (kDebugKnob || is_dragging) {
      return std::nullopt;
    }
    auto bounds = kGaugeOval;
    bounds.left -= 2_mm;
    return bounds;
  }

  struct ChangeEnumKnobAction : public Action {
    TrackedPtr<EnumKnobWidget> widget;
    time::SteadyPoint start_time;
    ui::Pointer::IconOverride scroll_icon;

    ChangeEnumKnobAction(ui::Pointer& pointer, EnumKnobWidget& enum_knob_widget)
        : Action(pointer),
          widget(&enum_knob_widget),
          scroll_icon(pointer, ui::Pointer::kIconAllScroll) {
      if (widget) {
        widget->is_dragging = true;
        auto& history = widget->knob.history;
        if (!history.empty()) {
          Vec2 pos = pointer.PositionWithin(*widget);
          Vec2 shift = pos - history.back();
          for (auto& point : history) {
            point += shift;
          }
        }
        widget->click_wiggle.velocity += 5;
        start_time = time::SteadyNow();
        widget->WakeAnimation();
      }
    }
    void Update() override {
      if (!widget) {
        pointer.ReplaceAction(*this, nullptr);
        return;
      }
      Vec2 pos = pointer.PositionWithin(*widget);
      widget->knob.Update(pos);
      widget->WakeAnimation();
    }

    ~ChangeEnumKnobAction() {
      if (!widget) {
        return;
      }
      widget->click_wiggle.value += widget->knob.value;
      widget->knob.value = 0;

      if ((time::SteadyNow() - start_time) < kClickWigglePeriod / 2) {
        widget->knob.value -= 1;
        widget->click_wiggle.value += 1;
      }
      widget->is_dragging = false;
      widget->WakeAnimation();
    }
  };

  std::unique_ptr<Action> FindAction(ui::Pointer& pointer, ui::ActionTrigger trigger) override {
    if (trigger == ui::PointerButton::Left) {
      return std::make_unique<ChangeEnumKnobAction>(pointer, *this);
    }
    return nullptr;
  }
};

struct ConditionCodeWidget : public EnumKnobWidget {
  WeakPtr<Instruction> instruction_weak;
  int token_i;

  std::optional<Wave1D> wave;
  animation::SpringV2<float> water_level;
  animation::SpringV2<float> spill_tween;
  std::optional<Vec2> root_position;

  ConditionCodeWidget(ui::Widget* parent, WeakPtr<Instruction> instruction_weak, int token_i)
      : EnumKnobWidget(parent, X86::CondCode::LAST_VALID_COND + 1),
        instruction_weak(instruction_weak),
        token_i(token_i) {}

  int KnobGet() const override {
    auto instruction = instruction_weak.Lock().Cast<Instruction>();
    return (X86::CondCode)instruction->mc_inst.getOperand(token_i).getImm();
  }

  void KnobSet(int new_value) override {
    auto instruction = instruction_weak.Lock().Cast<Instruction>();
    instruction->mc_inst.getOperand(token_i).setImm(new_value);
    if (auto assembler = FindAssembler(*instruction)) {
      assembler->UpdateMachineCode();
    }
  }

  animation::Phase Tick(time::Timer& timer) override {
    auto phase = EnumKnobWidget::Tick(timer);

    phase |= water_level.SineTowards((float)(value == X86::CondCode::COND_O), timer.d, 2);
    phase |= spill_tween.SineTowards((water_level == 1) || (water_level > 0 && spill_tween == 1),
                                     timer.d, 5);
    if (water_level > 0 && !wave.has_value()) {
      wave = Wave1D(30, 0.5, 0.005, 1);
      if (auto* _mw = ToyStore().FindOrNull(*root_machine)) {
        root_position = ui::TransformBetween(*this, *_mw).mapPoint({0, 0});
      } else {
        root_position = Vec2{};
      }
    } else if (water_level == 0 && wave.has_value()) {
      wave.reset();
    }
    if (wave) {
      Vec2 new_position;
      if (auto* _mw = ToyStore().FindOrNull(*root_machine)) {
        new_position = ui::TransformBetween(*this, *_mw).mapPoint({0, 0});
      }
      auto delta = new_position - *root_position;
      root_position = new_position;

      float vx = delta.x / timer.d;
      float ax = (vx - last_vx) / timer.d;
      last_vx = vx;

      float dvx = ax * timer.d;
      float dx = dvx * timer.d;

      if (abs(dx) > 0.001_mm) {
        float column_width = kWaterRadius * 2 / wave->n;

        auto new_heights = std::vector<float>(wave->n);
        auto new_velocity = std::vector<float>(wave->n);
        auto amplitude = wave->Amplitude();
        auto velocity = wave->Velocity();
        for (int i = 0; i < wave->n; ++i) {
          // Take the current column height and distribute it to target columns A & B
          float target_i = std::clamp<float>(i - dx / column_width, 0, wave->n - 1);
          float target_floor = floorf(target_i);
          float target_ceil = ceilf(target_i);

          float x = 2 * (i + 0.5f) / wave->n - 1;
          float y = sqrtf(1 - x * x);

          float t = target_i - target_floor;
          float h = (amplitude[i] + 1) * y;
          float v = velocity[i];
          new_heights[target_floor] += h * (1 - t);
          new_heights[target_ceil] += h * t;
          new_velocity[target_floor] += v * (1 - t);
          new_velocity[target_ceil] += v * t;
        }
        for (int i = 0; i < wave->n - 1; ++i) {
          float x = (i + 0.5f) / wave->n * 2 - 1;
          float y = sqrtf(1 - x * x);
          float max_volume = y * 2;
          if (new_heights[i] > max_volume) {  // carry forward
            new_heights[i + 1] += new_heights[i] - max_volume;
            new_heights[i] = max_volume;
          }
        }
        for (int i = wave->n - 1; i > 0; --i) {
          float x = (i + 0.5f) / wave->n * 2 - 1;
          float y = sqrtf(1 - x * x);
          float max_volume = y * 2;
          if (new_heights[i] > max_volume) {  // carry backward
            new_heights[i - 1] += new_heights[i] - max_volume;
            new_heights[i] = max_volume;
          }
        }
        for (int i = 0; i < wave->n; ++i) {
          float x = (i + 0.5f) / wave->n * 2 - 1;
          float y = sqrtf(1 - x * x);
          float max_volume = y * 2;

          amplitude[i] = std::clamp<float>((new_heights[i] / y - 1), -1, 1);
          velocity[i] = new_velocity[i];
        }

        for (int i = 0; i < wave->n; ++i) {
          if (amplitude[i] >= 1 || amplitude[i] <= 0) {
            velocity[i] = 0;
          }
        }
      }

      phase |= wave->Tick(timer);
      wave->ZeroMeanAmplitude();
    }
    return phase;
  }

  void DrawKnobBackground(SkCanvas& canvas, int val) const override {
    if (spill_tween > 0) {
      static const SkPath kSpill = PathFromSVG(
          "M-3.69-3.13c-.01 0-.03 0-.05 0-.43.05-.35.89-.75 "
          "1.05-.24.09-.51-.25-.75-.15-.27.11-.09.61-.5.73-.32.08.1.71.06 "
          "1.06-.05.3-.34.56-.29.85.04.27.42.39.5.65.06.18-.1.39-.04.58.08.21.25.43.46.51.2.08."
          "43-.12.63-.06.24.08.34.43.59.49.14.04.31-.01.44-.08.07-.04.12-.1.15-.17 0 0 0 0 0 0A4 "
          "4 0 01-4 0a4 4 0 01.93-2.56s0 0 0 0c-.11-.25-.35-.56-.62-.57z",
          SVGUnit_Millimeters);
      SkPaint spill_fill;
      spill_fill.setAlphaf(spill_tween * 0.25);
      canvas.save();
      float spill_scale = lerp(0.7f, 1, spill_tween);
      canvas.scale(spill_scale, spill_scale);
      canvas.drawPath(kSpill, spill_fill);
      canvas.restore();
    }
    EnumKnobWidget::DrawKnobBackground(canvas, val);
    DrawConditionCodeBackground(canvas, (X86::CondCode)val);
  }

  void DrawKnobSymbol(SkCanvas& canvas, int val) const override {
    DrawConditionCodeSymbol(canvas, (X86::CondCode)val);
  }

  // Draws the waving water
  void DrawKnobBelowGlass(SkCanvas& canvas) const override {
    if (!wave.has_value()) return;
    SkPath water;
    SinCos angle_0;
    SinCos angle_n;
    for (int i = 0; i < wave->n; ++i) {
      float x = i * kWaterOval.Width() / (wave->n - 1) + kWaterOval.left;
      float y = wave->state[i] * kWaterRadius +
                std::lerp(kWaterOval.bottom, kWaterOval.CenterY(), water_level);
      float d = hypotf(x, y);
      float max_d = i * (i - wave->n + 1.f) / wave->n / wave->n * kWaterRadius / 8 + kWaterRadius;
      if (d > max_d) {
        x *= max_d / d;
        y *= max_d / d;
        d = max_d;
      }
      if (i == 0) {
        angle_0 = SinCos::FromVec2({x, y}, d);
        water.moveTo(x, y);
      } else {
        water.lineTo(x, y);
      }
      if (i == wave->n - 1) {
        angle_n = SinCos::FromVec2({x, y}, d);
      }
    }
    float start_deg = angle_n.ToDegrees();
    float sweep_deg = (angle_0 - angle_n).ToDegreesNegative();
    water.arcTo(kWaterOval.sk, start_deg, sweep_deg, false);
    water.close();
    water.toggleInverseFillType();

    SkPaint displacement_paint;
    displacement_paint.setImageFilter(SkImageFilters::Magnifier(
        kWaterOval.sk, 1.5, kWaterRadius * 0.9, kDefaultSamplingOptions, nullptr));

    SkCanvas::SaveLayerRec mask_rec(&kWaterOval.sk, nullptr,
                                    SkCanvas::kInitWithPrevious_SaveLayerFlag);
    SkCanvas::SaveLayerRec displacement_rec(&kWaterOval.sk, &displacement_paint,
                                            SkCanvas::kInitWithPrevious_SaveLayerFlag);
    canvas.saveLayer(mask_rec);
    canvas.saveLayer(displacement_rec);
    canvas.restore();
    SkPaint clear_paint;
    clear_paint.setBlendMode(SkBlendMode::kClear);
    canvas.drawPath(water, clear_paint);
    SkPaint inner_shadow_paint;
    inner_shadow_paint.setColor("#a3b8c6"_color);
    // inner_shadow_paint.setColor("#def3ffd5"_color);
    inner_shadow_paint.setBlendMode(SkBlendMode::kMultiply);
    inner_shadow_paint.setMaskFilter(
        SkMaskFilter::MakeBlur(kOuter_SkBlurStyle, kWaterRadius * 0.1f, true));
    canvas.drawPath(water, inner_shadow_paint);
    canvas.restore();
  }

  // Draws the glass crack
  void DrawKnobOverGlass(SkCanvas& canvas) const override {
    if (water_level == 0) return;
    static const SkPath kCracks[] = {
        PathFromSVG("m-4.01.02.01.27.27-.23.37-.17z", SVGUnit_Millimeters),
        PathFromSVG("m-4-.06c-.01.2 0 .26.02.48l.28-.26.36-.16.48.08-.2-.19-.3-.1z",
                    SVGUnit_Millimeters),
        PathFromSVG(
            "m-3.38-.32-.62.17c-.02.2.01.5.04.72l.3-.29.35-.19.44.16.72-.41-.79.07-.13-.13z",
            SVGUnit_Millimeters),
        PathFromSVG("M-3.06-.23-2.97-.15-2.23-.31-1.6.01-2.21.04-2.87.34-3.31.13-3.61.32-3.92."
                    "75C-3.97.5-4 "
                    ".25-4 0-4-.07-4-.13-3.99-.2L-3.38-.36Z",
                    SVGUnit_Millimeters),
    };
    SkPaint crack_fill;
    float crack_tween = lerp(0, std::size(kCracks) - 1, clamp<float>(water_level * 4, 0, 1));
    int crack_i = roundf(crack_tween);
    canvas.drawPath(kCracks[crack_i], crack_fill);
  }
};

struct LoopConditionCodeWidget : public EnumKnobWidget {
  WeakPtr<Instruction> instruction_weak;

  LoopConditionCodeWidget(Widget* parent, WeakPtr<Instruction> instruction_weak)
      : EnumKnobWidget(parent, 2), instruction_weak(instruction_weak) {}

  int KnobGet() const override {
    auto instruction = instruction_weak.Lock().Cast<Instruction>();
    auto opcode = instruction->mc_inst.getOpcode();
    if (opcode == X86::LOOPE) {
      return 0;
    } else {
      return 1;
    }
  }

  void KnobSet(int new_value) override {
    auto instruction = instruction_weak.Lock().Cast<Instruction>();
    auto opcode = instruction->mc_inst.getOpcode();
    if (new_value == 1 && opcode == X86::LOOPE) {
      instruction->mc_inst.setOpcode(X86::LOOPNE);
    } else if (new_value == 0 && opcode == X86::LOOPNE) {
      instruction->mc_inst.setOpcode(X86::LOOPE);
    } else {
      LOG << "Can't set condition code for loop instruction";
    }
    if (auto assembler = FindAssembler(*instruction)) {
      assembler->UpdateMachineCode();
    }
  }

  void DrawKnobBackground(SkCanvas& canvas, int val) const override {
    EnumKnobWidget::DrawKnobBackground(canvas, val);
    DrawConditionCodeBackground(canvas, X86::CondCode::COND_E);
  }

  void DrawKnobSymbol(SkCanvas& canvas, int val) const override {
    X86::CondCode cond = val == 0 ? X86::CondCode::COND_E : X86::CondCode::COND_NE;
    DrawConditionCodeSymbol(canvas, cond);
  }
};

Instruction::Widget::Widget(ui::Widget* parent, Object& object) : Toy(parent, object) {
  auto instruction = LockObject<Instruction>();
  if (instruction->BufferSize() > 0) {
    auto buffer_ptr =
        NestedWeakPtr<Buffer>(instruction->AcquireWeakPtr<ReferenceCounted>(), instruction.Get());
    imm_widget = std::make_unique<ui::SmallBufferWidget>(this, std::move(buffer_ptr));
    imm_widget->local_to_parent.setIdentity();
    imm_widget->fonts[(int)Buffer::Type::Text] = &HeavyFont();
    imm_widget->fonts[(int)Buffer::Type::Unsigned] = &HeavyFont();
    imm_widget->fonts[(int)Buffer::Type::Signed] = &HeavyFont();
    imm_widget->fonts[(int)Buffer::Type::Hexadecimal] = &HeavyFont();
    imm_widget->Measure();
  }
  auto tokens = PrintInstruction(instruction->mc_inst);
  for (int token_i = 0; token_i < tokens.size(); ++token_i) {
    auto& token = tokens[token_i];
    if (token.tag == Token::ConditionCode || token.tag == Token::FixedCondition) {
      auto opcode = instruction->mc_inst.getOpcode();
      unique_ptr<EnumKnobWidget> cond_widget;
      if (opcode == X86::LOOPE || opcode == X86::LOOPNE) {
        cond_widget = make_unique<LoopConditionCodeWidget>(this, instruction->AcquireWeakPtr());
      } else {
        cond_widget =
            make_unique<ConditionCodeWidget>(this, instruction->AcquireWeakPtr(), token.cond_code);
      }
      cond_widget->local_to_parent.setIdentity();
      condition_code_widget = std::move(cond_widget);
    }
  }
}

PersistentImage reverse = PersistentImage::MakeFromAsset(
    embedded::assets_card_reverse_webp,
    PersistentImage::MakeArgs{.width = Instruction::Widget::kWidth -
                                       Instruction::Widget::kBorderMargin * 2});

animation::Phase Instruction::Widget::Tick(time::Timer& timer) {
  auto instruction = this->LockObject<Instruction>();
  auto& inst = instruction->mc_inst;

  tokens = PrintInstruction(inst);
  auto& heavy_font = HeavyFont();

  // Measure the lines
  float token_width_min[tokens.size()];
  float token_width_max[tokens.size()];
  float token_width_base[tokens.size()];
  SmallVector<float, 6> line_widths_min;
  SmallVector<float, 6> line_widths_max;
  line_widths_min.emplace_back(0);
  line_widths_max.emplace_back(0);
  for (int token_i = 0; token_i < tokens.size(); ++token_i) {
    auto& token = tokens[token_i];
    if (token.tag == Token::BreakLine) {
      line_widths_min.emplace_back(0);
      line_widths_max.emplace_back(0);
      continue;
    }
    switch (token.tag) {
      case Token::String: {
        token_width_base[token_i] = heavy_font.MeasureText(token.str);
        break;
      }
      case Token::RegisterOperand:  // fallthrough
      case Token::FixedRegister:
        token_width_base[token_i] = kRegisterTokenWidth;
        break;
      case Token::ImmediateOperand:
        token_width_base[token_i] = imm_widget->width;
        break;
      case Token::FixedFlag:
        token_width_base[token_i] = kFixedFlagWidth;
        break;
      case Token::ConditionCode:  // fallthrough
      case Token::FixedCondition:
        token_width_base[token_i] = kConditionCodeTokenWidth;
        break;
      default:
        break;
    }
    if (token.tag == Token::String) {
      token_width_min[token_i] = token_width_base[token_i] * kMinTextScale;
      token_width_max[token_i] = token_width_base[token_i] * kMaxTextScale;
    } else {
      token_width_min[token_i] = token_width_max[token_i] = token_width_base[token_i];
    }
    line_widths_min.back() += token_width_min[token_i];
    line_widths_max.back() += token_width_max[token_i];
  }
  int n_lines = line_widths_min.size();

  // Place tokens
  token_position.resize(tokens.size());
  string_width_scale.resize(tokens.size());
  scale = 1;
  {
    float longest_line_width = 0;
    for (int line = 0; line < n_lines; ++line) {
      longest_line_width = std::max(longest_line_width, line_widths_min[line]);
    }
    // Figure out maximum scale
    Rect natural_size = {
        /* left */
        kXCenter - longest_line_width / 2,
        /* bottom */ kYCenter - kLineHeight * ((int)line_widths_min.size()) / 2,
        /* right */ kXCenter + longest_line_width / 2,
        /* top */ kYCenter + kLineHeight * ((int)line_widths_min.size()) / 2,
    };

    scale = std::min((kYCenter - kYMin) / (kYCenter - natural_size.bottom),
                     (kYMax - kYCenter) / (natural_size.top - kYCenter));
    scale = std::min(scale, kXRange / natural_size.Width());

    float line_width_f[n_lines];
    for (int line = 0; line < n_lines; ++line) {
      float line_width_min = line_widths_min[line] * scale;
      float line_width_max = line_widths_max[line] * scale;
      line_width_f[line] = Saturate((kXRange - line_width_min) / (line_width_max - line_width_min));
    }

    int line = 0;
    float x = kXCenter - lerp(line_widths_min[line], line_widths_max[line], line_width_f[line]) / 2;
    float y = kYCenter - kHeavyFontSize / 2 + kLineHeight * (n_lines - 1) / 2;
    for (int token_i = 0; token_i < tokens.size(); ++token_i) {
      auto& token = tokens[token_i];
      if (token.tag == Token::BreakLine) {
        line++;
        x = kXCenter - lerp(line_widths_min[line], line_widths_max[line], line_width_f[line]) / 2;
        y -= kLineHeight;
        continue;
      } else if (token.tag == Token::ImmediateOperand || token.tag == Token::ConditionCode ||
                 token.tag == Token::FixedCondition) {
        SkMatrix mat = SkMatrix::I();
        mat.preScale(scale, scale, kXCenter, kYCenter);
        mat.preTranslate(x, y);
        if (token.tag == Token::ImmediateOperand) {
          imm_widget->local_to_parent = SkM44(mat);
        } else {
          mat.preTranslate(kConditionCodeTokenWidth / 2, kConditionCodeTokenWidth / 2 - 2_mm);
          condition_code_widget->local_to_parent = SkM44(mat);
        }
      }
      token_position[token_i] = Vec2{x, y};
      string_width_scale[token_i] = lerp(kMinTextScale, kMaxTextScale, line_width_f[line]);
      if (token.tag == Token::String) {
        x += lerp(token_width_min[token_i], token_width_max[token_i], line_width_f[line]);
      } else {
        x += token_width_min[token_i];
      }
    }
  }

  return animation::Finished;
}

void Instruction::Widget::Draw(SkCanvas& canvas) const {
  auto instruction = this->LockObject<Instruction>();
  auto& inst = instruction->mc_inst;

  auto mat = canvas.getLocalToDeviceAs3x3();
  float det = mat.rc(0, 0) * mat.rc(1, 1) - mat.rc(0, 1) * mat.rc(1, 0);
  bool is_flipped = det > 0;
  SkRRect rrect = kInstructionRRect;

  // Paper fill
  auto color = "#e6e6e6"_color;
  SkPaint paper_paint;
  auto color_shader = SkShaders::Color(color);
  auto color_transparent_shader = SkShaders::Color(SK_ColorTRANSPARENT);
  auto paper_transparent_shader = SkShaders::Blend(SkBlenders::Arithmetic(0, 0.89, 0.11, 0, false),
                                                   *paper_texture.shader, color_transparent_shader);
  auto overlayed_shader =
      SkShaders::Blend(SkBlendMode::kOverlay, color_shader, paper_transparent_shader);
  paper_paint.setShader(overlayed_shader);
  canvas.drawRRect(rrect, paper_paint);

  constexpr float kHeight = Instruction::Widget::kHeight;
  {  // Vignette
    SkPaint vignette_paint;
    float r = hypotf(Instruction::Widget::kWidth, kHeight) / 2;
    SkColor colors[2] = {"#20100800"_color, "#20100810"_color};
    vignette_paint.setShader(
        SkGradientShader::MakeRadial(SkPoint::Make(Instruction::Widget::kWidth / 2, kHeight / 2), r,
                                     colors, nullptr, 2, SkTileMode::kClamp));
    canvas.drawRRect(kInstructionRRect, vignette_paint);
  }

  // Bevel
  SkPoint points[2] = {SkPoint::Make(0, kHeight), SkPoint::Make(0, 0)};
  SkColor colors[4] = {
      "#ffffff"_color,
      "#cccccc"_color,
      "#bbbbbb"_color,
      "#888888"_color,
  };
  float pos[4] = {0, 3_mm / kHeight, 1 - 3_mm / kHeight, 1};
  float bevel_width = 0.4_mm;

  SkPaint bevel_paint;
  bevel_paint.setShader(SkGradientShader::MakeLinear(points, colors, pos, 4, SkTileMode::kClamp));
  bevel_paint.setAntiAlias(true);
  bevel_paint.setStyle(SkPaint::kStroke_Style);
  bevel_paint.setStrokeWidth(bevel_width);
  bevel_paint.setAlphaf(0.5);

  SkRRect inset_rrect = rrect;
  inset_rrect.inset(bevel_width / 2, bevel_width / 2);
  canvas.drawRRect(inset_rrect, bevel_paint);

  if (is_flipped) {
    canvas.translate(Instruction::Widget::kWidth / 2, Instruction::Widget::kHeight / 2);
    canvas.translate(-reverse.width() / 2, -reverse.height() / 2);
    reverse.draw(canvas);
    return;
  }

  auto& subscript_font = SubscriptFont();
  // Assembly text
  auto assembly_text = AssemblyText(inst);
  auto& fine_font = FineFont();
  using Widget = Instruction::Widget;
  auto assembly_text_width = fine_font.MeasureText(assembly_text);
  Vec2 assembly_text_offset = Vec2{Widget::kBorderMargin - kFineFontSize / 2,
                                   kHeight - kFineFontSize / 2 - Widget::kBorderMargin};

  auto machine_text = MachineText(inst);
  auto machine_text_width = fine_font.MeasureText(machine_text);
  Vec2 machine_text_offset =
      Vec2{Widget::kWidth - Widget::kBorderMargin + kFineFontSize / 2 - machine_text_width,
           -kFineFontSize / 2 + Widget::kBorderMargin};

  {
    canvas.save();
    SkPaint text_paint;
    text_paint.setColor("#000000"_color);
    text_paint.setAntiAlias(true);
    canvas.translate(assembly_text_offset.x, assembly_text_offset.y);
    fine_font.DrawText(canvas, assembly_text, text_paint);
    canvas.restore();

    canvas.save();
    canvas.translate(machine_text_offset.x, machine_text_offset.y);
    fine_font.DrawText(canvas, machine_text, text_paint);
    canvas.restore();
  }

  {  // Border
    canvas.save();
    Rect asm_rect = Rect::MakeCornerZero(assembly_text_width, kFineFontSize)
                        .Outset(kFineFontSize / 2)
                        .MoveBy(assembly_text_offset);
    canvas.clipRect(asm_rect, SkClipOp::kDifference);
    Rect code_rect = Rect::MakeCornerZero(machine_text_width, kFineFontSize)
                         .Outset(kFineFontSize / 2)
                         .MoveBy(machine_text_offset);
    canvas.clipRect(code_rect, SkClipOp::kDifference);
    SkPaint border_paint;
    border_paint.setColor("#000000"_color);
    border_paint.setAntiAlias(true);
    border_paint.setStyle(SkPaint::kStroke_Style);
    border_paint.setStrokeWidth(0.1_mm);
    SkRRect border_rrect = rrect;
    border_rrect.inset(Widget::kBorderMargin, Widget::kBorderMargin);
    SkVector radii[] = {
        SkVector::Make(1_mm, 1_mm),
        SkVector::Make(1_mm, 1_mm),
        SkVector::Make(1_mm, 1_mm),
        SkVector::Make(1_mm, 1_mm),
    };
    border_rrect.setRectRadii(border_rrect.rect(), radii);
    canvas.drawRRect(border_rrect, border_paint);
    canvas.restore();
  }

  {
    // Contents

    auto& heavy_font = HeavyFont();

    SkPaint text_paint;
    text_paint.setColor("#000000"_color);
    text_paint.setAntiAlias(true);

    auto& assembler = LLVM_Assembler::Get();

    auto default_mat = canvas.getLocalToDevice();
    auto mat = default_mat.asM33();
    mat.preScale(scale, scale, kXCenter, kYCenter);

    for (int token_i = 0; token_i < tokens.size(); ++token_i) {
      auto& token = tokens[token_i];
      if (token_i >= token_position.size()) {
        LOG << "Token " << token_i << " is out of bounds";
        continue;
      }
      canvas.setMatrix(mat);
      switch (token.tag) {
        case Token::String: {
          canvas.translate(token_position[token_i].x, token_position[token_i].y);
          canvas.scale(string_width_scale[token_i], 1);
          heavy_font.DrawText(canvas, token.str, text_paint);
          break;
        }
        case Token::ConditionCode:
        case Token::FixedCondition:
        case Token::ImmediateOperand: {
          // Drawn as a widget
          break;
        }
        case Token::FixedRegister: {
          canvas.translate(token_position[token_i].x, token_position[token_i].y - 2_mm);
          canvas.scale(kRegisterIconScale, kRegisterIconScale);
          for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
            bool found =
                assembler.mc_reg_info->isSubRegisterEq(kRegisters[i].llvm_reg, token.fixed_reg);
            if (found) {
              static sk_sp<SkColorFilter> filter = color::MakeTintFilter("#3d9bd1"_color, 40);
              kRegisters[i].image.paint.setColorFilter(filter);
              // kRegisters[i].image.paint.setColorFilter(color::DesaturateFilter());
              kRegisters[i].image.draw(canvas);
              kRegisters[i].image.paint.setColorFilter(nullptr);
              break;
            }
          }
          canvas.setMatrix(mat);
          {
            std::string text;
            for (int reg_class_i = 0; reg_class_i < assembler.mc_reg_info->getNumRegClasses();
                 ++reg_class_i) {
              auto& reg_class = assembler.mc_reg_info->getRegClass(reg_class_i);
              if (reg_class.contains(token.fixed_reg)) {
                text = std::to_string(reg_class.RegSizeInBits);
                break;
              }
            }
            auto text_width = subscript_font.MeasureText(text);
            canvas.translate(token_position[token_i].x + kRegisterIconWidth / 2 - text_width / 2,
                             token_position[token_i].y - 2_mm - kSubscriptFontSize / 2);
            subscript_font.DrawText(canvas, text, text_paint);
          }
          break;
        }
        case Token::RegisterOperand: {
          canvas.translate(token_position[token_i].x, token_position[token_i].y - 2_mm);
          canvas.scale(kRegisterIconScale, kRegisterIconScale);
          for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
            bool found = assembler.mc_reg_info->isSubRegisterEq(
                kRegisters[i].llvm_reg, inst.getOperand(token.reg).getReg());
            if (found) {
              kRegisters[i].image.draw(canvas);
              break;
            }
          }
          canvas.setMatrix(mat);
          {
            auto& instr_info = assembler.mc_instr_info->get(inst.getOpcode());
            auto reg_class = instr_info.operands()[token.reg].RegClass;
            auto& class_info = assembler.mc_reg_info->getRegClass(reg_class);
            auto text = std::to_string(class_info.RegSizeInBits);
            auto text_width = subscript_font.MeasureText(text);
            canvas.translate(token_position[token_i].x + kRegisterIconWidth / 2 - text_width / 2,
                             token_position[token_i].y - 2_mm - kSubscriptFontSize / 2);
            subscript_font.DrawText(canvas, text, text_paint);
          }
          break;
        }
        case Token::FixedFlag: {
          canvas.translate(token_position[token_i].x + 1_mm, token_position[token_i].y - 2_mm);
          DrawFlag(canvas, token.flag);
          break;
        }
        default:
          break;
      }
    }
    canvas.setMatrix(default_mat);
  }

  DrawChildren(canvas);
}

Vec2AndDir Instruction::Widget::ArgStart(const Argument& arg, ui::Widget* coordinate_space) {
  if (&arg == &jump_arg) {
    Vec2AndDir pos_dir{.pos = kRect.RightCenter(), .dir = 0_deg};
    if (coordinate_space) {
      auto m = TransformBetween(*this, *coordinate_space);
      pos_dir.pos = m.mapPoint(pos_dir.pos);
    }
    return pos_dir;
  }
  return Object::Toy::ArgStart(arg, coordinate_space);
}

void Instruction::Widget::FillChildren(Vec<ui::Widget*>& children) {
  if (imm_widget) {
    children.emplace_back(imm_widget.get());
  }
  if (condition_code_widget) {
    children.emplace_back(condition_code_widget.get());
  }
}

void Instruction::SerializeState(ObjectSerializer& writer) const {
  auto& assembler = LLVM_Assembler::Get();
  writer.Key("opcode");
  auto opcode_name = assembler.mc_instr_info->getName(mc_inst.getOpcode());
  writer.String(opcode_name.data(), opcode_name.size());
  auto this_mut = const_cast<Instruction*>(this);
  auto imm_bytes = this_mut->BufferRead();
  if (!imm_bytes.empty()) {
    writer.Key("immediate_mode");
    switch (imm_type) {
      case Buffer::Type::Signed:
        writer.String("signed");
        break;
      case Buffer::Type::Unsigned:
        writer.String("unsigned");
        break;
      case Buffer::Type::Hexadecimal:
        writer.String("hexadecimal");
        break;
      case Buffer::Type::Text:
        writer.String("text");
        break;
      default:
        ERROR << "Can't serialize unknown immediate operand";
        writer.Null();
        break;
    }
  }
  if (mc_inst.getNumOperands() > 0) {
    writer.Key("operands");
    writer.StartArray();
    bool has_imm = false;
    for (int i = 0; i < mc_inst.getNumOperands(); ++i) {
      auto& operand = mc_inst.getOperand(i);
      if (operand.isImm()) {
        has_imm = true;
        if (imm_type == Buffer::Type::Signed) {
          writer.Int64(operand.getImm());
        } else if (imm_type == Buffer::Type::Unsigned) {
          writer.Uint64(operand.getImm());
        } else if (imm_type == Buffer::Type::Hexadecimal) {
          while (imm_bytes.size() < 8) {
            imm_bytes += '\0';
          }
          uint64_t value = *reinterpret_cast<uint64_t*>(imm_bytes.data());
          auto str = f("{:x}", value);
          writer.String(str.data(), str.size());
        } else if (imm_type == Buffer::Type::Text) {
          writer.String(imm_bytes.data(), imm_bytes.size());
        }
      } else if (operand.isReg()) {
        writer.String(assembler.mc_reg_info->getName(operand.getReg()));
      } else {
        writer.Null();
      }
    }
    writer.EndArray();
  }
}

bool Instruction::DeserializeKey(ObjectDeserializer& d, StrView key) {
  static StringMap<unsigned> opcode_map = [] {
    auto& assembler = LLVM_Assembler::Get();
    StringMap<unsigned> map;
    for (int i = 0; i < assembler.mc_instr_info->getNumOpcodes(); ++i) {
      map[assembler.mc_instr_info->getName(i)] = i;
    }
    return map;
  }();
  static StringMap<unsigned> reg_map = [] {
    auto& assembler = LLVM_Assembler::Get();
    StringMap<unsigned> map;
    for (int i = 0; i < assembler.mc_reg_info->getNumRegs(); ++i) {
      map[assembler.mc_reg_info->getName(i)] = i;
    }
    return map;
  }();

  auto& assembler = LLVM_Assembler::Get();
  Status status;

  if (key == "opcode") {
    Str opcode_name;
    d.Get(opcode_name, status);
    if (!OK(status)) {
      AppendErrorMessage(status) += "Opcode name must be a string";
    } else {
      auto opcode_i = opcode_map.find(opcode_name);
      if (opcode_i == opcode_map.end()) {
        AppendErrorMessage(status) += "Opcode name is not a valid x86 LLVM opcode name";
      } else {
        mc_inst.setOpcode(opcode_i->second);
      }
    }
  } else if (key == "immediate_mode") {
    Str mode_name;
    d.Get(mode_name, status);
    if (!OK(status)) {
      AppendErrorMessage(status) += "Immediate mode must be a string";
    } else if (mode_name == "signed") {
      imm_type = Buffer::Type::Signed;
    } else if (mode_name == "unsigned") {
      imm_type = Buffer::Type::Unsigned;
    } else if (mode_name == "hexadecimal") {
      imm_type = Buffer::Type::Hexadecimal;
    } else if (mode_name == "text") {
      imm_type = Buffer::Type::Text;
    } else {
      AppendErrorMessage(status) += "Unknown immediate mode";
    }
  } else if (key == "operands") {
    auto& instr_info = assembler.mc_instr_info->get(mc_inst.getOpcode());
    for (auto operand_i : ArrayView(d, status)) {
      if (operand_i >= instr_info.getNumOperands()) {
        auto& msg = AppendErrorMessage(status);
        msg += "Too many operands for ";
        msg += assembler.mc_instr_info->getName(mc_inst.getOpcode());
        break;
      }
      auto& operand = instr_info.operands()[operand_i];
      if (operand.OperandType == MCOI::OPERAND_REGISTER) {
        // Recover register index from string
        Str reg_name;
        d.Get(reg_name, status);
        if (!OK(status)) {
          auto& msg = AppendErrorMessage(status);
          msg += "Operand ";
          msg += std::to_string(operand_i);
          msg += " must be a valid x86 LLVM register name";
          break;
        }
        auto reg_i = reg_map.find(reg_name);
        if (reg_i == reg_map.end()) {
          auto& msg = AppendErrorMessage(status);
          msg += "Operand ";
          msg += std::to_string(operand_i);
          msg += " must be a valid x86 LLVM register name";
          break;
        }
        mc_inst.addOperand(MCOperand::createReg(reg_i->second));
      } else {
        // Immediate
        if (imm_type == Buffer::Type::Signed) {
          int64_t immediate;
          d.Get(immediate, status);
          if (!OK(status)) {
            auto& msg = AppendErrorMessage(status);
            msg += "Operand ";
            msg += std::to_string(operand_i);
            msg += " of ";
            msg += assembler.mc_instr_info->getName(mc_inst.getOpcode());
            msg += " must be an integer";
            break;
          }
          mc_inst.addOperand(MCOperand::createImm(immediate));
        } else if (imm_type == Buffer::Type::Unsigned) {
          uint64_t immediate;
          d.Get(immediate, status);
          if (!OK(status)) {
            auto& msg = AppendErrorMessage(status);
            msg += "Operand ";
            msg += std::to_string(operand_i);
            msg += " of ";
            msg += assembler.mc_instr_info->getName(mc_inst.getOpcode());
            msg += " must be an unsigned integer";
            break;
          }
          mc_inst.addOperand(MCOperand::createImm(immediate));
        } else if (imm_type == Buffer::Type::Hexadecimal || imm_type == Buffer::Type::Text) {
          Str str;
          d.Get(str, status);
          if (!OK(status)) {
            auto& msg = AppendErrorMessage(status);
            msg += "Operand ";
            msg += std::to_string(operand_i);
            msg += " of ";
            msg += assembler.mc_instr_info->getName(mc_inst.getOpcode());
            msg += " must be a string";
            break;
          }
          if (imm_type == Buffer::Type::Hexadecimal) {
            uint64_t value = 0;
            sscanf(str.data(), "%lx", &value);
            mc_inst.addOperand(MCOperand::createImm(value));
          } else {
            while (str.size() < 8) {
              str += '\0';
            }
            int64_t value = *reinterpret_cast<int64_t*>(str.data());
            mc_inst.addOperand(MCOperand::createImm(value));
          }
        }
      }
    }
  } else {
    return false;
  }

  if (!OK(status)) {
    ReportError(status.ToStr());
  }
  return true;
}

std::unique_ptr<Object::Toy> Instruction::MakeToy(ui::Widget* parent) {
  return make_unique<Widget>(parent, *this);
}
}  // namespace automat::library
