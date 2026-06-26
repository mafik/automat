// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_assembler.hpp"

#include <include/core/SkBlendMode.h>
#include <include/core/SkBlurTypes.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPathTypes.h>
#include <include/core/SkShader.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradient.h>
#include <include/pathops/SkPathOps.h>
#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCInstBuilder.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#include "../build/generated/krita_assembler.hpp"
#include "animation.hpp"
#include "automat.hpp"
#include "drawing.hpp"
#include "embedded.hpp"
#include "font.hpp"
#include "global_resources.hpp"
#include "library_instruction.hpp"
#include "machine_code.hpp"
#include "make_object_option.hpp"
#include "math.hpp"
#include "root_widget.hpp"
#include "status.hpp"
#include "svg.hpp"
#include "textures.hpp"
#include "time.hpp"
#include "ui_enum_knob_widget.hpp"
#include "widget.hpp"

#if defined _WIN32
#pragma comment(lib, "ntdll.lib")
#endif  // __linux__

using namespace llvm;
using namespace std;

namespace automat::library {

struct ImageWidget : ui::Widget {
  PersistentImage& image;
  ImageWidget(ui::Widget* parent, PersistentImage& image) : ui::Widget(parent), image(image) {}
  Optional<Rect> TextureBounds() const override {
    return Rect::MakeCornerZero(image.width(), image.height());
  }
  SkPath Shape() const override {
    return SkPath::Rect(SkRect::MakeWH(image.width(), image.height()));
  }
  void Draw(SkCanvas& canvas) const override { image.draw(canvas); }
};

struct RegistersMenuOption : TextOption, OptionsProvider {
  WeakPtr<Assembler> weak;
  RegistersMenuOption(WeakPtr<Assembler> weak) : TextOption("Registers"), weak(weak) {}
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<RegistersMenuOption>(weak);
  }
  void VisitOptions(const OptionsVisitor& visitor) const override {
    auto assembler = weak.Lock();
    if (!assembler) return;
    for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
      MakeObjectOption opt(assembler->regs[i]);
      visitor(opt);
    }
  }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    return OpenMenu(pointer);
  }
  Dir PreferredDir() const override { return S; }
};

void AssemblerWidget::VisitOptions(const OptionsVisitor& visitor) const {
  ObjectToy::VisitOptions(visitor);
  RegistersMenuOption registers_option{owner.Copy<Assembler>()};
  visitor(registers_option);
}

Assembler::Assembler() {
  Status status;
  mc_controller = mc::Controller::Make(std::bind_front(&Assembler::ExitCallback, this));
  if (!OK(status)) {
    ERROR << "Failed to create Assembler: " << status;
  }
  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    regs[i] = MAKE_PTR(Register, WeakPtr<Assembler>(this), i);
  }
}

static ui::Tock RefreshState(Assembler& assembler, time::SteadyPoint now) {
  if (assembler.mc_controller == nullptr) {
    return ui::Tock::Draw;
  }
  if (now > assembler.last_state_refresh) {
    auto old_regs = assembler.state.regs;
    Status ignore;
    assembler.mc_controller->GetState(assembler.state, ignore);
    // Wake all registers widgets where values have changed.
    // TODO: external registers
    for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
      int regs_idx = kRegisters[i].regs_index;
      if (old_regs[regs_idx] != assembler.state.regs[regs_idx]) {
        if (auto reg = assembler.regs[i]) {
          reg->WakeToys();
        }
      }
    }
    assembler.last_state_refresh = now;
  }
  if (assembler.running->IsRunning()) {
    return ui::Tock::Drawing;
  } else {
    return ui::Tock::Draw;
  }
}

