// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_instruction.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkColor.h>
#include <include/core/SkMaskFilter.h>
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

#include "argument.hh"
#include "color.hh"
#include "drawable.hh"
#include "embedded.hh"
#include "font.hh"
#include "library_assembler.hh"
#include "llvm_asm.hh"
#include "math.hh"
#include "svg.hh"
#include "textures.hh"

using namespace std;
using namespace llvm;
using namespace maf;

namespace automat::library {

RegisterPresentation kRegisters[kGeneralPurposeRegisterCount] = {
    RegisterPresentation{
        .image =
            PersistentImage::MakeFromAsset(maf::embedded::assets_reg_ax_webp,
                                           PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
        .llvm_reg = X86::RAX,
        .name = "RAX",
    },
    RegisterPresentation{
        .image =
            PersistentImage::MakeFromAsset(maf::embedded::assets_reg_bx_webp,
                                           PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
        .llvm_reg = X86::RBX,
        .name = "RBX",
    },
    RegisterPresentation{
        .image =
            PersistentImage::MakeFromAsset(maf::embedded::assets_reg_cx_webp,
                                           PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
        .llvm_reg = X86::RCX,
        .name = "RCX",
    },
    RegisterPresentation{
        .image =
            PersistentImage::MakeFromAsset(maf::embedded::assets_reg_dx_webp,
                                           PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
        .llvm_reg = X86::RDX,
        .name = "RDX",
    },
    RegisterPresentation{
        .image =
            PersistentImage::MakeFromAsset(maf::embedded::assets_reg_si_webp,
                                           PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
        .llvm_reg = X86::RSI,
        .name = "RSI",
    },
    RegisterPresentation{
        .image =
            PersistentImage::MakeFromAsset(maf::embedded::assets_reg_di_webp,
                                           PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
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

JumpArgument::JumpArgument() : Argument("Jump") {}

PaintDrawable& JumpArgument::Icon() { return jump_icon; }

JumpArgument jump_arg;

Argument assembler_arg = []() {
  Argument arg("Assembler", Argument::kRequiresObject);
  arg.RequireInstanceOf<Assembler>();
  arg.autoconnect_radius = INFINITY;
  arg.tint = "#ff0000"_color;
  arg.style = Argument::Style::Invisible;
  return arg;
}();

static Assembler* FindAssembler(Location& here) {
  Assembler* assembler =
      assembler_arg.FindObject<Assembler>(here, {.if_missing = Argument::IfMissing::ReturnNull});
  return assembler;
}

static Assembler* FindOrCreateAssembler(Location& here) {
  Assembler* assembler = assembler_arg.FindObject<Assembler>(
      here, {.if_missing = Argument::IfMissing::CreateFromPrototype});
  return assembler;
}

void Instruction::Args(std::function<void(Argument&)> cb) {
  auto opcode = mc_inst.getOpcode();
  if (opcode != X86::JMP_1 && opcode != X86::JMP_4) {
    cb(next_arg);
  }
  cb(assembler_arg);
  auto& assembler = LLVM_Assembler::Get();
  auto& info = assembler.mc_instr_info->get(opcode);
  if (info.isBranch()) {
    cb(jump_arg);
  }
}

std::shared_ptr<Object> Instruction::ArgPrototype(const Argument& arg) {
  if (&arg == &assembler_arg) {
    return std::make_shared<Assembler>();
  }
  return nullptr;
}

void Instruction::ConnectionAdded(Location& here, Connection& connection) {
  if (&connection.argument == &assembler_arg) {
    if (auto assembler = connection.to.As<Assembler>()) {
      assembler->UpdateMachineCode();
    }
  } else if (&connection.argument == &jump_arg || &connection.argument == &next_arg) {
    if (auto assembler = FindAssembler(here)) {
      assembler->UpdateMachineCode();
    }
  }
}

void Instruction::ConnectionRemoved(Location& here, Connection& connection) {
  if (&connection.argument == &assembler_arg) {
    if (auto assembler = connection.to.As<Assembler>()) {
      assembler->UpdateMachineCode();
    }
  } else if (&connection.argument == &jump_arg || &connection.argument == &next_arg) {
    if (auto assembler = FindAssembler(here)) {
      assembler->UpdateMachineCode();
    }
  }
}

string_view Instruction::Name() const { return "Instruction"; }
shared_ptr<Object> Instruction::Clone() const { return make_shared<Instruction>(*this); }

void Instruction::OnRun(Location& here) {
  auto assembler = FindOrCreateAssembler(here);
  assembler->RunMachineCode(this);
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

maf::Str Instruction::ToAsmStr() const { return AssemblyText(mc_inst); }

static std::string MachineText(const mc::Inst& mc_inst) {
  auto& llvm_asm = LLVM_Assembler::Get();
  SmallVector<char, 128> buffer;
  SmallVector<MCFixup, 1> fixups;
  llvm_asm.mc_code_emitter->encodeInstruction(mc_inst, buffer, fixups, *llvm_asm.mc_subtarget_info);
  return BytesToHex(buffer);
}

constexpr float kFineFontSize = 2_mm;

static gui::Font& FineFont() {
  static auto font = gui::Font::MakeV2(gui::Font::GetGrenzeThin(), kFineFontSize);
  return *font;
}

constexpr float kHeavyFontSize = 4_mm;

static gui::Font& HeavyFont() {
  static auto font = gui::Font::MakeV2(gui::Font::GetGrenzeSemiBold(), kHeavyFontSize);
  return *font;
}

constexpr float kSubscriptFontSize = 2_mm;

static gui::Font& SubscriptFont() {
  static auto font = gui::Font::MakeV2(gui::Font::GetGrenzeSemiBold(), kSubscriptFontSize);
  return *font;
}

static const SkRRect kInstructionRRect =
    SkRRect::MakeRectXY(Instruction::Widget::kRect.sk, 3_mm, 3_mm);

static const SkPath kInstructionShape = SkPath::RRect(kInstructionRRect);

Instruction::Widget::Widget(std::weak_ptr<Object> object) { this->object = std::move(object); }

SkPath Instruction::Widget::Shape() const { return kInstructionShape; }

PersistentImage paper_texture = PersistentImage::MakeFromAsset(
    maf::embedded::assets_04_paper_C_grain_webp,
    PersistentImage::MakeArgs{.tile_x = SkTileMode::kRepeat, .tile_y = SkTileMode::kRepeat});

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
    unsigned cond_code;
    X86::CondCode fixed_cond;
  };
};

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
          {.tag = Token::String, .str = "Rotate"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "left"},
          {.tag = Token::BreakLine},
          {.tag = Token::ImmediateOperand, .imm = 2},
          {.tag = Token::String, .str = " times"},
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
          {.tag = Token::String, .str = "Rotate"},
          {.tag = Token::FixedFlag, .flag = Flag::CF},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "right"},
          {.tag = Token::BreakLine},
          {.tag = Token::ImmediateOperand, .imm = 2},
          {.tag = Token::String, .str = " times"},
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
          {.tag = Token::String, .str = "Rotate"},    {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "left"},      {.tag = Token::BreakLine},
          {.tag = Token::ImmediateOperand, .imm = 2}, {.tag = Token::String, .str = " times"},
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
          {.tag = Token::String, .str = "Rotate"},    {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "right"},     {.tag = Token::BreakLine},
          {.tag = Token::ImmediateOperand, .imm = 2}, {.tag = Token::String, .str = " times"},
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
          {.tag = Token::String, .str = "Set"},
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to "},
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
          {.tag = Token::String, .str = "Multiply"},  {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "by 2"},      {.tag = Token::BreakLine},
          {.tag = Token::ImmediateOperand, .imm = 2}, {.tag = Token::String, .str = " times"},
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
          {.tag = Token::String, .str = "Divide"},    {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "by 2"},      {.tag = Token::BreakLine},
          {.tag = Token::ImmediateOperand, .imm = 2}, {.tag = Token::String, .str = " times"},
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
          {.tag = Token::String, .str = "Divide ±"},  {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "by 2"},      {.tag = Token::BreakLine},
          {.tag = Token::ImmediateOperand, .imm = 2}, {.tag = Token::String, .str = " times"},
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
  static const SkPath spike = []() {
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

void DrawConditionCode(SkCanvas& canvas, X86::CondCode cond_code) {
  SkPaint bg_paint;
  bg_paint.setColor("#2e542a"_color);
  canvas.drawRoundRect(kConditionCodeRect.sk, 2_mm, 4_mm, bg_paint);
  SkPaint outline_paint;
  outline_paint.setColor("#000000"_color);
  outline_paint.setStyle(SkPaint::kStroke_Style);
  outline_paint.setStrokeWidth(0.4_mm);
  canvas.drawRoundRect(kConditionCodeRect.sk, 2_mm, 4_mm, outline_paint);

  SkPath path;
  switch (cond_code) {
    case X86::CondCode::COND_O: {  // overflow
      break;
    }
    case X86::CondCode::COND_NO: {  // no overflow
      break;
    }
    case X86::CondCode::COND_L:  // fallthrough
    case X86::CondCode::COND_B: {
      static const SkPath static_path = PathFromSVG("m-10-1 0 2 19 9 1-2-17-8 17-8-1-2z");
      path = static_path;
      break;
    }
    case X86::CondCode::COND_GE:  // fallthrough
    case X86::CondCode::COND_AE: {
      static const SkPath static_path =
          PathFromSVG("m10-1 0 2-19 9-1-2 17-8-17-8 1-2zm0 4-20 10 1 2 19-9.5z");
      path = static_path;
      break;
    }
    case X86::CondCode::COND_E: {
      static const SkPath static_path = PathFromSVG("m-10-2 0-2 20 0 0 2zm0 4 20 0 0 2-20 0z");
      path = static_path;
      break;
    }
    case X86::CondCode::COND_NE: {
      static const SkPath static_path =
          PathFromSVG("m-10-2 0-2 20 0 0 2zm0 4 20 0 0 2-20 0zm14-9.5-7 16-2-1 7-16z");
      path = static_path;
      break;
    }
    case X86::CondCode::COND_LE:  // fallthrough
    case X86::CondCode::COND_BE: {
      static const SkPath static_path =
          PathFromSVG("m-10-1 0 2 19 9 1-2-17-8 17-8-1-2zm0 4 20 10-1 2-19-9.5z");
      path = static_path;
      break;
    }
    case X86::CondCode::COND_G:  // fallthrough
    case X86::CondCode::COND_A: {
      static const SkPath static_path = PathFromSVG("m10-1 0 2-19 9-1-2 17-8-17-8 1-2z");
      path = static_path;
      break;
    }
    case X86::CondCode::COND_S: {  // sign / negative
      break;
    }
    case X86::CondCode::COND_NS: {  // not sign / positive
      break;
    }
    case X86::CondCode::COND_P: {  // even number of bits in the lowest byte
      break;
    }
    case X86::CondCode::COND_NP: {  // odd number of bits in the lowest byte
      break;
    }
    default: {
      break;
    }
  }
  SkPaint symbol_paint;
  symbol_paint.setColor("#ffffff"_color);
  canvas.translate(kConditionCodeTokenWidth / 2, kConditionCodeTokenHeight / 2);
  canvas.drawPath(path, symbol_paint);
  canvas.translate(-kConditionCodeTokenWidth / 2, -kConditionCodeTokenHeight / 2);
}

void Instruction::DrawInstruction(SkCanvas& canvas, const mc::Inst& inst) {
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
    vignette_paint.setBlendMode(SkBlendMode::kMultiply);
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

  if (inst.getOpcode() == X86::INSTRUCTION_LIST_END) {
    return;
  }

  auto& subscript_font = SubscriptFont();
  // Assembly text
  auto assembly_text = AssemblyText(inst);
  auto& fine_font = FineFont();
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

  {  // Contents

    // TODO: the layout might be precomputed / cached

    auto tokens = PrintInstruction(inst);
    auto& heavy_font = HeavyFont();

    constexpr float kRegisterTokenWidth = kRegisterIconWidth;
    constexpr float kRegisterIconScale = kRegisterTokenWidth / kRegisterIconWidth;
    constexpr float kImmediateTokenWidth = 10_mm;
    constexpr float kFixedFlagWidth = 6_mm;
    constexpr float kMinTextScale = 0.9;
    constexpr float kMaxTextScale = 1.1;

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
          token_width_base[token_i] = kImmediateTokenWidth;
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
    Vec2 token_position[tokens.size()];
    float string_width_scale[tokens.size()];
    float scale = 1;
    constexpr float x_min = Widget::kRect.left + Widget::kBorderMargin * 2;
    constexpr float x_max = Widget::kRect.right - Widget::kBorderMargin * 2;
    constexpr float x_range = x_max - x_min;
    constexpr float x_center = (x_max + x_min) / 2;
    constexpr float y_max = Widget::kRect.top - Widget::kBorderMargin * 2;
    constexpr float y_min = Widget::kRect.bottom + Widget::kBorderMargin * 2;
    constexpr float y_range = y_max - y_min;
    constexpr float y_center = (y_max + y_min) / 2;
    constexpr float line_height = 11_mm;
    {
      float longest_line_width = 0;
      for (int line = 0; line < n_lines; ++line) {
        longest_line_width = std::max(longest_line_width, line_widths_min[line]);
      }
      // Figure out maximum scale
      Rect natural_size = {
          /* left */ x_center - longest_line_width / 2,
          /* bottom */ y_center - line_height * ((int)line_widths_min.size()) / 2,
          /* right */ x_center + longest_line_width / 2,
          /* top */ y_center + line_height * ((int)line_widths_min.size()) / 2,
      };

      scale = std::min((y_center - y_min) / (y_center - natural_size.bottom),
                       (y_max - y_center) / (natural_size.top - y_center));
      scale = std::min(scale, x_range / natural_size.Width());

      float line_width_f[n_lines];
      for (int line = 0; line < n_lines; ++line) {
        float line_width_min = line_widths_min[line] * scale;
        float line_width_max = line_widths_max[line] * scale;
        line_width_f[line] =
            Saturate((x_range - line_width_min) / (line_width_max - line_width_min));
      }

      int line = 0;
      float x =
          x_center - lerp(line_widths_min[line], line_widths_max[line], line_width_f[line]) / 2;
      float y = y_center - kHeavyFontSize / 2 + line_height * (n_lines - 1) / 2;
      for (int token_i = 0; token_i < tokens.size(); ++token_i) {
        auto& token = tokens[token_i];
        if (token.tag == Token::BreakLine) {
          line++;
          x = x_center - lerp(line_widths_min[line], line_widths_max[line], line_width_f[line]) / 2;
          y -= line_height;
          continue;
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

    SkPaint text_paint;
    text_paint.setColor("#000000"_color);
    text_paint.setAntiAlias(true);

    auto& assembler = LLVM_Assembler::Get();

    canvas.translate(x_center, y_center);
    canvas.scale(scale, scale);
    canvas.translate(-x_center, -y_center);
    auto mat = canvas.getLocalToDevice();

    for (int token_i = 0; token_i < tokens.size(); ++token_i) {
      auto& token = tokens[token_i];
      if (token_i) {
        canvas.setMatrix(mat);
      }
      switch (token.tag) {
        case Token::String: {
          canvas.translate(token_position[token_i].x, token_position[token_i].y);
          canvas.scale(string_width_scale[token_i], 1);
          heavy_font.DrawText(canvas, token.str, text_paint);
          break;
        }
        case Token::ImmediateOperand: {
          auto mat = canvas.getLocalToDevice();
          canvas.translate(token_position[token_i].x, token_position[token_i].y);
          int64_t immediate_value = inst.getOperand(token.imm).getImm();
          std::string immediate_str = std::to_string(immediate_value);
          Rect immediate_rect = Rect::MakeCornerZero(kImmediateTokenWidth, kHeavyFontSize);
          immediate_rect.bottom -= 2_mm;
          immediate_rect.top += 2_mm;
          SkPaint immediate_bg_paint;
          immediate_bg_paint.setColor("#ffffff"_color);
          immediate_bg_paint.setAntiAlias(true);
          canvas.drawRoundRect(immediate_rect, 1_mm, 1_mm, immediate_bg_paint);
          canvas.translate(1_mm, 0);
          heavy_font.DrawText(canvas, immediate_str, text_paint);
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
        case Token::ConditionCode: {
          canvas.translate(token_position[token_i].x, token_position[token_i].y - 2_mm);
          DrawConditionCode(canvas, (X86::CondCode)inst.getOperand(token.cond_code).getImm());
          break;
        }
        case Token::FixedCondition: {
          canvas.translate(token_position[token_i].x, token_position[token_i].y - 2_mm);
          DrawConditionCode(canvas, token.fixed_cond);
          break;
        }
        default:
          break;
      }
    }
  }
}

void Instruction::Widget::Draw(SkCanvas& canvas) const {
  if (auto obj = object.lock()) {
    if (auto inst = dynamic_cast<Instruction*>(obj.get())) {
      DrawInstruction(canvas, inst->mc_inst);
    }
  }
}

Vec2AndDir Instruction::Widget::ArgStart(const Argument& arg) {
  if (&arg == &jump_arg) {
    return Vec2AndDir{.pos = kRect.RightCenter(), .dir = 0_deg};
  }
  return gui::Widget::ArgStart(arg);
}

void Instruction::SerializeState(Serializer& writer, const char* key) const {
  writer.Key(key);
  writer.StartObject();
  auto& assembler = LLVM_Assembler::Get();
  writer.Key("opcode");
  auto opcode_name = assembler.mc_instr_info->getName(mc_inst.getOpcode());
  writer.String(opcode_name.data(), opcode_name.size());
  if (mc_inst.getNumOperands() > 0) {
    writer.Key("operands");
    writer.StartArray();
    for (int i = 0; i < mc_inst.getNumOperands(); ++i) {
      auto& operand = mc_inst.getOperand(i);
      if (operand.isImm()) {
        writer.Int64(operand.getImm());
      } else if (operand.isReg()) {
        writer.String(assembler.mc_reg_info->getName(operand.getReg()));
      } else {
        writer.Null();
      }
    }
    writer.EndArray();
  }
  writer.EndObject();
}

void Instruction::DeserializeState(Location& l, Deserializer& d) {
  static StringMap<unsigned> opcode_map = []() {
    auto& assembler = LLVM_Assembler::Get();
    StringMap<unsigned> map;
    for (int i = 0; i < assembler.mc_instr_info->getNumOpcodes(); ++i) {
      map[assembler.mc_instr_info->getName(i)] = i;
    }
    return map;
  }();
  static StringMap<unsigned> reg_map = []() {
    auto& assembler = LLVM_Assembler::Get();
    StringMap<unsigned> map;
    for (int i = 0; i < assembler.mc_reg_info->getNumRegs(); ++i) {
      map[assembler.mc_reg_info->getName(i)] = i;
    }
    return map;
  }();

  auto& assembler = LLVM_Assembler::Get();
  Status status;
  for (auto& key : ObjectView(d, status)) {
    if (key == "opcode") {
      Str opcode_name;
      d.Get(opcode_name, status);
      if (!OK(status)) {
        AppendErrorMessage(status) += "Opcode name must be a string";
        break;
      }
      auto opcode_i = opcode_map.find(opcode_name);
      if (opcode_i == opcode_map.end()) {
        AppendErrorMessage(status) += "Opcode name is not a valid x86 LLVM opcode name";
        break;
      }
      mc_inst.setOpcode(opcode_i->second);
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
            // TODO: fill with a generic register
            break;
          }
          auto reg_i = reg_map.find(reg_name);
          if (reg_i == reg_map.end()) {
            auto& msg = AppendErrorMessage(status);
            msg += "Operand ";
            msg += std::to_string(operand_i);
            msg += " must be a valid x86 LLVM register name";
            // TODO: fill with a generic register
            break;
          }
          mc_inst.addOperand(MCOperand::createReg(reg_i->second));
        } else {
          // Immediate
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
        }
      }
    }
  }
  if (!OK(status)) {
    l.ReportError(status.ToStr());
  }
}

}  // namespace automat::library
