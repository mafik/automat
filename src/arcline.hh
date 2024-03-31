#pragma once

#include <include/core/SkPath.h>

#include "math.hh"
#include "vec.hh"

namespace maf {

// ArcLine describes a shape composed of lines and arcs. It is always closed and follows a CCW
// winding order.
//
// The basic functionality to outset the shape is implemented, but it is not perfect. A couple
// edge-cases are still not handled:
// - The path is not normalized - so two consecutive lines can be merged into one line, two arcs
// with the same center can be merged into one arc.
// - The user must take care to finish the path in the same position as it started. Otherwise
// outsetting might lead to broken shapes.
// - The path could be re-normalized after the outset operation to merge consecutive lines and arcs
struct ArcLine {
  struct Line {
    float length;
  };

  struct Arc {
    float radius;
    float sweep_angle;
  };

  enum class Type : uint8_t { Line, Arc } type;
  union Segment {
    Line line;
    Arc arc;
  };

  Vec2 start;
  float start_angle;
  Vec<Type> types;
  Vec<Segment> segments;

  ArcLine(Vec2 start, float start_angle);
  ArcLine(const ArcLine& copy) = default;

  ArcLine& MoveBy(float length);

  // When `sweep_angle` is positive, the arc turns to the left.
  // Turn radius should be always positive.
  ArcLine& TurnBy(float sweep_angle, float turn_radius);

  ArcLine& Outset(float offset);

  SkPath ToPath(bool close = true) const;

  // An operation that moves the ArcLine sideways without changing its direction.
  // A sideways move is performed by two arcs and an optional move between them.
  //
  // Constructor of this class calculates the parameters of the operation.
  // The operation can then be performed using the Apply method.
  struct TurnShift {
    const float turn_radius;
    float first_turn_angle;
    float move_between_turns;
    float distance_forward;

    TurnShift(float distance_sideways, float turn_radius);
    void Apply(ArcLine& line) const;
  };
};

}  // namespace maf