void Assembler::ExitCallback(mc::CodePoint code_point) {
  running->Done();
  RefreshState(*this, time::SteadyNow());
  Instruction* exit_inst = nullptr;
  if (code_point.instruction) {
    auto exit_mc_inst = code_point.instruction->Lock();
    if (exit_mc_inst) {
      mc::Inst* exit_mc_inst_raw = const_cast<mc::Inst*>(exit_mc_inst.Get());
      constexpr int mc_inst_offset = offsetof(Instruction, mc_inst);
      exit_inst = reinterpret_cast<Instruction*>(reinterpret_cast<char*>(exit_mc_inst_raw) -
                                                 mc_inst_offset);
    }
  }
  if (exit_inst) {
    if (code_point.stop_type == mc::StopType::Next) {
      // LOG << "Exiting through " << exit_inst->ToAsmStr() << "->next";
      ScheduleNext(*exit_inst);
    } else if (code_point.stop_type == mc::StopType::Jump) {
      // LOG << "Exiting through " << exit_inst->ToAsmStr() << "->jump";
      ScheduleArgumentTargets(exit_inst->jump_arg.Bind());
    } else {
      ERROR << "Exiting through " << exit_inst->ToAsmStr() << "->instruction body (?!)";
    }
  } else {
    ERROR << "Exiting through unknown instruction??";
  }
}

Ptr<Object> Assembler::Clone() const { return MAKE_PTR(Assembler); }

void UpdateCode(automat::mc::Controller& controller,
                std::vector<Ptr<automat::library::Instruction>>&& instructions, Status& status) {
  // Sorting allows us to more efficiently search for instructions.
  std::sort(instructions.begin(), instructions.end());

  int n = instructions.size();

  automat::mc::Program program;
  program.resize(n);

  for (int i = 0; i < n; ++i) {
    automat::library::Instruction* obj_raw = instructions[i].get();
    const automat::mc::Inst* inst_raw = &obj_raw->mc_inst;
    auto* loc = obj_raw->here;
    int next = -1;
    int jump = -1;
    if (loc) {
      auto FindInstruction = [obj_raw, &instructions, n](automat::Argument::Table& arg) -> int {
        if (auto target = automat::Argument(*obj_raw, arg).Find()) {
          if (auto* to_inst =
                  dynamic_cast<automat::library::Instruction*>(target.Owner<Object>())) {
            // Find the instruction in our sorted list
            for (int j = 0; j < n; ++j) {
              if (instructions[j].get() == to_inst) {
                return j;
              }
            }
          }
        }
        return -1;
      };
      next = FindInstruction(Instruction::next_tbl);
      jump = FindInstruction(Instruction::jump_arg_tbl);
    }
    program[i].next = next;
    program[i].jump = jump;
  }
  for (int i = 0; i < n; ++i) {
    Instruction* obj_raw = instructions[i].get();
    const mc::Inst* inst_raw = &obj_raw->mc_inst;
    program[i].inst =
        NestedPtr<const mc::Inst>(std::move(instructions[i]).Cast<ReferenceCounted>(), inst_raw);
  }

  controller.UpdateCode(std::move(program), status);
}

// Returning arrays of Ptrs is really bad but it seems to be necessary here.
std::vector<Ptr<Instruction>> FindInstructions(Location& assembler_loc) {
  std::vector<Ptr<Instruction>> instructions;
  // Find all Instructions that are connected to this Assembler via assembler_arg
  // We take advantage of instructions_weak, which is the reverse pointer of assembler_arg
  auto* assembler = assembler_loc.As<Assembler>();
  for (auto& inst_weak : assembler->instructions_weak) {
    auto inst = inst_weak.Lock();
    if (inst) {
      instructions.emplace_back(std::move(inst));
    }
  }
  return instructions;
}

void Assembler::UpdateMachineCode() {
  if (!here) {
    return;
  }
  auto instructions = FindInstructions(*here);
  Status status;
  if (mc_controller == nullptr) {
    ERROR_ONCE << "Unable to update Assembler: no mc_controller";
    return;
  }
  library::UpdateCode(*mc_controller, std::move(instructions), status);
  if (!OK(status)) {
    ERROR << "Failed to update Assembler: " << status;
  }
}

void Assembler::RunMachineCode(library::Instruction* entry_point,
                               std::unique_ptr<RunTask>&& run_task) {
  running->BeginLongRunning(std::move(run_task));

  Status status;
  auto inst = entry_point->ToMC();
  RefreshState(*this, time::SteadyNow());
  mc_controller->Execute(inst, status);
  if (!OK(status)) {
    ERROR << "Failed to execute Assembler: " << status;
  }
}

