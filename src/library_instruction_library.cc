// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_instruction_library.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkColor.h>
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
#include "automat.hh"
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

  std::vector<bool> visited(mc_instr_info.getNumOpcodes(), false);

  auto AddOpcode = [&](int i) {
    if (visited[i]) return;
    visited[i] = true;

    MCInstBuilder builder(i);
    const MCInstrDesc& op = mc_instr_info.get(i);

    auto operands = op.operands();

    deque<unsigned> read_from_queue = {read_from.begin(), read_from.end()};  // register index 0..5
    deque<unsigned> write_to_queue = {write_to.begin(), write_to.end()};     // register index 0..5

    auto implicit_defs = op.implicit_defs();
    for (auto& def : implicit_defs) {
      for (int write_to_i = 0; write_to_i < write_to_queue.size(); ++write_to_i) {
        auto reg_ours = write_to_queue[write_to_i];
        auto reg_llvm = kRegisters[reg_ours].llvm_reg;

        if (reg_llvm == def || mc_reg_info.isSuperRegister(def, reg_llvm)) {
          write_to_queue.erase(write_to_queue.begin() + write_to_i);
          break;
        }
      }
    }
    for (auto& use : op.implicit_uses()) {
      for (int read_from_i = 0; read_from_i < read_from_queue.size(); ++read_from_i) {
        auto reg_ours = read_from_queue[read_from_i];
        auto reg_llvm = kRegisters[reg_ours].llvm_reg;

        if (reg_llvm == use || mc_reg_info.isSuperRegister(use, reg_llvm)) {
          read_from_queue.erase(read_from_queue.begin() + read_from_i);
          break;
        }
      }
    }

    for (int operand_i = 0; operand_i < operands.size(); ++operand_i) {
      auto& operand = operands[operand_i];
      if (operand.OperandType == MCOI::OPERAND_REGISTER) {
        deque<unsigned>* queue = (operand_i == 0) ? &write_to_queue : &read_from_queue;

        unsigned reg_llvm;

        if (auto tied_to = op.getOperandConstraint(operand_i, MCOI::OperandConstraint::TIED_TO);
            tied_to != -1) {
          assert(tied_to < operand_i);
          MCOperand& tied_operand = ((MCInst&)builder).getOperand(tied_to);
          reg_llvm = tied_operand.getReg();
        } else if (queue->empty()) {
          const MCRegisterClass& reg_class = mc_reg_info.getRegClass(operand.RegClass);
          auto reg_class_name = mc_reg_info.getRegClassName(&reg_class);
          reg_llvm = reg_class.getRegister(0);
        } else {
          auto super_reg = kRegisters[queue->front()].llvm_reg;
          const MCRegisterClass& reg_class = mc_reg_info.getRegClass(operand.RegClass);
          reg_llvm = reg_class.getRegister(0);
          for (auto reg : reg_class) {
            if (reg == super_reg || mc_reg_info.isSuperRegister(reg, super_reg)) {
              reg_llvm = reg;
              queue->pop_front();
              break;
            }
          }
        }
        builder.addReg(reg_llvm);
      } else if (operand.OperandType == MCOI::OPERAND_IMMEDIATE) {
        builder.addImm(0);
      } else if (operand.OperandType == MCOI::OPERAND_MEMORY) {
        // TODO: memory...
      } else if (operand.OperandType == MCOI::OPERAND_PCREL) {
        builder.addImm(0);
      } else if (operand.OperandType == X86::OPERAND_COND_CODE) {
        builder.addImm(X86::CondCode::COND_NE);
      } else {
        assert(false);
      }
      if (operand.isBranchTarget()) {
      }
    }

    if (!read_from_queue.empty() || !write_to_queue.empty()) {
      return;
    }

    MCInst& inst = builder;
    instructions.push_back(inst);
  };

  auto AddGroup = [&](const x86::Group& group) {
    for (auto& opcode : group.opcodes) {
      AddOpcode(opcode);
    }
  };

  auto AddCategory = [&](const x86::Category& category) {
    if (selected_group == -1) {
      for (auto& group : category.groups) {
        AddGroup(group);
      }
    } else {
      AddGroup(category.groups[selected_group]);
    }
  };

  if (selected_category == -1) {
    for (auto& category : x86::kCategories) {
      AddCategory(category);
    }
  } else {
    AddCategory(x86::kCategories[selected_category]);
  }
}

