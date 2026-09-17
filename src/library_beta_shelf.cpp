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

void FillFfmpegMenu(Interface, Menu& menu) {
  using enum ui::Dir;
  menu.Place(N, Proto("avformat"));
  menu.Place(S, Proto("avcodec"));
}

void FillTensorFlowMenu(Interface, Menu& menu) {
  using enum ui::Dir;
  menu.Place(N, Proto("tf:tensor"));
  menu.Place(S, Proto("Square"));
}

constinit Interface::Table kFfmpegMenu = MenuTable("FFmpeg", &FillFfmpegMenu);
constinit Interface::Table kTensorFlowMenu = MenuTable("TensorFlow", &FillTensorFlowMenu);

void FillPipelinesMenu(Interface self, Menu& menu) {
  using enum ui::Dir;
  menu.Place(NW, Proto("GStreamer"));
  menu.Place(N, Interface(self.object_ptr, &kFfmpegMenu));
  menu.Place(NE, Interface(self.object_ptr, &kTensorFlowMenu));
  menu.Place(SW, Proto("GEGL"));
  menu.Place(SE, Proto("PipeWire"));
  menu.Place(S, Proto("pipewire:node"));
}

constinit Interface::Table kPipelinesMenu = MenuTable("Pipelines", &FillPipelinesMenu);

struct BetaShelfToy : ObjectToy {
  BetaShelfToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {}

  StrView Name() const override { return "BetaShelfToy"; }

  SkPath Shape() const override { return SkPath::Circle(0, 0, kStampRadius); }

  Optional<Rect> DrawBounds() const override { return Shape().getBounds().makeOutset(2_mm, 2_mm); }

  void Draw(SkCanvas& canvas) const override {
    ui::beta::DrawBetaStamp(canvas, {0, 0}, kStampRadius - 1_mm, -12, ID());
  }

  void FillMenu(ui::Pointer& pointer, Menu& menu) override {
    using enum ui::Dir;
    ObjectToy::FillMenu(pointer, menu);
    auto shelf = LockOwner();
    if (!shelf) return;
    menu.Place(W, Proto("Program Launcher"));
    menu.Place(SW, Proto("File"));
    menu.Place(N, Interface(*shelf, kPipelinesMenu));
  }
};

}  // namespace

std::unique_ptr<ObjectToy> BetaShelf::MakeToy(ui::Widget* parent) {
  return std::make_unique<BetaShelfToy>(parent, *this);
}

}  // namespace automat::library