void Assembler::SerializeState(ObjectSerializer& writer) const {
  mc::Controller::State mc_state = {};
  {
    auto mut_this = const_cast<Assembler*>(this);
    Status ignore;
    mc_controller->GetState(mc_state, ignore);
  }
  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    auto& reg = kRegisters[i];
    if (mc_state.regs[reg.regs_index] == 0) continue;
    writer.Key(reg.name.data(), reg.name.size());
    auto hex_value = ValToHex(mc_state.regs[reg.regs_index]);
    writer.String(hex_value.data(), hex_value.size());
  }
  // TODO: store currently executing instruction
}

bool Assembler::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    auto& reg = kRegisters[i];
    if (key == reg.name) {
      Str hex_value;
      d.Get(hex_value, status);
      if (hex_value.size() != 16) {
        AppendErrorMessage(status) += "Registers should have 16 hex digits";
      } else {
        static_assert(sizeof(state.regs[reg.regs_index]) == 8);
        HexToBytesUnchecked(hex_value, (char*)&state.regs[reg.regs_index]);
        // Update the controller state
        if (mc_controller) {
          mc_controller->ChangeState([&](mc::Controller::State& mc_state) { mc_state = state; },
                                     status);
        }
      }
      if (!OK(status)) {
        ReportError(status.ToStr());
      }
      return true;
    }
  }
  return false;
}

AssemblerWidget::AssemblerWidget(Widget* parent, Assembler& assembler)
    : ObjectToy(parent, assembler) {
  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    reg_widgets[i] = std::make_unique<RegisterWidget>(this, *assembler.regs[i]);

    reg_widgets[i]->local_to_parent.preScale(0.5, 0.5);
  }
#define POS_REG(idx)                                                    \
  reg_widgets[idx]->local_to_parent =                                   \
      SkM44(*SkMatrix::Rect2Rect(reg_widgets[idx]->Shape().getBounds(), \
                                 krita::assembler::Reg##idx, SkMatrix::kCenter_ScaleToFit));
  POS_REG(0);
  POS_REG(1);
  POS_REG(2);
  POS_REG(3);
  POS_REG(4);
  POS_REG(5);
  POS_REG(6);
  POS_REG(7);
#undef POS_REG
}

std::string_view AssemblerWidget::Name() const { return "Assembler"; }
SkPath AssemblerWidget::Shape() const {
  static SkPath shape = krita::assembler::Shape();
  return shape;
}

ui::Tock AssemblerWidget::Tick(time::Timer& timer) {
  auto assembler = LockObject<Assembler>();
  if (!assembler || assembler->mc_controller == nullptr) {
    return Tock::Draw;
  }
  Tock tock = RefreshState(*assembler, timer.now);

  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    reg_widgets[i]->Tick(timer);
  }

  return tock;
}

void AssemblerWidget::Draw(SkCanvas& canvas) const {
  krita::assembler::slab.draw(canvas);
  BakeChildren(canvas);
}

struct RegisterIndexKnobWidget : public ui::EnumKnobWidget {
  WeakPtr<Register> register_weak;

  RegisterIndexKnobWidget(ui::Widget* parent, WeakPtr<Register> register_weak)
      : ui::EnumKnobWidget(parent, kGeneralPurposeRegisterCount),
        register_weak(std::move(register_weak)) {}

  int KnobGet() const override {
    if (auto reg = register_weak.Lock()) {
      return reg->register_index;
    }
    return 0;
  }

  void KnobSet(int new_value) override {
    auto reg = register_weak.Lock();
    if (!reg) return;
    int old_value = reg->register_index;
    if (new_value == old_value) return;
    reg->register_index = new_value;
    reg->WakeToys();
  }

  void DrawKnobSymbol(SkCanvas& canvas, int val) const override {
    canvas.save();
    constexpr float kIconScale = 0.5f;
    canvas.scale(kIconScale, kIconScale);
    canvas.translate(-kRegisterIconWidth / 2, -kRegisterIconWidth / 2);
    kRegisters[val].image.draw(canvas);
    canvas.restore();
  }
};

