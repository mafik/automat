// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_assembler.hh"

#include <include/effects/SkGradientShader.h>
#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCInstBuilder.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#include "animation.hh"
#include "drawing.hh"
#include "embedded.hh"
#include "font.hh"
#include "global_resources.hh"
#include "library_instruction.hh"
#include "machine_code.hh"
#include "math.hh"
#include "root_widget.hh"
#include "status.hh"
#include "svg.hh"
#include "textures.hh"
#include "time.hh"
#include "widget.hh"

#if defined _WIN32
#pragma comment(lib, "ntdll.lib")
#endif  // __linux__

using namespace llvm;
using namespace std;
using namespace maf;

namespace automat::library {

struct ShowRegisterOption : Option {
  std::weak_ptr<Assembler> weak;
  int register_index;  // Must be < kGeneralPurposeRegisterCount

  ShowRegisterOption(std::weak_ptr<Assembler> weak, int register_index)
      : Option("Show"), weak(weak), register_index(register_index) {}

  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<ShowRegisterOption>(weak, register_index);
  }

  std::unique_ptr<Action> Activate(gui::Pointer& pointer) const override {
    if (auto assembler = weak.lock()) {
      assembler->reg_objects_idx[register_index] = std::make_shared<Register>(weak, register_index);
      assembler->WakeWidgetsAnimation();
    }
    return nullptr;
  }
};

struct HideRegisterOption : Option {
  std::weak_ptr<Assembler> weak;
  int register_index;  // Must be < kGeneralPurposeRegisterCount

  HideRegisterOption(std::weak_ptr<Assembler> weak, int register_index)
      : Option("Hide"), weak(weak), register_index(register_index) {}

  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<HideRegisterOption>(weak, register_index);
  }

  std::unique_ptr<Action> Activate(gui::Pointer& pointer) const override {
    if (auto assembler = weak.lock()) {
      assembler->reg_objects_idx[register_index].reset();
      assembler->WakeWidgetsAnimation();
    }
    return nullptr;
  }
};

struct ImageWidget : gui::Widget {
  PersistentImage& image;
  ImageWidget(PersistentImage& image) : image(image) {}
  Optional<Rect> TextureBounds() const override {
    return Rect::MakeCornerZero(image.width(), image.height());
  }
  SkPath Shape() const override {
    return SkPath::Rect(SkRect::MakeWH(image.width(), image.height()));
  }
  void Draw(SkCanvas& canvas) const override { image.draw(canvas); }
};

struct RegisterMenuOption : Option, OptionsProvider {
  std::weak_ptr<Assembler> weak;
  int register_index;

  RegisterMenuOption(std::weak_ptr<Assembler> weak, int register_index)
      : Option(std::make_shared<ImageWidget>(kRegisters[register_index].image)),
        weak(weak),
        register_index(register_index) {}
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<RegisterMenuOption>(weak, register_index);
  }
  void VisitOptions(const OptionsVisitor& visitor) const override {
    auto assembler = weak.lock();
    if (assembler->reg_objects_idx[register_index] == nullptr) {
      ShowRegisterOption show{weak, register_index};
      visitor(show);
    } else {
      HideRegisterOption hide{weak, register_index};
      visitor(hide);
    }
  }
  std::unique_ptr<Action> Activate(gui::Pointer& pointer) const override {
    return OpenMenu(pointer);
  }
};

struct RegistersMenuOption : Option, OptionsProvider {
  std::weak_ptr<Assembler> weak;
  RegistersMenuOption(std::weak_ptr<Assembler> weak) : Option("Registers"), weak(weak) {}
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<RegistersMenuOption>(weak);
  }
  void VisitOptions(const OptionsVisitor& visitor) const override {
    for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
      RegisterMenuOption opt{weak, i};
      visitor(opt);
    }
  }
  std::unique_ptr<Action> Activate(gui::Pointer& pointer) const override {
    return OpenMenu(pointer);
  }
};

void AssemblerWidget::VisitOptions(const OptionsVisitor& visitor) const {
  FallbackWidget::VisitOptions(visitor);
  RegistersMenuOption registers_option{assembler_weak};
  visitor(registers_option);
}

