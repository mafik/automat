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

#include <automat/x86.hh>
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

  auto AddOpcode = [&](int i) {
    auto name = mc_instr_info.getName(i);

    MCInstBuilder builder(i);

    // TODO: Custom tablegen processor to output just the instructions we want & info we need

    // TODO: Atomic instructions
    if (name.contains("LOCK_")) return;
    // TODO: EVEX (requires Advanced Performance Extensions - only on newer CPUs - else crash)
    if (name.contains("_NF")) return;
    if (name.contains("_EVEX")) return;
    if (name.contains("_ND")) return;
    if (name.contains("_REV")) return;
    if (name.contains("_alt")) return;

    const MCInstrDesc& op = mc_instr_info.get(i);

    if (op.isPreISelOpcode()) return;
    if (op.isPseudo()) return;
    if (op.isMetaInstruction()) return;

    // TODO: Memory-related instructions
    if (op.mayLoad()) return;
    if (op.mayStore()) return;

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
      return;
    }

    instructions.push_back(inst);
  };

  if (selected_category == -1) {
    for (int i = 0; i < mc_instr_info.getNumOpcodes(); i++) {
      AddOpcode(i);
    }
  } else {
    auto& category = x86::kCategories[selected_category];
    if (selected_group == -1) {
      std::vector<bool> visited(mc_instr_info.getNumOpcodes(), false);
      for (auto& group : category.groups) {
        for (auto& opcode : group.opcodes) {
          if (visited[opcode]) continue;
          visited[opcode] = true;
          AddOpcode(opcode);
        }
      }
    } else {
      auto& group = category.groups[selected_group];
      for (auto& opcode : group.opcodes) {
        AddOpcode(opcode);
      }
    }
  }
}

string_view InstructionLibrary::Name() const { return "Instruction Library"; }

shared_ptr<Object> InstructionLibrary::Clone() const { return make_shared<InstructionLibrary>(); }

InstructionLibrary::Widget::Widget(std::weak_ptr<Object> object) : object(object) {}
SkPath InstructionLibrary::Widget::Shape() const { return SkPath::Circle(0, 0, 10_cm); }

constexpr float kRoseFanDegrees = 180;
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
        (fmodf(i * 1.7156420f + segment * 1.92345678f + 0.2f, 1.f) - 0.5) * 1.5 *
        0.5;  // Pseudo-random

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

const float kThrowEndDistance = kCornerDist * 1;  // 10_cm;
constexpr int kMaxInstructions = 10;
constexpr float kCategoryLetterSize = 3_mm;
constexpr bool kDebugRoseDrawing = false;
constexpr bool kDebugAnimation = false;

static int VisibleInstructions(InstructionLibrary& library) {
  return std::min<int>(library.instructions.size(), kMaxInstructions);
}