RegisterWidget::RegisterWidget(Widget* parent, Object& reg)
    : ObjectToy(parent, reg),
      small_buffer_widget(
          this, NestedWeakPtr<Buffer>(reg.AcquireWeakPtr(), &static_cast<Register&>(reg))),
      register_index_knob(std::make_unique<RegisterIndexKnobWidget>(
          this, static_cast<Register&>(reg).AcquireWeakPtr())) {
  small_buffer_widget.Measure();
  small_buffer_widget.local_to_parent.setIdentity();
  small_buffer_widget.local_to_parent.preTranslate(
      -small_buffer_widget.width - small_buffer_widget.vertical_margin -
          register_index_knob->kGaugeRadius,
      small_buffer_widget.vertical_margin - small_buffer_widget.height / 2);
  layers.OrderInside(&small_buffer_widget);
  layers.OrderInside(register_index_knob.get());
}
RegisterWidget::~RegisterWidget() = default;

RRect RegisterWidget::MarbleShape() const {
  Rect rect(-small_buffer_widget.width - small_buffer_widget.vertical_margin * 2 -
                register_index_knob->kGaugeRadius,
            -register_index_knob->kGaugeRadius - small_buffer_widget.vertical_margin,
            register_index_knob->kGaugeRadius + small_buffer_widget.vertical_margin,
            register_index_knob->kGaugeRadius + small_buffer_widget.vertical_margin);
  return RRect::MakeSimple(rect, rect.Height() / 2);
}

RRect RegisterWidget::SpearShaft() const {
  float margin = small_buffer_widget.vertical_margin;
  float width = margin * 0.5f;
  float bottom = register_index_knob->kGaugeRadius + margin - width / 4;
  float left = -width / 2;
  float right = left + width;
  float top = bottom + kCellHeight * 8 + margin * 2 + width;
  Rect rect(left, bottom, right, top);
  auto rrect = RRect::MakeWithRadii(rect, 0, 0, 0, 0);
  rrect.radii[0].x = width / 2;
  rrect.radii[0].y = width / 4;
  rrect.radii[1].x = width / 2;
  rrect.radii[1].y = width / 4;
  return rrect;
}

SkPath RegisterWidget::SpearTip() const {
  auto shaft = SpearShaft();
  float width = shaft.rect.Width();
  Vec2 lower = shaft.rect.TopCenter().Down(width * 0.5);
  auto left = lower.Left(width * 2).Up(width * 1);
  auto upper = lower.Up(width * 6);
  auto right = lower.Right(width * 2).Up(width * 1);
  return SkPathBuilder()
      .moveTo(lower)
      .lineTo(left)
      .lineTo(upper)
      .lineTo(right)
      .lineTo(lower)
      .close()
      .detach();
}

Rect RegisterWidget::Checkerboard() const {
  return Rect::MakeAtZero<::RightX, ::BottomY>(8 * kCellWidth, 8 * kCellHeight)
      .MoveBy({0, register_index_knob->kGaugeRadius + small_buffer_widget.vertical_margin * 2});
}

static constexpr float kBitPositionFontSize = RegisterWidget::kCellHeight * 0.42;

static ui::Font& BitPositionFont() {
  static auto font = ui::Font::MakeV2(ui::Font::GetGrenzeRegular(), kBitPositionFontSize);
  return *font;
}

static constexpr float kByteValueFontSize = 3_mm;  // RegisterWidget::kCellHeight * 1;

static ui::Font& ByteValueFont() {
  static auto font = ui::Font::MakeV2(ui::Font::GetHeavyData(), kByteValueFontSize);
  return *font;
}

float RegisterWidget::HexWidth() const {
  static const float kHexWidth = ByteValueFont().MeasureText("00") + kHexMargin * 2;
  return kHexWidth;
}

SkPath RegisterWidget::FlagFront() const {
  float hex_width = HexWidth();
  auto c = Checkerboard();
  auto br = c.BottomRightCorner();
  auto bl = c.BottomLeftCorner().Left(hex_width);
  auto tl = c.TopLeftCorner().Left(hex_width);
  auto tr = c.TopRightCorner();
  auto r = kCellWidth / 4;
  return SkPathBuilder()
      .moveTo(br)
      .lineTo(bl)
      .arcTo(bl.Left(r), bl.Left(r).Up(r), r)  // bend on the bottom left
      .lineTo(tl.Left(r).Up(r))
      .arcTo(tl.Left(r), tl, r)  // bend on the top left
      .lineTo(tr)
      .arcTo(tr.Right(r), tr.Right(r).Down(r), r)  // bend on the top right
      .lineTo(br.Right(r).Down(r))
      .arcTo(br.Right(r), br, r)  // bend on the bottom right
      .close()
      .detach();
}