string_view InstructionLibrary::Name() const { return "Instruction Library"; }

Ptr<Object> InstructionLibrary::Clone() const { return MakePtr<InstructionLibrary>(); }

InstructionLibrary::Widget::Widget(WeakPtr<Object> object) {
  this->object = std::move(object);
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
}
SkPath InstructionLibrary::Widget::Shape() const { return SkPath::Circle(0, 0, 10_cm); }

constexpr float kRoseFanDegrees = 180;
constexpr float kStartDist = 0;  // distance from the center, at which stalk starts
constexpr float kCornerDist = Instruction::Widget::kDiagonal / 2;
constexpr float kRoseDist = 8_cm;
constexpr static Rect kFrontInstructionRect =
    Instruction::Widget::kRect.MoveBy(-Instruction::Widget::kRect.Size() / 2);

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

// Return a value from 0 (i=0) to -90 (i=visible_instructions-1) following a nice curve.
static float CardAngleDeg(float i, int visible_instructions, float helix_tween) {
  float t = i / std::max<int>(1, visible_instructions - 1);
  float ret = CosineInterpolate(90, -90, 0.5 + t / 2);  // curve when helix is not hovered
  float ret2 = lerp(0, -90, t);                         // linear curve when helix is hovered
  return lerp(ret, ret2, helix_tween * 0.7f);           // blend between the two curves
}

void InstructionLibrary::Widget::FillChildren(maf::Vec<Ptr<gui::Widget>>& children) {
  for (auto& card : instruction_helix) {
    if (card.throw_t < 0.5) {
      children.push_back(card.widget);
    }
  }
  for (auto& card : std::ranges::reverse_view(instruction_helix)) {
    if (card.throw_t >= 0.5) {
      children.push_back(card.widget);
    }
  }
}

