#pragma once

// Contains the code related to drawing the optical connector.
//
// This type of connector can transmit boolean & event signals.

#include <memory>

#include "arcline.hh"
#include "argument.hh"
#include "location.hh"
#include "math.hh"
#include "optional.hh"
#include "sincos.hh"
#include "time.hh"
#include "vec.hh"
#include "widget.hh"

namespace automat::gui {

struct OpticalConnectorPimpl;
constexpr float kCableWidth = 2_mm;

struct OpticalConnectorState {
  float dispenser_v;

  struct CableSection {
    Vec2 pos;
    Vec2 vel;
    Vec2 acc;

    SinCos dir;  // Direction of the cable at this point. This is calculated from the previous and
                 // next points.
    SinCos
        true_dir_offset;  // Difference between the "true" dir (coming from the arcline) and `dir`.

    // Distance to the next element
    float distance;

    SinCos next_dir_delta;            // 0 when the cable is straight
    SinCos prev_dir_delta = 180_deg;  // M_PI when the cable is straight
  };

  maf::Vec<CableSection> sections;
  maf::Optional<maf::ArcLine> arcline;

  bool stabilized = false;
  Vec2 stabilized_start;
  maf::Optional<Vec2> stabilized_end;

  Location& location;
  Argument& arg;

  animation::Spring<float> steel_insert_hidden;
  SkColor tint = "#808080"_color;

  std::unique_ptr<OpticalConnectorPimpl> pimpl;

  OpticalConnectorState(Location&, Argument& arg, Vec2AndDir start);
  ~OpticalConnectorState();

  Vec2 PlugTopCenter() const;
  Vec2 PlugBottomCenter() const;
  SkPath Shape() const;
  SkMatrix ConnectorMatrix() const;
};

enum class CableTexture {
  Smooth,
  Braided,
};

ArcLine RouteCable(DrawContext&, Vec2AndDir start, maf::Span<Vec2AndDir> ends);

void SimulateCablePhysics(DrawContext&, float dt, OpticalConnectorState&, Vec2AndDir start,
                          maf::Span<Vec2AndDir> end_candidates);

void DrawOpticalConnector(DrawContext&, OpticalConnectorState&, PaintDrawable& icon);

void DrawCable(DrawContext&, SkPath&, sk_sp<SkColorFilter>&, CableTexture,
               float width = kCableWidth);

}  // namespace automat::gui