SkPath RegisterWidget::FlagBack() const {
  float hex_width = HexWidth();
  auto c = Checkerboard();
  Vec2 bottom = c.TopLeftCorner().Left(hex_width);
  Vec2 center = bottom.Up(kBendR);
  Vec2 left = center.Left(kBendR);
  Vec2 top = center.Up(kBendR);
  Vec2 end2 = c.TopRightCorner().Right(kBendR * 3);
  Vec2 end1 = end2.Up(kBendR * 2).Right(kBendR * 2);
  Vec2 end3 = end1.Down(kCellWidth);
  return SkPathBuilder()
      .moveTo(bottom)
      .arcTo(left.Down(kBendR), left, kBendR)
      .arcTo(left.Up(kBendR), top, kBendR)
      .lineTo(end1)
      .lineTo(end2)
      .lineTo(end3)
      .lineTo(top.Down(kCellWidth))
      .close()
      .detach();
}

SkPath RegisterWidget::Shape() const {
  if (cached_shape.isEmpty()) {
    cached_shape = SkPath::RRect(MarbleShape());
    auto Union = [this](const SkPath& other) {
      if (auto result = Op(cached_shape, other, kUnion_SkPathOp)) {
        cached_shape = *std::move(result);
      }
    };
    Union(SkPath::RRect(SpearShaft()));
    Union(SpearTip());
    Union(FlagFront());
    Union(FlagBack());
  }
  return cached_shape;
}
std::string_view RegisterWidget::Name() const { return "Register"; }

void RegisterWidget::ConnectionPositions(Vec<Vec2AndDir>& out_positions) const {
  auto front = Rect(FlagFront().getBounds());
  auto back = Rect(FlagBack().getBounds());
  out_positions.push_back(Vec2AndDir{.pos = back.TopCenter(), .dir = -90_deg});
  out_positions.push_back(Vec2AndDir{.pos = front.RightCenter(), .dir = -180_deg});
  out_positions.push_back(Vec2AndDir{.pos = front.LeftCenter(), .dir = 0_deg});
}

static const SkPath kFlagPole = PathFromSVG(
    "m-.5-.7c-1.8-7.1-2.3-14.5-2.5-21.9-.3.2-.8.3-1.3.4.7-1 1.4-1.8 1.8-3 .3 1.2.8 2 1.6 2.9-.4 "
    "0-.7-.1-1.2-.3 0 7.4 1 14.7 2.5 21.9.5.2.8.5.9.7h-2.5c.1-.2.3-.5.7-.7z");

static const SkPath kFlag = PathFromSVG(
    R"(m-3.5-21.7c.2-.5 3.1 1 4.6.9 1.6-.1 3.1-1.4 4.7-1.3 1.5.1 2.6 1.8 4.1 1.9 2 .2 3.9-1.4 6-1.5 2.7-.1 8 1.2 8 1.2s-6.7 1-9.7 2.5c-1.8.8-2.8 3-4.7 3.6-1.3.4-2.6-.7-3.9-.4-1.7.4-2.8 2.2-4.4 2.8-1.3.5-4.1.9-4.2.5-.4-3.4-.8-6.6-.6-10.2z)");

// Shift the byte values up so that they're vertically centered with their rows
static constexpr float kByteValueFontShiftUp =
    (RegisterWidget::kCellHeight - kByteValueFontSize) / 2;

// Shift the font up, so that its top is aligned with the middle of the cell
static constexpr float kBitPositionFontShiftUp =
    RegisterWidget::kCellHeight / 2 - kBitPositionFontSize;

ui::Tock RegisterWidget::Tick(time::Timer& timer) {
  Tock tock;
  if (auto register_obj = LockRegister()) {
    register_index = register_obj->register_index;
    if (auto assembler = register_obj->assembler_arg->FindObject()) {
      tock = RefreshState(*assembler, timer.now);
      reg_value = assembler->state.regs[kRegisters[register_index].regs_index];
    }
  }

  small_buffer_widget.WakeAnimation();
  return tock;
}

