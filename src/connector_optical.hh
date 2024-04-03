#pragma once

// Contains the code related to drawing the optical connector.
//
// This type of connector can transmit boolean & event signals.

#include "connection.hh"
#include "math.hh"
#include "vec.hh"
#include "widget.hh"

namespace automat::gui {

struct OpticalConnectorState : ConnectionState {
  float dispenser_v;

  struct CableSection {
    Vec2 pos;
    Vec2 vel;
  };

  maf::Vec<CableSection> sections;
  // TODO: when the cable simulation stabilizes, draw it as a simple ArcLine.
};

void DrawOpticalConnector(DrawContext&, OpticalConnectorState&, Vec2 start, Vec2 end);

}  // namespace automat::gui