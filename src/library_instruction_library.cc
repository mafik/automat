// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_instruction_library.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkPathMeasure.h>
#include <llvm/MC/MCInstBuilder.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/lib/Target/X86/MCTargetDesc/X86MCTargetDesc.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#include <cmath>
#include <ranges>

#include "animation.hh"
#include "embedded.hh"
#include "font.hh"
#include "library_instruction.hh"
#include "llvm_asm.hh"
#include "math.hh"
#include "root_widget.hh"
#include "textures.hh"
#include "time.hh"

using namespace std;
using namespace llvm;
using namespace maf;

namespace automat::library {

const unsigned SetImmediateGroupOpcodes[] = {
    X86::MOV8ri,
    X86::MOV16ri,
    X86::MOV32ri,
    X86::MOV64ri,
};

constexpr InstructionGroup SetImmediateGroup = {
    .name = "Set to",
    .opcodes = SetImmediateGroupOpcodes,
};

const unsigned CopyRegisterGroupOpcodes[] = {
    X86::MOV8rr,
    X86::MOV16rr,
    X86::MOV32rr,
    X86::MOV64rr,
};

constexpr InstructionGroup CopyRegisterGroup = {
    .name = "Copy",
    .opcodes = CopyRegisterGroupOpcodes,
};

constexpr InstructionGroup BaseGroups[] = {SetImmediateGroup, CopyRegisterGroup, CopyRegisterGroup,
                                           CopyRegisterGroup, CopyRegisterGroup};
constexpr InstructionCategory BaseCategory = {
    .name = "Base",
    .groups = BaseGroups,
};

constexpr InstructionCategory JumpsCategory = {
    .name = "Jumps",
    .groups = BaseGroups,
};

constexpr InstructionCategory MathCategory = {
    .name = "Math",
    .groups = BaseGroups,
};

constexpr InstructionCategory LogicCategory = {
    .name = "Logic",
    .groups = BaseGroups,
};

constexpr InstructionCategory StackCategory = {
    .name = "Stack",
    .groups = BaseGroups,
};

constexpr InstructionCategory InstructionCategories[] = {BaseCategory, JumpsCategory, MathCategory,
                                                         LogicCategory, StackCategory};

const std::span<const InstructionCategory> InstructionLibrary::kCategories = InstructionCategories;

InstructionLibrary::InstructionLibrary() {
  Filter();
  selected_category = 1;
}

void InstructionLibrary::Filter() {
  instructions.clear();
  auto& mc_instr_info = *LLVM_Assembler::Get().mc_instr_info;
  auto& mc_inst_printer = *LLVM_Assembler::Get().mc_inst_printer;
  auto& mc_subtarget_info = *LLVM_Assembler::Get().mc_subtarget_info;
  auto& mc_reg_info = *LLVM_Assembler::Get().mc_reg_info;
  auto& mc_asm_info = *LLVM_Assembler::Get().mc_asm_info;

  int n = mc_instr_info.getNumOpcodes();
  for (int i = 0; i < n; i++) {
    auto name = mc_instr_info.getName(i);

    MCInstBuilder builder(i);

    // TODO: Custom tablegen processor to output just the instructions we want & info we need

    // TODO: Atomic instructions
    if (name.contains("LOCK_")) continue;
    // TODO: EVEX (requires Advanced Performance Extensions - only on newer CPUs - else crash)
    if (name.contains("_NF")) continue;
    if (name.contains("_EVEX")) continue;
    if (name.contains("_ND")) continue;
    if (name.contains("_REV")) continue;
    if (name.contains("_alt")) continue;

    const MCInstrDesc& op = mc_instr_info.get(i);

    if (op.isPreISelOpcode()) continue;
    if (op.isPseudo()) continue;
    if (op.isMetaInstruction()) continue;

    // TODO: Memory-related instructions
    if (op.mayLoad()) continue;
    if (op.mayStore()) continue;

    int num_operands = op.getNumOperands();

    auto operands = op.operands();
    for (int operand_i = 0; operand_i < operands.size(); ++operand_i) {
      auto& operand = operands[operand_i];
      if (operand.OperandType == MCOI::OPERAND_REGISTER) {
        const MCRegisterClass& reg_class = mc_reg_info.getRegClass(operand.RegClass);
        auto reg_class_name = mc_reg_info.getRegClassName(&reg_class);

        builder.addReg(reg_class.getRegister(0));
      } else if (operand.OperandType == MCOI::OPERAND_IMMEDIATE) {
        builder.addImm(0);
      } else if (operand.OperandType == MCOI::OPERAND_MEMORY) {
        // TODO: memory...
      } else if (operand.OperandType == MCOI::OPERAND_PCREL) {
        builder.addImm(0);
      } else {
        // TODO: investigate (conditions)
      }
      if (operand.isBranchTarget()) {
      }
      if (auto tied_to = op.getOperandConstraint(operand_i, MCOI::OperandConstraint::TIED_TO);
          tied_to != -1) {
      }
    }

    auto implicit_defs = op.implicit_defs();
    for (auto& def : implicit_defs) {
    }
    for (auto& use : op.implicit_uses()) {
    }

    MCInst& inst = builder;

    std::string info;
    bool is_deprecated = mc_instr_info.getDeprecatedInfo(inst, mc_subtarget_info, info);
    if (is_deprecated) {
      continue;
    }

    instructions.push_back(inst);
  }
}

string_view InstructionLibrary::Name() const { return "Instruction Library"; }

shared_ptr<Object> InstructionLibrary::Clone() const { return make_shared<InstructionLibrary>(); }

InstructionLibrary::Widget::Widget(std::weak_ptr<Object> object) : object(object) {}
SkPath InstructionLibrary::Widget::Shape() const { return SkPath::Circle(0, 0, 10_cm); }

constexpr float kRoseFanDegrees = 120;
constexpr float kStartDist = Instruction::Widget::kHeight / 2;
const float kCornerDist = Instruction::Widget::kDiagonal / 2;
constexpr float kRoseDist = 8_cm;

static SkPathMeasure CategoryPathMeasure(int i, int n) {
  float branch_dir =
      kRoseFanDegrees * M_PI / 180 * i / (n - 1) + (180 - kRoseFanDegrees) * M_PI / 180 / 2;

  SinCos branch_dir_sin_cos = SinCos::FromRadians(branch_dir);

  float angle_off_vertical = branch_dir - M_PI / 2;

  constexpr int kSegmentCount = 2;
  SkPath stem_path;
  Vec2 segment_start = Vec2::Polar(branch_dir_sin_cos, kStartDist);
  float segment_start_angle_offset = 0;
  constexpr float kSegmentLength = (kRoseDist - kStartDist) / kSegmentCount;
  constexpr float kControlPointDistance = kSegmentLength / 2;
  stem_path.moveTo(segment_start);
  for (int segment = 0; segment < kSegmentCount; ++segment) {
    float segment_end_dist = kStartDist + kSegmentLength * (segment + 1);
    Vec2 segment_end = Vec2::Polar(branch_dir_sin_cos, segment_end_dist);
    float segment_end_angle_offset =
        (fmodf(i * 1.7156420f + segment * 1.92345678f + 0.2f, 1.f) - 0.5) * 1.5;  // Pseudo-random

    if (segment == kSegmentCount - 1) {
      segment_end_angle_offset = angle_off_vertical / 3;
    }

    Vec2 cp1 = segment_start +
               Vec2::Polar(branch_dir_sin_cos + SinCos::FromRadians(segment_start_angle_offset),
                           kControlPointDistance);
    Vec2 cp2 = segment_end -
               Vec2::Polar(branch_dir_sin_cos + SinCos::FromRadians(segment_end_angle_offset),
                           kControlPointDistance);
    stem_path.cubicTo(cp1, cp2, segment_end);
    segment_start = segment_end;
    segment_start_angle_offset = segment_end_angle_offset;
  }

  return SkPathMeasure(stem_path, false, 1000);
}

struct StalkMetrics {
  const float kMaximumStalkLength;
  const float kMinimumStalkLength;  // Length of the stalk in its shortest state
  const float kStalkLengthRange;
  const float kStalkTipDistance;