void RegisterWidget::Draw(SkCanvas& canvas) const {
  auto marble_shape = MarbleShape();

  float margin = small_buffer_widget.vertical_margin;
  float height = margin * 2 + register_index_knob->kGaugeRadius * 2;

  Rect base_rect = Checkerboard();

  {    // Marble
    {  // Marble color
      constexpr SkColor marble_color = "#938681"_color;
      SkPaint marble_paint;
      marble_paint.setColor(marble_color);
      canvas.drawRRect(marble_shape, marble_paint);
    }

    {  // Marble texture
      SkPaint marble_texture;
      static auto marble_texture_image = PersistentImage::MakeFromAsset(
          embedded::assets_17_texture_melt_png,
          {.tile_x = SkTileMode::kRepeat, .tile_y = SkTileMode::kRepeat});
      marble_texture.setShader(*marble_texture_image.shader);
      marble_texture.setBlendMode(SkBlendMode::kOverlay);
      marble_texture.setAlphaf(0.18f);
      canvas.drawRRect(marble_shape, marble_texture);
    }

    {  // Marble bevel
      SkPaint bevel_paint;
      SkPoint pts[] = {marble_shape.rect.TopCenter(), marble_shape.rect.BottomCenter()};
      SkColor4f colors[] = {
          "#ffffff80"_color4f,  // white light
          "#ffffff00"_color4f,  // transparent white
      };
      float pos[] = {0, 0.5};

      bevel_paint.setShader(SkShaders::LinearGradient(
          pts, SkGradient(SkGradient::Colors{colors, pos, SkTileMode::kClamp}, {})));

      bevel_paint.setStyle(SkPaint::kStroke_Style);
      bevel_paint.setStrokeWidth(margin * 0.25f);
      bevel_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, margin * 0.125f));
      auto bevel_shape = marble_shape.Outset(-margin * 0.125f);
      canvas.save();
      canvas.clipRRect(marble_shape);
      canvas.drawRRect(bevel_shape, bevel_paint);
      canvas.restore();
    }

    {  // Marble shade
      SkPaint shade_paint;

      shade_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, height * 0.25f));
      shade_paint.setBlendMode(SkBlendMode::kOverlay);
      shade_paint.setAlphaf(0.4f);
      auto shade_shape = marble_shape.MoveBy(Vec2(0, height * 0.25f));
      canvas.save();
      canvas.clipRRect(marble_shape);
      auto path = SkPath::RRect(shade_shape);
      path.toggleInverseFillType();
      canvas.drawPath(path, shade_paint);
      // canvas.drawRRect(shade_shape, shade_paint);
      canvas.restore();
    }
  }

  SkPaint dark_paint;
  dark_paint.setColor("#dcca85"_color);
  SkPaint light_paint;
  light_paint.setColor("#fefdfb"_color);

  SkColor4f back_color = "#8d7c60"_color4f;

  {  // Back side of the flag on the bottom right bit
    Vec2 center = Vec2(base_rect.right, base_rect.bottom - kBendR);
    Vec2 top = center.Up(kBendR);
    Vec2 bottom = center.Down(kBendR);
    Vec2 right = center.Right(kBendR);
    auto path = SkPathBuilder()
                    .moveTo(bottom)
                    .lineTo(top)
                    .arcTo(right.Up(kBendR), right, kBendR)
                    .arcTo(right.Down(kBendR), bottom, kBendR)
                    .close()
                    .detach();
    SkColor4f colors[2];
    colors[0] = back_color;
    colors[1] = color::AdjustLightness(colors[0], -20);

    SkPaint bend_paint;
    SkPoint points[] = {center, right};
    bend_paint.setShader(SkShaders::LinearGradient(
        points, SkGradient{SkGradient::Colors{colors, SkTileMode::kClamp}, {}}));

    canvas.drawPath(path, bend_paint);
  }

  {  // Back side of the flag visible above the checkerboard
    auto path = FlagBack();
    SkColor4f colors[2];
    colors[0] = back_color;
    colors[1] = color::AdjustLightness(colors[0], -20);

    SkPaint bend_paint;
    SkPoint points[2];
    points[1] = path.getBounds().TL();
    points[0] = points[1] + Vec2(kBendR, 0);
    bend_paint.setShader(SkShaders::LinearGradient(
        points, SkGradient{SkGradient::Colors{colors, SkTileMode::kClamp}, {}}));

    canvas.drawPath(path, bend_paint);
  }

  {  // Spear
    canvas.drawRRect(SpearShaft(), SkPaint());
    canvas.drawPath(SpearTip(), SkPaint());
  }

  auto& bit_position_font = BitPositionFont();
  auto& byte_value_font = ByteValueFont();
  float hex_width = byte_value_font.MeasureText("00") + kHexMargin * 2;
  {  // flag background
    auto br = base_rect.BottomRightCorner();
    auto bl = base_rect.BottomLeftCorner().Left(hex_width);
    auto r = kBendR;
    auto path = FlagFront();

    SkPoint points[] = {bl.Left(r), br.Right(r)};
    SkColor4f colors[4];
    colors[1] = colors[2] = dark_paint.getColor4f();
    colors[0] = colors[3] = color::AdjustLightness(colors[1], -20);
    float pos[4] = {};
    pos[1] = r / ((br - bl).x + r * 2);
    pos[2] = 1 - pos[1];
    pos[3] = 1;

    SkPaint bg_paint;
    bg_paint.setShader(SkShaders::LinearGradient(
        points, SkGradient{SkGradient::Colors{colors, pos, SkTileMode::kClamp}, {}}));

    canvas.drawPath(path, bg_paint);
  }

  for (int row = 0; row < 8; ++row) {
    float bottom = base_rect.bottom + kCellHeight * row;
    float top = bottom + kCellHeight;
    int byte_value = (reg_value >> (row * 8)) & 0xFF;
    for (int bit = 0; bit < 8; ++bit) {
      float right = base_rect.right - kCellWidth * bit;
      float left = right - kCellWidth;
      SkPaint* cell_paint = &light_paint;
      bool light = bit % 2 == row % 2;
      if (light) {
        // light cell
        canvas.drawRect(SkRect::MakeLTRB(left, bottom, right, top), light_paint);
        cell_paint = &dark_paint;
      }

      int position = row * 8 + bit;
      std::string position_str = f("{}", position);
      float position_text_width = bit_position_font.MeasureText(position_str);
      canvas.save();
      canvas.translate(left + (kCellWidth - position_text_width) * 0.5,
                       bottom + kBitPositionFontShiftUp);
      bit_position_font.DrawText(canvas, position_str, *cell_paint);
      canvas.restore();

      SkPaint pole_paint;
      SkPaint flag_paint;
      SkPoint points[2] = {SkPoint::Make(-kCellWidth * 0.2, 0),
                           SkPoint::Make(kCellWidth * 1.2, kCellHeight * 0.1)};
      SkColor4f colors[5] = {"#ff0000"_color4f, "#800000"_color4f, "#ff0000"_color4f,
                             "#800000"_color4f, "#ff0000"_color4f};
      flag_paint.setShader(SkShaders::LinearGradient(
          points, SkGradient{SkGradient::Colors{colors, SkTileMode::kClamp}, {}}));
      if (reg_value & (1ULL << position)) {
        canvas.save();
        canvas.translate(left + kCellWidth * 0.2, bottom);
        canvas.scale(0.5, 0.5);
        canvas.drawPath(kFlagPole, pole_paint);
        canvas.drawPath(kFlag, flag_paint);
        canvas.restore();
      }

      if (bit == 7 && !light) {
        // Flag bending on the left
        left -= hex_width;
        auto path = SkPathBuilder()
                        .moveTo(left, bottom)
                        .arcTo(SkPoint(left - kBendR, bottom),
                               SkPoint(left - kBendR, bottom + kBendR), kBendR)
                        .lineTo(SkPoint(left - kBendR, top + kBendR))
                        .arcTo(SkPoint(left - kBendR, top), SkPoint(left, top), kBendR)
                        .lineTo(SkPoint(left + hex_width, top))
                        .lineTo(SkPoint(left + hex_width, bottom))
                        .close()
                        .detach();
        SkColor4f colors[2];
        colors[0] = cell_paint->getColor4f();
        colors[1] = color::AdjustLightness(colors[0], -20);

        SkPaint bend_paint;
        SkPoint points[] = {SkPoint::Make(left, bottom), SkPoint::Make(left - kBendR, bottom)};
        bend_paint.setShader(SkShaders::LinearGradient(
            points, SkGradient{SkGradient::Colors{colors, SkTileMode::kClamp}, {}}));

        canvas.drawPath(path, bend_paint);
      }
      if (bit == 0 && !light) {
        // Flag bending on the right
        auto path =
            SkPathBuilder()
                .moveTo(right, bottom)
                .lineTo(SkPoint(right, top))
                .arcTo(SkPoint(right + kBendR, top), SkPoint(right + kBendR, top - kBendR), kBendR)
                .lineTo(SkPoint(right + kBendR, bottom - kBendR))
                .arcTo(SkPoint(right + kBendR, bottom), SkPoint(right, bottom), kBendR)
                .close()
                .detach();
        SkColor4f colors[2];
        colors[0] = cell_paint->getColor4f();
        colors[1] = color::AdjustLightness(colors[0], -20);

        SkPaint bend_paint;
        SkPoint points[] = {SkPoint::Make(right, bottom), SkPoint::Make(right + kBendR, bottom)};
        bend_paint.setShader(SkShaders::LinearGradient(
            points, SkGradient{SkGradient::Colors{colors, SkTileMode::kClamp}, {}}));

        canvas.drawPath(path, bend_paint);
      }
    }
    auto byte_value_str = f("{:02X}", byte_value);
    SkPaint hex_paint;
    hex_paint.setColor("#205130"_color);
    hex_paint.setBlendMode(SkBlendMode::kHardLight);
    canvas.save();
    canvas.translate(base_rect.left + kHexMargin - hex_width, bottom + kByteValueFontShiftUp);
    byte_value_font.DrawText(canvas, byte_value_str, hex_paint);
    canvas.restore();
  }

  BakeChildren(canvas);
}

