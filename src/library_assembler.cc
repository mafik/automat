// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_assembler.hh"

#include <include/core/SkMatrix.h>
#include <include/effects/SkGradientShader.h>
#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCInstBuilder.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#include "animation.hh"
#include "automat.hh"
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

namespace automat::library {

struct ShowRegisterOption : TextOption {
  WeakPtr<Assembler> weak;
  int register_index;  // Must be < kGeneralPurposeRegisterCount

  ShowRegisterOption(WeakPtr<Assembler> weak, int register_index)
      : TextOption("Show"), weak(weak), register_index(register_index) {}

  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<ShowRegisterOption>(weak, register_index);
  }

  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (auto assembler = weak.lock()) {
      assembler->reg_objects_idx[register_index] = MAKE_PTR(Register, weak, register_index);
      assembler->WakeToys();
    }
    return nullptr;
  }
};

struct HideRegisterOption : TextOption {
  WeakPtr<Assembler> weak;
  int register_index;  // Must be < kGeneralPurposeRegisterCount

  HideRegisterOption(WeakPtr<Assembler> weak, int register_index)
      : TextOption("Hide"), weak(weak), register_index(register_index) {}

  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<HideRegisterOption>(weak, register_index);
  }

  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (auto assembler = weak.lock()) {
      assembler->reg_objects_idx[register_index].reset();
      assembler->WakeToys();
    }
    return nullptr;
  }
};

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

struct RegisterMenuOption : Option, OptionsProvider {
  WeakPtr<Assembler> weak;
  int register_index;

  RegisterMenuOption(WeakPtr<Assembler> weak, int register_index)
      : weak(weak), register_index(register_index) {}

  std::unique_ptr<ui::Widget> MakeIcon(ui::Widget* parent) override {
    return std::make_unique<ImageWidget>(parent, kRegisters[register_index].image);
  }
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<RegisterMenuOption>(weak, register_index);
  }
  void VisitOptions(const OptionsVisitor& visitor) const override {
    auto assembler = weak.lock();
    auto& reg = assembler->reg_objects_idx[register_index];
    if (reg.is_shared || reg.weak.IsExpired()) {
      if (assembler->reg_objects_idx[register_index] == nullptr) {
        ShowRegisterOption show{weak, register_index};
        visitor(show);
      } else {
        HideRegisterOption hide{weak, register_index};
        visitor(hide);
      }
    }
  }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    return OpenMenu(pointer);
  }
};

struct RegistersMenuOption : TextOption, OptionsProvider {
  WeakPtr<Assembler> weak;
  RegistersMenuOption(WeakPtr<Assembler> weak) : TextOption("Registers"), weak(weak) {}
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<RegistersMenuOption>(weak);
  }
  void VisitOptions(const OptionsVisitor& visitor) const override {
    for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
      RegisterMenuOption opt{weak, i};
      visitor(opt);
    }
  }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    return OpenMenu(pointer);
  }
  Dir PreferredDir() const override { return S; }
};

void AssemblerWidget::VisitOptions(const OptionsVisitor& visitor) const {
  Object::Toy::VisitOptions(visitor);
  RegistersMenuOption registers_option{owner.Copy<Assembler>()};
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

static animation::Phase RefreshState(Assembler& assembler, time::SteadyPoint now) {
  if (assembler.mc_controller == nullptr) {
    return animation::Finished;
  }
  if (now > assembler.last_state_refresh) {
    auto old_regs = assembler.state.regs;
    Status ignore;
    assembler.mc_controller->GetState(assembler.state, ignore);
    // Wake all registers widgets where values have changed.
    for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
      if (old_regs[i] != assembler.state.regs[i]) {
        if (auto reg = assembler.reg_objects_idx[i].lock()) {
          reg->WakeToys();
        }
      }
    }
    assembler.last_state_refresh = now;
  }
  if (assembler.running.IsRunning()) {
    return animation::Animating;
  } else {
    return animation::Finished;
  }
}

