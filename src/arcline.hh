#pragma once

#include <include/core/SkPath.h>

#include "log.hh"
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

  struct Iterator {
    const ArcLine& arcline;
    U32 i;
    float i_fract;  // A value between 0 and 1.

    Vec2 segment_start_pos;
    float segment_start_angle;

    // Constructs a new iterator at the beginning of the ArcLine.
    Iterator(const ArcLine& arcline)
        : arcline(arcline),
          i(0),
          i_fract(0),
          segment_start_pos(arcline.start),
          segment_start_angle(arcline.start_angle) {}

    // Return the current position of the iterator.
    Vec2 Position() const {
      if (arcline.types[i] == Type::Line) {
        return segment_start_pos +
               Vec2::Polar(segment_start_angle, arcline.segments[i].line.length * i_fract);
      } else {
        const auto& arc = arcline.segments[i].arc;
        const float angle_to_center =
            segment_start_angle + (arc.sweep_angle > 0 ? M_PI / 2 : -M_PI / 2);
        const Vec2 center = segment_start_pos + Vec2::Polar(angle_to_center, arc.radius);
        const float angle_to_p = M_PI + angle_to_center + arc.sweep_angle * i_fract;
        return center + Vec2::Polar(angle_to_p, arc.radius);
      }
    }

    // Return the current angle of the iterator. May fall outside of [-PI, PI] range.
    float Angle() const {
      if (arcline.types[i] == Type::Line) {
        return segment_start_angle;
      } else {
        const auto& arc = arcline.segments[i].arc;
        return segment_start_angle + arc.sweep_angle * i_fract;
      }
    }

    // Move the iterator along the ArcLine by `length`.
    // The length can be negative, in which case the iterator will move backwards.
    // The iterator will stop at the end of the ArcLine.
    // Returns the actual distance the iterator moved.
    float Advance(float length);

    // Move the iterator to the end of the ArcLine.
    // Returns the actual distance the iterator moved.
    float AdvanceToEnd();
  };

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