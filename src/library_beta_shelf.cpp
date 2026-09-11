// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_beta_shelf.hpp"

#include <include/core/SkCanvas.h>

#include "menu.hpp"
#include "object_source.hpp"
#include "prototypes.hpp"
#include "ui_beta.hpp"
#include "units.hpp"

namespace automat::library {

namespace {

constexpr float kStampRadius = 1.1_cm;

Interface Proto(StrView name) {
  auto* proto = prototypes ? prototypes->Find(name) : nullptr;
  return proto ? Interface(*proto, kMakeObject) : Interface();
}

std::unique_ptr<Action> OpenFfmpegMenu(Interface, ui::Pointer& pointer, Toy* toy) {
  using enum ui::Dir;
  Interface options[ui::kDirCount];
  options[static_cast<int>(N)] = Proto("avformat");
  options[static_cast<int>(S)] = Proto("avcodec");
  return MakeMenuAction(pointer, OptionsProvider::MODE_2_DIR, options, toy);
}

std::unique_ptr<Action> OpenTensorFlowMenu(Interface, ui::Pointer& pointer, Toy* toy) {
  using enum ui::Dir;
  Interface options[ui::kDirCount];
  options[static_cast<int>(N)] = Proto("tf:tensor");
  options[static_cast<int>(S)] = Proto("Square");
  return MakeMenuAction(pointer, OptionsProvider::MODE_2_DIR, options, toy);
}

constinit ObjectSource::Table kFfmpegMenu =
    MenuTable<ObjectSource::Table>("FFmpeg", &OpenFfmpegMenu);
constinit ObjectSource::Table kTensorFlowMenu =
    MenuTable<ObjectSource::Table>("TensorFlow", &OpenTensorFlowMenu);

std::unique_ptr<Action> OpenPipelinesMenu(Interface self, ui::Pointer& pointer, Toy* toy) {
  using enum ui::Dir;
  Interface options[ui::kDirCount];
  options[static_cast<int>(NW)] = Proto("GStreamer");
  options[static_cast<int>(N)] = Interface(self.object_ptr, &kFfmpegMenu);
  options[static_cast<int>(NE)] = Interface(self.object_ptr, &kTensorFlowMenu);
  options[static_cast<int>(SW)] = Proto("GEGL");
  options[static_cast<int>(SE)] = Proto("PipeWire");
  options[static_cast<int>(S)] = Proto("pipewire:node");
  return MakeMenuAction(pointer, OptionsProvider::MODE_8_DIR, options, toy);
}

constinit ObjectSource::Table kPipelinesMenu =
    MenuTable<ObjectSource::Table>("Pipelines", &OpenPipelinesMenu);

struct BetaShelfToy : ObjectToy {
  BetaShelfToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {}

  StrView Name() const override { return "BetaShelfToy"; }

  SkPath Shape() const override { return SkPath::Circle(0, 0, kStampRadius); }

  Optional<Rect> DrawBounds() const override { return Shape().getBounds().makeOutset(2_mm, 2_mm); }

  void Draw(SkCanvas& canvas) const override {
    ui::beta::DrawBetaStamp(canvas, {0, 0}, kStampRadius - 1_mm, -12, ID());
  }

  MiniMenuMode MenuMode() override { return MODE_8_DIR; }
  Interface FindOption(ui::Pointer& pointer, ui::ActionTrigger trigger) override {
    using enum ui::Dir;
    auto shelf = LockOwner();
    if (!shelf) return {};
    switch (static_cast<ui::Dir>(trigger)) {
      case W:
        return Proto("Command");
      case SW:
        return Proto("File");
      case N:
        return Interface(*shelf, kPipelinesMenu);
      default:
        return ObjectToy::FindOption(pointer, trigger);
    }
  }
};

}  // namespace

std::unique_ptr<ObjectToy> BetaShelf::MakeToy(ui::Widget* parent) {
  return std::make_unique<BetaShelfToy>(parent, *this);
}

}  // namespace automat::library
