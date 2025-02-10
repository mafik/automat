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
#include <llvm/MC/MCInstBuilder.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#include <cmath>

#include "automat.hh"
#include "embedded.hh"
#include "font.hh"
#include "library_assembler.hh"
#include "library_instruction_library.hh"
#include "llvm_asm.hh"
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
    },
    RegisterPresentation{
        .image =
            PersistentImage::MakeFromAsset(maf::embedded::assets_reg_bx_webp,
                                           PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
        .llvm_reg = X86::RBX,
    },
    RegisterPresentation{
        .image =
            PersistentImage::MakeFromAsset(maf::embedded::assets_reg_cx_webp,
                                           PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
        .llvm_reg = X86::RCX,
    },
    RegisterPresentation{
        .image =
            PersistentImage::MakeFromAsset(maf::embedded::assets_reg_dx_webp,
                                           PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
        .llvm_reg = X86::RDX,
    },
    RegisterPresentation{
        .image =
            PersistentImage::MakeFromAsset(maf::embedded::assets_reg_si_webp,
                                           PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
        .llvm_reg = X86::RSI,
    },
    RegisterPresentation{
        .image =
            PersistentImage::MakeFromAsset(maf::embedded::assets_reg_di_webp,
                                           PersistentImage::MakeArgs{.width = kRegisterIconWidth}),
        .llvm_reg = X86::RDI,
    },
};

Argument assembler_arg = []() {
  Argument arg("Assembler", Argument::kRequiresObject);
  arg.RequireInstanceOf<Assembler>();
  arg.autoconnect_radius = INFINITY;
  arg.tint = "#ff0000"_color;
  arg.style = Argument::Style::Cable;
  return arg;
}();

static Assembler* FindOrCreateAssembler(Location& here) {
  Assembler* assembler = assembler_arg.FindObject<Assembler>(
      here, {.if_missing = Argument::IfMissing::CreateFromPrototype});
  return assembler;
}

void Instruction::Args(std::function<void(Argument&)> cb) { cb(assembler_arg); }
std::shared_ptr<Object> Instruction::ArgPrototype(const Argument& arg) {
  if (&arg == &assembler_arg) {
    Status status;
    auto obj = std::make_shared<Assembler>(status);
    if (OK(status)) {
      return obj;
    }
  }
  return nullptr;
}

void Instruction::ConnectionAdded(Location& here, Connection& connection) {
  if (&connection.argument == &assembler_arg) {
    if (auto assembler = connection.to.As<Assembler>()) {
      assembler->instructions.push_back(this);
      assembler->UpdateMachineCode();
    }
  }
}

void Instruction::ConnectionRemoved(Location& here, Connection& connection) {
  if (&connection.argument == &assembler_arg) {
    if (auto assembler = connection.to.As<Assembler>()) {
      assembler->instructions.erase(
          std::remove(assembler->instructions.begin(), assembler->instructions.end(), this),
          assembler->instructions.end());
      assembler->UpdateMachineCode();
    }
  }
}

string_view Instruction::Name() const { return "Instruction"; }
shared_ptr<Object> Instruction::Clone() const { return make_shared<Instruction>(*this); }

void Instruction::SetupDevelopmentScenario() {
  auto proto = InstructionLibrary();
  auto& new_inst = root_machine->Create(proto);
  // Instruction& inst = *new_inst.As<Instruction>();
  // inst.mc_inst =
  // MCInstBuilder(X86::ADD64rr).addReg(X86::RAX).addReg(X86::RAX).addReg(X86::RBX);
  // inst.mc_inst = MCInstBuilder(X86::MOV8ri).addReg(X86::AL).addImm(10);
}

LongRunning* Instruction::OnRun(Location& here) {
  auto assembler = assembler_arg.FindOrCreateObject<Assembler>(here);
  assembler->RunMachineCode(this);
  return nullptr;
}

static std::string AssemblyText(const llvm::MCInst& mc_inst) {
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

static const SkRect kInstructionRect =
    SkRect::MakeWH(Instruction::Widget::kWidth, Instruction::Widget::kHeight);

static const SkRRect kInstructionRRect = SkRRect::MakeRectXY(kInstructionRect, 3_mm, 3_mm);

static const SkPath kInstructionShape = SkPath::RRect(kInstructionRRect);

Instruction::Widget::Widget(std::weak_ptr<Object> object) { this->object = object; }

SkPath Instruction::Widget::Shape() const { return kInstructionShape; }

PersistentImage paper_texture = PersistentImage::MakeFromAsset(
    maf::embedded::assets_04_paper_C_grain_webp,
    PersistentImage::MakeArgs{.tile_x = SkTileMode::kRepeat, .tile_y = SkTileMode::kRepeat});

struct Token {
  enum Tag {
    String,
    RegisterOperand,
    ImmediateOperand,
    FixedRegister,
    FixedFlag,
  } tag;
  union {
    std::string_view str;
    unsigned reg;
    unsigned imm;
    unsigned fixed_reg;
    Flag flag;
  };
};

std::span<const Token> PrintInstruction(const llvm::MCInst& inst) {
  switch (inst.getOpcode()) {
    case X86::JMP_1:
    case X86::JMP_2:
    case X86::JMP_4: {
      constexpr static Token tokens[] = {{.tag = Token::String, .str = "Jump"sv}};
      return tokens;
    }

    case X86::XOR64i32:
    case X86::XOR32i32:
    case X86::XOR16i16:
    case X86::XOR8i8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"sv},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"sv},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "xor"sv},
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
          {.tag = Token::String, .str = "Set"sv}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"sv},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "xor"sv}, {.tag = Token::ImmediateOperand, .imm = 2},
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
          {.tag = Token::String, .str = "Set"sv}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"sv},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "xor"sv}, {.tag = Token::RegisterOperand, .reg = 2},
      };
      return tokens;
    }

    case X86::NOT8r:
    case X86::NOT16r:
    case X86::NOT32r:
    case X86::NOT64r: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Flip"sv},
          {.tag = Token::RegisterOperand, .reg = 0},
      };
      return tokens;
    }
    case X86::ANDN64rr:
    case X86::ANDN32rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"sv},   {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to ¬("sv}, {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "and"sv},   {.tag = Token::RegisterOperand, .reg = 2},
          {.tag = Token::String, .str = ")"sv},
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
          {.tag = Token::String, .str = "Set"sv}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"sv},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "and"sv}, {.tag = Token::RegisterOperand, .reg = 2},
      };
      return tokens;
    }
    case X86::AND8i8:
    case X86::AND16i16:
    case X86::AND32i32:
    case X86::AND64i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"sv},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"sv},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "and"sv},
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
          {.tag = Token::String, .str = "Set"sv}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"sv},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "and"sv}, {.tag = Token::ImmediateOperand, .imm = 2},
      };
      return tokens;
    }

    case X86::OR8i8:
    case X86::OR32i32:
    case X86::OR16i16:
    case X86::OR64i32: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"sv},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"sv},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "or"sv},
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
          {.tag = Token::String, .str = "Set"sv}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"sv},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "or"sv},  {.tag = Token::ImmediateOperand, .imm = 2},
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
          {.tag = Token::String, .str = "Set"sv}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"sv},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "or"sv},  {.tag = Token::RegisterOperand, .reg = 2},
      };
      return tokens;
    }

    case X86::INC8r:
    case X86::INC64r:
    case X86::INC32r:
    case X86::INC16r: {
      constexpr static Token tokens[] = {
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "+1"sv},
      };
      return tokens;
    }

    case X86::DEC8r:
    case X86::DEC64r:
    case X86::DEC32r:
    case X86::DEC16r: {
      constexpr static Token tokens[] = {
          {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "-1"sv},
      };
      return tokens;
    }

    case X86::ADC64i32:
    case X86::ADC32i32:
    case X86::ADC16i16:
    case X86::ADC8i8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"sv},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"sv},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "+"sv},
          {.tag = Token::ImmediateOperand, .imm = 0},
          {.tag = Token::String, .str = "+"sv},
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
          {.tag = Token::String, .str = "Set"sv}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"sv},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "+"sv},   {.tag = Token::ImmediateOperand, .imm = 2},
          {.tag = Token::String, .str = "+"sv},   {.tag = Token::FixedFlag, .flag = Flag::CF},
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
          {.tag = Token::String, .str = "Set"sv}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"sv},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "+"sv},   {.tag = Token::RegisterOperand, .reg = 2},
          {.tag = Token::String, .str = "+"sv},   {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }

    case X86::ADD64i32:
    case X86::ADD32i32:
    case X86::ADD16i16:
    case X86::ADD8i8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"sv},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"sv},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "+"sv},
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
          {.tag = Token::String, .str = "Set"sv}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"sv},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "+"sv},   {.tag = Token::ImmediateOperand, .imm = 2},
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
          {.tag = Token::String, .str = "Set"sv}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"sv},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "+"sv},   {.tag = Token::RegisterOperand, .reg = 2},
      };
      return tokens;
    }

    case X86::ADCX32rr:
    case X86::ADCX64rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"sv}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"sv},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "+"sv},   {.tag = Token::RegisterOperand, .reg = 2},
          {.tag = Token::String, .str = "+"sv},   {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }

    case X86::ADOX32rr:
    case X86::ADOX64rr: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"sv}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"sv},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "+"sv},   {.tag = Token::RegisterOperand, .reg = 2},
          {.tag = Token::String, .str = "+"sv},   {.tag = Token::FixedFlag, .flag = Flag::OF},
      };
      return tokens;
    }

    case X86::SBB32i32:
    case X86::SBB64i32:
    case X86::SBB16i16:
    case X86::SBB8i8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"sv},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"sv},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "-"sv},
          {.tag = Token::ImmediateOperand, .imm = 0},
          {.tag = Token::String, .str = "-"sv},
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
          {.tag = Token::String, .str = "Set"sv}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"sv},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "-"sv},   {.tag = Token::ImmediateOperand, .imm = 2},
          {.tag = Token::String, .str = "-"sv},   {.tag = Token::FixedFlag, .flag = Flag::CF},
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
          {.tag = Token::String, .str = "Set"sv}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"sv},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "-"sv},   {.tag = Token::RegisterOperand, .reg = 2},
          {.tag = Token::String, .str = "-"sv},   {.tag = Token::FixedFlag, .flag = Flag::CF},
      };
      return tokens;
    }

    case X86::SUB64i32:
    case X86::SUB32i32:
    case X86::SUB16i16:
    case X86::SUB8i8: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Set"sv},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "to"sv},
          {.tag = Token::FixedRegister, .fixed_reg = X86::RAX},
          {.tag = Token::String, .str = "-"sv},
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
          {.tag = Token::String, .str = "Set"sv}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"sv},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "-"sv},   {.tag = Token::ImmediateOperand, .imm = 2},
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
          {.tag = Token::String, .str = "Set"sv}, {.tag = Token::RegisterOperand, .reg = 0},
          {.tag = Token::String, .str = "to"sv},  {.tag = Token::RegisterOperand, .reg = 1},
          {.tag = Token::String, .str = "-"sv},   {.tag = Token::RegisterOperand, .reg = 2},
      };
      return tokens;
    }

    case X86::RCL64r1:
    case X86::RCL32r1:
    case X86::RCL16r1:
    case X86::RCL8r1: {
      constexpr static Token tokens[] = {{.tag = Token::String, .str = "Rotate"sv},
                                         {.tag = Token::FixedFlag, .flag = Flag::CF},
                                         {.tag = Token::RegisterOperand, .reg = 0},
                                         {.tag = Token::String, .str = "once left"sv}};
      return tokens;
    }

    case X86::RCL64rCL:
    case X86::RCL32rCL:
    case X86::RCL16rCL:
    case X86::RCL8rCL: {
      constexpr static Token tokens[] = {{.tag = Token::String, .str = "Rotate"sv},
                                         {.tag = Token::FixedFlag, .flag = Flag::CF},
                                         {.tag = Token::RegisterOperand, .reg = 0},
                                         {.tag = Token::String, .str = "left"sv},
                                         {.tag = Token::FixedRegister, .fixed_reg = X86::CL},
                                         {.tag = Token::String, .str = "times"sv}};
      return tokens;
    }

    case X86::RCL64ri:
    case X86::RCL32ri:
    case X86::RCL16ri:
    case X86::RCL8ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Rotate"sv},  {.tag = Token::FixedFlag, .flag = Flag::CF},
          {.tag = Token::RegisterOperand, .reg = 0},  {.tag = Token::String, .str = "left"sv},
          {.tag = Token::ImmediateOperand, .imm = 2}, {.tag = Token::String, .str = "times"sv}};
      return tokens;
    }

    case X86::RCR64r1:
    case X86::RCR32r1:
    case X86::RCR16r1:
    case X86::RCR8r1: {
      constexpr static Token tokens[] = {{.tag = Token::String, .str = "Rotate"sv},
                                         {.tag = Token::FixedFlag, .flag = Flag::CF},
                                         {.tag = Token::RegisterOperand, .reg = 0},
                                         {.tag = Token::String, .str = "once right"sv}};
      return tokens;
    }

    case X86::RCR64rCL:
    case X86::RCR32rCL:
    case X86::RCR16rCL:
    case X86::RCR8rCL: {
      constexpr static Token tokens[] = {{.tag = Token::String, .str = "Rotate"sv},
                                         {.tag = Token::FixedFlag, .flag = Flag::CF},
                                         {.tag = Token::RegisterOperand, .reg = 0},
                                         {.tag = Token::String, .str = "right"sv},
                                         {.tag = Token::FixedRegister, .fixed_reg = X86::CL},
                                         {.tag = Token::String, .str = "times"sv}};
      return tokens;
    }

    case X86::RCR16ri:
    case X86::RCR32ri:
    case X86::RCR64ri:
    case X86::RCR8ri: {
      constexpr static Token tokens[] = {
          {.tag = Token::String, .str = "Rotate"sv},  {.tag = Token::FixedFlag, .flag = Flag::CF},
          {.tag = Token::RegisterOperand, .reg = 0},  {.tag = Token::String, .str = "right"sv},
          {.tag = Token::ImmediateOperand, .imm = 2}, {.tag = Token::String, .str = "times"sv}};
      return tokens;
    }

      /* TODO: Handle
         ROL8r1
         ROL64ri
         ROL64rCL
         ROL64r1
         ROL32ri
         ROL32rCL
         ROL32r1
         ROL16ri
         ROL16rCL
         ROL16r1
         ROL8rCL
         ROL8ri
         ROR16r1
         ROR16rCL
         ROR16ri
         ROR32r1
         ROR32rCL
         ROR32ri
         ROR64r1
         ROR64rCL
         ROR64ri
         ROR8r1
         ROR8rCL
         ROR8ri
         RORX32ri
         RORX64ri
  */

    default: {
      static std::set<unsigned> warned_opcodes;
      if (warned_opcodes.insert(inst.getOpcode()).second) {
        LOG << "Unhandled opcode "
            << LLVM_Assembler::Get().mc_instr_info->getName(inst.getOpcode());
      }
      return {};
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

void Instruction::DrawInstruction(SkCanvas& canvas, const llvm::MCInst& inst) {
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

  if (inst.getOpcode() != X86::INSTRUCTION_LIST_END) {
    // Assembly text
    auto text = AssemblyText(inst);
    auto& fine_font = FineFont();
    auto assembly_text_width = fine_font.MeasureText(text);
    Vec2 assembly_text_offset = Vec2{Widget::kBorderMargin - kFineFontSize / 2,
                                     kHeight - kFineFontSize / 2 - Widget::kBorderMargin};

    {
      canvas.save();
      SkPaint text_paint;
      text_paint.setColor("#000000"_color);
      text_paint.setAntiAlias(true);
      canvas.translate(assembly_text_offset.x, assembly_text_offset.y);
      fine_font.DrawText(canvas, text, text_paint);
      canvas.restore();
    }

    {  // Border
      canvas.save();
      Rect text_rect = Rect::MakeCornerZero(assembly_text_width, kFineFontSize)
                           .Outset(kFineFontSize / 2)
                           .MoveBy(assembly_text_offset);
      canvas.clipRect(text_rect, SkClipOp::kDifference);
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
      auto tokens = PrintInstruction(inst);
      auto& heavy_font = HeavyFont();
      constexpr float kSpaceWidth = kHeavyFontSize / 4;

      constexpr float kRegisterTokenWidth = kRegisterIconWidth;
      constexpr float kRegisterIconScale = kRegisterTokenWidth / kRegisterIconWidth;
      constexpr float kImmediateTokenWidth = 10_mm;
      float total_width = 0;
      // TODO: this might be cached
      // Wrap & scale down to fit
      for (auto& token : tokens) {
        switch (token.tag) {
          case Token::String:
            total_width += heavy_font.MeasureText(token.str);
            break;
          case Token::RegisterOperand:
          case Token::FixedRegister:
            total_width += kRegisterTokenWidth;
            break;
          case Token::ImmediateOperand:
            total_width += kImmediateTokenWidth;
            break;
          case Token::FixedFlag:
            total_width += 6_mm;
            break;
          default:
            break;
        }
        total_width += kSpaceWidth;
      }

      SkPaint text_paint;
      text_paint.setColor("#000000"_color);
      text_paint.setAntiAlias(true);

      float x_min = kInstructionRect.left() + Widget::kBorderMargin;
      float x_max = kInstructionRect.right() - Widget::kBorderMargin;
      float x_center = (x_max + x_min) / 2;
      float x = x_center - total_width / 2;
      float y = kInstructionRect.centerY() - kHeavyFontSize / 2;

      auto& assembler = LLVM_Assembler::Get();

      for (auto& token : tokens) {
        switch (token.tag) {
          case Token::String:
            canvas.translate(x, y);
            heavy_font.DrawText(canvas, token.str, text_paint);
            canvas.translate(-x, -y);
            x += heavy_font.MeasureText(token.str);
            break;
          case Token::ImmediateOperand: {
            canvas.save();
            canvas.translate(x, y);
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
            canvas.restore();
            x += kImmediateTokenWidth;
            break;
          }
          case Token::FixedRegister:
            canvas.translate(x, y - 2_mm);
            canvas.scale(kRegisterIconScale, kRegisterIconScale);
            for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
              bool found =
                  assembler.mc_reg_info->isSubRegisterEq(kRegisters[i].llvm_reg, token.fixed_reg);
              if (found) {
                kRegisters[i].image.draw(canvas);
                break;
              }
            }
            canvas.scale(1 / kRegisterIconScale, 1 / kRegisterIconScale);
            canvas.translate(-x, -y + 2_mm);
            x += kRegisterIconWidth;
            break;
          case Token::RegisterOperand:
            canvas.translate(x, y - 2_mm);
            canvas.scale(kRegisterIconScale, kRegisterIconScale);
            for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
              bool found = assembler.mc_reg_info->isSubRegisterEq(
                  kRegisters[i].llvm_reg, inst.getOperand(token.reg).getReg());
              if (found) {
                kRegisters[i].image.draw(canvas);
                break;
              }
            }
            canvas.scale(1 / kRegisterIconScale, 1 / kRegisterIconScale);
            canvas.translate(-x, -y + 2_mm);
            x += kRegisterIconWidth;
            break;
          case Token::FixedFlag:
            canvas.translate(x, y - 2_mm);
            DrawFlag(canvas, token.flag);
            // x += kFlagWidth;
            canvas.translate(-x, -y + 2_mm);
            x += 6_mm;
            break;
          default:
            break;
        }
        x += kSpaceWidth;
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

std::unique_ptr<Action> Instruction::Widget::FindAction(gui::Pointer& p, gui::ActionTrigger btn) {
  if (btn == gui::PointerButton::Left) {
    auto* location = Closest<Location>(*p.hover);
    auto* machine = Closest<Machine>(*p.hover);
    if (location && machine) {
      auto contact_point = p.PositionWithin(*this);
      auto a = std::make_unique<DragLocationAction>(p, machine->Extract(*location));
      a->contact_point = contact_point;
      return a;
    }
  }
  return nullptr;
}

}  // namespace automat::library
