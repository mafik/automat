#include "arcline.hh"

#include <cmath>

namespace maf {

ArcLine::ArcLine(Vec2 start, float start_angle) : start(start), start_angle(start_angle) {}

ArcLine& ArcLine::MoveBy(float length) {
  types.push_back(Type::Line);
  segments.push_back({Line{length}});
  return *this;
}

// Turn a given point & angle by `sweep_angle` radians around a circle with `radius`.
static void Turn(Vec2& point, float& angle, float sweep_angle, float radius) {
  float center_angle = angle + (sweep_angle > 0 ? M_PI / 2 : -M_PI / 2);
  Vec2 center = point + Vec2::Polar(center_angle, radius);
  // TODO: there is probably some code necessary here to make sure that sweep_angle is applied
  // correctly
  point = center + Vec2::Polar(center_angle + M_PI + sweep_angle, radius);
  angle += sweep_angle;
  angle = fmod(angle + 2 * M_PI, 2 * M_PI);
}

ArcLine& ArcLine::TurnBy(float sweep_angle, float radius) {
  types.push_back(Type::Arc);
  Segment segment;
  segment.arc.radius = radius;
  segment.arc.sweep_angle = sweep_angle;
  segments.push_back(segment);
  return *this;
}

ArcLine& ArcLine::Outset(float offset) {
  start += Vec2::Polar(start_angle - M_PI / 2, offset);
  for (int i = 0; i < types.size(); ++i) {
    if (types[i] == Type::Arc) {
      auto& arc = segments[i].arc;
      float dir = arc.sweep_angle > 0 ? 1 : -1;
      arc.radius += offset * dir;
      if (arc.radius < 0) {
        float s = arc.radius;
        arc.radius = 0;
        float angle = (M_PI - fabsf(arc.sweep_angle)) / 2;
        float ctg = 1 / tan(angle);
        float delta_length = s * ctg;

        int prev_i = (i + types.size() - 1) % types.size();
        if (types[prev_i] == Type::Line) {
          segments[prev_i].line.length += delta_length;
        }
        int next_i = (i + 1) % types.size();
        if (types[next_i] == Type::Line) {
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

SkPath ArcLine::ToPath(bool close) const {
  SkPath path;
  path.moveTo(start.x, start.y);
  Vec2 p = start;
  float current_alpha = start_angle;
  for (int i = 0; i < types.size(); ++i) {
    if (types[i] == Type::Line) {
      p += Vec2::Polar(current_alpha, segments[i].line.length);
      path.lineTo(p.x, p.y);
    } else {
      float r = segments[i].arc.radius;
      float sweep_angle = segments[i].arc.sweep_angle;
      float dir = sweep_angle > 0 ? 1 : -1;
      float alpha0 = current_alpha;
      Vec2 center = p + Vec2::Polar(current_alpha + dir * M_PI / 2, r);
      Turn(p, current_alpha, sweep_angle, r);
      SkRect oval = SkRect::MakeXYWH(center.x - r, center.y - r, 2 * r, 2 * r);
      path.arcTo(oval, dir * -90 + alpha0 * 180 / M_PI, sweep_angle * 180 / M_PI, false);
    }
  }
  if (close) {
    path.close();
  }
  return path;
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
  const float delta_x = distance_sideways / 2;
  const float delta_x_abs = std::abs(delta_x);
  if (delta_x_abs < turn_radius) {
    float r_minus_x = turn_radius - delta_x_abs;
    float delta_y = sqrt(turn_radius * turn_radius - r_minus_x * r_minus_x);
    first_turn_angle = atan2(delta_y, r_minus_x);
    move_between_turns = 0;
    distance_forward = delta_y * 2;
  } else {
    first_turn_angle = M_PI / 2;
    move_between_turns = (delta_x_abs - turn_radius) * 2;
    distance_forward = turn_radius * 2;
  }
  if (delta_x < 0) {
    first_turn_angle = -first_turn_angle;
  }
}
void ArcLine::TurnShift::Apply(ArcLine& line) const {
  line.TurnBy(first_turn_angle, turn_radius);
  if (move_between_turns > 0) {
    line.MoveBy(move_between_turns);
  }
  line.TurnBy(-first_turn_angle, turn_radius);
}

float ArcLine::Iterator::Advance(float length) {
  float distance = 0;
  while (length != 0) {
    const auto& segment = arcline.segments[i];
    float segment_length = arcline.types[i] == Type::Line
                               ? segment.line.length
                               : fabsf(segment.arc.sweep_angle) * segment.arc.radius;
    float remaining_segment_length = segment_length * (length > 0 ? 1 - i_fract : i_fract);
    if (fabsf(length) < remaining_segment_length) {
      i_fract += length / segment_length;
      distance += length;
      length = 0;
      break;
    }
    if (length > 0) {
      distance += remaining_segment_length;
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
      distance -= remaining_segment_length;
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
          segment_start_angle += M_PI;
          Turn(segment_start_pos, segment_start_angle, -arc.sweep_angle, arc.radius);
          segment_start_angle -= M_PI;
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
      distance += fabsf(arcline.segments[i].arc.sweep_angle) * arcline.segments[i].arc.radius *
                  (1 - i_fract);
    }
    i_fract = 0;
    ++i;
  }
  for (; i < arcline.types.size() - 1; ++i) {
    if (arcline.types[i] == Type::Line) {
      segment_start_pos += Vec2::Polar(segment_start_angle, arcline.segments[i].line.length);
      distance += arcline.segments[i].line.length;
    } else {
      float r = arcline.segments[i].arc.radius;
      float sweep_angle = arcline.segments[i].arc.sweep_angle;
      float dir = sweep_angle > 0 ? 1 : -1;
      float alpha0 = segment_start_angle;
      Vec2 center = segment_start_pos + Vec2::Polar(segment_start_angle + dir * M_PI / 2, r);
      Turn(segment_start_pos, segment_start_angle, sweep_angle, r);
      distance += fabsf(sweep_angle) * r;
    }
  }
  i_fract = 1;
  return distance;
}
}  // namespace maf