Assembler::Assembler() {
  Status status;
  mc_controller = mc::Controller::Make(std::bind_front(&Assembler::ExitCallback, this));
  if (!OK(status)) {
    ERROR << "Failed to create Assembler: " << status;
  }
}

Assembler::~Assembler() {}

void Assembler::ExitCallback(mc::CodePoint code_point) {
  Instruction* exit_inst = nullptr;
  if (code_point.instruction) {
    auto exit_mc_inst = code_point.instruction->lock();
    if (exit_mc_inst) {
      mc::Inst* exit_mc_inst_raw = const_cast<mc::Inst*>(exit_mc_inst.get());
      constexpr int mc_inst_offset = offsetof(Instruction, mc_inst);
      exit_inst = reinterpret_cast<Instruction*>(reinterpret_cast<char*>(exit_mc_inst_raw) -
                                                 mc_inst_offset);
    }
  }
  auto* loc = here.lock().get();
  auto [begin, end] = loc->incoming.equal_range(&assembler_arg);
  for (auto it = begin; it != end; ++it) {
    auto& conn = *it;
    auto& inst_loc = conn->from;
    if (auto inst = inst_loc.As<Instruction>()) {
      inst_loc.long_running = nullptr;
    }
  }
  if (exit_inst) {
    if (code_point.stop_type == mc::StopType::Next) {
      // LOG << "Exiting through " << exit_inst->ToAsmStr() << "->next";
      ScheduleNext(*exit_inst->here.lock());
    } else if (code_point.stop_type == mc::StopType::Jump) {
      // LOG << "Exiting through " << exit_inst->ToAsmStr() << "->jump";
      ScheduleArgumentTargets(*exit_inst->here.lock(), jump_arg);
    } else {
      ERROR << "Exiting through " << exit_inst->ToAsmStr() << "->instruction body (?!)";
    }
  } else {
    ERROR << "Exiting through unknown instruction??";
  }
  WakeWidgetsAnimation();
}

std::shared_ptr<Object> Assembler::Clone() const { return std::make_shared<Assembler>(); }

void UpdateCode(automat::mc::Controller& controller,
                std::vector<std::shared_ptr<automat::library::Instruction>>&& instructions,
                maf::Status& status) {
  // Sorting allows us to more efficiently search for instructions.
  using comp = std::owner_less<std::shared_ptr<automat::library::Instruction>>;
  std::sort(instructions.begin(), instructions.end(), comp{});

  int n = instructions.size();

  automat::mc::Program program;
  program.resize(n);

  for (int i = 0; i < n; ++i) {
    automat::library::Instruction* obj_raw = instructions[i].get();
    const automat::mc::Inst* inst_raw = &obj_raw->mc_inst;
    auto* loc = obj_raw->here.lock().get();
    int next = -1;
    int jump = -1;
    if (loc) {
      auto FindInstruction = [loc, &instructions, n](const automat::Argument& arg) -> int {
        if (auto it = loc->outgoing.find(&arg); it != loc->outgoing.end()) {
          automat::Location* to_loc = &(*it)->to;
          if (auto to_inst = to_loc->As<automat::library::Instruction>()) {
            auto it = std::lower_bound(instructions.begin(), instructions.end(), to_loc->object,
                                       std::owner_less{});
            if (it != instructions.end()) {
              return std::distance(instructions.begin(), it);
            }
          }
        }
        return -1;
      };
      next = FindInstruction(next_arg);
      jump = FindInstruction(library::jump_arg);
    }
    program[i].next = next;
    program[i].jump = jump;
  }
  for (int i = 0; i < n; ++i) {
    Instruction* obj_raw = instructions[i].get();
    const mc::Inst* inst_raw = &obj_raw->mc_inst;
    program[i].inst = std::shared_ptr<const mc::Inst>(std::move(instructions[i]), inst_raw);
  }

  controller.UpdateCode(std::move(program), status);
}

