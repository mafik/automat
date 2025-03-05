// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "arcline.hh"

#include <cmath>

#include "log.hh"
#include "sincos.hh"

namespace maf {

ArcLine::ArcLine(Vec2 start, SinCos start_angle) : start(start), start_angle(start_angle) {}

ArcLine ArcLine::MakeFromPath(const SkPath& path) {
  LOG << "Converting path to ArcLine...";
  LOG_Indent();
  SkPath::Iter iter(path, true);
  Vec2 pts[4];
  Vec2 arcline_end = {0, 0};
  SinCos arcline_end_dir = 0_deg;
  ArcLine arcline(arcline_end, arcline_end_dir);
  while (true) {
    auto verb = iter.next(&pts[0].sk);
    switch (verb) {
      case SkPath::kMove_Verb:
        if (!arcline.segments.empty()) {
          ERROR << "Multi-contour path cannot be (yet) converted to ArcLine";
        }
        LOG << "Move to " << pts[0].ToStrMetric();
        arcline.types.clear();
        arcline.segments.clear();
        break;
      case SkPath::kLine_Verb: {
        LOG << "Line from " << pts[0].ToStrMetric() << " to " << pts[1].ToStrMetric();
        if (arcline.segments.empty()) {
          arcline.start = pts[0];
          Vec2 delta = pts[1] - pts[0];
          arcline.start_angle = SinCos::FromVec2(delta);
          arcline.types.push_back(Type::Line);
          arcline.segments.emplace_back(Line{.length = Length(delta)});
          arcline_end = pts[1];
          arcline_end_dir = arcline.start_angle;
        } else {
          Vec2 delta = pts[1] - pts[0];
          SinCos dir = SinCos::FromVec2(delta);
          if (arcline_end_dir != dir) {
            arcline.TurnConvex(dir - arcline_end_dir, 0);
            arcline_end_dir = dir;
          }
          arcline.MoveBy(Length(delta));
          arcline_end = pts[1];
        }
        break;
      }
      case SkPath::kQuad_Verb:
        LOG << "Unsupported verb: Quad";
        break;
      case SkPath::kCubic_Verb:
        LOG << "Unsupported verb: Cubic";
        break;
      case SkPath::kClose_Verb:
        LOG << "Close";
        if (arcline_end_dir != arcline.start_angle) {
          arcline.TurnConvex(arcline.start_angle - arcline_end_dir, 0);
          arcline_end_dir = arcline.start_angle;
        }
        break;
      case SkPath::kConic_Verb: {
        // Note: we only support sections of a circle - not arbitrary conics!
        float weight = iter.conicWeight();
        float angle_rad = acosf(weight) * 2;
        float radius = sqrt(LengthSquared(pts[0] - pts[2]) / (2 - 2 * cos(angle_rad)));
        LOG << "Conic from " << pts[0].ToStrMetric() << " to " << pts[2].ToStrMetric();
        SinCos arc_start_angle = SinCos::FromVec2(pts[1] - pts[0]);
        if (arcline.segments.empty()) {
          arcline.start = pts[0];
          arcline.start_angle = arc_start_angle;
          arcline_end = arcline.start;
          arcline_end_dir = arcline.start_angle;
        }
        if (arc_start_angle != arcline_end_dir) {
          arcline.TurnConvex(arc_start_angle - arcline_end_dir, 0);
          arcline_end_dir = arc_start_angle;
        }
        arcline.TurnConvex(SinCos::FromRadians(angle_rad), radius);
        arcline_end = pts[2];
        arcline_end_dir = SinCos::FromVec2(pts[2] - pts[1]);
        break;
      }
      case SkPath::kDone_Verb:
        LOG_Unindent();
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
  if (!types.empty() && types.back() == Type::Arc && segments.back().arc.radius == radius) {
    segments.back().arc.sweep_angle = segments.back().arc.sweep_angle + sweep_angle;
    return *this;
  }
  types.push_back(Type::Arc);
  segments.emplace_back(Arc{.radius = radius, .sweep_angle = sweep_angle});
  return *this;
}

ArcLine& ArcLine::Outset(float offset) {
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
      Turn(p, current_alpha, segments[i].arc.sweep_angle, segments[i].arc.radius);
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
}  // namespace maf