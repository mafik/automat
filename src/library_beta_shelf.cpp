// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_beta_shelf.hpp"

#include <include/core/SkCanvas.h>

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

Interface FfmpegOption(Interface, ui::Dir dir) {
  using enum ui::Dir;
  switch (dir) {
    case N:
      return Proto("avformat");
    case S:
      return Proto("avcodec");
    default:
      return {};
  }
}

Interface TensorFlowOption(Interface, ui::Dir dir) {
  using enum ui::Dir;
  switch (dir) {
    case N:
      return Proto("tf:tensor");
    case S:
      return Proto("Square");
    default:
      return {};
  }
}

constinit Interface::Table kFfmpegMenu = MenuTable("FFmpeg", MODE_2_DIR, &FfmpegOption);
constinit Interface::Table kTensorFlowMenu = MenuTable("TensorFlow", MODE_2_DIR, &TensorFlowOption);

Interface PipelinesOption(Interface self, ui::Dir dir) {
  using enum ui::Dir;
  switch (dir) {
    case NW:
      return Proto("GStreamer");
    case N:
      return Interface(self.object_ptr, &kFfmpegMenu);
    case NE:
      return Interface(self.object_ptr, &kTensorFlowMenu);
    case SW:
      return Proto("GEGL");
    case SE:
      return Proto("PipeWire");
    case S:
      return Proto("pipewire:node");
    default:
      return {};
  }
}

constinit Interface::Table kPipelinesMenu = MenuTable("Pipelines", MODE_8_DIR, &PipelinesOption);

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
        return Proto("Program Launcher");
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