// Returning arrays of shared_ptrs is really bad but it seems to be necessary here.
std::vector<std::shared_ptr<Instruction>> FindInstructions(Location& assembler_loc) {
  auto [begin, end] = assembler_loc.incoming.equal_range(&assembler_arg);
  std::vector<std::shared_ptr<Instruction>> instructions;
  for (auto it = begin; it != end; ++it) {
    auto& conn = *it;
    auto& inst_loc = conn->from;
    auto inst = inst_loc.As<Instruction>();
    if (!inst) {
      continue;
    }
    instructions.push_back(inst->SharedPtr());
  }
  return instructions;
}

void Assembler::UpdateMachineCode() {
  auto here_ptr = here.lock();
  if (!here_ptr) {
    return;
  }
  auto instructions = FindInstructions(*here_ptr);
  Status status;
  library::UpdateCode(*mc_controller, std::move(instructions), status);
  if (!OK(status)) {
    ERROR << "Failed to update Assembler: " << status;
  }
}

void Assembler::RunMachineCode(library::Instruction* entry_point) {
  {  // Mark instructions as "long running", using the assembler as the LongRunning object.
    auto here_ptr = here.lock();
    if (here_ptr) {
      auto [begin, end] = here_ptr->incoming.equal_range(&assembler_arg);
      for (auto it = begin; it != end; ++it) {
        auto& conn = *it;
        auto& inst_loc = conn->from;
        auto inst = inst_loc.As<Instruction>();
        if (!inst) {
          continue;
        }
        inst_loc.long_running = this;
      }
    }
  }

  Status status;
  mc_controller->Execute(entry_point->ToMC(), status);
  if (!OK(status)) {
    ERROR << "Failed to execute Assembler: " << status;
  }
}

void Assembler::Cancel() {
  Status status;
  mc_controller->Cancel(status);
  if (!OK(status)) {
    ERROR << "Failed to cancel Assembler: " << status;
    return;
  }
  auto* loc = here.lock().get();
  auto [begin, end] = loc->incoming.equal_range(&assembler_arg);
  for (auto it = begin; it != end; ++it) {
    auto& conn = *it;
    auto& inst_loc = conn->from;
    if (auto inst = inst_loc.As<Instruction>()) {
      inst_loc.long_running = nullptr;
    }
  }
}

AssemblerWidget::AssemblerWidget(std::weak_ptr<Assembler> assembler_weak)
    : assembler_weak(assembler_weak) {}

std::string_view AssemblerWidget::Name() const { return "Assembler"; }
SkPath AssemblerWidget::Shape() const { return SkPath::RRect(kRRect.sk); }

static constexpr float kFlatBorderWidth = 3_mm;
static constexpr RRect kBorderLightsRRect = AssemblerWidget::kRRect.Outset(-kFlatBorderWidth / 2);
static constexpr RRect kBorderMidRRect = AssemblerWidget::kRRect.Outset(-kFlatBorderWidth);
static constexpr RRect kInnerRRect = kBorderMidRRect.Outset(-kFlatBorderWidth);