  StalkMetrics(SkPathMeasure& path_measure, float growth)
      : kMaximumStalkLength(path_measure.getLength()),
        kMinimumStalkLength((kCornerDist - kStartDist) / (kRoseDist - kStartDist) *
                            kMaximumStalkLength),
        kStalkLengthRange(kMaximumStalkLength - kMinimumStalkLength),
        kStalkTipDistance(kMinimumStalkLength + kStalkLengthRange * growth) {}
};

static pair<Vec2, float> RosePosition(SkPathMeasure& path_measure, float growth) {
  StalkMetrics stalk_metrics(path_measure, growth);

  Vec2 rose_pos;
  Vec2 rose_tangent;
  (void)path_measure.getPosTan(stalk_metrics.kStalkTipDistance, &rose_pos.sk, &rose_tangent.sk);

  float final_rose_dir = atan(rose_tangent);

  return {rose_pos, final_rose_dir};
}

animation::Phase InstructionLibrary::Widget::Tick(time::Timer& timer) {
  auto object_shared_ptr = object.lock();
  if (!object_shared_ptr) return animation::Finished;

  auto* library = dynamic_cast<InstructionLibrary*>(object_shared_ptr.get());
  if (!library) return animation::Finished;

  animation::Phase phase = animation::Finished;

  lock_guard lock(library->mutex);
  auto& instructions = library->instructions;

  // Animate everything to match the state of `library`.
  auto& root = FindRootWidget();

  constexpr int kMaxInstructions = 10;
  int n = std::min<int>(library->instructions.size(), kMaxInstructions);

  if (instruction_helix.empty()) {
    for (int i = 0; i < n; ++i) {
      instruction_helix.push_back(InstructionCard{library->instructions[i]});
      float t = (float)i / (n - 1);
      instruction_helix.back().angle = CosineInterpolate(90, -90, 0.5 + t / 2);
    }
  }

  for (int i = 0; i < library->kCategories.size(); ++i) {
    if (category_states.size() <= i) {
      category_states.push_back(CategoryState{
          .growth = 0,
      });
      for (int j = 0; j < library->kCategories[i].groups.size(); ++j) {
        category_states.back().leaves.push_back(CategoryState::LeafState{
            .growth = 0,
        });
      }
    }
  }

  for (int i = 0; i < category_states.size(); ++i) {
    auto& category = InstructionLibrary::kCategories[i];
    auto& category_state = category_states[i];
    auto path_measure = CategoryPathMeasure(i, category_states.size());
    StalkMetrics stalk_metrics(path_measure, category_state.growth);

    float target_length = 0;
    if (i == library->selected_category) {
      target_length = 1;
    }
    phase |= category_state.growth.SineTowards(target_length, timer.d, 1);
    phase |= category_state.shake.SpringTowards(0, timer.d, 0.2, 0.5);

    for (int j = 0; j < category.groups.size(); ++j) {
      auto& leaf_state = category_state.leaves[j];

      float group_distance = stalk_metrics.kMinimumStalkLength + stalk_metrics.kStalkLengthRange *
                                                                     (j + 0.5) /
                                                                     (category.groups.size() + 1.5);
      float target_growth = 0;
      float animation_period_seconds = 0.1;
      float target_hue_rotate = 0;
      if (group_distance < stalk_metrics.kStalkTipDistance) {
        animation_period_seconds = 0.5;
        target_growth = 1;
        if (library->selected_group == j) {
          target_growth = 1.1;
          target_hue_rotate = -0.15;
        }
      }
      phase |= leaf_state.growth.SineTowards(target_growth, timer.d, animation_period_seconds);
      phase |= animation::LinearApproach(target_hue_rotate, timer.d, 0.3, leaf_state.hue_rotate);
      phase |= leaf_state.shake.SpringTowards(0, timer.d, 0.1, 0.5);
      Vec2 leaf_base_position;
      Vec2 stalk_tangent;
      (void)path_measure.getPosTan(group_distance, &leaf_base_position.sk, &stalk_tangent.sk);
      SinCos leaf_dir = SinCos::FromVec2(stalk_tangent, 1);
      leaf_dir = leaf_dir + (j % 2 ? 60_deg : -60_deg);

      leaf_state.position = leaf_base_position + Vec2::Polar(leaf_dir, 1_cm * leaf_state.growth);
      leaf_state.radius = 5_mm * leaf_state.growth;
    }

    auto [rose_pos, final_rose_dir] = RosePosition(path_measure, category_state.growth);
    category_state.position = rose_pos + Vec2::Polar(final_rose_dir, 8_mm);
    category_state.radius = 5_mm + 5_mm * category_state.growth;
  }

  return phase;
}

PersistentImage rose0 = PersistentImage::MakeFromAsset(maf::embedded::assets_rose_0_webp,
                                                       PersistentImage::MakeArgs{.width = 3_cm});

PersistentImage rose1 = PersistentImage::MakeFromAsset(maf::embedded::assets_rose_1_webp,
                                                       PersistentImage::MakeArgs{.width = 3_cm});

PersistentImage rose2 = PersistentImage::MakeFromAsset(maf::embedded::assets_rose_2_webp,
                                                       PersistentImage::MakeArgs{.width = 3_cm});

PersistentImage rose3 = PersistentImage::MakeFromAsset(maf::embedded::assets_rose_3_webp,
                                                       PersistentImage::MakeArgs{.width = 3_cm});

PersistentImage rose4 = PersistentImage::MakeFromAsset(maf::embedded::assets_rose_4_webp,
                                                       PersistentImage::MakeArgs{.width = 3_cm});

PersistentImage rose5 = PersistentImage::MakeFromAsset(maf::embedded::assets_rose_5_webp,
                                                       PersistentImage::MakeArgs{.width = 3_cm});

PersistentImage rose6 = PersistentImage::MakeFromAsset(maf::embedded::assets_rose_6_webp,
                                                       PersistentImage::MakeArgs{.width = 3_cm});

PersistentImage* rose_images[] = {&rose0, &rose1, &rose2, &rose3, &rose4, &rose5, &rose6};

PersistentImage stalk = PersistentImage::MakeFromAsset(maf::embedded::assets_stalk_png,
                                                       PersistentImage::MakeArgs{.width = 1_cm});

PersistentImage leaf = PersistentImage::MakeFromAsset(maf::embedded::assets_leaf_webp,
                                                      PersistentImage::MakeArgs{.width = 1.2_cm});
constexpr float kCategoryLetterSize = 3_mm;

static gui::Font& HeavyFont() {
  static auto font = gui::Font::MakeV2(gui::Font::GetGrenzeSemiBold(), kCategoryLetterSize);
  return *font;
}

static gui::Font& RegularFont() {
  static auto font = gui::Font::MakeV2(gui::Font::GetGrenzeRegular(), kCategoryLetterSize);
  return *font;
}

static gui::Font& LightFont() {
  static auto font = gui::Font::MakeV2(gui::Font::GetGrenzeLight(), kCategoryLetterSize);
  return *font;
}

constexpr bool kDebugRoseDrawing = false;

void InstructionLibrary::Widget::Draw(SkCanvas& canvas) const {
  SkPaint text_shadow_paint;
  // It's scaled down by Font::DrawText so it's really in ~pixels
  text_shadow_paint.setMaskFilter(SkMaskFilter::MakeBlur(kOuter_SkBlurStyle, 0.5));
  text_shadow_paint.setColor("#000000"_color);

  SkPaint text_fill_paint;
  text_fill_paint.setStyle(SkPaint::Style::kFill_Style);
  text_fill_paint.setColor("#ffffff"_color);

  SkPaint debug_paint;
  debug_paint.setStyle(SkPaint::Style::kStroke_Style);
  debug_paint.setColor("#ff0000"_color);

  gui::Font font = LightFont();
  // For each category draw a rose:
  // 1. Cubic curve with control points along a line from the center to the edge.
  // 2. Pseudo-random offsets to tangents to make the curve more interesting.
  // 3. Leaves drawn alternatively along the curve, at offset to the current tangent.
  // 4. Rose drawn at the end of the curve.
  for (int i = 0; i < category_states.size(); ++i) {
    // Number between 0 and 1 that indicates how much the rose has grown.
    float growth = category_states[i].growth;

    auto& category = InstructionLibrary::kCategories[i];
    float branch_dir = kRoseFanDegrees * M_PI / 180 * i / (category_states.size() - 1) +
                       (180 - kRoseFanDegrees) * M_PI / 180 / 2;

    SinCos branch_dir_sin_cos = SinCos::FromRadians(branch_dir);

    SkPathMeasure path_measure = CategoryPathMeasure(i, category_states.size());
    StalkMetrics stalk_metrics(path_measure, growth);

    float step_length = 1_cm;
    float step_width = 1_cm;

    float top_width = 5_mm + 2_mm * growth;
    float bottom_width = 9_mm + 2_mm * growth;

    // Draw the stalk
    for (float distance = stalk_metrics.kStalkTipDistance; distance > 0; distance -= step_length) {
      Vec2 start_position;
      Vec2 start_tangent;
      Vec2 end_position;
      Vec2 end_tangent;
      (void)path_measure.getPosTan(distance, &start_position.sk, &start_tangent.sk);
      float end_distance = max<float>(0, distance - step_length);
      (void)path_measure.getPosTan(end_distance, &end_position.sk, &end_tangent.sk);

      Vec2 start_normal = Rotate90DegreesClockwise(start_tangent);
      Vec2 end_normal = Rotate90DegreesClockwise(end_tangent);

      float start_width =
          lerp(top_width, bottom_width,
               (stalk_metrics.kStalkTipDistance - distance) / stalk_metrics.kStalkLengthRange);
      float end_width =
          lerp(top_width, bottom_width,
               (stalk_metrics.kStalkTipDistance - end_distance) / stalk_metrics.kStalkLengthRange);

      Vec2 cubics[12];

      Vec2 top_left = start_position - start_normal * start_width / 2;
      Vec2 top_right = start_position + start_normal * start_width / 2;
      Vec2 bottom_right = end_position + end_normal * end_width / 2;
      Vec2 bottom_left = end_position - end_normal * end_width / 2;

      float left_dist = Length(top_left - bottom_left);
      float right_dist = Length(top_right - bottom_right);

      cubics[0] = top_left;  // top left
      cubics[1] = top_left + start_normal * start_width / 3;
      cubics[2] = top_right - start_normal * start_width / 3;
      cubics[3] = top_right;  // top right
      cubics[4] = top_right - start_tangent * right_dist / 3;
      cubics[5] = bottom_right + end_tangent * right_dist / 3;
      cubics[6] = bottom_right;  // bottom right
      cubics[7] = bottom_right - end_normal * end_width / 3;
      cubics[8] = bottom_left + end_normal * end_width / 3;
      cubics[9] = bottom_left;  // bottom left
      cubics[10] = bottom_left + end_tangent * left_dist / 3;
      cubics[11] = top_left - start_tangent * left_dist / 3;

      SkPoint texCoords[4] = {
          {0, 0},
          {1_cm, 0},
          {1_cm, 1_cm},
          {0, 1_cm},
      };

      canvas.drawPatch(&cubics[0].sk, nullptr, texCoords, SkBlendMode::kSrcOver, stalk.paint);
    }

    auto& category_state = category_states[i];

    // Draw leaves along the stalk
    int group_count = category.groups.size();

    for (int j = 0; j < group_count; ++j) {
      auto& leaf_state = category_state.leaves[j];
      float group_distance = stalk_metrics.kMinimumStalkLength +
                             stalk_metrics.kStalkLengthRange * (j + 0.5) / (group_count + 1.5);
      if (group_distance >= stalk_metrics.kStalkTipDistance) {
        continue;
      }
      Vec2 leaf_base_position;
      Vec2 stalk_tangent;
      (void)path_measure.getPosTan(group_distance, &leaf_base_position.sk, &stalk_tangent.sk);
      SinCos leaf_dir = SinCos::FromVec2(stalk_tangent, 1);
      leaf_dir = leaf_dir + (j % 2 ? 60_deg : -60_deg);

      // Stamp the leaf texture
      canvas.save();
      canvas.translate(leaf_base_position.x, leaf_base_position.y);
      canvas.rotate(leaf_dir.ToDegrees());
      if (j % 2) {
        canvas.scale(1, -1);
      }
      canvas.rotate(-62 + leaf_state.shake);
      canvas.scale(leaf_state.growth, leaf_state.growth);
      if (leaf_state.hue_rotate) {
        float hsla_matrix[20] = {1, 0, 0, 0, leaf_state.hue_rotate, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0,
                                 0, 0, 1, 0};
        leaf.paint.setColorFilter(SkColorFilters::HSLAMatrix(hsla_matrix));
      } else {
        leaf.paint.setColorFilter(nullptr);
      }
      leaf.draw(canvas);
      canvas.restore();

      auto& group = category.groups[j];

      float group_name_width = font.MeasureText(group.name);
      // Draw text
      canvas.save();
      canvas.translate(leaf_state.position.x - group_name_width / 2,
                       leaf_state.position.y - kCategoryLetterSize / 2);
      font.DrawText(canvas, group.name, text_shadow_paint);
      font.DrawText(canvas, group.name, text_fill_paint);
      canvas.restore();

      if constexpr (kDebugRoseDrawing) {
        canvas.drawLine(leaf_base_position, leaf_base_position + Vec2::Polar(leaf_dir, 1_cm),
                        debug_paint);
        canvas.drawCircle(leaf_state.position, leaf_state.radius, debug_paint);
      }
    }

    // Draw rose
    auto [rose_pos, final_rose_dir] = RosePosition(path_measure, growth);

    auto& rose = *rose_images[1 + (int)round(growth * (size(rose_images) - 1 - 2))];

    canvas.save();
    canvas.translate(rose_pos.x, rose_pos.y);
    canvas.translate(-rose.width() / 2, rose.height() / 2);
    canvas.rotate(final_rose_dir * 180 / M_PI - 90, rose.width() / 2, -rose.height() / 2);
    canvas.scale(rose.scale, -rose.scale);
    canvas.translate(5, -65);
    canvas.rotate(category_state.shake, 128, 128 + 64);
    canvas.drawImage(*rose.image, 0, 0);
    canvas.restore();

    auto category_name = category.name;
    float category_name_width = font.MeasureText(category_name);

    Vec2 category_name_position = rose_pos + Vec2::Polar(final_rose_dir, 8_mm);

    canvas.save();
    canvas.translate(category_name_position.x - category_name_width / 2,
                     category_name_position.y - kCategoryLetterSize / 2);
    font.DrawText(canvas, category_name, text_shadow_paint);
    font.DrawText(canvas, category_name, text_fill_paint);
    canvas.restore();

    float category_radius = 5_mm + 5_mm * growth;

    if constexpr (kDebugRoseDrawing) {
      canvas.drawCircle(category_name_position, category_radius, debug_paint);
    }
  }

  for (auto& card : std::ranges::reverse_view(instruction_helix)) {
    canvas.save();
    canvas.rotate(card.angle);
    canvas.translate(-Instruction::Widget::kWidth / 2, -Instruction::Widget::kHeight / 2);
    Instruction::DrawInstruction(canvas, card.mc_inst);
    canvas.restore();
  }
}

void InstructionLibrary::Widget::PointerMove(gui::Pointer& p, Vec2 position) {
  Vec2 local_position = TransformDown(*this).mapPoint(position);
  for (int i = 0; i < category_states.size(); ++i) {
    auto& category_state = category_states[i];

    float category_distance = Length(category_state.position - local_position);
    bool category_hovered = category_distance < category_state.radius;
    if (category_hovered && !category_state.hovered) {
      if (category_state.shake.velocity >= 0) {
        category_state.shake.velocity += 80;
      } else {
        category_state.shake.velocity -= 80;
      }
      WakeAnimation();
    }
    category_state.hovered = category_hovered;

    for (int j = 0; j < category_state.leaves.size(); ++j) {
      auto& leaf_state = category_state.leaves[j];
      bool hovered = Length(leaf_state.position - local_position) < leaf_state.radius;

      if (hovered && !leaf_state.hovered) {
        if (leaf_state.shake.velocity >= 0) {
          leaf_state.shake.velocity += 80;
        } else {
          leaf_state.shake.velocity -= 80;
        }
        WakeAnimation();
      }

      leaf_state.hovered = hovered;
    }
  }
}

void InstructionLibrary::Widget::PointerOver(gui::Pointer& p) { StartWatching(p); }

void InstructionLibrary::Widget::PointerLeave(gui::Pointer& p) { StopWatching(p); }

std::unique_ptr<Action> InstructionLibrary::Widget::FindAction(gui::Pointer& p,
                                                               gui::ActionTrigger btn) {
  if (btn == gui::PointerButton::Left) {
    auto contact_point = p.PositionWithin(*this);
    for (int i = 0; i < category_states.size(); ++i) {
      auto& category_state = category_states[i];
      float distance = Length(category_state.position - contact_point);
      if (distance < category_state.radius) {
        auto object_shared_ptr = object.lock();
        if (!object_shared_ptr) return nullptr;

        auto* library = dynamic_cast<InstructionLibrary*>(object_shared_ptr.get());
        if (!library) return nullptr;

        lock_guard lock(library->mutex);

        if (library->selected_category != i) {
          library->selected_category = i;
          library->selected_group = -1;
        } else {
          library->selected_category = -1;
          library->selected_group = -1;
        }
        WakeAnimation();
        return nullptr;
      }

      for (int j = 0; j < category_state.leaves.size(); ++j) {
        auto& leaf_state = category_state.leaves[j];
        float distance = Length(leaf_state.position - contact_point);
        if (distance < leaf_state.radius) {
          auto object_shared_ptr = object.lock();
          if (!object_shared_ptr) return nullptr;

          auto* library = dynamic_cast<InstructionLibrary*>(object_shared_ptr.get());
          if (!library) return nullptr;

          lock_guard lock(library->mutex);

          library->selected_category = i;
          if (library->selected_group == j) {
            library->selected_group = -1;
          } else {
            library->selected_group = j;
          }
          leaf_state.shake.velocity = 150;
          WakeAnimation();
          return nullptr;
        }
      }
    }

    auto* location = Closest<Location>(*p.hover);
    auto* machine = Closest<Machine>(*p.hover);
    if (location && machine) {
      auto a = std::make_unique<DragLocationAction>(p, machine->Extract(*location));
      a->contact_point = contact_point;
      return a;
    }
  }
  return nullptr;
}

}  // namespace automat::library
