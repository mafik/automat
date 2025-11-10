// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "arcline.hh"

#include <cavc/polyline.hpp>
#include <cavc/polylineoffset.hpp>
#include <cmath>

#include "log.hh"
#include "sincos.hh"

namespace automat {

constexpr bool kDebugOutset = false;
constexpr bool kDebugMakeFromPath = false;

ArcLine::ArcLine(Vec2 start, SinCos start_angle) : start(start), start_angle(start_angle) {}

ArcLine ArcLine::MakeFromPath(const SkPath& path) {
  if constexpr (kDebugMakeFromPath) {
    LOG << "Converting path to ArcLine...";
    LOG_Indent();
  }
  SkPath::Iter iter(path, true);
  Vec2 pts[4];
  Vec2 &p0 = pts[0], &p1 = pts[1], &p2 = pts[2], &p3 = pts[3];
  Vec2 arcline_end = {0, 0};
  SinCos arcline_end_dir = 0_deg;
  ArcLine arcline(arcline_end, arcline_end_dir);

  auto LineTo = [&arcline_end, &arcline_end_dir, &arcline](Vec2 to) {
    Vec2 delta = to - arcline_end;
    float length = Length(delta);
    SinCos dir = SinCos::FromVec2(delta, length);
    if (arcline_end_dir != dir) {
      arcline.TurnConvex(dir - arcline_end_dir, 0);
      arcline_end_dir = dir;
    }
    arcline.MoveBy(length);
    arcline_end = to;
  };
  while (true) {
    auto verb = iter.next(&p0.sk);
    switch (verb) {
      case SkPath::kMove_Verb:
        if (!arcline.segments.empty()) {
          ERROR << "Multi-contour path cannot be (yet) converted to ArcLine";
        }
        if constexpr (kDebugMakeFromPath) {
          LOG << "Move to " << p0.ToStrMetric();
        }
        arcline.types.clear();
        arcline.segments.clear();
        arcline.start = p0;
        arcline_end = p0;
        break;
      case SkPath::kLine_Verb: {
        if constexpr (kDebugMakeFromPath) {
          LOG << "Line from " << p0.ToStrMetric() << " to " << p1.ToStrMetric();
        }
        if (arcline.segments.empty()) {
          arcline.start = p0;
          Vec2 delta = p1 - p0;
          arcline.start_angle = SinCos::FromVec2(delta);
          arcline.types.push_back(Type::Line);
          arcline.segments.emplace_back(Line{.length = Length(delta)});
          arcline_end = p1;
          arcline_end_dir = arcline.start_angle;
        } else {
          LineTo(p1);
        }
        break;
      }
      case SkPath::kQuad_Verb: {
        if constexpr (kDebugMakeFromPath) {
          LOG << "Quadratic from " << p0.ToStrMetric() << " to " << p2.ToStrMetric();
        }
        LineTo(EvalBezierAtFixedT<0.5f>(p0, p1, p2));
        LineTo(p2);
        break;
      }
      case SkPath::kCubic_Verb: {
        if constexpr (kDebugMakeFromPath) {
          LOG << "Cubic from " << p0.ToStrMetric() << " to " << p3.ToStrMetric();
        }
        LineTo(EvalBezierAtFixedT<0.25f>(p0, p1, p2, p3));
        LineTo(EvalBezierAtFixedT<0.5f>(p0, p1, p2, p3));
        LineTo(EvalBezierAtFixedT<0.75f>(p0, p1, p2, p3));
        LineTo(p3);
        break;
      }
      case SkPath::kClose_Verb:
        if constexpr (kDebugMakeFromPath) {
          LOG << "Close";
        }
        if (arcline_end_dir != arcline.start_angle) {
          arcline.TurnConvex(arcline.start_angle - arcline_end_dir, 0);
          arcline_end_dir = arcline.start_angle;
        }
        break;
      case SkPath::kConic_Verb: {
        // Note: we only support sections of a circle - not arbitrary conics!
        SinCos arc_start_angle = SinCos::FromVec2(p1 - p0);
        SinCos arc_end_angle = SinCos::FromVec2(p2 - p1);
        SinCos sweep_angle = arc_end_angle - arc_start_angle;
        float radius = sqrt(LengthSquared(p0 - p2) / (2 - 2 * (float)sweep_angle.cos));
        if constexpr (kDebugMakeFromPath) {
          LOG << "Conic from " << p0.ToStrMetric() << " to " << p2.ToStrMetric();
        }
        if (arcline.segments.empty()) {
          arcline.start = p0;
          arcline.start_angle = arc_start_angle;
          arcline_end = arcline.start;
          arcline_end_dir = arcline.start_angle;
        }
        if (arc_start_angle != arcline_end_dir) {
          arcline.TurnConvex(arc_start_angle - arcline_end_dir, 0);
          arcline_end_dir = arc_start_angle;
        }
        arcline.TurnConvex(sweep_angle, radius);
        arcline_end = p2;
        arcline_end_dir = arc_end_angle;
        break;
      }
      case SkPath::kDone_Verb:
        if constexpr (kDebugMakeFromPath) {
          LOG_Unindent();
        }
        return arcline;
    }
  }
}

ArcLine& ArcLine::MoveBy(float length) {
  if (!types.empty() && types.back() == Type::Line) {
    segments.back().line.length += length;
  } else {
    types.push_back(Type::Line);
    segments.emplace_back(Line{length});
  }
  return *this;
}

// Turn a given point & angle by `sweep_angle` radians around a circle with `radius`.
static void Turn(Vec2& point, SinCos& angle, SinCos sweep_angle, float radius) {
  Vec2 center = point + Vec2::Polar(angle + 90_deg, radius);
  point = center + Vec2::Polar(angle - 90_deg + sweep_angle, radius);
  angle = angle + sweep_angle;
}

ArcLine& ArcLine::TurnConvex(SinCos sweep_angle, float radius) {
  if (sweep_angle.sin.value < 0) {
    radius = -radius;
  }
  return TurnBy(sweep_angle, radius);
}

ArcLine& ArcLine::TurnBy(SinCos sweep_angle, float radius) {
  if (sweep_angle == 0_deg) {
    return *this;
  }
  // TODO: collapse turns with the same radius, but not when they exceed 180°
  // if (!types.empty() && types.back() == Type::Arc && segments.back().arc.radius == radius) {
  //   auto new_sweep_angle = segments.back().arc.sweep_angle + sweep_angle;
  //   segments.back().arc.sweep_angle = new_sweep_angle;
  //   return *this;
  // }
  types.push_back(Type::Arc);
  segments.emplace_back(Arc{.radius = radius, .sweep_angle = sweep_angle});
  return *this;
}

ArcLine& ArcLine::Outset(float offset) {
  constexpr float kScale = 32.f;
  cavc::Polyline<float> pline;
  Vec2 p = start;
  SinCos current_alpha = start_angle;
  int turn_count = 0;
  if constexpr (kDebugOutset) {
    LOG << "Outset by " << offset;
    LOG_Indent();
    LOG << "Start at " << p;
  }
  pline.addVertex(p.x * kScale, p.y * kScale, 0);
  for (int i = 0; i < types.size(); ++i) {
    if (types[i] == Type::Line) {
      if constexpr (kDebugOutset) {
        LOG << "Move by " << segments[i].line.length;
      }
      p += Vec2::Polar(current_alpha, segments[i].line.length);
      pline.addVertex(p.x * kScale, p.y * kScale, 0);
    } else if (types[i] == Type::Arc) {
      if constexpr (kDebugOutset) {
        LOG << "Turn by " << segments[i].arc.sweep_angle.ToDegrees() << "°";
      }
      auto& arc = segments[i].arc;
      auto p0 = p;
      bool sin_was_positive = current_alpha.sin >= 0;
      Turn(p, current_alpha, arc.sweep_angle, arc.radius);
      bool sin_is_positive = current_alpha.sin >= 0;
      if (arc.sweep_angle.sin >= 0) {
        if (!sin_was_positive && sin_is_positive) {
          if constexpr (kDebugOutset) {
            LOG << "CCW turn completed";
          }
          ++turn_count;
        }
      } else {
        if (sin_was_positive && !sin_is_positive) {
          if constexpr (kDebugOutset) {
            LOG << "CW turn completed";
          }
          --turn_count;
        }
      }
      if (fabsf(arc.radius) > 0) {
        // float bulge = tanf(arc.sweep_angle.ToRadians() / 4);
        // Another way to calculate the bulge, reducing the floating point error
        float bulge = ((float)M_SQRT2 - sqrtf(1.f + (float)arc.sweep_angle.cos)) /
                      sqrtf(1 - (float)arc.sweep_angle.cos);
        if (arc.sweep_angle.sin < 0) {
          bulge = -bulge;
        }
        pline.vertexes().back().bulge() = bulge;
        pline.addVertex(p.x * kScale, p.y * kScale, 0);
      }
    }
  }
  bool sin_was_positive = current_alpha.sin >= 0;
  bool sin_is_positive = start_angle.sin >= 0;
  SinCos final_sweep = start_angle - current_alpha;
  if (sin_was_positive != sin_is_positive) {
    if (final_sweep.sin >= 0) {
      if constexpr (kDebugOutset) {
        LOG << "Leftover CCW turn completed";
      }
      ++turn_count;
    } else {
      if constexpr (kDebugOutset) {
        LOG << "Leftover CW turn completed";
      }
      --turn_count;
    }
  }
  if constexpr (kDebugOutset) {
    LOG << "Turn count: " << turn_count;
  }
  // Last vertex is always in the same place as the start, so we can remove it.
  pline.vertexes().pop_back();
  pline.isClosed() = true;

  if (turn_count < 0) {
    cavc::invertDirection(pline);
  }

  auto result = cavc::parallelOffset(pline, -offset * kScale);

  segments.clear();
  types.clear();
  if constexpr (kDebugOutset) {
    LOG_Unindent();
  }
  if (result.empty()) {
    return *this;
  }
  auto& result0 = result[0];
  auto& first_vertex = result0.vertexes().front();
  auto& second_vertex_ref = result0.vertexes()[1];
  Vec2 second_vertex = Vec2(second_vertex_ref.x() / kScale, second_vertex_ref.y() / kScale);
  start = Vec2(first_vertex.x() / kScale, first_vertex.y() / kScale);
  start_angle = SinCos::FromVec2(second_vertex - start);
  if (!first_vertex.bulgeIsZero()) {
    start_angle = start_angle - SinCos::FromRadians(atanf(first_vertex.bulge()) * 2);
  }
  Vec2 p0 = start;
  SinCos p0_angle = start_angle;
  for (int i = 1; i <= result0.vertexes().size(); ++i) {
    auto& p0_ref = result0.vertexes()[i - 1];
    auto& p1_ref = result0.vertexes()[i % result0.vertexes().size()];

    Vec2 p1(p1_ref.x() / kScale, p1_ref.y() / kScale);
    float bulge = p0_ref.bulge();
    bool bulge_is_zero = p0_ref.bulgeIsZero();

    // There are two ways to calculate the bulge angle.
    // This approach produces lower floating point error:
    float bulge2 = bulge * bulge;
    float denom = 1.f + bulge2;
    float bulge_cos = (1.f - bulge2) / denom;
    float bulge_sin = 2 * bulge / denom;
    SinCos bulge_angle = SinCos(bulge_sin, bulge_cos);

    // Another approach, using inverse trigonometric functions:
    // SinCos bulge_angle = bulge_is_zero ? 0_deg : SinCos::FromRadians(atanf(bulge) * 2);

    Vec2 delta = p1 - p0;
    float length = Length(delta);
    SinCos p1_angle = SinCos::FromVec2(delta, length);
    SinCos new_p0_angle = p1_angle - bulge_angle;
    if (new_p0_angle != p0_angle) {
      TurnConvex(new_p0_angle - p0_angle, 0);
    }
    if (bulge_is_zero) {
      MoveBy(length);
    } else {
      float sin = (float)bulge_angle.sin;
      float radius = sin == 0 ? length / 2 : length / (2 * sin);
      TurnConvex(bulge_angle * 2, radius);
    }
    p0_angle = p1_angle + bulge_angle;
    p0 = p1;
  }
  if (p0_angle != start_angle) {
    TurnConvex(start_angle - p0_angle, 0);
  }

  return *this;

  // Old code
  // Keeping this around since it's fairly efficient for convex shapes

  start += Vec2::Polar(start_angle - 90_deg, offset);
  for (int i = 0; i < types.size(); ++i) {
    if (types[i] == Type::Arc) {
      auto& arc = segments[i].arc;
      bool sign = std::signbit(arc.radius);
      arc.radius += offset;
      if (std::signbit(arc.radius) != sign) {  // outset shortens the nearby segments
        float s = arc.radius;
        arc.radius = sign ? -0.f : 0.f;
        float angle = (M_PI - fabsf(sign ? arc.sweep_angle.ToRadiansNegative()
                                         : arc.sweep_angle.ToRadiansPositive())) /
                      2;
        float ctg = 1 / tan(angle);
        float delta_length = s * ctg;

        int prev_i = (i + types.size() - 1) % types.size();
        if (types[prev_i] == Type::Line) {
          segments[prev_i].line.length += delta_length;
        }
        int next_i = (i + 1) % types.size();
        if (types[next_i] == Type::Line) {
          LOG << "Changing the length of the next line segment by " << delta_length;
          segments[next_i].line.length += delta_length;
          if (next_i == 0) {
            start -= Vec2::Polar(start_angle, delta_length);
          }
        }
      }
    }
  }
  return *this;
}

std::string ArcLine::ToStr() const {
  std::stringstream ss;
  ss << "ArcLine(" << start.ToStr() << ", " << start_angle.ToStr();
  for (int i = 0; i < types.size(); ++i) {
    if (types[i] == Type::Line) {
      ss << ", move by " << segments[i].line.length;
    } else {
      ss << ", turn by " << segments[i].arc.sweep_angle.ToDegrees() << "°";
      if (segments[i].arc.radius != 0) {
        ss << " with radius " << segments[i].arc.radius;
      }
    }
  }
  ss << ")";
  return ss.str();
}

std::string ArcLine::ToStrCpp() const {
  std::stringstream ss;
  ss << "ArcLine(Vec2(" << start.x * 1000 << "_mm, " << start.y * 1000 << "_mm), "
     << start_angle.ToDegrees() << "_deg)";
  for (int i = 0; i < types.size(); ++i) {
    if (types[i] == Type::Line) {
      ss << ".MoveBy(" << segments[i].line.length * 1000 << "_mm)";
    } else {
      ss << ".TurnBy(" << segments[i].arc.sweep_angle.ToDegrees() << "_deg, "
         << segments[i].arc.radius * 1000 << "_mm)";
    }
  }
  ss << ";";
  return ss.str();
}

SkPath ArcLine::ToPath(bool close, float length_limit) const {
  SkPath path;
  path.moveTo(start.x, start.y);
  if (length_limit <= 0) {
    return path;
  }
  Vec2 p = start;
  SinCos current_alpha = start_angle;
  float length = 0;
  for (int i = 0; i < types.size(); ++i) {
    float remaining = length_limit - length;
    if (types[i] == Type::Line) {
      if (segments[i].line.length < remaining) {
        p += Vec2::Polar(current_alpha, segments[i].line.length);
        path.lineTo(p.x, p.y);
        length += segments[i].line.length;
      } else {
        p += Vec2::Polar(current_alpha, remaining);
        path.lineTo(p.x, p.y);
        length = length_limit;
        break;
      }
    } else {
      float sweep_radians = segments[i].arc.ToRadians();
      float r = segments[i].arc.radius;
      float r_abs = fabsf(r);
      float l = fabsf(sweep_radians * r);
      Vec2 center = p + Vec2::Polar(current_alpha + 90_deg, r);
      SkRect oval = SkRect::MakeXYWH(center.x - r_abs, center.y - r_abs, 2 * r_abs, 2 * r_abs);
      SinCos alpha0 = current_alpha;
      if (l < remaining) {
        SinCos sweep_angle = segments[i].arc.sweep_angle;
        Turn(p, current_alpha, sweep_angle, r);
        path.arcTo(oval, (std::signbit(r) ? 90 : -90) + alpha0.ToDegrees(),
                   segments[i].arc.ToDegrees(), false);
        length += l;
      } else {
        sweep_radians = remaining / r;
        path.arcTo(oval, (std::signbit(r) ? 90 : -90) + alpha0.ToDegrees(),
                   sweep_radians * 180 / M_PI, false);
        length = length_limit;
        break;
      }
    }
  }
  if (close) {
    path.close();
  }
  return path;
}

Rect ArcLine::Bounds() const {
  Vec2 p = start;
  Rect bounds = Rect(p.x, p.y, p.x, p.y);
  SinCos current_alpha = start_angle;
  for (int i = 0; i < types.size(); ++i) {
    if (types[i] == Type::Line) {
      p += Vec2::Polar(current_alpha, segments[i].line.length);
    } else {
      auto start_quadrant = current_alpha.Quadrant();
      auto sweep_angle = segments[i].arc.sweep_angle;
      auto radius = segments[i].arc.radius;
      auto start_alpha = current_alpha;

      // Inlined TurnBy
      Vec2 center = p + Vec2::Polar(current_alpha + 90_deg, radius);
      p = center + Vec2::Polar(current_alpha - 90_deg + sweep_angle, radius);
      current_alpha = current_alpha + sweep_angle;

      auto end_quadrant = current_alpha.Quadrant();

      // Check if we're making a large (180+ deg) turn and end up in the same quadrant.
      // If that's the case then we need to adjust the start / end quadrants.
      // We try to adjust the quadrant that's closer to a cardinal dir.
      if (start_quadrant == end_quadrant) {
        if (radius >= 0 && sweep_angle.sin < 0) {
          if (start_alpha.CardinalDistance() < current_alpha.CardinalDistance()) {
            start_quadrant += 1;
            if (start_quadrant > 4) start_quadrant = 1;
          } else {
            end_quadrant -= 1;
            if (end_quadrant < 1) end_quadrant = 4;
          }
        }
        if (radius < 0 && sweep_angle.sin > 0) {
          if (start_alpha.CardinalDistance() < current_alpha.CardinalDistance()) {
            start_quadrant -= 1;
            if (start_quadrant < 1) start_quadrant = 4;
          } else {
            end_quadrant += 1;
            if (end_quadrant > 4) end_quadrant = 1;
          }
        }
      }

      // Simulate the line movement from the start to end quadrants.
      // Whenever a quadrant changes, we should expand the bounding box to include the new point.
      if (start_quadrant != end_quadrant) {
        if (radius >= 0) {  // CCW
          while (start_quadrant != end_quadrant) {
            switch (start_quadrant) {
              case 1:
                bounds.ExpandToInclude(center + Vec2(radius, 0));
                start_quadrant = 2;
                break;
              case 2:
                bounds.ExpandToInclude(center + Vec2(0, radius));
                start_quadrant = 3;
                break;
              case 3:
                bounds.ExpandToInclude(center + Vec2(-radius, 0));
                start_quadrant = 4;
                break;
              case 4:
                bounds.ExpandToInclude(center + Vec2(0, -radius));
                start_quadrant = 1;
                break;
            }
          }
        } else {  // CW
          while (start_quadrant != end_quadrant) {
            switch (start_quadrant) {
              case 1:
                bounds.ExpandToInclude(center + Vec2(0, -radius));
                start_quadrant = 4;
                break;
              case 2:
                bounds.ExpandToInclude(center + Vec2(radius, 0));
                start_quadrant = 1;
                break;
              case 3:
                bounds.ExpandToInclude(center + Vec2(0, radius));
                start_quadrant = 2;
                break;
              case 4:
                bounds.ExpandToInclude(center + Vec2(-radius, 0));
                start_quadrant = 3;
                break;
            }
          }
        }
      }
    }
    bounds.ExpandToInclude(p);
  }
  return bounds;
}

ArcLine::TurnShift::TurnShift(float distance_sideways, float turn_radius)
    : turn_radius(turn_radius) {
  /*
        .           \
       /|           |
      / |           \
     /  |            } turn_radius
    /   |           /
   /____|           |
   `-.__| } delta_x /
   \_  _/
     \/
   delta_y

  */
  if (distance_sideways == 0) {
    first_turn_angle = 0_deg;
    move_between_turns = 0;
    distance_forward = 0;
    return;
  }
  const float delta_x = distance_sideways / 2;
  const float delta_x_abs = std::abs(delta_x);
  if (delta_x_abs < turn_radius) {
    float r_minus_x = turn_radius - delta_x_abs;
    float delta_y = sqrt(turn_radius * turn_radius - r_minus_x * r_minus_x);
    first_turn_angle = SinCos::FromRadians(atan2(delta_y, r_minus_x));
    move_between_turns = 0;
    distance_forward = delta_y * 2;
  } else {
    first_turn_angle = 90_deg;
    move_between_turns = (delta_x_abs - turn_radius) * 2;
    distance_forward = turn_radius * 2;
  }
  if (delta_x < 0) {
    first_turn_angle = -first_turn_angle;
  }
}
void ArcLine::TurnShift::Apply(ArcLine& line) const {
  line.TurnConvex(first_turn_angle, turn_radius);
  if (move_between_turns > 0) {
    line.MoveBy(move_between_turns);
  }
  line.TurnConvex(-first_turn_angle, turn_radius);
}
void ArcLine::TurnShift::ApplyNegative(ArcLine& line) const {
  line.TurnConvex(-first_turn_angle, turn_radius);
  if (move_between_turns > 0) {
    line.MoveBy(move_between_turns);
  }
  line.TurnConvex(first_turn_angle, turn_radius);
}

float ArcLine::Iterator::Advance(float length) {
  float distance = 0;
  while (length != 0) {
    const auto& segment = arcline.segments[i];
    float segment_length = arcline.types[i] == Type::Line
                               ? segment.line.length
                               : fabsf(segment.arc.ToRadians() * segment.arc.radius);
    float remaining_segment_length = segment_length * (length > 0 ? 1 - i_fract : i_fract);
    float length_abs = fabsf(length);
    if (length_abs < remaining_segment_length) {
      i_fract += length / segment_length;
      distance += length_abs;
      length = 0;
      break;
    }
    distance += remaining_segment_length;
    if (length > 0) {
      length -= remaining_segment_length;
      if (i == arcline.types.size() - 1) {
        i_fract = 1;
        break;
      } else {
        if (arcline.types[i] == Type::Line) {
          segment_start_pos += Vec2::Polar(segment_start_angle, segment_length);
        } else {
          const auto& arc = segment.arc;
          Turn(segment_start_pos, segment_start_angle, arc.sweep_angle, arc.radius);
        }
        ++i;
        i_fract = 0;
      }
    } else {  // length < 0
      length += remaining_segment_length;
      if (i == 0) {
        i_fract = 0;
        segment_start_angle = arcline.start_angle;
        segment_start_pos = arcline.start;
        break;
      } else {
        i_fract = 1;
        --i;
        if (arcline.types[i] == Type::Line) {
          segment_start_pos -= Vec2::Polar(segment_start_angle, arcline.segments[i].line.length);
        } else {
          const auto& arc = arcline.segments[i].arc;
          segment_start_angle = segment_start_angle.Opposite();
          Turn(segment_start_pos, segment_start_angle, -arc.sweep_angle, -arc.radius);
          segment_start_angle = segment_start_angle.Opposite();
        }
      }
    }
  }
  return distance;
}

float ArcLine::Iterator::AdvanceToEnd() {
  float distance = 0;
  if (i_fract > 0) {
    if (arcline.types[i] == Type::Line) {
      distance += arcline.segments[i].line.length * (1 - i_fract);
    } else {
      distance += fabsf(arcline.segments[i].arc.ToRadians() * arcline.segments[i].arc.radius) *
                  (1 - i_fract);
    }
    i_fract = 0;
    ++i;
  }
  for (; i < ((I32)arcline.types.size()) - 1; ++i) {
    if (arcline.types[i] == Type::Line) {
      segment_start_pos += Vec2::Polar(segment_start_angle, arcline.segments[i].line.length);
      distance += arcline.segments[i].line.length;
    } else {
      auto& arc = arcline.segments[i].arc;
      Turn(segment_start_pos, segment_start_angle, arc.sweep_angle, arc.radius);
      distance += fabsf(arc.ToRadians() * arc.radius);
    }
  }
  if (i == arcline.types.size() - 1) {
    if (arcline.types[i] == Type::Line) {
      distance += arcline.segments[i].line.length;
    } else {
      auto& arc = arcline.segments[i].arc;
      distance += fabsf(arc.ToRadians() * arc.radius);
    }
  }
  i_fract = 1;
  return distance;
}
}  // namespace automat