animation::Phase AssemblerWidget::Tick(time::Timer& timer) {
  auto assembler = assembler_weak.lock();
  if (!assembler || assembler->mc_controller == nullptr) {
    return animation::Finished;
  }
  animation::Phase phase;
  Status status;
  assembler->mc_controller->GetState(state, status);
  if (!OK(status)) {
    ERROR << "Failed to get Assembler state: " << status;
    return animation::Finished;
  }
  if (state.current_instruction.lock()) {
    phase |= animation::Animating;
  }
  // Register widgets indexed by register index.
  std::array<RegisterWidget*, kGeneralPurposeRegisterCount> reg_widgets_idx = {};

  // Index register widgets by register index.
  // Also remove any register widgets that have been deleted.
  for (int i = 0; i < reg_widgets.size(); ++i) {
    auto& reg_widget = reg_widgets[i];
    auto* register_widget = reg_widget.get();
    if (auto register_obj = register_widget->register_weak.lock()) {
      reg_widgets_idx[register_obj->register_index] = register_widget;
    } else {
      reg_widgets.EraseIndex(i);
      --i;
    }
  }
  // Create new register objects for registers that have non-zero values.
  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    auto new_value = state.regs[i];
    if (new_value == 0) continue;
    if (assembler->reg_objects_idx[i] != nullptr) continue;
    assembler->reg_objects_idx[i] = std::make_shared<Register>(assembler_weak, i);
  }

  // Create new register widgets for register objects that don't have a widget.
  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    if (assembler->reg_objects_idx[i] == nullptr) continue;
    if (reg_widgets_idx[i] != nullptr) continue;
    auto new_value = state.regs[i];
    auto register_widget =
        std::make_shared<RegisterWidget>(assembler->reg_objects_idx[i]->WeakPtr());
    register_widget->parent = SharedPtr();
    register_widget->FixParents();
    register_widget->local_to_parent = SkM44::Translate(0, 10_cm);
    reg_widgets_idx[i] = register_widget.get();
    reg_widgets.emplace_back(std::move(register_widget));
    std::sort(reg_widgets.begin(), reg_widgets.end(), [](const auto& a, const auto& b) {
      auto* a_register_widget = static_cast<RegisterWidget*>(a.get());
      auto* b_register_widget = static_cast<RegisterWidget*>(b.get());
      return a_register_widget->register_weak.lock()->register_index <
             b_register_widget->register_weak.lock()->register_index;
    });
  }
  // Update register widgets where the value has changed.
  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    if (reg_widgets_idx[i] == nullptr) continue;
    auto new_value = state.regs[i];
    auto child = reg_widgets_idx[i];
    if (child->reg_value != new_value) {
      child->reg_value = new_value;
      child->WakeAnimation();
    }
  }
  int n = reg_widgets.size();
  int columns = std::ceil(std::sqrt(n));
  int rows = n ? (n + columns - 1) / columns : 0;
  int total_cells = columns * rows;
  int empty_cells_in_first_row = total_cells - n;

  constexpr float kMargin = 1_cm;

  constexpr float kCellMarginWidth = RegisterWidget::kBaseRect.Width() + kMargin;

  float total_width = RegisterWidget::kBaseRect.Width() * columns + kMargin * (columns + 1);
  float available_width = kInnerRRect.rect.Width();
  float target_scale = available_width / total_width;

  for (int child_i = 0; child_i < reg_widgets.size(); ++child_i) {
    auto* child = static_cast<RegisterWidget*>(reg_widgets[child_i].get());

    int effective_i = child_i + empty_cells_in_first_row;
    int row = effective_i / columns;
    int column;
    int columns_in_row;
    if (row == 0) {
      column = child_i;
      columns_in_row = columns - empty_cells_in_first_row;
    } else {
      column = effective_i % columns;
      columns_in_row = columns;
    }
    float x = column * (RegisterWidget::kBaseRect.Width() + kMargin) -
              ((columns_in_row - 1) / 2.f) * (RegisterWidget::kBaseRect.Width() + kMargin);
    float y = -row * (RegisterWidget::kBaseRect.Height() + kMargin) +
              ((rows - 1) / 2.f) * (RegisterWidget::kBaseRect.Height() + kMargin);

    SkMatrix child_mat = child->local_to_parent.asM33();

    SkMatrix target_mat = SkMatrix::Scale(target_scale, target_scale);
    target_mat.preTranslate(x, y);

    phase |=
        animation::ExponentialApproach(target_scale, timer.d, 0.2, child_mat[SkMatrix::kMScaleX]);
    phase |=
        animation::ExponentialApproach(target_scale, timer.d, 0.2, child_mat[SkMatrix::kMScaleY]);
    phase |= animation::ExponentialApproach(target_mat[SkMatrix::kMTransX], timer.d, 0.2,
                                            child_mat[SkMatrix::kMTransX]);
    phase |= animation::ExponentialApproach(target_mat[SkMatrix::kMTransY], timer.d, 0.2,
                                            child_mat[SkMatrix::kMTransY]);

    child->local_to_parent = SkM44(child_mat);
  }

  return phase;
}

