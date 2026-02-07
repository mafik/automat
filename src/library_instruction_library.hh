// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkPathMeasure.h>
#include <llvm/MC/MCInst.h>

#include "animation.hh"
#include "base.hh"
#include "library_instruction.hh"
#include "random.hh"

namespace automat::library {

struct InstructionLibrary : Object {
  enum class RegisterWidth {
    kNone,
    k8b,
    k16b,
    k32b,
    k64b,
  };

  // Mutex, used to synchronize various threads that may access the library (mostly Widgets on the
  // UI thread).
  std::mutex mutex;

  // Filters
  int selected_category = -1;
  int selected_group = -1;

  // LLVM enums of registers that should be read from
  std::vector<unsigned> read_from = {};
  // LLVM enums of registers that should be written to
  std::vector<unsigned> write_to = {};

  RegisterWidth register_width = RegisterWidth::kNone;

  // Potential instructions (after filtering)
  std::deque<llvm::MCInst> instructions;  // "deck", lol

  InstructionLibrary();

  // Updates the `instructions` deque.
  // Caller should hold `mutex` when calling this.
  void Filter();

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;

  struct Widget : Toy, ui::PointerMoveCallback {
    struct InstructionCard {
      std::unique_ptr<Instruction::Widget> widget;
      Ptr<Instruction> instruction;
      float angle = 0;
      int library_index = -1;
      float throw_direction_deg = NAN;
      float throw_t = 0;  // 0..1
    };

    std::deque<InstructionCard> instruction_helix;
    XorShift32 rng;
    animation::SpringV2<float> rotation_offset_t = 0;
    float rotation_offset_t_target = 0;
    bool helix_hovered = false;
    animation::SpringV2<float> helix_hover_tween = 0;
    float new_cards_dir_deg = NAN;

    // True if the button is pressed
    struct RegisterFilter {
      bool pressed = false;
      float pressed_animation = 0;
      bool hovered = false;
      float hovered_animation = 0;

      int count = 0;  // how many cards would be in the library if this filter was toggled
    };

    RegisterFilter read_from[kGeneralPurposeRegisterCount] = {};
    RegisterFilter write_to[kGeneralPurposeRegisterCount] = {};

    struct CategoryState {
      struct LeafState {
        bool hovered = false;

        animation::SpringV2<float> growth;
        float hue_rotate = 0;
        animation::SpringV2<float> shake;

        // Computed and cached in `Tick`, so that MouseMove can be faster
        Vec2 position;
        float radius;
      };

      bool hovered = false;
      std::vector<LeafState> leaves;
      animation::SpringV2<float> growth;
      animation::SpringV2<float> shake;

      // Computed and cached in `Tick`, so that MouseMove can be faster
      Vec2 position;
      float radius;
    };

    std::vector<CategoryState> category_states;

    Widget(ui::Widget* parent, Object&);

    std::string_view Name() const override { return "Instruction Library Widget"; }
    SkPath Shape() const override;
    animation::Phase Tick(time::Timer&) override;
    void Draw(SkCanvas&) const override;
    std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override;

    void FillChildren(Vec<ui::Widget*>& children) override;
    bool AllowChildPointerEvents(ui::Widget& child) const override { return false; }

    void PointerMove(ui::Pointer&, Vec2 position) override;
    void PointerOver(ui::Pointer&) override;
    void PointerLeave(ui::Pointer&) override;
  };

  unique_ptr<Toy> MakeToy(ui::Widget* parent) override {
    return make_unique<Widget>(parent, *this);
  }
};

}  // namespace automat::library
