// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

// Contains the code related to drawing the optical connector.
//
// This type of connector can transmit boolean & event signals.

#include "arcline.hh"
#include "argument.hh"
#include "location.hh"
#include "math.hh"
#include "optional.hh"
#include "sincos.hh"
#include "vec.hh"

namespace automat::ui {

struct CablePhysicsSimulation {
  float dispenser_v;

  struct CableSection {
    Vec2 pos;
    Vec2 vel;
    Vec2 acc;

    SinCos dir;  // Direction of the cable at this point. This is calculated from the previous
                 // and next points.
    SinCos
        true_dir_offset;  // Difference between the "true" dir (coming from the arcline) and `dir`.

    // Distance to the next element
    float distance;

    SinCos next_dir_delta;                             // 0 when the cable is straight
    SinCos prev_dir_delta = SinCos::FromDegrees(180);  // M_PI when the cable is straight
  };

  Vec<CableSection> sections;
  Optional<ArcLine> arcline;

  bool stabilized = false;
  Vec2 stabilized_start;
  Optional<Vec2> stabilized_end;

  // The length of the cable calculated during the last DrawCable call.
  mutable float approx_length = 0;
  animation::SpringV2<float> connector_scale = 1;

  Location& location;
  Argument& arg;

  animation::Spring<float> steel_insert_hidden;
  bool hidden = false;

  float cable_width = 2_mm;
  float lightness_pct = 0;

  CablePhysicsSimulation(Location&, Argument& arg, Vec2AndDir start);
  ~CablePhysicsSimulation();

  Vec2 PlugTopCenter() const;
  SkPath Shape() const;
  SkMatrix ConnectorMatrix() const;
};

ArcLine RouteCable(Vec2AndDir start, Span<const Vec2AndDir> ends, SkCanvas* debug_canvas = nullptr);

animation::Phase SimulateCablePhysics(time::Timer&, CablePhysicsSimulation&, Vec2AndDir start,
                                      Span<Vec2AndDir> end_candidates);

void DrawOpticalConnector(SkCanvas&, const CablePhysicsSimulation&, PaintDrawable& icon);

// Draws the given path as a cable and possibly update its length.
void DrawCable(SkCanvas&, SkPath&, sk_sp<SkColorFilter>&, CableTexture, float start_width,
               float end_width, float* length = nullptr);

}  // namespace automat::ui