void AssemblerWidget::Draw(SkCanvas& canvas) const {
  float one_pixel = 1.0f / canvas.getTotalMatrix().getScaleX();
  SkPaint flat_border_paint;
  flat_border_paint.setColor("#9b252a"_color);
  canvas.drawDRRect(kRRect.sk, kBorderMidRRect.sk, flat_border_paint);
  SkPaint bevel_border_paint;
  bevel_border_paint.setColor("#7d2627"_color);
  SetRRectShader(bevel_border_paint, kBorderMidRRect, "#3a2021"_color, "#7e2627"_color,
                 "#d86355"_color);

  canvas.drawDRRect(kBorderMidRRect.sk, kInnerRRect.sk, bevel_border_paint);

  SkPaint bg_paint = [&]() {
    static auto builder =
        resources::RuntimeEffectBuilder(embedded::assets_assembler_stars_rt_sksl.content);

    builder->uniform("uv_to_pixel") = canvas.getTotalMatrix();

    auto shader = builder->makeShader();
    SkPaint paint;
    paint.setShader(shader);
    return paint;
  }();
  canvas.drawRRect(kInnerRRect.Outset(one_pixel).sk, bg_paint);

  canvas.save();
  canvas.clipRRect(kInnerRRect.sk);

  DrawChildren(canvas);
  canvas.restore();

  constexpr int kNumLights = 4 * 6;
  Vec2 light_positions[kNumLights];
  kBorderLightsRRect.EquidistantPoints(light_positions);
  Vec2 center{};
  constexpr float kLightRange = 5_mm;
  constexpr float kLightRadius = 1_mm;

  SkColor bulb_colors[] = {
      "#ffffa2"_color,  // light center
      "#ffff70"_color,  // light mid
      "#ffff93"_color,  // outer light edge (faint yellow)
  };
  SkPaint bulb_paint;
  bulb_paint.setShader(SkGradientShader::MakeRadial(center, kLightRadius, bulb_colors, nullptr, 3,
                                                    SkTileMode::kClamp));

  SkColor glow_colors[] = {
      "#5b0e00"_color,    // shadow
      "#5b0e00"_color,    // shadow
      "#ec4329"_color,    // warm red
      "#ec432980"_color,  // half-transparent warm red
      "#ec432900"_color,  // transparent warm red
  };
  SkPaint glow_paint;
  float glow_positions[] = {0, kLightRadius / kLightRange, kLightRadius * 1.1 / kLightRange,
                            kLightRadius * 2 / kLightRange, 1};
  glow_paint.setShader(SkGradientShader::MakeRadial(center, kLightRange, glow_colors,
                                                    glow_positions, 5, SkTileMode::kClamp));
  canvas.save();
  canvas.clipRRect(kRRect.sk);
  canvas.clipRRect(kBorderMidRRect.sk, SkClipOp::kDifference);
  for (int i = 0; i < kNumLights; ++i) {
    canvas.save();
    canvas.translate(light_positions[i].x, light_positions[i].y);
    canvas.drawCircle(0, 0, kLightRange, glow_paint);
    canvas.drawCircle(0, 0, kLightRadius, bulb_paint);
    canvas.restore();
  }
  canvas.restore();
}

void AssemblerWidget::FillChildren(maf::Vec<std::shared_ptr<gui::Widget>>& children) {
  // Expensive copy of a bunch of shared_ptrs :/
  for (auto& child : reg_widgets) {
    children.emplace_back(child->SharedPtr());
  }
}

void AssemblerWidget::TransformUpdated() { WakeAnimation(); }

SkPath RegisterWidget::Shape() const { return SkPath::Rect(kBoundingRect.sk); }
std::string_view RegisterWidget::Name() const { return "Register"; }

static const SkPath kFlagPole = PathFromSVG(
    "m-.5-.7c-1.8-7.1-2.3-14.5-2.5-21.9-.3.2-.8.3-1.3.4.7-1 1.4-1.8 1.8-3 .3 1.2.8 2 1.6 2.9-.4 "
    "0-.7-.1-1.2-.3 0 7.4 1 14.7 2.5 21.9.5.2.8.5.9.7h-2.5c.1-.2.3-.5.7-.7z");

static const SkPath kFlag = PathFromSVG(
    R"(m-3.5-21.7c.2-.5 3.1 1 4.6.9 1.6-.1 3.1-1.4 4.7-1.3 1.5.1 2.6 1.8 4.1 1.9 2 .2 3.9-1.4 6-1.5 2.7-.1 8 1.2 8 1.2s-6.7 1-9.7 2.5c-1.8.8-2.8 3-4.7 3.6-1.3.4-2.6-.7-3.9-.4-1.7.4-2.8 2.2-4.4 2.8-1.3.5-4.1.9-4.2.5-.4-3.4-.8-6.6-.6-10.2z)");

