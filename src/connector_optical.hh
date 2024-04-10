#pragma once

// Contains the code related to drawing the optical connector.
//
// This type of connector can transmit boolean & event signals.

#include "arcline.hh"
#include "math.hh"
#include "optional.hh"
#include "vec.hh"
#include "widget.hh"

namespace automat::gui {

struct OpticalConnectorState {
  float dispenser_v;

  struct CableSection {
    Vec2 pos;
    Vec2 vel;
    Vec2 acc;

    float dir;  // Direction of the cable at this point

    // Distance to the next element
    float distance;

    float next_dir_delta;         // 0 when the cable is straight
    float prev_dir_delta = M_PI;  // M_PI when the cable is straight
  };

  maf::Vec<CableSection> sections;
  maf::Optional<maf::ArcLine> arcline;

  OpticalConnectorState(Vec2 start);

  Vec2 PlugTopCenter() const;
  Vec2 PlugBottomCenter() const;
};

void SimulateCablePhysics(float dt, OpticalConnectorState&, Vec2 start, maf::Optional<Vec2> end);

void DrawOpticalConnector(DrawContext&, OpticalConnectorState&);

}  // namespace automat::gui