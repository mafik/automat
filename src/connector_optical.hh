#pragma once

// Contains the code related to drawing the optical connector.
//
// This type of connector can transmit boolean & event signals.

#include "math.hh"
#include "widget.hh"

namespace automat::gui {

// TODO: maybe add some animation state here and pass it to DrawOpticalConnector
// struct OpticalConnectorState {
//   maf::Vec<Vec2> points;
//   maf::Vec<Vec2> points_velocity;
//   Vec2 velocity;
//   float angle;
//   float angle_velocity;
// };

void DrawOpticalConnector(DrawContext&, Vec2 start, Vec2 end);

}  // namespace automat::gui