static constexpr float kBitPositionFontSize = RegisterWidget::kCellHeight * 0.42;

static gui::Font& BitPositionFont() {
  static auto font = gui::Font::MakeV2(gui::Font::GetGrenzeRegular(), kBitPositionFontSize);
  return *font;
}

static constexpr float kByteValueFontSize = 3_mm;  // RegisterWidget::kCellHeight * 1;

static gui::Font& ByteValueFont() {
  static auto font = gui::Font::MakeV2(gui::Font::GetHeavyData(), kByteValueFontSize);
  return *font;
}

// Shift the byte values up so that they're vertically centered with their rows
static constexpr float kByteValueFontShiftUp =
    (RegisterWidget::kCellHeight - kByteValueFontSize) / 2;

// Shift the font up, so that its top is aligned with the middle of the cell
static constexpr float kBitPositionFontShiftUp =
    RegisterWidget::kCellHeight / 2 - kBitPositionFontSize;

void RegisterWidget::Draw(SkCanvas& canvas) const {
  int register_index = 0;
  if (auto register_obj = register_weak.lock()) {
    register_index = register_obj->register_index;
  }
  SkPaint dark_paint;
  dark_paint.setColor("#dcca85"_color);
  canvas.drawRect(kBaseRect.sk, dark_paint);
  SkPaint light_paint;
  light_paint.setColor("#fefdfb"_color);

  auto& bit_position_font = BitPositionFont();
  auto& byte_value_font = ByteValueFont();
  for (int row = 0; row < 8; ++row) {
    float bottom = kInnerRect.bottom + kCellHeight * row;
    float top = bottom + kCellHeight;
    int byte_value = (reg_value >> (row * 8)) & 0xFF;
    canvas.save();
    canvas.translate(kBaseRect.right + 0.5_mm, bottom + kByteValueFontShiftUp);
    auto byte_value_str = f("%02X", byte_value);
    byte_value_font.DrawText(canvas, byte_value_str, dark_paint);
    canvas.restore();
    for (int bit = 0; bit < 8; ++bit) {
      float right = kInnerRect.right - kCellWidth * bit;
      float left = right - kCellWidth;
      SkPaint* cell_paint = &light_paint;
      if (bit % 2 == row % 2) {
        // light cell
        canvas.drawRect(SkRect::MakeLTRB(left, bottom, right, top), light_paint);
        cell_paint = &dark_paint;
      }

      int position = row * 8 + bit;
      std::string position_str = f("%d", position);
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
      SkColor colors[5] = {"#ff0000"_color, "#800000"_color, "#ff0000"_color, "#800000"_color,
                           "#ff0000"_color};
      flag_paint.setShader(
          SkGradientShader::MakeLinear(points, colors, nullptr, 5, SkTileMode::kClamp));
      if (reg_value & (1ULL << position)) {
        canvas.save();
        canvas.translate(left + kCellWidth * 0.2, bottom);
        canvas.scale(0.5, 0.5);
        canvas.drawPath(kFlagPole, pole_paint);
        canvas.drawPath(kFlag, flag_paint);
        canvas.restore();
      }
    }
  }

  canvas.save();

  canvas.translate(-kRegisterIconWidth / 2, kBaseRect.top - kRegisterIconWidth * 0.15);
  kRegisters[register_index].image.draw(canvas);
  canvas.restore();
}

void RegisterWidget::VisitOptions(const OptionsVisitor& visitor) const {
  auto register_obj = register_weak.lock();
  RegisterMenuOption register_menu_option = {register_obj->assembler_weak,
                                             register_obj->register_index};
  register_menu_option.VisitOptions(visitor);
}

Register::Register(std::weak_ptr<Assembler> assembler_weak, int register_index)
    : assembler_weak(assembler_weak), register_index(register_index) {}
std::shared_ptr<Object> Register::Clone() const {
  return std::make_shared<Register>(assembler_weak, register_index);
}
}  // namespace automat::library