animation::Phase InstructionLibrary::Widget::Tick(time::Timer& timer) {
  animation::Phase phase = animation::Finished;

  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    phase |= animation::LinearApproach(read_from[i].hovered, timer.d, 10,
                                       read_from[i].hovered_animation);
    phase |=
        animation::LinearApproach(write_to[i].hovered, timer.d, 10, write_to[i].hovered_animation);

    phase |=
        animation::LinearApproach(read_from[i].pressed, timer.d, 5, read_from[i].pressed_animation);
    phase |=
        animation::LinearApproach(write_to[i].pressed, timer.d, 5, write_to[i].pressed_animation);
  }

  auto object_Ptr = object.lock();
  if (!object_Ptr) return animation::Finished;

  auto* library = dynamic_cast<InstructionLibrary*>(object_Ptr.get());
  if (!library) return animation::Finished;

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

  float helix_tween_target = (helix_hovered && isnan(new_cards_dir_deg)) ? 1 : 0;
  phase |= helix_hover_tween.SineTowards(helix_tween_target, timer.d, 0.5);

  for (int i = 0; i < n; ++i) {
    llvm::MCInst& inst = library->instructions[i];
    // Ensure that instruction_helix contains a card with the given `inst`.
    bool found = false;
    for (int j = 0; j < instruction_helix.size(); ++j) {
      auto& card = instruction_helix[j];
      if (card.instruction->mc_inst.getOpcode() == inst.getOpcode()) {
        if (isnan(card.throw_direction_deg) || (j == i)) {
          // Only update the rotation if the card is not being animated
          card.angle = CardAngleDeg(i + rotation_offset_t, n, helix_hover_tween);
        }
        card.library_index = i;
        card.instruction->mc_inst = inst;
        found = true;
        // New cards should be inserted after the card that matches InstructionLibrary deck.
        insert_index = j + 1;
        break;
      }
    }
    if (!found) {
      auto instruction = MakePtr<Instruction>();
      instruction->mc_inst = inst;
      auto widget = MakePtr<Instruction::Widget>(instruction->AcquireWeakPtr<Object>());
      widget->parent = AcquirePtr();
      auto it = instruction_helix.insert(instruction_helix.begin() + insert_index,
                                         InstructionCard{widget, instruction});
      it->angle = CardAngleDeg(i + rotation_offset_t, n, helix_hover_tween);
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
  while (!instruction_helix.empty() && instruction_helix.back().library_index == -1) {
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
      card_copy.angle =
          CardAngleDeg(card_copy.library_index + rotation_offset_t, n, helix_hover_tween);
      instruction_helix.erase(instruction_helix.begin() + j);
      instruction_helix.insert(instruction_helix.begin() + card_copy.library_index, card_copy);
      if (move_down) {
        --j;
      }
    }
  }

  phase |= rotation_offset_t.SineTowards(rotation_offset_t_target, timer.d, 0.2);

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

  for (auto& card : instruction_helix) {
    SkMatrix transform;
    float rotation_deg = card.angle;
    float throw_t = 0;
    if (!isnan(card.throw_direction_deg)) {
      throw_t = card.throw_t;
      float throw_distance = sin(throw_t * M_PI) * kThrowEndDistance;
      Vec2 throw_vec = Vec2::Polar(SinCos::FromDegrees(card.throw_direction_deg), throw_distance);
      transform.preTranslate(throw_vec.x, throw_vec.y);
      rotation_deg = CosineInterpolate(rotation_deg, -90, throw_t);
      float scale = std::cos(throw_t * M_PI);
      transform.preRotate(card.throw_direction_deg);
      transform.preScale(scale, 1);
      transform.preRotate(-card.throw_direction_deg);
    }
    transform.preRotate(rotation_deg);
    transform.preTranslate(-Instruction::Widget::kWidth / 2, -Instruction::Widget::kHeight / 2);
    card.widget->local_to_parent = SkM44(transform);
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

PersistentImage venus = PersistentImage::MakeFromAsset(
    maf::embedded::assets_venus_webp,
    PersistentImage::MakeArgs{.height = Instruction::Widget::kHeight});

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

auto read_icon = PersistentImage::MakeFromAsset(maf::embedded::assets_reg_read_webp,
                                                PersistentImage::MakeArgs{.width = 9_mm});

auto write_icon = PersistentImage::MakeFromAsset(maf::embedded::assets_reg_write_webp,
                                                 PersistentImage::MakeArgs{.width = 9_mm});

constexpr float kTableCellSize = 8_mm;
constexpr float kTableRadius = 2_mm;
constexpr float kTableMarginTop = -kCornerDist - kTableCellSize;
constexpr int kTableCols = 1 + size(kRegisters);
constexpr int kTableRows = 2;
constexpr Rect kRegisterTableRect =
    Rect::MakeAtZero<CenterX, TopY>(kTableCols * kTableCellSize, kTableRows* kTableCellSize)
        .MoveBy({0, kTableMarginTop});

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

      float group_name_width = font.MeasureText(group.shortcut);
      // Draw text
      canvas.save();
      canvas.translate(leaf_state.position.x - group_name_width / 2,
                       leaf_state.position.y - kCategoryLetterSize / 2);
      font.DrawText(canvas, group.shortcut, text_shadow_paint);
      font.DrawText(canvas, group.shortcut, text_fill_paint);
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

  {  // Venus
    canvas.save();
    canvas.translate(-venus.width() / 2 - 0.7_cm, -venus.height() / 2);
    venus.draw(canvas);
    canvas.restore();
  }

  {  // Draw Register Table
    auto register_table_rr = SkRRect::MakeRectXY(kRegisterTableRect.sk, kTableRadius, kTableRadius);

    SkPaint register_table_paint;
    register_table_paint.setColor("#e4e4e4"_color);

    canvas.drawRRect(register_table_rr, register_table_paint);

    canvas.save();
    canvas.clipRRect(register_table_rr);
    SkPaint hovered_paint;
    hovered_paint.setColor(SK_ColorWHITE);
    SkPaint pressed_paint;
    pressed_paint.setColor("#003052"_color);
    pressed_paint.setStyle(SkPaint::Style::kStroke_Style);
    pressed_paint.setStrokeWidth(0.2_mm);
    pressed_paint.setAntiAlias(true);
    for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
      if (read_from[i].hovered_animation > 0 || read_from[i].pressed_animation > 0) {
        Rect rect_read = Rect::MakeAtZero<LeftX, TopY>(kTableCellSize, kTableCellSize)
                             .MoveBy({kRegisterTableRect.left + kTableCellSize * (i + 1),
                                      kRegisterTableRect.top});

        if (read_from[i].hovered_animation > 0) {
          hovered_paint.setAlphaf(read_from[i].hovered_animation);
          canvas.drawRect(rect_read.sk, hovered_paint);
        }
        if (read_from[i].pressed_animation > 0) {
          rect_read = rect_read.Outset(-0.25_mm);
          canvas.drawArc(rect_read.sk, 90, -360 * read_from[i].pressed_animation, false,
                         pressed_paint);
        }
      }
      if (write_to[i].hovered_animation > 0 || write_to[i].pressed_animation > 0) {
        Rect rect_write = Rect::MakeAtZero<LeftX, BottomY>(kTableCellSize, kTableCellSize)
                              .MoveBy({kRegisterTableRect.left + kTableCellSize * (i + 1),
                                       kRegisterTableRect.bottom});
        if (write_to[i].hovered_animation > 0) {
          hovered_paint.setAlphaf(write_to[i].hovered_animation);
          canvas.drawRect(rect_write.sk, hovered_paint);
        }
        if (write_to[i].pressed_animation > 0) {
          rect_write = rect_write.Outset(-0.25_mm);
          canvas.drawArc(rect_write.sk, 90, -360 * write_to[i].pressed_animation, false,
                         pressed_paint);
        }
      }
    }
    canvas.restore();

    SkPaint line_paint;
    line_paint.setColor("#000000"_color);
    line_paint.setAntiAlias(true);
    line_paint.setStyle(SkPaint::kStroke_Style);
    line_paint.setStrokeWidth(0.1_mm);

    // Horizontal line
    canvas.drawLine(kRegisterTableRect.LeftCenter(), kRegisterTableRect.RightCenter(), line_paint);

    // Vertical lines
    Vec2 top = kRegisterTableRect.TopLeftCorner();
    Vec2 bottom = kRegisterTableRect.BottomLeftCorner();
    for (int i = 1; i < kTableCols; ++i) {
      top.x += kTableCellSize;
      bottom.x += kTableCellSize;
      canvas.drawLine(top, bottom, line_paint);
    }

    // Register icons
    canvas.save();
    canvas.translate(kRegisterTableRect.left, kRegisterTableRect.top);
    canvas.translate(kTableCellSize / 2, kTableCellSize / 2);
    canvas.translate(-kRegisterIconWidth / 2, -kRegisterIconWidth / 2);
    canvas.translate(kTableCellSize, 0.5_mm);
    for (int i = 0; i < size(kRegisters); ++i) {
      auto& register_presentation = kRegisters[i];
      register_presentation.image.draw(canvas);
      canvas.translate(kTableCellSize, 0);
    }
    canvas.restore();

    canvas.save();
    canvas.translate(kRegisterTableRect.left, kRegisterTableRect.top);
    canvas.translate(kTableCellSize / 2 - read_icon.width() / 2,
                     kTableCellSize / 2 - read_icon.height() / 2);
    canvas.translate(0, -kTableCellSize);
    read_icon.draw(canvas);
    canvas.translate(0, -kTableCellSize);
    write_icon.draw(canvas);
    canvas.restore();

    SkPaint table_text_paint;
    table_text_paint.setColor("#003052"_color);
    table_text_paint.setStyle(SkPaint::Style::kFill_Style);

    for (int i = 0; i < size(kRegisters); ++i) {
      canvas.save();
      float w = font.MeasureText(std::to_string(read_from[i].count));
      canvas.translate(kRegisterTableRect.left + kTableCellSize * (i + 1.5f) - w / 2,
                       kRegisterTableRect.top - kTableCellSize * 0.5f - kCategoryLetterSize / 2);
      font.DrawText(canvas, std::to_string(read_from[i].count), table_text_paint);
      canvas.restore();

      canvas.save();
      float w2 = font.MeasureText(std::to_string(write_to[i].count));
      canvas.translate(kRegisterTableRect.left + kTableCellSize * (i + 1.5f) - w2 / 2,
                       kRegisterTableRect.top - kTableCellSize * 1.5f - kCategoryLetterSize / 2);
      font.DrawText(canvas, std::to_string(write_to[i].count), table_text_paint);
      canvas.restore();
    }
  }

  DrawChildren(canvas);
}

