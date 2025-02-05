// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkPathMeasure.h>
#include <llvm/MC/MCInst.h>

#include "animation.hh"
#include "base.hh"
#include "random.hh"

namespace automat::library {

struct InstructionLibrary : Object {
  constexpr static int kGeneralPurposeRegisterCount = 8;

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
  bool read_from[kGeneralPurposeRegisterCount] = {};
  bool write_to[kGeneralPurposeRegisterCount] = {};
  RegisterWidth register_width = RegisterWidth::kNone;

  // Potential instructions (after filtering)
  std::deque<llvm::MCInst> instructions;  // "deck", lol

  InstructionLibrary();

  // Updates the `instructions` deque.
  // Caller should hold `mutex` when calling this.
  void Filter();

  std::string_view Name() const override;
  std::shared_ptr<Object> Clone() const override;

  struct Widget : gui::Widget, gui::PointerMoveCallback {
    std::weak_ptr<Object> object;

    struct InstructionCard {
      llvm::MCInst mc_inst;
      float angle = 0;
      int library_index = -1;
      float throw_direction_deg = NAN;
      float throw_t = 0;  // 0..1
    };

    std::deque<InstructionCard> instruction_helix;
    maf::XorShift32 rng;
    animation::SpringV2<float> rotation_offset_t = 0;
    float rotation_offset_t_target = 0;
    bool wobble_cards = false;  // true when helix is hovered
    animation::SpringV2<float> wobble_amplitude = 0;
    float new_cards_dir_deg = NAN;

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

    Widget(std::weak_ptr<Object> object);

    std::string_view Name() const override { return "Instruction Library Widget"; }
    SkPath Shape() const override;
    animation::Phase Tick(time::Timer&) override;
    void Draw(SkCanvas&) const override;
    std::unique_ptr<Action> FindAction(gui::Pointer& p, gui::ActionTrigger btn) override;

    void PointerMove(gui::Pointer&, Vec2 position) override;
    void PointerOver(gui::Pointer&) override;
    void PointerLeave(gui::Pointer&) override;
  };

  std::shared_ptr<gui::Widget> MakeWidget() override { return std::make_shared<Widget>(WeakPtr()); }
};

}  // namespace automat::library