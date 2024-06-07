#pragma once

// Contains the code related to drawing the optical connector.
//
// This type of connector can transmit boolean & event signals.

#include "arcline.hh"
#include "location.hh"
#include "math.hh"
#include "optional.hh"
#include "time.hh"
#include "vec.hh"
#include "widget.hh"

namespace automat::gui {

struct OpticalConnectorState : Location::NextObserver {
  float dispenser_v;

  struct CableSection {
    Vec2 pos;
    Vec2 vel;
    Vec2 acc;

    float dir;  // Direction of the cable at this point. This is calculated from the previous and
                // next points.
    float
        true_dir_offset;  // Difference between the "true" dir (coming from the arcline) and `dir`.

    // Distance to the next element
    float distance;

    float next_dir_delta;         // 0 when the cable is straight
    float prev_dir_delta = M_PI;  // M_PI when the cable is straight
  };

  maf::Vec<CableSection> sections;
  maf::Optional<maf::ArcLine> arcline;

  bool stabilized = false;
  Vec2 stabilized_start;
  maf::Optional<Vec2> stabilized_end;

  Location& location;
  time::SystemPoint last_activity = time::kZero;

  animation::Spring steel_insert_hidden;

  OpticalConnectorState(Location&, Vec2AndDir start);
  ~OpticalConnectorState();

  Vec2 PlugTopCenter() const;
  Vec2 PlugBottomCenter() const;

  void OnNextActivated(Location& source) override;
};

// TODO: grabbing the connector causes it to move a bit

void SimulateCablePhysics(float dt, OpticalConnectorState&, Vec2AndDir start,
                          maf::Optional<Vec2> end);

void DrawOpticalConnector(DrawContext&, OpticalConnectorState&);

}  // namespace automat::gui