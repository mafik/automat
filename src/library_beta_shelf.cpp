// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_beta_shelf.hpp"

#include <include/core/SkCanvas.h>

#include "make_object_option.hpp"
#include "prototypes.hpp"
#include "ui_beta.hpp"
#include "units.hpp"

namespace automat::library {

namespace {

constexpr float kStampRadius = 1.1_cm;

void VisitProto(OptionVisitor& visitor, StrView name, Option::Dir dir) {
  if (auto* proto = prototypes ? prototypes->Find(name) : nullptr) {
    MakeObjectOption opt(proto->AcquirePtr(), dir);
    visitor(opt);
  }
}

// A named sub-menu; activating it opens a ring with the visited options.
struct GroupOption : TextOption, OptionsProvider {
  GroupOption(Str label) : TextOption(label) {}
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) override { return OpenMenu(pointer); }
};

struct FfmpegOption : GroupOption {
  FfmpegOption() : GroupOption("FFmpeg") {}
  Ptr<Option> Clone() const override { return MAKE_PTR(FfmpegOption); }
  void Options(ui::Pointer&, OptionVisitor& visitor) override {
    VisitProto(visitor, "avformat", Option::N);
    VisitProto(visitor, "avcodec", Option::S);
  }
  Dir PreferredDir() const override { return N; }
};

struct TensorFlowOption : GroupOption {
  TensorFlowOption() : GroupOption("TensorFlow") {}
  Ptr<Option> Clone() const override { return MAKE_PTR(TensorFlowOption); }
  void Options(ui::Pointer&, OptionVisitor& visitor) override {
    VisitProto(visitor, "tf:tensor", Option::N);
    VisitProto(visitor, "Square", Option::S);
  }
  Dir PreferredDir() const override { return NE; }
};

struct PipelinesOption : GroupOption {
  PipelinesOption() : GroupOption("Pipelines") {}
  Ptr<Option> Clone() const override { return MAKE_PTR(PipelinesOption); }
  void Options(ui::Pointer&, OptionVisitor& visitor) override {
    VisitProto(visitor, "GStreamer", Option::NW);
    static FfmpegOption ffmpeg;
    visitor(ffmpeg);
    VisitProto(visitor, "GEGL", Option::SW);
    VisitProto(visitor, "PipeWire", Option::SE);
    VisitProto(visitor, "pipewire:node", Option::S);
    static TensorFlowOption tensorflow;
    visitor(tensorflow);
  }
  Dir PreferredDir() const override { return N; }
};

struct BetaShelfToy : ObjectToy {
  BetaShelfToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {}

  StrView Name() const override { return "BetaShelfToy"; }

  SkPath Shape() const override { return SkPath::Circle(0, 0, kStampRadius); }

  Optional<Rect> DrawBounds() const override { return Shape().getBounds().makeOutset(2_mm, 2_mm); }

  void Draw(SkCanvas& canvas) const override {
    ui::beta::DrawBetaStamp(canvas, {0, 0}, kStampRadius - 1_mm, -12, ID());
  }

  void Options(ui::Pointer& pointer, OptionVisitor& visitor) override {
    ObjectToy::Options(pointer, visitor);
    VisitProto(visitor, "Command", Option::W);
    VisitProto(visitor, "File", Option::SW);
    VisitProto(visitor, "Leptonica", Option::E);
    static PipelinesOption pipelines;
    visitor(pipelines);
  }
};

}  // namespace

std::unique_ptr<ObjectToy> BetaShelf::MakeToy(ui::Widget* parent) {
  return std::make_unique<BetaShelfToy>(parent, *this);
}

}  // namespace automat::library