struct RegisterFilterButton {
  int reg;  // 0..kGeneralPurposeRegisterCount
  bool read;
};

std::optional<RegisterFilterButton> FindRegisterFilterButton(Vec2 contact_point) {
  Rect register_table_rect = kRegisterTableRect;
  register_table_rect.left += kTableCellSize;
  if (register_table_rect.Contains(contact_point)) {
    auto table_point = contact_point - register_table_rect.BottomLeftCorner();
    int reg = table_point.x / kTableCellSize;
    int read = table_point.y / kTableCellSize;
    return RegisterFilterButton{reg, (bool)read};
  }
  return std::nullopt;
}

void InstructionLibrary::Widget::PointerMove(gui::Pointer& p, Vec2 position) {
  Vec2 local_position = TransformDown(*this).mapPoint(position);

  bool new_read_from[kGeneralPurposeRegisterCount] = {};
  bool new_write_to[kGeneralPurposeRegisterCount] = {};

  if (auto reg_btn = FindRegisterFilterButton(local_position)) {
    (reg_btn->read ? new_read_from : new_write_to)[reg_btn->reg] = true;
  }

  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    if (new_read_from[i] != read_from[i].hovered) {
      read_from[i].hovered = new_read_from[i];
      WakeAnimation();
    }
    if (new_write_to[i] != write_to[i].hovered) {
      write_to[i].hovered = new_write_to[i];
      WakeAnimation();
    }
  }

  bool new_wobble_cards = Length(local_position) < kCornerDist;
  if (kFrontInstructionRect.Contains(local_position)) {
    new_wobble_cards = false;
  }
  if (helix_hovered != new_wobble_cards) {
    helix_hovered = new_wobble_cards;
    WakeAnimation();
  }

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
  animation::Phase phase = animation::Finished;

  if (helix_hovered) {
    helix_hovered = false;
    phase |= animation::Animating;
  }

  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    if (read_from[i].hovered) {
      read_from[i].hovered = false;
      phase |= animation::Animating;
    }
    if (write_to[i].hovered) {
      write_to[i].hovered = false;
      phase |= animation::Animating;
    }
  }
  if (phase != animation::Finished) {
    WakeAnimation();
  }
}

