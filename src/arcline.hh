// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkPath.h>

#include "math.hh"
#include "sincos.hh"
#include "vec.hh"

namespace maf {

// TODO: move the turn direction to the `radius` sign bit (from the `sweep_angle` sign)
// TODO: update those docs!
//
// ArcLine describes a shape composed of lines and arcs.
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
    SinCos sweep_angle;
    float ToRadians() const {
      return std::signbit(radius) ? sweep_angle.ToRadiansNegative()
                                  : sweep_angle.ToRadiansPositive();
    }
    float ToDegrees() const {
      return std::signbit(radius) ? sweep_angle.ToDegreesNegative()
                                  : sweep_angle.ToDegreesPositive();
    }
  };

  enum class Type : uint8_t { Line, Arc } type;
  union Segment {
    Segment(const Segment& other) : arc(other.arc) {}
    Segment(Line&& line) : line(line) {}
    Segment(Arc&& arc) : arc(arc) {}
    Arc arc;
    Line line;
  };

  Vec2 start;
  SinCos start_angle;
  Vec<Type> types;
  Vec<Segment> segments;

  ArcLine(Vec2 start, SinCos start_angle);
  ArcLine(const ArcLine& copy) = default;

  ArcLine& MoveBy(float length);

  // Turn the ArcLine by at most 180 degrees.
  // When `sweep_angle` is positive, the arc turns to the left.
  // Turn radius should be always positive.
  ArcLine& TurnConvex(SinCos sweep_angle, float turn_radius);

  // Turn the ArcLine by at most 360 degrees.
  // When `turn_radius` is positive, the arc turns to the left.
  ArcLine& TurnBy(SinCos sweep_angle, float turn_radius);

  ArcLine& Outset(float offset);

  SkPath ToPath(bool close = true, float length_limit = HUGE_VALF) const;
  Rect Bounds() const;

  struct Iterator {
    const ArcLine& arcline;
    I32 i;
    float i_fract;  // A value between 0 and 1.

    Vec2 segment_start_pos;
    SinCos segment_start_angle;

    // Constructs a new iterator at the beginning of the ArcLine.
    Iterator(const ArcLine& arcline)
        : arcline(arcline),
          i(0),
          i_fract(0),
          segment_start_pos(arcline.start),
          segment_start_angle(arcline.start_angle) {}

    // Return the current position of the iterator.
    Vec2 Position() const {
      if (arcline.segments.empty()) {
        return arcline.start;
      } else if (arcline.types[i] == Type::Line) {
        return segment_start_pos +
               Vec2::Polar(segment_start_angle, arcline.segments[i].line.length * i_fract);
      } else {
        const auto& arc = arcline.segments[i].arc;
        const Vec2 center =
            segment_start_pos + Vec2::Polar(segment_start_angle + 90_deg, arc.radius);
        return center + Vec2::Polar(Angle() - 90_deg, arc.radius);
      }
    }

    // Return the current angle of the iterator.
    SinCos Angle() const {
      if (arcline.segments.empty()) {
        return arcline.start_angle;
      } else if (arcline.types[i] == Type::Line) {
        return segment_start_angle;
      } else {
        const auto& arc = arcline.segments[i].arc;
        return segment_start_angle + (std::signbit(arc.radius)
                                          ? arc.sweep_angle.ScaleNegative(i_fract)
                                          : arc.sweep_angle.ScalePositive(i_fract));
      }
    }

    // Move the iterator along the ArcLine by `length`.
    // The length can be negative, in which case the iterator will move backwards.
    // The iterator will stop at the end of the ArcLine.
    // Returns the actual distance the iterator moved (always >= 0).
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
    SinCos first_turn_angle;
    float move_between_turns;
    float distance_forward;

    TurnShift(float distance_sideways, float turn_radius);
    void Apply(ArcLine& line) const;
    void ApplyNegative(ArcLine& line) const;
  };
};

}  // namespace maf