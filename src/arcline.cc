#include "arcline.hh"

#include <cmath>

#include "log.hh"

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

SkPath ArcLine::ToPath() const {
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
  path.close();
  return path;
}

}  // namespace maf