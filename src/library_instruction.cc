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
#include "llvm_asm.hh"
#include "textures.hh"

using namespace std;
using namespace llvm;
using namespace maf;

namespace automat::library {

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
  auto proto = Instruction();
  auto& new_inst = root_machine->Create(proto);
  Instruction& inst = *new_inst.As<Instruction>();
  // inst.mc_inst =
  // MCInstBuilder(X86::ADD64rr).addReg(X86::RAX).addReg(X86::RAX).addReg(X86::RBX);
  inst.mc_inst = MCInstBuilder(X86::MOV8ri).addReg(X86::AL).addImm(10);
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

static const SkRect kInstructionRect =
    SkRect::MakeWH(Instruction::Widget::kWidth, Instruction::Widget::kHeight);

static const SkRRect kInstructionRRect = SkRRect::MakeRectXY(kInstructionRect, 3_mm, 3_mm);

static const SkPath kInstructionShape = SkPath::RRect(kInstructionRRect);

Instruction::Widget::Widget(std::weak_ptr<Object> object) { this->object = object; }

SkPath Instruction::Widget::Shape() const { return kInstructionShape; }

PersistentImage paper_texture = PersistentImage::MakeFromAsset(
    maf::embedded::assets_04_paper_C_grain_webp,
    PersistentImage::MakeArgs{.tile_x = SkTileMode::kRepeat, .tile_y = SkTileMode::kRepeat});

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