struct ScrollDeckAction : Action {
  SinCos angle;
  Ptr<gui::Widget> widget;
  Ptr<Object> object;

  InstructionLibrary::Widget& library_widget;  // same as widget
  InstructionLibrary& library;

  ScrollDeckAction(gui::Pointer& pointer, Ptr<gui::Widget> widget, Ptr<Object> object)
      : Action(pointer),
        widget(widget),
        object(object),
        library_widget(dynamic_cast<InstructionLibrary::Widget&>(*widget)),
        library(dynamic_cast<InstructionLibrary&>(*object)) {
    auto pos = pointer.PositionWithin(*widget);
    angle = SinCos::FromVec2(pos);
    library_widget.new_cards_dir_deg = (angle + 180_deg).ToDegrees();
    widget->WakeAnimation();
  }
  ~ScrollDeckAction() override {
    library_widget.rotation_offset_t_target = 0;
    library_widget.new_cards_dir_deg = NAN;
    library_widget.WakeAnimation();
  }
  void Update() override {
    auto pos = pointer.PositionWithin(*widget);
    auto new_angle = SinCos::FromVec2(pos);
    auto diff = new_angle - angle;

    auto diff_deg = diff.ToDegrees();
    lock_guard lock(library.mutex);
    int n = VisibleInstructions(library);
    float card0_deg = CardAngleDeg(0, n, library_widget.helix_hover_tween);
    float card1_deg = CardAngleDeg(1, n, library_widget.helix_hover_tween);
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
          if (card.instruction->mc_inst.getOpcode() == mc_inst.getOpcode()) {
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
          if (card.instruction->mc_inst.getOpcode() == mc_inst.getOpcode()) {
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
};

static void UpdateFilterCounters(InstructionLibrary& library, InstructionLibrary::Widget& widget) {
  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    widget.read_from[i].pressed = false;
    widget.write_to[i].pressed = false;
  }
  for (auto reg : library.read_from) {
    widget.read_from[reg].pressed = true;
  }
  for (auto reg : library.write_to) {
    widget.write_to[reg].pressed = true;
  }
  auto read_from_backup = library.read_from;
  auto write_to_backup = library.write_to;

  // Toggle each "read_from" filter, count instructions, and then restore.
  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    // Toggle the filter value.
    if (widget.read_from[i].pressed) {
      // Remove i from library.read_from
      library.read_from.erase(std::remove(library.read_from.begin(), library.read_from.end(), i),
                              library.read_from.end());
    } else {
      library.read_from.push_back(i);
    }
    library.Filter();
    // Record count for the toggled state.
    widget.read_from[i].count = library.instructions.size();
    // Restore the original state.
    library.read_from = read_from_backup;
  }

  // Toggle each "write_to" filter, count instructions, and then restore.
  for (int i = 0; i < kGeneralPurposeRegisterCount; ++i) {
    if (widget.write_to[i].pressed) {
      library.write_to.erase(std::remove(library.write_to.begin(), library.write_to.end(), i),
                             library.write_to.end());
    } else {
      library.write_to.push_back(i);
    }
    library.Filter();
    widget.write_to[i].count = library.instructions.size();
    library.write_to = write_to_backup;
  }

  // Finally, restore the filter results by applying the original state.
  library.Filter();
}

std::unique_ptr<Action> InstructionLibrary::Widget::FindAction(gui::Pointer& p,
                                                               gui::ActionTrigger btn) {
  if (btn == gui::PointerButton::Left) {
    auto contact_point = p.PositionWithin(*this);

    if (kFrontInstructionRect.Contains(contact_point)) {
      auto loc = MakePtr<Location>();
      loc->parent_location = root_location;
      loc->parent = root_machine;

      loc->InsertHere(instruction_helix.front().instruction->Clone());
      audio::Play(embedded::assets_SFX_toolbar_pick_wav);
      contact_point -= kFrontInstructionRect.BottomLeftCorner();
      loc->position = loc->animation_state.position = p.PositionWithinRootMachine() - contact_point;
      return std::make_unique<DragLocationAction>(p, std::move(loc), contact_point);
    }

    if (Length(contact_point) < kCornerDist) {
      return std::make_unique<ScrollDeckAction>(p, AcquirePtr<Widget>(), object.lock());
    }

    if (auto reg_btn = FindRegisterFilterButton(contact_point)) {
      auto object_Ptr = object.lock();
      if (!object_Ptr) return nullptr;

      auto* library = dynamic_cast<InstructionLibrary*>(object_Ptr.get());
      if (!library) return nullptr;

      lock_guard lock(library->mutex);

      vector<unsigned>* queue = reg_btn->read ? &library->read_from : &library->write_to;
      // Check if reg is in the queue
      if (auto it = std::find(queue->begin(), queue->end(), reg_btn->reg); it != queue->end()) {
        queue->erase(it);
      } else {
        queue->push_back(reg_btn->reg);
      }
      UpdateFilterCounters(*library, *this);
      WakeAnimation();
      return nullptr;
    }

    for (int i = 0; i < category_states.size(); ++i) {
      auto& category_state = category_states[i];
      float distance = Length(category_state.position - contact_point);
      if (distance < category_state.radius) {
        auto object_Ptr = object.lock();
        if (!object_Ptr) return nullptr;

        auto* library = dynamic_cast<InstructionLibrary*>(object_Ptr.get());
        if (!library) return nullptr;

        lock_guard lock(library->mutex);

        if (library->selected_category != i) {
          library->selected_category = i;
          library->selected_group = -1;
        } else {
          library->selected_category = -1;
          library->selected_group = -1;
        }
        UpdateFilterCounters(*library, *this);
        WakeAnimation();
        return nullptr;
      }

      for (int j = 0; j < category_state.leaves.size(); ++j) {
        auto& leaf_state = category_state.leaves[j];
        float distance = Length(leaf_state.position - contact_point);
        if (distance < leaf_state.radius) {
          auto object_Ptr = object.lock();
          if (!object_Ptr) return nullptr;

          auto* library = dynamic_cast<InstructionLibrary*>(object_Ptr.get());
          if (!library) return nullptr;

          lock_guard lock(library->mutex);

          library->selected_category = i;
          if (library->selected_group == j) {
            library->selected_group = -1;
          } else {
            library->selected_group = j;
          }
          leaf_state.shake.velocity = 150;
          UpdateFilterCounters(*library, *this);
          WakeAnimation();
          return nullptr;
        }
      }
    }
  }
  return FallbackWidget::FindAction(p, btn);
}

}  // namespace automat::library
