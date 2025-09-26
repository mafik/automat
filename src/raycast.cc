// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "raycast.hh"

#include <include/core/SkPath.h>
#include <modules/pathops/src/SkIntersections.h>
#include <modules/pathops/src/SkOpContour.h>

namespace automat {

std::optional<Vec2> Raycast(const SkPath& path, const Vec2AndDir& ray) {
  SkIntersections intersections;
  SkPath::Iter iter(path, false);
  SkPath::Verb verb;
  SkDLine ray_line;
  SkPoint ray_line_points[2] = {ray.pos.sk, ray.pos.sk + Vec2::Polar(ray.dir, 10000).sk};
  SkDPoint ray_line_start(ray_line_points[0].x(), ray_line_points[0].y());
  ray_line.set(ray_line_points);
  double best_dist = 10000;
  Vec2 best_point;
  while (true) {
    SkPoint points[4];
    verb = iter.next(points);
    if (verb == SkPath::kMove_Verb) {
      continue;
    } else if (SkPath::kLine_Verb == verb) {
      SkDLine line;
      line.set(points);
      if (intersections.intersectRay(line, ray_line) == 0) continue;
    } else if (SkPath::kQuad_Verb == verb) {
      SkDQuad quad;
      quad.set(points);
      if (intersections.intersectRay(quad, ray_line) == 0) continue;
    } else if (SkPath::kConic_Verb == verb) {
      SkDConic conic;
      conic.set(points, iter.conicWeight());
      if (intersections.intersectRay(conic, ray_line) == 0) continue;
    } else if (SkPath::kCubic_Verb == verb) {
      SkDCubic cubic;
      cubic.set(points);
      if (intersections.intersectRay(cubic, ray_line) == 0) continue;
    } else if (SkPath::kClose_Verb == verb) {
      continue;
    } else if (SkPath::kDone_Verb == verb) {
      break;
    } else {
      ERROR_ONCE << "Unknown verb: " << verb;
      continue;  // this can still mostly work but will not intersect unknown verbs
    }
    double best_dist_section;
    int closest_index = intersections.closestTo(0, 10000, ray_line_start, &best_dist_section);
    if (best_dist_section < best_dist) {
      best_dist = best_dist_section;
      best_point = intersections.pt(closest_index).asSkPoint();
    }
  }

  if (best_dist < 10000) {
    return best_point;
  }
  return std::nullopt;
}

}  // namespace automat