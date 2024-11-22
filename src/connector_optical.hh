// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
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
#include "vec.hh"
#include "widget.hh"

namespace automat::gui {

struct OpticalConnectorPimpl;

struct CablePhysicsSimulation {
  float dispenser_v;

  struct CableSection {
    Vec2 pos;
    Vec2 vel;
    Vec2 acc;

    maf::SinCos dir;  // Direction of the cable at this point. This is calculated from the previous
                      // and next points.
    maf::SinCos
        true_dir_offset;  // Difference between the "true" dir (coming from the arcline) and `dir`.

    // Distance to the next element
    float distance;

    maf::SinCos next_dir_delta;                                  // 0 when the cable is straight
    maf::SinCos prev_dir_delta = maf::SinCos::FromDegrees(180);  // M_PI when the cable is straight
  };

  maf::Vec<CableSection> sections;
  maf::Optional<maf::ArcLine> arcline;

  bool stabilized = false;
  Vec2 stabilized_start;
  maf::Optional<Vec2> stabilized_end;

  // The length of the cable calculated during the last DrawCable call.
  mutable float approx_length = 0;
  animation::SpringV2<float> connector_scale = 1;

  Location& location;
  Argument& arg;

  animation::Spring<float> steel_insert_hidden;
  bool hidden = false;

  float cable_width = 2_mm;
  float lightness_pct;

  mutable std::unique_ptr<OpticalConnectorPimpl> pimpl;

  CablePhysicsSimulation(Location&, Argument& arg, Vec2AndDir start);
  ~CablePhysicsSimulation();

  Vec2 PlugTopCenter() const;
  SkPath Shape() const;
  SkMatrix ConnectorMatrix() const;
};

maf::ArcLine RouteCable(Vec2AndDir start, maf::Span<const Vec2AndDir> ends,
                        DrawContext* debug_ctx = nullptr);

animation::Phase SimulateCablePhysics(time::Timer&, CablePhysicsSimulation&, Vec2AndDir start,
                                      maf::Span<Vec2AndDir> end_candidates);

void DrawOpticalConnector(DrawContext&, const CablePhysicsSimulation&, PaintDrawable& icon);

// Draws the given path as a cable and possibly update its length.
void DrawCable(DrawContext&, SkPath&, sk_sp<SkColorFilter>&, CableTexture, float start_width,
               float end_width, float* length = nullptr);

}  // namespace automat::gui