static float CardAngleDeg(float i, int visible_instructions) {
  float t = i / std::max<int>(1, visible_instructions - 1);
  return CosineInterpolate(90, -90, 0.5 + t / 2);
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

  int n = VisibleInstructions(*library);

  bool small_deck = instructions.size() <= kMaxInstructions;

  int insert_index = 0;

  for (auto& card : instruction_helix) {
    card.library_index = -1;
  }

  float wobble = 0;
  float wobble_amplitude_target = (wobble_cards && isnan(new_cards_dir_deg)) ? 1 : 0;
  if (wobble_amplitude_target > 0) {
    phase |= animation::Animating;
  }
  phase |= wobble_amplitude.SineTowards(wobble_amplitude_target, timer.d, 5);
  wobble = wobble_amplitude * sin(timer.NowSeconds() * M_PI * 2) / 20;

  for (int i = 0; i < n; ++i) {
    llvm::MCInst& inst = library->instructions[i];
    // Ensure that instruction_helix contains a card with the given `inst`.
    bool found = false;
    for (int j = 0; j < instruction_helix.size(); ++j) {
      auto& card = instruction_helix[j];
      if (card.mc_inst.getOpcode() == inst.getOpcode()) {
        if (isnan(card.throw_direction_deg) || (j == i)) {
          // Only update the rotation if the card is not being animated
          card.angle = CardAngleDeg(i + rotation_offset_t + wobble, n);
        }
        card.library_index = i;
        found = true;
        // New cards should be inserted after the card that matches InstructionLibrary deck.
        insert_index = j + 1;
        break;
      }
    }
    if (!found) {
      auto it =
          instruction_helix.insert(instruction_helix.begin() + insert_index, InstructionCard{inst});
      it->angle = CardAngleDeg(i + rotation_offset_t + wobble, n);
      it->library_index = i;
      if (insert_index == 0) {
        if (isnan(new_cards_dir_deg)) {
          it->throw_direction_deg = rng.RollFloat(-180, 180);
        } else {
          it->throw_direction_deg = new_cards_dir_deg;
        }
        it->throw_t = 0.5;
      }
      insert_index++;
    }
  }

  // TODO: This makes category switching boring.
  // Consider animations based on card target states (index up, index down, remove, add)
  while (instruction_helix.back().library_index == -1) {
    instruction_helix.pop_back();
  }

  for (int j = 0; j < instruction_helix.size(); ++j) {
    auto& card = instruction_helix[j];
    if (card.library_index >= 0) {
      if (isnan(card.throw_direction_deg)) {
        card.throw_t = 0;
      } else {
        if (j < card.library_index) {
          // We should move the card deeper into the deck (reordered)
          phase |= animation::LinearApproach(1, timer.d, kDebugAnimation ? 1 : 5, card.throw_t);
        } else {
          // Card is moving back to the deck
          phase |= animation::LinearApproach(0, timer.d, kDebugAnimation ? 1 : 5, card.throw_t);
          if (card.throw_t == 0) {
            card.throw_direction_deg = NAN;
          }
        }
      }
    } else {
      if (isnan(card.throw_direction_deg)) {
        card.throw_direction_deg = rng.RollFloat(-180, 180);
      }
      phase |= animation::LinearApproach(1, timer.d, kDebugAnimation ? 1 : 5, card.throw_t);
      if (card.throw_t >= 1) {
        // Delete the card
        instruction_helix.erase(instruction_helix.begin() + j);
        --j;
        continue;
      }
    }
  }

  // Adjust z-order of cards that move their position in the deck
  for (int j = 0; j < instruction_helix.size(); ++j) {
    auto& card = instruction_helix[j];
    if (isnan(card.throw_direction_deg)) {
      continue;  // skip cards which are not being animated
    }
    bool move_down = card.library_index > j && card.throw_t > 0.5;
    bool move_up = card.library_index >= 0 && card.library_index < j && card.throw_t < 0.5;
    if (move_down || move_up) {
      auto card_copy = card;
      card_copy.angle = CardAngleDeg(card_copy.library_index + rotation_offset_t + wobble, n);
      instruction_helix.erase(instruction_helix.begin() + j);
      instruction_helix.insert(instruction_helix.begin() + card_copy.library_index, card_copy);
      if (move_down) {
        --j;
      }
    }
  }

  phase |= rotation_offset_t.SineTowards(rotation_offset_t_target, timer.d, 0.2);

  for (int i = 0; i < std::size(x86::kCategories); ++i) {
    if (category_states.size() <= i) {
      category_states.push_back(CategoryState{
          .growth = 0,
      });
      for (int j = 0; j < x86::kCategories[i].groups.size(); ++j) {
        category_states.back().leaves.push_back(CategoryState::LeafState{
            .growth = 0,
        });
      }
    }
  }

  for (int i = 0; i < category_states.size(); ++i) {
    auto& category = x86::kCategories[i];
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

PersistentImage reverse = PersistentImage::MakeFromAsset(
    maf::embedded::assets_card_reverse_webp,
    PersistentImage::MakeArgs{.width = Instruction::Widget::kWidth -
                                       Instruction::Widget::kBorderMargin * 2});

PersistentImage* rose_images[] = {&rose0, &rose1, &rose2, &rose3, &rose4, &rose5, &rose6};

PersistentImage stalk = PersistentImage::MakeFromAsset(maf::embedded::assets_stalk_png,
                                                       PersistentImage::MakeArgs{.width = 1_cm});

PersistentImage leaf = PersistentImage::MakeFromAsset(maf::embedded::assets_leaf_webp,
                                                      PersistentImage::MakeArgs{.width = 1.2_cm});

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
  constexpr int kCategoryCount = std::size(x86::kCategories);
  // For each category draw a rose:
  // 1. Cubic curve with control points along a line from the center to the edge.
  // 2. Pseudo-random offsets to tangents to make the curve more interesting.
  // 3. Leaves drawn alternatively along the curve, at offset to the current tangent.
  // 4. Rose drawn at the end of the curve.
  for (int i = 0; i < kCategoryCount; ++i) {
    // Number between 0 and 1 that indicates how much the rose has grown.
    float growth = category_states[i].growth;

    auto& category = x86::kCategories[i];
    float branch_dir = kRoseFanDegrees * M_PI / 180 * i / (kCategoryCount - 1) +
                       (180 - kRoseFanDegrees) * M_PI / 180 / 2;

    SinCos branch_dir_sin_cos = SinCos::FromRadians(branch_dir);

    SkPathMeasure path_measure = CategoryPathMeasure(i, kCategoryCount);
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

  auto DrawCard = [&](const InstructionCard& card) {
    canvas.save();
    float rotation_deg = card.angle;
    float throw_t = 0;
    if (!isnan(card.throw_direction_deg)) {
      throw_t = card.throw_t;
      float throw_distance = sin(throw_t * M_PI) * kThrowEndDistance;
      Vec2 throw_vec = Vec2::Polar(SinCos::FromDegrees(card.throw_direction_deg), throw_distance);
      canvas.translate(throw_vec.x, throw_vec.y);
      rotation_deg = CosineInterpolate(rotation_deg, -90, throw_t);
      float scale = std::cos(throw_t * M_PI);
      scale *= scale;
      canvas.rotate(card.throw_direction_deg);
      canvas.scale(scale, 1);
      canvas.rotate(-card.throw_direction_deg);
    }
    canvas.rotate(rotation_deg);
    canvas.translate(-Instruction::Widget::kWidth / 2, -Instruction::Widget::kHeight / 2);
    if (throw_t > 0.5) {
      MCInst fake_inst;
      fake_inst.setOpcode(X86::INSTRUCTION_LIST_END);
      Instruction::DrawInstruction(canvas, fake_inst);
      canvas.translate(Instruction::Widget::kWidth / 2, Instruction::Widget::kHeight / 2);
      canvas.translate(-reverse.width() / 2, -reverse.height() / 2);
      reverse.draw(canvas);
    } else {
      Instruction::DrawInstruction(canvas, card.mc_inst);
    }
    canvas.restore();
  };

  for (auto& card : instruction_helix) {
    if (card.throw_t >= 0.5) {
      DrawCard(card);
    }
  }

  for (auto& card : std::ranges::reverse_view(instruction_helix)) {
    if (card.throw_t < 0.5) {
      DrawCard(card);
    }
  }
}

void InstructionLibrary::Widget::PointerMove(gui::Pointer& p, Vec2 position) {
  Vec2 local_position = TransformDown(*this).mapPoint(position);

  if (Length(local_position) < kCornerDist) {
    // Over card helix
    wobble_cards = true;
    WakeAnimation();
    return;
  }
  wobble_cards = false;

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

void InstructionLibrary::Widget::PointerLeave(gui::Pointer& p) {
  StopWatching(p);
  wobble_cards = false;
}

struct ScrollDeckAction : Action {
  SinCos angle;
  std::shared_ptr<gui::Widget> widget;
  std::shared_ptr<Object> object;

  InstructionLibrary::Widget& library_widget;  // same as widget
  InstructionLibrary& library;

  ScrollDeckAction(gui::Pointer& pointer, std::shared_ptr<gui::Widget> widget,
                   std::shared_ptr<Object> object)
      : Action(pointer),
        widget(widget),
        object(object),
        library_widget(dynamic_cast<InstructionLibrary::Widget&>(*widget)),
        library(dynamic_cast<InstructionLibrary&>(*object)) {}
  void Begin() override {
    auto pos = pointer.PositionWithin(*widget);
    angle = SinCos::FromVec2(pos);
    library_widget.new_cards_dir_deg = (angle + 180_deg).ToDegrees();
  }
  void Update() override {
    auto pos = pointer.PositionWithin(*widget);
    auto new_angle = SinCos::FromVec2(pos);
    auto diff = new_angle - angle;

    auto diff_deg = diff.ToDegrees();
    lock_guard lock(library.mutex);
    int n = VisibleInstructions(library);
    float card0_deg = CardAngleDeg(0, n);
    float card1_deg = CardAngleDeg(1, n);
    float step_deg = card0_deg - card1_deg;
    if (abs(diff_deg) > step_deg / 2) {
      bool twist_left = diff_deg > 0;
      angle = angle + SinCos::FromDegrees(twist_left ? step_deg : -step_deg);
      diff = new_angle - angle;
      diff_deg = diff.ToDegrees();

      if (twist_left) {
        auto& mc_inst = library.instructions.front();
        for (int j = 0; j < library_widget.instruction_helix.size(); ++j) {
          auto& card = library_widget.instruction_helix[j];
          if (card.mc_inst.getOpcode() == mc_inst.getOpcode()) {
            card.throw_direction_deg = (new_angle + 180_deg).ToDegrees();
            break;
          }
        }
        library.instructions.push_back(library.instructions.front());
        library.instructions.pop_front();
      } else {
        auto& mc_inst = library.instructions.back();
        for (int j = 0; j < library_widget.instruction_helix.size(); ++j) {
          auto& card = library_widget.instruction_helix[j];
          if (card.mc_inst.getOpcode() == mc_inst.getOpcode()) {
            card.throw_direction_deg = (new_angle + 180_deg).ToDegrees();
            card.throw_t = 1;
            break;
          }
        }
        library_widget.new_cards_dir_deg = (new_angle + 180_deg).ToDegrees();
        library.instructions.push_front(library.instructions.back());
        library.instructions.pop_back();
      }
    }
    library_widget.rotation_offset_t = library_widget.rotation_offset_t_target =
        -diff_deg / step_deg;
    library_widget.WakeAnimation();
  }
  void End() override {
    library_widget.rotation_offset_t_target = 0;
    library_widget.new_cards_dir_deg = NAN;
    library_widget.WakeAnimation();
  }
};

std::unique_ptr<Action> InstructionLibrary::Widget::FindAction(gui::Pointer& p,
                                                               gui::ActionTrigger btn) {
  if (btn == gui::PointerButton::Left) {
    auto contact_point = p.PositionWithin(*this);

    if (Length(contact_point) < kCornerDist) {
      return std::make_unique<ScrollDeckAction>(p, SharedPtr<Widget>(), object.lock());
    }

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
        library->Filter();
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
          library->Filter();
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