Register::Register(WeakPtr<Assembler> assembler_weak, int register_index)
    : register_index(register_index) {
  assembler_arg->Connect(Interface{assembler_weak.GetUnsafe(), nullptr});
}

Ptr<Object> Register::Clone() const {
  return MAKE_PTR(Register, assembler_arg->FindObjectWeak(), register_index);
}

void Register::BufferVisit(const BufferVisitor& visitor) {
  auto assembler = assembler_arg->FindObject();
  if (assembler == nullptr) {
    ReportError("Register is not connected to an assembler");
    return;
  }
  if (assembler->mc_controller == nullptr) {
    ReportError("Assembler is not connected to a mc_controller");
    return;
  }
  Status status;
  assembler->mc_controller->ChangeState(
      [&](mc::Controller::State& state) {
        uint64_t* ptr = &state.regs[kRegisters[register_index].regs_index];
        char* char_ptr = reinterpret_cast<char*>(ptr);
        visitor(span<char>(char_ptr, sizeof(state.regs[0])));
      },
      status);
  if (!OK(status)) {
    ReportError(status.ToStr());
    return;
  }
  WakeToys();
}

void Register::SetText(std::string_view text) {
  auto assembler = assembler_arg->FindObject();
  if (assembler == nullptr) {
    ReportError("Register is not connected to an assembler");
    return;
  }
  if (assembler->mc_controller == nullptr) {
    ReportError("Assembler is not connected to a mc_controller");
    return;
  }
  Status status;
  assembler->mc_controller->ChangeState(
      [&](mc::Controller::State& state) {
        uint64_t reg_value = 0;
        memcpy(&reg_value, text.data(), std::min(text.size(), sizeof(reg_value)));
        state.regs[kRegisters[register_index].regs_index] = reg_value;
      },
      status);
  if (!OK(status)) {
    ReportError(status.ToStr());
    return;
  }
  WakeToys();
}

void Register::SerializeState(ObjectSerializer& writer) const {
  writer.Key("reg");
  auto& reg = kRegisters[register_index];
  writer.String(reg.name.data(), reg.name.size());
}

bool Register::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "reg") {
    Status status;
    std::string reg_name;
    d.Get(reg_name, status);
    if (!OK(status)) {
      ReportError(status.ToStr());
      register_index = 0;
      return true;
    }
    for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
      if (kRegisters[i].name == reg_name) {
        register_index = i;
        return true;
      }
    }
    ReportError(f("Unknown register name: {}", reg_name));
    register_index = 0;
    return true;
  }
  return false;
}

}  // namespace automat::library