void Assembler::ExitCallback(mc::CodePoint code_point) {
  running.Done(*this);
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
      ScheduleArgumentTargets(*exit_inst, jump_arg);
    } else {
      ERROR << "Exiting through " << exit_inst->ToAsmStr() << "->instruction body (?!)";
    }
  } else {
    ERROR << "Exiting through unknown instruction??";
  }
}

Ptr<Object> Assembler::Clone() const { return MAKE_PTR(Assembler); }

void Assembler::Atoms(const std::function<void(Atom&)>& cb) {
  cb(*this);
  cb(running);
}

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
      auto FindInstruction = [obj_raw, &instructions, n](const automat::Argument& arg) -> int {
        if (auto target = arg.Find(*obj_raw)) {
          if (auto* to_inst = dynamic_cast<automat::library::Instruction*>(target.Get())) {
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
      next = FindInstruction(next_arg);
      jump = FindInstruction(library::jump_arg);
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
  running.BeginLongRunning(std::move(run_task));

  Status status;
  auto inst = entry_point->ToMC();
  RefreshState(*this, time::SteadyNow());
  mc_controller->Execute(inst, status);
  if (!OK(status)) {
    ERROR << "Failed to execute Assembler: " << status;
  }
}

void Assembler::Running::OnCancel() {
  Status status;
  auto& as = GetAssembler();
  as.mc_controller->Cancel(status);
  if (!OK(status)) {
    ERROR << "Failed to cancel Assembler: " << status;
    return;
  }
}

Ptr<Location> Assembler::Extract(Object& descendant) {
  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    auto* reg = reg_objects_idx[i].get();
    if (reg != &descendant) continue;
    auto loc = MAKE_PTR(Location, root_location);
    loc->InsertHere(reg_objects_idx[i].borrow());
    audio::Play(embedded::assets_SFX_toolbar_pick_wav);
    WakeToys();
    return loc;
  }
  return nullptr;
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
    if (mc_state.regs[i] == 0) continue;
    writer.Key(reg.name.data(), reg.name.size());
    auto hex_value = ValToHex(mc_state.regs[i]);
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
        static_assert(sizeof(state.regs[i]) == 8);
        HexToBytesUnchecked(hex_value, (char*)&state.regs[i]);
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
    : Object::Toy(parent, assembler) {}

std::string_view AssemblerWidget::Name() const { return "Assembler"; }
SkPath AssemblerWidget::Shape() const { return SkPath::RRect(kRRect.sk); }

static constexpr float kFlatBorderWidth = 3_mm;
static constexpr RRect kBorderLightsRRect = AssemblerWidget::kRRect.Outset(-kFlatBorderWidth / 2);
static constexpr RRect kBorderMidRRect = AssemblerWidget::kRRect.Outset(-kFlatBorderWidth);
static constexpr RRect kInnerRRect = kBorderMidRRect.Outset(-kFlatBorderWidth);

animation::Phase AssemblerWidget::Tick(time::Timer& timer) {
  auto assembler = LockObject<Assembler>();
  if (!assembler || assembler->mc_controller == nullptr) {
    return animation::Finished;
  }
  animation::Phase phase = RefreshState(*assembler, timer.now);
  // Register widgets indexed by register index.
  std::array<RegisterWidget*, kGeneralPurposeRegisterCount> reg_widgets_idx = {};

  // Index register widgets by register index. Delete them if their register object is gone or if
  // they're no longer owned by the assembler.
  for (int i = 0; i < reg_widgets.size(); ++i) {
    auto* reg_widget = reg_widgets[i];
    auto register_obj = reg_widget->LockRegister();
    int register_index = -1;
    if (register_obj != nullptr) {
      register_index = register_obj->register_index;
      auto assembler_reg = assembler->reg_objects_idx[register_index].get();
      if (assembler_reg == nullptr) {
        register_index = -1;
      }
    }
    if (register_index == -1) {
      reg_widgets.EraseIndex(i);
      --i;
    } else {
      reg_widgets_idx[register_index] = reg_widget;
    }
  }
  // Create new register objects for registers that have non-zero values.
  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    if (assembler->state.regs[i] == 0) continue;
    if (assembler->reg_objects_idx[i] != nullptr) continue;
    assembler->reg_objects_idx[i] = MAKE_PTR(Register, owner.Copy<Assembler>(), i);
  }

  // Create new register widgets for register objects that don't have a widget.
  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    auto assembler_reg = assembler->reg_objects_idx[i].get();
    // Now create a widget if needed.
    if (assembler_reg) {
      if (reg_widgets_idx[i] != nullptr) continue;
      auto* register_widget = ToyStore().FindOrNull(*assembler_reg);
      if (register_widget == nullptr) {
        register_widget = &ToyStore().FindOrMake(*assembler_reg, this);
        register_widget->local_to_parent = SkM44::Translate(0, 10_cm);
      } else {
        register_widget->Reparent(*this);
      }
      reg_widgets_idx[i] = register_widget;
      reg_widgets.emplace_back(register_widget);
      register_widget->WakeAnimation();
      register_widget->ValidateHierarchy();
      std::sort(reg_widgets.begin(), reg_widgets.end(), [](auto* a, auto* b) {
        return a->LockRegister()->register_index < b->LockRegister()->register_index;
      });
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
    auto* child = reg_widgets[child_i];

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
    Status status;
    static auto effect = resources::CompileShader(embedded::assets_assembler_stars_rt_sksl, status);
    assert(effect);
    SkRuntimeEffectBuilder builder(effect);
    builder.uniform("uv_to_pixel") = canvas.getTotalMatrix();
    auto shader = builder.makeShader();
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

void AssemblerWidget::FillChildren(Vec<ui::Widget*>& children) {
  for (auto* child : reg_widgets) {
    children.emplace_back(child);
  }
}

void AssemblerWidget::TransformUpdated() {
  WakeAnimation();
  RedrawThisFrame();
}

bool AssemblerWidget::CanDrop(Location& loc) const {
  if (auto reg = loc.As<Register>()) {
    if (auto my_assembler = LockObject<Assembler>()) {
      if (auto my_reg = my_assembler->reg_objects_idx[reg->register_index].lock()) {
        return my_reg.get() == reg;
      }
    }
  }
  return false;
}

void AssemblerWidget::DropLocation(Ptr<Location>&& loc) {
  if (auto reg = loc->As<Register>()) {
    if (auto my_assembler = LockObject<Assembler>()) {
      my_assembler->reg_objects_idx[reg->register_index] = loc->Take().Cast<Register>();
      my_assembler->WakeToys();
    }
  }
}
SkMatrix AssemblerWidget::DropSnap(const Rect& bounds, Vec2 bounds_origin, Vec2* fixed_point) {
  auto local_to_machine = TransformBetween(*this, *root_machine);
  auto my_rect = kRRect.rect.Outset(-2 * kFlatBorderWidth);
  local_to_machine.mapRect(&my_rect.sk);
  SkMatrix matrix;
  if (bounds.left < my_rect.left) {
    matrix.postTranslate(my_rect.left - bounds.left, 0);
  }
  if (bounds.right > my_rect.right) {
    matrix.postTranslate(my_rect.right - bounds.right, 0);
  }
  if (bounds.bottom < my_rect.bottom) {
    matrix.postTranslate(0, my_rect.bottom - bounds.bottom);
  }
  if (bounds.top > my_rect.top) {
    matrix.postTranslate(0, my_rect.top - bounds.top);
  }
  return matrix;
}

SkPath RegisterWidget::Shape() const { return SkPath::Rect(kBoundingRect.sk); }
std::string_view RegisterWidget::Name() const { return "Register"; }

static const SkPath kFlagPole = PathFromSVG(
    "m-.5-.7c-1.8-7.1-2.3-14.5-2.5-21.9-.3.2-.8.3-1.3.4.7-1 1.4-1.8 1.8-3 .3 1.2.8 2 1.6 2.9-.4 "
    "0-.7-.1-1.2-.3 0 7.4 1 14.7 2.5 21.9.5.2.8.5.9.7h-2.5c.1-.2.3-.5.7-.7z");

static const SkPath kFlag = PathFromSVG(
    R"(m-3.5-21.7c.2-.5 3.1 1 4.6.9 1.6-.1 3.1-1.4 4.7-1.3 1.5.1 2.6 1.8 4.1 1.9 2 .2 3.9-1.4 6-1.5 2.7-.1 8 1.2 8 1.2s-6.7 1-9.7 2.5c-1.8.8-2.8 3-4.7 3.6-1.3.4-2.6-.7-3.9-.4-1.7.4-2.8 2.2-4.4 2.8-1.3.5-4.1.9-4.2.5-.4-3.4-.8-6.6-.6-10.2z)");

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

// Shift the byte values up so that they're vertically centered with their rows
static constexpr float kByteValueFontShiftUp =
    (RegisterWidget::kCellHeight - kByteValueFontSize) / 2;

// Shift the font up, so that its top is aligned with the middle of the cell
static constexpr float kBitPositionFontShiftUp =
    RegisterWidget::kCellHeight / 2 - kBitPositionFontSize;

animation::Phase RegisterWidget::Tick(time::Timer& timer) {
  animation::Phase phase = animation::Finished;
  if (auto register_obj = LockRegister()) {
    auto register_index = register_obj->register_index;
    if (auto assembler = register_obj->assembler_weak.lock()) {
      phase = RefreshState(*assembler, timer.now);
      auto reg_value = assembler->state.regs[register_index];
    }
  }
  return phase;
}

void RegisterWidget::Draw(SkCanvas& canvas) const {
  int register_index = 0;
  uint64_t reg_value = 0;
  if (auto register_obj = LockRegister()) {
    register_index = register_obj->register_index;
    if (auto assembler = register_obj->assembler_weak.lock()) {
      reg_value = assembler->state.regs[register_index];
    }
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
    auto byte_value_str = f("{:02X}", byte_value);
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
  Object::Toy::VisitOptions(visitor);
  auto register_obj = LockRegister();
  RegisterMenuOption register_menu_option = {register_obj->assembler_weak,
                                             register_obj->register_index};
  register_menu_option.VisitOptions(visitor);
}

Register::Register(WeakPtr<Assembler> assembler_weak, int register_index)
    : assembler_weak(assembler_weak), register_index(register_index) {}

Ptr<Object> Register::Clone() const { return MAKE_PTR(Register, assembler_weak, register_index); }

struct RegisterAssemblerArgument : Argument {
  StrView Name() const override { return "Reg's Assembler"sv; }

  float AutoconnectRadius() const override { return INFINITY; }
  SkColor Tint() const override { return "#ff0000"_color; }
  Style GetStyle() const override { return Style::Spotlight; }

  void CanConnect(Object& start, Atom& end, Status& status) const override {
    if (!dynamic_cast<Assembler*>(&end)) {
      AppendErrorMessage(status) += "Must connect to an Assembler";
    }
  }

  void Connect(Object& start, const NestedPtr<Atom>& end) override {
    if (auto* reg = dynamic_cast<Register*>(&start)) {
      if (end) {
        if (auto* assembler = dynamic_cast<Assembler*>(end.Get())) {
          reg->assembler_weak = assembler->AcquireWeakPtr();
        }
      } else {
        reg->assembler_weak = {};
      }
    }
  }

  NestedPtr<Atom> Find(const Object& start) const override {
    auto* reg = dynamic_cast<const Register*>(&start);
    if (reg == nullptr) return {};
    return reg->assembler_weak.Lock();
  }
};

static RegisterAssemblerArgument register_assembler_arg;

void Register::Atoms(const std::function<void(Atom&)>& cb) { cb(register_assembler_arg); }

void Register::SetText(std::string_view text) {
  auto assembler = assembler_weak.Lock();
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
        state.regs[register_index] = reg_value;
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
