// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "connector_optical.hh"

#include <include/core/SkBlendMode.h>
#include <include/core/SkBlurTypes.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkColorFilter.h>
#include <include/core/SkDrawable.h>
#include <include/core/SkImage.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMesh.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkVertices.h>
#include <include/effects/SkGradientShader.h>
#include <include/effects/SkRuntimeEffect.h>

#include <cmath>
#include <memory>
#include <numbers>

#include "../build/generated/embedded.hh"
#include "arcline.hh"
#include "argument.hh"
#include "color.hh"
#include "font.hh"
#include "global_resources.hh"
#include "log.hh"
#include "math.hh"
#include "sincos.hh"
#include "svg.hh"
#include "textures.hh"

using namespace maf;

namespace automat::gui {

constexpr bool kDebugCable = false;
constexpr float kCasingWidth = 8_mm;
constexpr float kCasingHeight = 8_mm;
constexpr float kStep = 5_mm;
constexpr float kCrossSize = 0.001;
constexpr SkColor kRoutingDebugColor = "#28387f"_color;

// The job of RouteCable is to find a visually pleasing path from the given start (point &
// direction) to the given end point. End point is assumed to always point down (could change in the
// future).
//
// Current algorithms:
//
// 1. For the case with connections going down
//    - go horizontally down (as much as possible)
//    - turn sideways (left or right) to compensate horizontal distance between start & end
//    - ONLY IF there is not enough vertical space - turn vertically (up or down) to compensate for
//    the vertical distance between start & end
//    - complete the path symmetrically
// 2. For the case with connections going down where the first algorithm fails
//    - turn 180 degrees, away from the end point
//    - go up, to match the Y position of the end point
//    - turn 90 degree towards the end point
//    - go horizontally to the end point X
//    - final 90 degree turn to arrive at the end point
// 3. For the case with connections starting in arbitrary directions
//    - imagine pairs of circles touching start & end points tangent to the direction of the cable
//    - follow the circumference of the circle until a straight line can be drawn to the next circle
//    - follow the circumference of the target circle until the end point is reached
// 4. Hypothetical approach for the case where the lines coming from the start & end points
// intersect at a convenient point (after start, before end) (maybe TODO)
//    - go straight to the intersection point
//    - turn towards the end point
//    - go straight to the end point

static ArcLine RouteCableDown(Vec2AndDir start, Vec2 cable_end) {
  ArcLine cable = ArcLine(start.pos, -90_deg);
  Vec2 delta = cable_end - start.pos;
  float distance = Length(delta);
  float turn_radius = std::max<float>(distance / 8, 0.01);

  if (fabsf(delta.x) < 1e-7f) {
    delta.x = 0.f;
  }

  auto horizontal_shift = ArcLine::TurnShift(delta.x, turn_radius);
  float move_down = (-delta.y - horizontal_shift.distance_forward) / 2;
  if (move_down < 0) {
    // Increase the turn radius of the vertical move to allow ∞-type routing
    float vertical_turn_radius = std::max(turn_radius, horizontal_shift.move_between_turns * 0.5f);
    auto vertical_shift = ArcLine::TurnShift(
        cable_end.x < start.pos.x ? move_down * 2 : -move_down * 2, vertical_turn_radius);

    float move_side = (horizontal_shift.move_between_turns - vertical_shift.distance_forward) / 2;
    if (move_side < 0) {
      // If there is not enough space to route the cable in the middle, we will route it around the
      // objects.
      float x = start.pos.x;
      float y = start.pos.y;
      float dir;
      if (start.pos.x > cable_end.x) {
        dir = 1;
      } else {
        dir = -1;
      }
      cable.TurnConvex(90_deg * dir, turn_radius);
      x += turn_radius * dir;
      y += turn_radius;
      cable.TurnConvex(90_deg * dir, turn_radius);
      x += turn_radius * dir;
      y -= turn_radius;
      float move_up = cable_end.y - y;
      float move_down = -move_up;
      if (move_up > 0) {
        cable.MoveBy(move_up);
      }
      y = cable_end.y;
      cable.TurnConvex(90_deg * dir, turn_radius);
      x -= turn_radius * dir;
      y -= turn_radius;
      cable.MoveBy(dir * (x - cable_end.x) - turn_radius);
      cable.TurnConvex(90_deg * dir, turn_radius);
      if (move_down > 0) {
        cable.MoveBy(move_down);
      }
    } else {
      cable.TurnConvex(horizontal_shift.first_turn_angle, turn_radius);
      if (move_side > 0) {
        cable.MoveBy(move_side);
      }
      vertical_shift.Apply(cable);
      if (move_side > 0) {
        cable.MoveBy(move_side);
      }
      cable.TurnConvex(-horizontal_shift.first_turn_angle, turn_radius);
    }
  } else {
    if (move_down > 0) {
      cable.MoveBy(move_down);
    }
    horizontal_shift.Apply(cable);
    if (move_down > 0) {
      cable.MoveBy(move_down);
    }
  }
  return cable;
}

static ArcLine RoutCableStraight(Vec2AndDir start, Vec2AndDir end,
                                 SkCanvas* debug_canvas = nullptr) {
  float radius = 1_cm;
  ArcLine cable = ArcLine(start.pos, start.dir);

  SinCos best_start_turn;
  SinCos best_end_turn;
  float best_line_length;
  float best_total_length = HUGE_VALF;
  float best_start_radius;
  float best_end_radius;

  for (bool start_left : {false, true}) {
    Vec2 start_circle_center =
        start.pos + Vec2::Polar(start.dir + (start_left ? 90_deg : -90_deg), radius);
    if constexpr (kDebugCable) {
      SkPaint circle_paint;
      circle_paint.setStyle(SkPaint::kStroke_Style);
      circle_paint.setColor(kRoutingDebugColor);
      if (debug_canvas) debug_canvas->drawCircle(start_circle_center, radius, circle_paint);
    }
    for (bool end_left : {false, true}) {
      Vec2 end_circle_center =
          end.pos + Vec2::Polar(end.dir + (end_left ? 90_deg : -90_deg), radius);
      if constexpr (kDebugCable) {
        SkPaint circle_paint;
        circle_paint.setStyle(SkPaint::kStroke_Style);
        circle_paint.setColor(kRoutingDebugColor);
        if (debug_canvas) debug_canvas->drawCircle(end_circle_center, radius, circle_paint);
      }
      Vec2 circle_diff = end_circle_center - start_circle_center;
      float circle_dist = Length(circle_diff);
      SinCos circle_angle = SinCos::FromVec2(circle_diff, circle_dist);
      SinCos line_dir;
      float line_length;
      if (start_left == end_left) {
        line_dir = circle_angle;
        line_length = circle_dist;
      } else if (circle_dist > 2 * radius) {
        line_length = sqrt(circle_dist * circle_dist - radius * radius * 4);
        line_dir = circle_angle +
                   SinCos::FromRadians(acosf(line_length / circle_dist) * (start_left ? 1 : -1));
      } else {
        continue;
      }

      SinCos start_turn = line_dir - start.dir;
      SinCos end_turn = end.dir - line_dir;

      float total_length =
          fabsf(start_left ? start_turn.ToRadiansPositive() : start_turn.ToRadiansNegative()) *
              radius +
          line_length +
          fabsf(end_left ? end_turn.ToRadiansPositive() : end_turn.ToRadiansNegative()) * radius;
      if (end_left) {
        total_length += 0.0001f;
      }
      if (total_length < best_total_length) {
        best_total_length = total_length;
        best_start_turn = start_turn;
        best_end_turn = end_turn;
        best_line_length = line_length;
        best_start_radius = start_left ? radius : -radius;
        best_end_radius = end_left ? radius : -radius;
      }
    }
  }

  cable.TurnBy(best_start_turn, best_start_radius);
  cable.MoveBy(best_line_length);
  cable.TurnBy(best_end_turn, best_end_radius);
  return cable;
}

static ArcLine RouteCableOneEnd(Vec2AndDir start, Vec2AndDir end,
                                SkCanvas* debug_canvas = nullptr) {
  if (start.dir == -90_deg && end.dir == -90_deg) {
    return RouteCableDown(start, end.pos);
  } else {
    return RoutCableStraight(start, end, debug_canvas);
  }
}

ArcLine RouteCable(Vec2AndDir start, const maf::Span<const Vec2AndDir> cable_ends,
                   SkCanvas* debug_canvas) {
  float best_total_length = HUGE_VALF;
  ArcLine best_route = ArcLine(start.pos, start.dir);
  for (auto& end : cable_ends) {
    ArcLine current = RouteCableOneEnd(start, end, debug_canvas);
    float current_length = ArcLine::Iterator(current).AdvanceToEnd();
    if (current_length < best_total_length) {
      best_total_length = current_length;
      best_route = current;
    }
  }
  return best_route;
}

// This function walks along the given arcline (from the end to its start) and adds
// an anchor every kStep distance. It populates the `anchors` and `anchor_tangents` vectors.
static void PopulateAnchors(Vec<Vec2>& anchors, Vec<SinCos>& anchor_dir, const ArcLine& arcline) {
  auto it = ArcLine::Iterator(arcline);
  Vec2 dispenser = it.Position();
  float cable_length = it.AdvanceToEnd();
  Vec2 tail = it.Position();

  anchors.push_back(tail);
  anchor_dir.push_back(it.Angle().Opposite());
  for (float cable_pos = kStep; cable_pos < cable_length - 1_mm; cable_pos += kStep) {
    it.Advance(-kStep);
    anchors.push_back(it.Position());
    anchor_dir.push_back(it.Angle().Opposite());
  }
  anchors.push_back(dispenser);
  anchor_dir.push_back(it.Angle().Opposite());
}

// Simulate the dispenser pulling in the cable. This function may remove some of the cable segments
// but will always leave at least two - starting & ending points.
//
// Returns true if the dispenser is active and pulling the cable in.
//
// WARNING: this function will adjust the length of the final cable segment (the one closest to the
// dispenser). Don't change it or visual glitches will occur.
static bool SimulateDispenser(CablePhysicsSimulation& state, float dt, Size anchor_count) {
  bool pulling = anchor_count < state.sections.size();
  if (pulling) {
    state.dispenser_v += 5e-1 * dt;
    state.dispenser_v *= expf(-1 * dt);  // Limit the maximum speed
    float retract = state.dispenser_v * dt;
    // Shorten the final link by pulling it towards the dispenser
    float total_dist = 0;
    int i = state.sections.size() - 2;
    for (; i >= 0; --i) {
      total_dist += state.sections[i].distance;
      if (total_dist > retract) {
        break;
      }
    }
    if ((i < 0) || (retract > total_dist)) {
      i = 0;
      retract = total_dist;
    }
    for (int j = state.sections.size() - 2; j > i; --j) {
      state.sections.EraseIndex(j);
    }
    float remaining = total_dist - retract;
    if (total_dist > 0 && remaining == 0) {
      audio::Play(embedded::assets_SFX_cable_click_wav);
    }
    // Move chain[i] to |remaining| distance from dispenser
    state.sections[i].distance = remaining;
  } else {
    state.dispenser_v = 0;
    do {  // Add a new link if the last one is too far from the dispenser
      auto delta = state.sections[state.sections.size() - 2].pos - state.sections.back().pos;
      auto current_dist = Length(delta);
      float extend_threshold = kStep + state.cable_width / 2;
      if (current_dist > extend_threshold) {
        state.sections[state.sections.size() - 2].distance = kStep;
        auto new_it = state.sections.insert(
            state.sections.begin() + state.sections.size() - 1,
            CablePhysicsSimulation::CableSection{
                .pos = state.sections[state.sections.size() - 2].pos -
                       Vec2::Polar(state.sections.back().dir, state.cable_width / 2) -
                       delta / current_dist * kStep,
                .vel = Vec2(0, 0),
                .acc = Vec2(0, 0),
                .distance = current_dist - kStep,
            });
      } else if (state.sections.size() < anchor_count) {
        auto new_it = state.sections.insert(
            state.sections.begin() + state.sections.size() - 1,
            CablePhysicsSimulation::CableSection{
                .pos = state.sections.back().pos -
                       Vec2::Polar(state.sections.back().dir, state.cable_width / 2),
                .vel = Vec2(0, 0),
                .acc = Vec2(0, 0),
                .distance = state.cable_width / 2,
            });
        break;
      } else {
        break;
      }
    } while (state.sections.size() < anchor_count);
  }

  return pulling;
}

animation::Phase SimulateCablePhysics(time::Timer& timer, CablePhysicsSimulation& state,
                                      Vec2AndDir dispenser, maf::Span<Vec2AndDir> end_candidates) {
  SkCanvas* debug_canvas = nullptr;
  if constexpr (kDebugCable) {
    // TODO: actually display this recording
    SkPictureRecorder recorder;
    debug_canvas = recorder.beginRecording(SkRect::MakeXYWH(-50_cm, -50_cm, 100_cm, 100_cm));

    // Draw the end candidates
    SkPaint end_paint;
    end_paint.setStyle(SkPaint::kStroke_Style);
    end_paint.setStrokeWidth(1_mm);
    end_paint.setColor(kRoutingDebugColor);
    SkPaint circle_paint;
    circle_paint.setStyle(SkPaint::kFill_Style);
    circle_paint.setColor(kRoutingDebugColor);
    for (auto& end : end_candidates) {
      debug_canvas->drawLine(end.pos, end.pos + Vec2::Polar(end.dir, 2_mm), end_paint);
      // Now let's draw a circle at the end point
      debug_canvas->drawCircle(end.pos, 1_mm, circle_paint);
    }
  }

  animation::Phase phase = animation::Finished;
  float dt = timer.d;
  if (&state.arg == &next_arg) {
    state.lightness_pct = 100;
    phase |= animation::ExponentialApproach(0, (timer.now - state.location.last_finished).count(),
                                            0.1, state.lightness_pct);
  } else {
    state.lightness_pct = state.arg.IsOn(state.location) ? 100 : 0;
  }

  for (auto& end : end_candidates) {
    end.pos -= Vec2::Polar(end.dir, kCasingHeight * state.connector_scale);
  }

  bool simulate_physics = true;

  Optional<Vec2> cable_end;
  SinCos cable_end_dir;
  for (auto& end : end_candidates) {
    cable_end = end.pos;
    if (state.stabilized && Length(dispenser.pos - state.stabilized_start) < 0.0001) {
      if (cable_end.has_value() == state.stabilized_end.has_value() &&
          (!cable_end.has_value() || Length(*cable_end - *state.stabilized_end) < 0.0001)) {
        simulate_physics = false;
      }
    }
  }
  if (end_candidates.empty() && state.stabilized && !state.stabilized_end.has_value()) {
    simulate_physics = false;
  }

  if (simulate_physics) {
    phase |= animation::Animating;
  } else {
    return phase;
  }

  if (!end_candidates.empty()) {  // Create the arcline & pull the cable towards it
    state.arcline = RouteCable(dispenser, end_candidates, nullptr);
    ArcLine::Iterator it = *state.arcline;
    it.AdvanceToEnd();
    cable_end = it.Position();
    cable_end_dir = it.Angle();
  } else {
    state.arcline.reset();
    cable_end.reset();
  }

  auto& chain = state.sections;
  if (cable_end) {
    chain.front().pos = *cable_end;
  }
  chain.back().pos = dispenser.pos;

  Vec<Vec2> anchors;
  Vec<SinCos> true_anchor_dir;
  if (state.arcline) {
    PopulateAnchors(anchors, true_anchor_dir, *state.arcline);
  }

  for (auto& link : chain) {
    link.acc = Vec2(0, 0);
  }

  // Dispenser pulling the chain in. The chain is pulled in when there are fewer anchors than cable
  // segments.
  bool dispenser_active = SimulateDispenser(state, dt, anchors.size());

  SinCos numerical_anchor_dir[anchors.size()];

  int anchor_i[chain.size()];  // Index of the anchor that the chain link is attached to

  // Match cable sections to anchors.
  // Sometimes there is more sections than anchors and sometimes there are more anchors than
  // sections. A cable section that doesn't have a matching anchor will be set to -1.
  for (int i = 0; i < chain.size(); ++i) {
    if (i == chain.size() - 1) {
      anchor_i[i] = anchors.size() - 1;  // This also handles the case when there are no anchors
    } else if (i >= ((int)anchors.size()) - 1) {
      anchor_i[i] = -1;
    } else {
      anchor_i[i] = i;
    }
  }

  // Move chain links towards anchors (more at the end of the cable)
  for (int i = 0; i < chain.size(); i++) {
    int ai = anchor_i[i];
    if (ai == -1) {
      continue;
    }
    // LERP the cable section towards its anchor point. More at the end of the cable.
    float time_factor = -expm1f(-dt * 60.0f);                  // approach 1 as dt -> infinity
    float offset_factor = std::max<float>(0, 1 - ai / 10.0f);  // 1 near the plug and falling to 0
    Vec2 new_pos = chain[i].pos + (anchors[ai] - chain[i].pos) * time_factor * offset_factor;
    chain[i].vel += (new_pos - chain[i].pos) / dt;
    chain[i].pos = new_pos;

    // Also apply a force towards the anchor. This is unrelated to LERP-ing above.
    chain[i].acc += (anchors[ai] - chain[i].pos) * 3e2;
  }

  constexpr float kDistanceEpsilon = 1e-6;
  auto last_two_pos_diff = chain[chain.size() - 1].pos - chain[chain.size() - 2].pos;
  auto last_two_pos_diff_len = Length(last_two_pos_diff);
  if (last_two_pos_diff_len > kDistanceEpsilon &&
      chain[chain.size() - 2].distance > kDistanceEpsilon) {
    chain[chain.size() - 1].dir = SinCos::FromVec2(last_two_pos_diff, last_two_pos_diff_len);
  } else {
    chain[chain.size() - 1].dir = dispenser.dir.Opposite();
  }
  if (Length(chain[1].pos - chain[0].pos) > kDistanceEpsilon &&
      chain[0].distance > kDistanceEpsilon) {
    chain[0].dir = SinCos::FromVec2(chain[1].pos - chain[0].pos);
  } else {
    chain[0].dir = dispenser.dir.Opposite();
  }
  for (int i = 1; i < chain.size() - 1; i++) {
    chain[i].dir = SinCos::FromVec2(chain[i + 1].pos - chain[i - 1].pos);
  }

  // Copy over the alignment of the anchors to the chain links.
  float total_anchor_distance = 0;
  for (int i = 0; i < chain.size(); ++i) {
    int ai = anchor_i[i];
    int prev_ai = i > 0 ? anchor_i[i - 1] : -1;
    int next_ai = i < chain.size() - 1 ? anchor_i[i + 1] : -1;

    if (ai != -1 && prev_ai != -1 && next_ai != -1) {
      numerical_anchor_dir[ai] = SinCos::FromVec2(anchors[next_ai] - anchors[prev_ai]);
    } else if (ai != -1 && prev_ai != -1) {
      numerical_anchor_dir[ai] = SinCos::FromVec2(anchors[ai] - anchors[prev_ai]);
    } else if (ai != -1 && next_ai != -1) {
      numerical_anchor_dir[ai] = SinCos::FromVec2(anchors[next_ai] - anchors[ai]);
    } else if (ai != -1) {
      numerical_anchor_dir[ai] = 90_deg;
    }
    SinCos true_dir_offset;
    if (ai != -1) {
      float distance_mm = Length(anchors[ai] - chain[i].pos) * 1000;
      total_anchor_distance += distance_mm;
      true_dir_offset = true_anchor_dir[ai] - chain[i].dir;
      true_dir_offset = true_dir_offset * (1.f - std::min<float>(distance_mm, 1));
      chain[i].true_dir_offset = true_dir_offset;
    } else {
      chain[i].true_dir_offset = chain[i].true_dir_offset * expf(-dt * 10);
    }
    if (ai != -1 && prev_ai != -1) {
      chain[i].prev_dir_delta =
          SinCos::FromVec2(anchors[prev_ai] - anchors[ai]) - numerical_anchor_dir[ai];
    } else {
      chain[i].prev_dir_delta = 180_deg;
    }
    if (ai != -1 && next_ai != -1) {
      chain[i].next_dir_delta =
          SinCos::FromVec2(anchors[next_ai] - anchors[ai]) - numerical_anchor_dir[ai];
    } else {
      chain[i].next_dir_delta = 0_deg;
    }
    if (dispenser_active && i == chain.size() - 2) {
      // pass
    } else {
      if (ai != -1 && next_ai != -1) {
        chain[i].distance = Length(anchors[next_ai] - anchors[ai]);
      } else {
        // Smoothly fade the distance to kStep
        float alpha = expf(-dt * 1);
        chain[i].distance = chain[i].distance * alpha + kStep * (1 - alpha);
      }
    }
  }
  if (cable_end) {
    chain.front().true_dir_offset = cable_end_dir.Opposite() - chain.front().dir;
  }
  chain.back().true_dir_offset = dispenser.dir.Opposite() - chain.back().dir;

  if (anchors.empty()) {
    state.stabilized = chain.size() == 2 && Length(chain[0].pos - chain[1].pos) < 0.0001;
  } else {
    float average_anchor_distance = total_anchor_distance / anchors.size();
    state.stabilized = average_anchor_distance < 0.1 && chain.size() == anchors.size();
  }
  if (state.stabilized) {
    state.stabilized_start = dispenser.pos;
    if (cable_end) {
      state.stabilized_end = *cable_end;
    } else {
      state.stabilized_end.reset();
      chain.front().true_dir_offset = 0_deg;
    }
  }

  for (int i = 0; i < chain.size() - 1; ++i) {
    chain[i].vel += chain[i].acc * dt;
  }

  {                      // Friction
    int friction_i = 0;  // Skip segment 0 (always attached to mouse)
    // Segments that have anchors have higher friction
    auto n_high_friction = std::min<int>(chain.size() - 1, anchors.size());
    for (; friction_i < n_high_friction; ++friction_i) {
      chain[friction_i].vel *= expf(-20 * dt);
    }
    // Segments without anchors are more free to move
    for (; friction_i < chain.size(); ++friction_i) {
      chain[friction_i].vel *= expf(-2 * dt);
    }
    if (cable_end.has_value()) {
      chain.front().vel = Vec2(0, 0);
    }
  }

  for (int i = 0; i < chain.size() - 1; ++i) {
    chain[i].pos += chain[i].vel * dt;
  }

  if (true) {  // Inverse kinematics solver
    bool distance_only = anchors.empty();
    for (int iter = 0; iter < 6; ++iter) {
      if (cable_end) {
        chain.front().pos = *cable_end;
      }
      chain.back().pos = dispenser.pos;
      chain.back().distance = kStep;

      CablePhysicsSimulation::CableSection cN;
      cN.pos = chain.back().pos + Vec2::Polar(chain.back().dir, kStep);

      int start = 1;
      int end = chain.size();
      int inc = 1;
      if (iter % 2) {
        start = chain.size() - 1;
        end = 0;
        inc = -1;
      }

      for (int i = start; i != end; i += inc) {
        auto& a = chain[i - 1];
        auto& b = chain[i];
        auto& c = i == chain.size() - 1 ? cN : chain[i + 1];

        Vec2 middle_pre_fix = (a.pos + b.pos + c.pos) / 3;

        SinCos a_dir_offset = b.prev_dir_delta;
        SinCos c_dir_offset = b.next_dir_delta;
        Vec2 a_target = b.pos + Vec2::Polar(chain[i].dir + a_dir_offset, a.distance);
        Vec2 c_target = b.pos + Vec2::Polar(chain[i].dir + c_dir_offset, b.distance);

        if (distance_only) {
          Vec2 ab = a.pos - b.pos;
          float l_ab = std::max<float>(1e-9, Length(ab));
          a_target = b.pos + ab / l_ab * a.distance;
          Vec2 bc = c.pos - b.pos;
          float l_bc = std::max<float>(1e-9, Length(bc));
          c_target = b.pos + bc / l_bc * b.distance;
        }

        float alpha = 0.4;
        Vec2 a_new = a.pos + (a_target - a.pos) * alpha;
        Vec2 c_new = c.pos + (c_target - c.pos) * alpha;

        Vec2 middle_post_fix = (a_new + b.pos + c_new) / 3;

        Vec2 correction = middle_pre_fix - middle_post_fix;

        a_new += correction;
        Vec2 b_new = b.pos + correction;
        c_new += correction;

        a.vel += (a_new - a.pos) / dt;
        b.vel += (b_new - b.pos) / dt;
        c.vel += (c_new - c.pos) / dt;
        a.pos = a_new;
        b.pos = b_new;
        c.pos = c_new;
      }
      if (cable_end) {
        chain.front().pos = *cable_end;
      }
      chain.back().pos = dispenser.pos;
    }
  }
  return phase;
}

Vec2 CablePhysicsSimulation::PlugTopCenter() const { return sections.front().pos; }

SkMatrix CablePhysicsSimulation::ConnectorMatrix() const {
  Vec2 pos = sections.front().pos;
  SinCos dir = sections.front().dir + sections.front().true_dir_offset - 90_deg;
  return dir.ToMatrix()
      .postTranslate(pos.x, pos.y)
      .preScale(connector_scale, connector_scale)
      .preTranslate(0, -kCasingHeight);
}

static const Rect kSteelRect = Rect(-3_mm, -1_mm, 3_mm, 1_mm);

SkPath CablePhysicsSimulation::Shape() const {
  auto rect = Rect(-kCasingWidth / 2, 0, kCasingWidth / 2, kCasingHeight);
  SkPath path = SkPath::Rect(rect);
  path.addRect(kSteelRect.sk.makeOffset(0, 2_mm * steel_insert_hidden));
  path.transform(ConnectorMatrix());
  return path;
}

static SkPoint conic(SkPoint p0, SkPoint p1, SkPoint p2, float w, float t) {
  float s = 1 - t;
  return {((s * s * p0.x()) + (2 * s * t * w * p1.x()) + (t * t * p2.x())) /
              ((s * s) + (w * 2 * s * t) + (t * t)),
          ((s * s * p0.y()) + (2 * s * t * w * p1.y()) + (t * t * p2.y())) /
              ((s * s) + (w * 2 * s * t) + (t * t))};
}

static SkPoint conic_tangent(SkPoint p0, SkPoint p1, SkPoint p2, float w, float t) {
  float s = 1 - t;
  float denominator = powf(-2 * (w - 1) * t * t + 2 * (w - 1) * t + 1, 2);
  float w0 = -2 * s * (t * (w - 1) - w) / denominator;
  float w1 = 2 * w * (2 * t - 1) / denominator;
  float w2 = 2 * t * (-w * t + t - 1) / denominator;
  return {p0.x() * w0 + p1.x() * w1 + p2.x() * w2, p0.y() * w0 + p1.y() * w1 + p2.y() * w2};
}

struct StrokeToMesh {
  struct VertexInfo {
    Vec2 coords;
    Vec2 uv;
    Vec2 tangent;
  } __attribute__((packed));

  Vec<VertexInfo> vertex_vector;
  Rect bounds = Rect(HUGE_VALF, HUGE_VALF, -HUGE_VALF, -HUGE_VALF);
  float length = 0;

  static const SkMeshSpecification::Attribute kAttributes[3];
  static const SkMeshSpecification::Varying kVaryings[3];

  virtual float GetWidth() const = 0;
  virtual bool IsConstantWidth() const { return false; }

  void Convert(const SkPath& path, float length_limit = HUGE_VALF) {
    SkPath::Iter iter(path, false);
    SkPath::Verb verb;
    do {
      SkPoint points[4];
      verb = iter.next(points);
      if (SkPath::kConic_Verb == verb) {
        float weight = iter.conicWeight();
        float angle = acosf(weight) * 2 * 180 / M_PI;
        int n_steps = ceil(angle * 2 / 5);
        Vec2 last_point = points[0];
        for (int step = 0; step <= n_steps; step++) {
          float t = (float)step / n_steps;
          Vec2 point = conic(points[0], points[1], points[2], weight, t);
          float delta_length = Length(point - last_point);
          bool limit_reached = false;
          if (length + delta_length >= length_limit) {
            t = (float)(step - 1 + (length_limit - length) / delta_length) / n_steps;
            point = conic(points[0], points[1], points[2], weight, t);
            length = length_limit;
            limit_reached = true;
          } else {
            length += delta_length;
          }
          Vec2 tangent = -conic_tangent(points[0], points[1], points[2], weight, t);
          Vec2 normal = Rotate90DegreesClockwise(tangent) * GetWidth() / 2 / Length(tangent);
          last_point = point;
          Vec2 left = point - normal;
          Vec2 right = point + normal;
          bounds.ExpandToInclude(left);
          bounds.ExpandToInclude(right);
          vertex_vector.push_back({
              .coords = left,
              .uv = Vec2(-1, length),
              .tangent = tangent,
          });
          vertex_vector.push_back({
              .coords = right,
              .uv = Vec2(1, length),
              .tangent = tangent,
          });
          if (limit_reached) {
            return;
          }
        }

      } else if (SkPath::kMove_Verb == verb) {
        // pass
      } else if (SkPath::kLine_Verb == verb) {
        Vec2 diff = points[1] - points[0];
        float segment_length = Length(diff);
        diff = diff / std::max(segment_length, 0.00001f);

        int n_steps = IsConstantWidth() ? 1 : std::max<int>(1, ceil(segment_length / 0.25_mm));
        for (int step = 0; step <= n_steps; ++step) {
          float t = (float)step / n_steps;

          float delta_length = step ? segment_length / n_steps : 0;
          bool limit_reached = false;
          if (length + delta_length >= length_limit) {
            t = (float)(step - 1 + (length_limit - length) / delta_length) / n_steps;
            length = length_limit;
            limit_reached = true;
          } else {
            length += delta_length;
          }

          Vec2 point = points[0] * (1 - t) + points[1] * t;
          Vec2 normal = Rotate90DegreesClockwise(diff) * GetWidth() / 2;
          Vec2 left = point - normal;
          Vec2 right = point + normal;
          bounds.ExpandToInclude(left);
          bounds.ExpandToInclude(right);
          vertex_vector.push_back({
              .coords = left,
              .uv = Vec2(-1, length),
              .tangent = diff,
          });
          vertex_vector.push_back({
              .coords = right,
              .uv = Vec2(1, length),
              .tangent = diff,
          });
          if (limit_reached) {
            return;
          }
        }
      } else if (SkPath::kCubic_Verb == verb) {
        Vec2 p0 = points[0];
        Vec2 p1 = points[1];
        Vec2 p2 = points[2];
        Vec2 p3 = points[3];
        constexpr int n_steps = 8;
        Vec2 last_point = p0;
        for (int step = 0; step <= n_steps; step++) {
          float t = (float)step / n_steps;
          Vec2 point = p0 * powf(1 - t, 3) + p1 * 3 * powf(1 - t, 2) * t +
                       p2 * 3 * (1 - t) * t * t + p3 * powf(t, 3);

          float delta_length = Length(point - last_point);
          bool limit_reached = false;
          if (length + delta_length >= length_limit) {
            t = (float)(step - 1 + (length_limit - length) / delta_length) / n_steps;
            point = p0 * powf(1 - t, 3) + p1 * 3 * powf(1 - t, 2) * t + p2 * 3 * (1 - t) * t * t +
                    p3 * powf(t, 3);
            length = length_limit;
            limit_reached = true;
          } else {
            length += delta_length;
          }

          Vec2 tangent = p0 * -3 * powf(1 - t, 2) + p1 * (3 * powf(1 - t, 2) - 6 * t * (1 - t)) +
                         p2 * (6 * t * (1 - t) - 3 * t * t) + p3 * 3 * powf(t, 2);
          Vec2 normal = Rotate90DegreesClockwise(tangent) * GetWidth() / 2 / Length(tangent);
          last_point = point;
          Vec2 left = point - normal;
          Vec2 right = point + normal;
          bounds.ExpandToInclude(left);
          bounds.ExpandToInclude(right);
          vertex_vector.push_back({
              .coords = left,
              .uv = Vec2(-1, length),
              .tangent = tangent,
          });
          vertex_vector.push_back({
              .coords = right,
              .uv = Vec2(1, length),
              .tangent = tangent,
          });
          if (limit_reached) {
            return;
          }
        }
      }
    } while (SkPath::kDone_Verb != verb);
  }

  sk_sp<SkMesh::VertexBuffer> BuildBuffer() {
    return SkMeshes::MakeVertexBuffer(vertex_vector.data(),
                                      vertex_vector.size() * sizeof(VertexInfo));
  }
};

const SkMeshSpecification::Attribute StrokeToMesh::kAttributes[3] = {
    {
        .type = SkMeshSpecification::Attribute::Type::kFloat2,
        .offset = 0,
        .name = SkString("position"),
    },
    {
        .type = SkMeshSpecification::Attribute::Type::kFloat2,
        .offset = 8,
        .name = SkString("uv"),
    },
    {
        .type = SkMeshSpecification::Attribute::Type::kFloat2,
        .offset = 16,
        .name = SkString("tangent"),
    },
};

const SkMeshSpecification::Varying StrokeToMesh::kVaryings[3] = {
    {
        .type = SkMeshSpecification::Varying::Type::kFloat2,
        .name = SkString("position"),
    },
    {
        .type = SkMeshSpecification::Varying::Type::kFloat2,
        .name = SkString("uv"),
    },
    {
        .type = SkMeshSpecification::Varying::Type::kFloat2,
        .name = SkString("tangent"),
    }};

struct StrokeToCable : StrokeToMesh {
  float start_offset = 0;
  float end_offset = 20_mm;  // default value shouold be overriden!
  float start_width = 2_mm;
  float end_width = 2_mm;

  float GetWidth() const override {
    if (start_width == end_width) {
      return start_width;
    } else {
      float a = std::clamp<float>((length - start_offset) / (end_offset - start_offset), 0, 1) *
                std::numbers::pi;    // scale position to [0, pi]
      float t = cos(a) * 0.5 + 0.5;  // map cos to [0, 1] range
      return t * start_width + (1 - t) * end_width;
    }
  }
  bool IsConstantWidth() const override { return start_width == end_width; }
};

void DrawCable(SkCanvas& canvas, SkPath& path, sk_sp<SkColorFilter>& color_filter,
               CableTexture texture, float start_width, float end_width, float* length_cache) {
  Rect clip = canvas.getLocalClipBounds();
  float max_width = std::max(start_width, end_width);
  Rect path_bounds = path.getBounds().makeOutset(max_width / 2, max_width / 2);
  if (!clip.sk.intersects(path_bounds.sk)) {
    return;
  }
  // TODO: adjust the tesselation density based on the zoom level

  static sk_sp<SkMeshSpecification>& mesh_specification = []() -> sk_sp<SkMeshSpecification>& {
    auto vs = SkString(R"(
      Varyings main(const Attributes attrs) {
        Varyings v;
        v.position = attrs.position;
        v.uv = attrs.uv;
        v.tangent = normalize(attrs.tangent);
        return v;
      }
    )");
    auto fs = SkString(R"(
      const float PI = 3.1415926535897932384626433832795;

      uniform float cable_width;

      uniform shader cable_color;
      uniform shader cable_normal;

      float3x3 transpose3x3(in float3x3 inMatrix) {
          float3 i0 = inMatrix[0];
          float3 i1 = inMatrix[1];
          float3 i2 = inMatrix[2];

          float3x3 outMatrix = float3x3(
                      float3(i0.x, i1.x, i2.x),
                      float3(i0.y, i1.y, i2.y),
                      float3(i0.z, i1.z, i2.z)
                      );

          return outMatrix;
      }

      float2 main(const Varyings v, out float4 color) {
        vec3 lightDir = normalize(vec3(0, 1, 1)); // normalized vector pointing from current fragment towards the light
        float h = sqrt(1 - v.uv.x * v.uv.x );
        float angle = acos(v.uv.x);

        vec3 T = vec3(normalize(v.tangent), 0);
        vec3 N = normalize(vec3(v.uv.x * T.y, -v.uv.x * T.x, h));
        vec3 B = cross(T, N);
        float3x3 TBN = float3x3(T, B, N);
        float3x3 TBN_inv = transpose3x3(TBN);

        vec2 texCoord = vec2(-angle / PI, v.uv.y / cable_width / 2) * 512;

        vec3 normalTanSpace = normalize(cable_normal.eval(texCoord).yxz * 2 - 1 + vec3(0, 0, 0.5)); // already in tangent space
        normalTanSpace.x = -normalTanSpace.x;
        vec3 lightDirTanSpace = normalize(TBN_inv * lightDir);
        vec3 viewDirTanSpace = normalize(TBN_inv * vec3(0, 0, 1));

        vec3 normal = normalize(TBN * normalTanSpace);

        color.rgba = cable_color.eval(texCoord).rgba;
        float light = min(1, 0.2 + max(dot(normalTanSpace, lightDirTanSpace), 0));
        color.rgb = light * color.rgb;

        color.rgb += pow(length(normal.xy), 8) * vec3(0.9, 0.9, 0.9) * 0.5; // rim lighting

        color.rgb += pow(max(dot(reflect(-lightDirTanSpace, normalTanSpace), viewDirTanSpace), 0), 10) * vec3(0.4, 0.4, 0.35);
        return v.position;
      }
    )");
    auto spec_result =
        SkMeshSpecification::Make(StrokeToMesh::kAttributes, sizeof(StrokeToMesh::VertexInfo),
                                  StrokeToMesh::kVaryings, vs, fs);

    if (!spec_result.error.isEmpty()) {
      ERROR << "Error creating mesh specification: " << spec_result.error.c_str();
      return resources::Hold<SkMeshSpecification>(nullptr);
    } else {
      return resources::Hold(spec_result.specification);
    }
  }();

  if (mesh_specification.get() == nullptr) {
    return;
  }

  StrokeToCable stroke_to_cable;
  stroke_to_cable.start_width = start_width;
  stroke_to_cable.end_width = end_width;
  if (length_cache) {
    stroke_to_cable.end_offset = *length_cache;
  }
  stroke_to_cable.Convert(path);
  if (length_cache) {
    *length_cache = stroke_to_cable.length;
  }

  if (stroke_to_cable.vertex_vector.empty()) {
    return;
  }

  auto vertex_buffer = stroke_to_cable.BuildBuffer();
  sk_sp<SkShader> cable_color, cable_normal;
  switch (texture) {
    case CableTexture::Braided:
      static auto braided_color = PersistentImage::MakeFromAsset(
          embedded::assets_cable_weave_color_webp, {
                                                       .scale = 1,
                                                       .tile_x = SkTileMode::kRepeat,
                                                       .tile_y = SkTileMode::kRepeat,
                                                   });
      cable_color = *braided_color.shader;
      static auto braided_normal = PersistentImage::MakeFromAsset(
          embedded::assets_cable_weave_normal_webp, {
                                                        .scale = 1,
                                                        .tile_x = SkTileMode::kRepeat,
                                                        .tile_y = SkTileMode::kRepeat,
                                                        .raw_shader = true,
                                                    });
      cable_normal = *braided_normal.shader;
      break;
    case CableTexture::Smooth:
      cable_color = SkShaders::Color(SkColorSetARGB(255, 0x80, 0x80, 0x80));
      cable_normal = SkShaders::Color(SkColorSetARGB(255, 0x80, 0x80, 0xff));
      break;
  }
  sk_sp<SkData> uniforms = SkData::MakeWithCopy(&max_width, 4);
  SkMesh::ChildPtr children[] = {cable_color, cable_normal};
  auto mesh_result = SkMesh::Make(mesh_specification, SkMesh::Mode::kTriangleStrip, vertex_buffer,
                                  stroke_to_cable.vertex_vector.size(), 0, uniforms, {children, 2},
                                  stroke_to_cable.bounds);
  if (!mesh_result.error.isEmpty()) {
    ERROR << "Error creating mesh: " << mesh_result.error.c_str();
    return;
  }
  SkPaint default_paint;
  default_paint.setColor(0xffffffff);
  default_paint.setAntiAlias(true);
  default_paint.setColorFilter(color_filter);
  canvas.drawMesh(mesh_result.mesh, nullptr, default_paint);

  static auto braided_color = PersistentImage::MakeFromAsset(
      embedded::assets_cable_weave_color_webp, {
                                                   .scale = 0.005,
                                                   .tile_x = SkTileMode::kRepeat,
                                                   .tile_y = SkTileMode::kRepeat,
                                               });
  SkPaint paint;
  paint.setShader(*braided_color.shader);

  float start_offset = 0;
  float end_offset = 20_mm;
  if (length_cache) {
    end_offset = *length_cache;
  }
  auto GetWidth = [start_width, end_width, start_offset, end_offset](float length) {
    if (start_width == end_width) {
      return start_width;
    } else {
      float a = std::clamp<float>((length - start_offset) / (end_offset - start_offset), 0, 1) *
                std::numbers::pi;    // scale position to [0, pi]
      float t = cos(a) * 0.5 + 0.5;  // map cos to [0, 1] range
      return t * start_width + (1 - t) * end_width;
    }
  };

  // Walk along the path and draw the cable segments
  SkPath::Iter iter(path, false);
  SkPath::Verb verb;
  float length = 0;
  do {
    SkPoint points[4];
    verb = iter.next(points);
    if (SkPath::kConic_Verb == verb) {
      float weight = iter.conicWeight();
      float angle_rad = acosf(weight) * 2;
      float angle = angle_rad * 180 / M_PI;
      int n_steps = ceil(angle * 2 / 5);
      Vec2 last_point = points[0];

      // For some reason, the analytical formula causes texture stretching.
      // float radius = sqrt(LengthSquared(points[0] - points[2]) / (2 - 2 * cos(angle_rad)));
      // float delta_length = angle_rad * radius / n_steps;

      auto builder = SkVertices::Builder(SkVertices::kTriangleStrip_VertexMode, 2 * (n_steps + 1),
                                         0, SkVertices::kHasTexCoords_BuilderFlag);

      for (int step = 0; step <= n_steps; step++) {
        float t = (float)step / n_steps;
        Vec2 point = conic(points[0], points[1], points[2], weight, t);
        float delta_length = Length(point - last_point);
        length += step ? delta_length : 0;
        Vec2 tangent = -conic_tangent(points[0], points[1], points[2], weight, t);
        Vec2 normal = Rotate90DegreesClockwise(tangent) * GetWidth(length) / 2 / Length(tangent);
        last_point = point;
        Vec2 left = point - normal;
        Vec2 right = point + normal;
        builder.positions()[2 * step] = left;
        builder.positions()[2 * step + 1] = right;
        builder.texCoords()[2 * step] = Vec2(-1, length * 400);
        builder.texCoords()[2 * step + 1] = Vec2(1, length * 400);
      }
      canvas.drawVertices(builder.detach(), SkBlendMode::kSrcOver, paint);

    } else if (SkPath::kMove_Verb == verb) {
      // pass
    } else if (SkPath::kLine_Verb == verb) {
      Vec2 diff = points[1] - points[0];
      float segment_length = Length(diff);
      diff = diff / std::max(segment_length, 0.00001f);

      int n_steps =
          (start_width == end_width) ? 1 : std::max<int>(1, ceil(segment_length / 0.25_mm));
      auto builder = SkVertices::Builder(SkVertices::kTriangleStrip_VertexMode, 2 * (n_steps + 1),
                                         0, SkVertices::kHasTexCoords_BuilderFlag);
      for (int step = 0; step <= n_steps; ++step) {
        float t = (float)step / n_steps;

        float delta_length = step ? segment_length / n_steps : 0;
        length += delta_length;

        Vec2 point = points[0] * (1 - t) + points[1] * t;
        Vec2 normal = Rotate90DegreesClockwise(diff) * GetWidth(length) / 2;
        Vec2 left = point - normal;
        Vec2 right = point + normal;
        builder.positions()[2 * step] = left;
        builder.positions()[2 * step + 1] = right;
        builder.texCoords()[2 * step] = Vec2(-1, length * 400);
        builder.texCoords()[2 * step + 1] = Vec2(1, length * 400);
      }
      canvas.drawVertices(builder.detach(), SkBlendMode::kSrcOver, paint);
    } else if (SkPath::kCubic_Verb == verb) {
      Vec2 p0 = points[0];
      Vec2 p1 = points[1];
      Vec2 p2 = points[2];
      Vec2 p3 = points[3];
      Vec2 tangent0 = Normalize(p1 - p0);
      Vec2 tangent1 = Normalize(p3 - p2);
      Vec2 normal0 = Rotate90DegreesClockwise(tangent0);
      Vec2 normal1 = Rotate90DegreesClockwise(tangent1);

      Vec2 cubics[12];

      float w0 = GetWidth(length) / 2;
      // approximate by averaging chord and convex hull length
      float segment_length =
          (Length(p0 - p3) + Length(p0 - p1) + Length(p1 - p2) + Length(p2 - p3)) / 2;
      float length_end = length + segment_length;
      float w1 = GetWidth(length_end) / 2;
      cubics[0] = p0 - normal0 * w0;
      cubics[1] = p0 - normal0 * w0 / 3;
      cubics[2] = p0 + normal0 * w0 / 3;
      cubics[3] = p0 + normal0 * w0;
      cubics[4] = p1 + normal0 * w0;
      cubics[5] = p2 + normal1 * w1;
      cubics[6] = p3 + normal1 * w1;
      cubics[7] = p3 + normal1 * w1 / 3;
      cubics[8] = p3 - normal1 * w1 / 3;
      cubics[9] = p3 - normal1 * w1;
      cubics[10] = p2 - normal1 * w1;
      cubics[11] = p1 - normal0 * w0;

      Vec2 tex_coords[4] = {
          Vec2(-1, length * 400),
          Vec2(1, length * 400),
          Vec2(1, length_end * 400),
          Vec2(-1, length_end * 400),
      };
      length = length_end;

      canvas.drawPatch(&cubics[0].sk, nullptr, &tex_coords[0].sk, SkBlendMode::kDstOver, paint);
    }
  } while (SkPath::kDone_Verb != verb);
}

template <typename T>
struct MeshWithUniforms {
  sk_sp<SkMesh::VertexBuffer> vertex_buffer;
  sk_sp<SkMeshSpecification> mesh_specification;
  const sk_sp<SkData> uniforms;
  SkRect bounds;
  SkPaint paint;

  MeshWithUniforms() : uniforms(SkData::MakeUninitialized(sizeof(T))) {
    paint.setColor(0xffffffff);
  }

  T& GetUniforms() { return *(T*)uniforms->writable_data(); }

  virtual void UpdateUniforms(SkCanvas&) = 0;

  void Draw(SkCanvas& canvas) {
    UpdateUniforms(canvas);
    auto mesh_result = SkMesh::Make(mesh_specification, SkMesh::Mode::kTriangleStrip, vertex_buffer,
                                    4, 0, SkData::MakeWithCopy(uniforms->data(), uniforms->size()),
                                    SkSpan<SkMesh::ChildPtr>(), bounds);
    if (!mesh_result.error.isEmpty()) {
      ERROR << "Error creating mesh: " << mesh_result.error.c_str();
    } else {
      canvas.drawMesh(mesh_result.mesh, nullptr, paint);
    }
  }
};

struct BlackCasingUniforms {
  float plug_width_pixels;
  float light_dir;
};

struct BlackCasing : MeshWithUniforms<BlackCasingUniforms> {
  BlackCasing() {}
  void UpdateUniforms(SkCanvas& canvas) override {
    BlackCasingUniforms& u = GetUniforms();
    u.plug_width_pixels = canvas.getTotalMatrix().mapRadius(kCasingWidth);
  }

  void DrawState(SkCanvas& canvas, const CablePhysicsSimulation& state) {
    BlackCasingUniforms& u = GetUniforms();
    u.light_dir = M_PI / 2 -
                  (state.sections.front().dir - state.sections.front().true_dir_offset).ToRadians();
    Draw(canvas);
  }
};

// Currently the code that allocates Pimpl is pretty ad-hoc. It only holds the mesh for black casing
// and is allocated in the drawing routine. It might be cleaner to allocate it in a constructor and
// also use it to store other private data (maybe some data from OpticalConnectorState could also be
// moved).
// For the sake of moving on, I'm leaving this for later. Maybe after a bigger rewrite this won't be
// necessary anyway.
struct OpticalConnectorPimpl {
  std::unique_ptr<BlackCasing> mesh;
};

void DrawOpticalConnector(SkCanvas& canvas, const CablePhysicsSimulation& state,
                          PaintDrawable& icon) {
  float dispenser_scale = state.location.GetAnimationState().scale;

  SkMatrix connector_matrix = state.ConnectorMatrix();

  SkPath p;
  if (state.stabilized) {
    if (state.arcline) {
      SkPath p2 = state.arcline->ToPath(false);
      p.reverseAddPath(p2);
    }
  } else {
    p.moveTo(state.sections[0].pos);
    for (int i = 1; i < state.sections.size(); i++) {
      Vec2 p1 = state.sections[i - 1].pos +
                Vec2::Polar(state.sections[i - 1].dir + state.sections[i - 1].true_dir_offset,
                            state.sections[i - 1].distance / 3);
      Vec2 p2 = state.sections[i].pos -
                Vec2::Polar(state.sections[i].dir + state.sections[i].true_dir_offset,
                            state.sections[i].distance / 3);
      p.cubicTo(p1, p2, state.sections[i].pos);
    }
  }
  p.setIsVolatile(true);

  // Draw the cable
  auto color_filter = color::MakeTintFilter(state.arg.tint, NAN);
  DrawCable(canvas, p, color_filter, CableTexture::Braided,
            state.cable_width * state.connector_scale, state.cable_width * dispenser_scale,
            &state.approx_length);

  Vec2 cable_end = state.PlugTopCenter();
  SinCos connector_dir = state.sections.front().dir + state.sections.front().true_dir_offset;

  canvas.save();
  canvas.concat(connector_matrix);

  constexpr float casing_left = -kCasingWidth / 2;
  constexpr float casing_right = kCasingWidth / 2;
  constexpr float casing_top = kCasingHeight;

  {  // Steel insert
    struct SteelInsert : MeshWithUniforms<float> {
      SteelInsert() : MeshWithUniforms() {}
      void UpdateUniforms(SkCanvas& canvas) override {
        GetUniforms() = canvas.getTotalMatrix().mapRadius(kSteelRect.Width());
      }
    };

    static Optional<SteelInsert> mesh = [&]() -> Optional<SteelInsert> {
      SteelInsert result;
      SkMeshSpecification::Attribute attributes[2] = {
          {
              .type = SkMeshSpecification::Attribute::Type::kFloat2,
              .offset = 0,
              .name = SkString("position"),
          },
          {
              .type = SkMeshSpecification::Attribute::Type::kFloat2,
              .offset = 8,
              .name = SkString("uv"),
          }};
      SkMeshSpecification::Varying varyings[2] = {
          {
              .type = SkMeshSpecification::Varying::Type::kFloat2,
              .name = SkString("position"),
          },
          {
              .type = SkMeshSpecification::Varying::Type::kFloat2,
              .name = SkString("uv"),
          }};
      auto vs = SkString(R"(
      Varyings main(const Attributes attrs) {
        Varyings v;
        v.position = attrs.position;
        v.uv = attrs.uv;
        return v;
      }
    )");
      auto fs = SkString(R"(
      const float kCaseSideRadius = 0.08;
      // NOTE: fix this once Skia supports array initializers here
      const vec3 kCaseBorderDarkColor = vec3(0x38, 0x36, 0x33) / 255; // subtle dark contour
      const vec3 kCaseBorderReflectionColor = vec3(0xdd, 0xdb, 0xd6) / 255; // canvas reflection
      const vec3 kCaseSideDarkColor = vec3(0x54, 0x51, 0x4e) / 255; // darker metal between reflections
      const vec3 kCaseFrontColor = vec3(0x85, 0x83, 0x80) / 255; // front color
      const float kBorderDarkWidth = 0.3;
      const float kCaseSideDarkH = 0.7;
      const float kCaseFrontH = 1;
      const vec3 kTopLightColor = vec3(0x32, 0x34, 0x39) / 255 - kCaseFrontColor;
      const float kBevelRadius = kBorderDarkWidth * kCaseSideRadius;

      uniform float plug_width_pixels;

      float2 main(const Varyings v, out float4 color) {
        float2 h = sin(min((0.5 - abs(0.5 - v.uv)) / kCaseSideRadius, 1) * 3.14159265358979323846 / 2);
        float bevel = 1 - length(1 - sin(min((0.5 - abs(0.5 - v.uv)) / kBevelRadius, 1) * 3.14159265358979323846 / 2));
        if (h.x < kCaseSideDarkH) {
          color.rgb = mix(kCaseBorderReflectionColor, kCaseSideDarkColor, (h.x - kBorderDarkWidth) / (kCaseSideDarkH - kBorderDarkWidth));
        } else {
          color.rgb = mix(kCaseSideDarkColor, kCaseFrontColor, (h.x - kCaseSideDarkH) / (kCaseFrontH - kCaseSideDarkH));
        }
        if (bevel < 1) {
          vec3 edge_color = kCaseBorderDarkColor;
          if (v.uv.y > 0.5) {
            edge_color = mix(edge_color, vec3(0.4), clamp((h.x - kCaseSideDarkH) / (kCaseFrontH - kCaseSideDarkH), 0, 1));
          }
          color.rgb = mix(edge_color, color.rgb, bevel);
        }
        color.a = 1;
        float radius_pixels = kBevelRadius * plug_width_pixels;
        // Make the corners transparent
        color.rgba *= clamp(bevel * max(radius_pixels / 2, 1), 0, 1);
        return v.position;
      }
    )");

      auto spec_result = SkMeshSpecification::Make(attributes, 16, varyings, vs, fs);
      if (!spec_result.error.isEmpty()) {
        ERROR << "Error creating mesh specification: " << spec_result.error.c_str();
      } else {
        Vec2 vertex_data[8] = {
            kSteelRect.BottomLeftCorner(), Vec2(0, 0), kSteelRect.BottomRightCorner(), Vec2(1, 0),
            kSteelRect.TopLeftCorner(),    Vec2(0, 1), kSteelRect.TopRightCorner(),    Vec2(1, 1),
        };
        result.vertex_buffer = SkMeshes::MakeVertexBuffer(vertex_data, sizeof(vertex_data));
        result.mesh_specification = spec_result.specification;
        result.bounds = kSteelRect.sk;
        return result;
      }
      return std::nullopt;
    }();
    if (mesh) {
      canvas.save();
      canvas.translate(0, 2_mm * state.steel_insert_hidden);
      mesh->Draw(canvas);

      auto builder = SkVertices::Builder(SkVertices::kTriangleStrip_VertexMode, 4, 0,
                                         SkVertices::kHasTexCoords_BuilderFlag);
      SkPoint* positions = builder.positions();
      positions[0] = kSteelRect.BottomLeftCorner();
      positions[1] = kSteelRect.BottomRightCorner();
      positions[2] = kSteelRect.TopLeftCorner();
      positions[3] = kSteelRect.TopRightCorner();

      SkPoint* tex_coords = builder.texCoords();
      tex_coords[0] = SkPoint::Make(0, 0);
      tex_coords[1] = SkPoint::Make(1, 0);
      tex_coords[2] = SkPoint::Make(0, 1);
      tex_coords[3] = SkPoint::Make(1, 1);

      SkPaint paint;
      paint.setColor("#cccccc"_color);
      canvas.drawVertices(builder.detach(), SkBlendMode::kScreen, paint);

      canvas.restore();
    }
  }

  {  // Black metal casing
    float pixels_per_meter = canvas.getLocalToDeviceAs3x3().mapRadius(1);
    canvas.translate(0, 1 / pixels_per_meter);  // move one pixel up to close the one pixel gap
    if (state.pimpl == nullptr) {
      state.pimpl = std::make_unique<OpticalConnectorPimpl>();
      state.pimpl->mesh = [&]() -> std::unique_ptr<BlackCasing> {
        std::unique_ptr<BlackCasing> result = std::make_unique<BlackCasing>();
        SkMeshSpecification::Attribute attributes[2] = {
            {
                .type = SkMeshSpecification::Attribute::Type::kFloat2,
                .offset = 0,
                .name = SkString("position"),
            },
            {
                .type = SkMeshSpecification::Attribute::Type::kFloat2,
                .offset = 8,
                .name = SkString("uv"),
            }};
        SkMeshSpecification::Varying varyings[3] = {
            {
                .type = SkMeshSpecification::Varying::Type::kFloat2,
                .name = SkString("position"),
            },
            {
                .type = SkMeshSpecification::Varying::Type::kFloat2,
                .name = SkString("uv"),
            },
            {
                .type = SkMeshSpecification::Varying::Type::kFloat,
                .name = SkString("light"),
            }};
        auto vs = SkString(R"(
      uniform float plug_width_pixels;
      uniform float light_dir;

      Varyings main(const Attributes attrs) {
        Varyings v;
        v.position = attrs.position;
        v.uv = attrs.uv;
        float cos_light_dir = cos(light_dir);
        float sin_light_dir = sin(light_dir);
        vec2 center = vec2(0.5, 0.5);
        vec2 delta = attrs.uv - center;
        v.light = cos_light_dir * delta.y - sin_light_dir * delta.x + center.y;
        return v;
      }
    )");
        auto fs = SkString(R"(
      const float kCaseSideRadius = 0.12;
      // NOTE: fix this once Skia supports array initializers here
      const vec3 kCaseBorderDarkColor = vec3(5) / 255; // subtle dark contour
      const vec3 kCaseBorderReflectionColor = vec3(115, 115, 115) / 255 * 1.6; // canvas reflection
      const vec3 kCaseSideDarkColor = vec3(42, 42, 42) / 255; // darker metal between reflections
      const vec3 kCaseSideLightColor = vec3(88, 88, 88) / 255; // side-light reflection
      const vec3 kCaseFrontColor = vec3(0x1c, 0x1c, 0x1c) * 2 / 255; // front color
      const float kBorderDarkWidth = 0.2;
      const float kCaseSideDarkH = 0.4;
      const float kCaseSideLightH = 0.8;
      const float kCaseFrontH = 1;
      const vec3 kTopLightColor = vec3(0.114, 0.114, 0.114) * 2;
      const float kBevelRadius = kBorderDarkWidth * kCaseSideRadius;

      uniform float plug_width_pixels;

      float2 main(const Varyings v, out float4 color) {
        float2 h = sin(min((0.5 - abs(0.5 - v.uv)) / kCaseSideRadius, 1) * 3.14159265358979323846 / 2);
        float bevel = 1 - length(1 - sin(min((0.5 - abs(0.5 - v.uv)) / kBevelRadius, 1) * 3.14159265358979323846 / 2));
        if (h.x < kCaseSideDarkH) {
          color.rgb = mix(kCaseBorderReflectionColor, kCaseSideDarkColor, (h.x - kBorderDarkWidth) / (kCaseSideDarkH - kBorderDarkWidth));
        } else if (h.x < kCaseSideLightH) {
          color.rgb = mix(kCaseSideDarkColor, kCaseSideLightColor, (h.x - kCaseSideDarkH) / (kCaseSideLightH - kCaseSideDarkH));
        } else {
          color.rgb = mix(kCaseSideLightColor, kCaseFrontColor, (h.x - kCaseSideLightH) / (kCaseFrontH - kCaseSideLightH));
        }
        if (bevel < 1) {
          vec3 edge_color = kCaseBorderDarkColor;
          if (v.uv.y > 0.5) {
            edge_color = mix(edge_color, vec3(0.4), clamp((h.x - kCaseSideDarkH) / (kCaseFrontH - kCaseSideDarkH), 0, 1));
          }
          color.rgb = mix(edge_color, color.rgb, bevel);
        }
        color.rgb += kTopLightColor * v.light;
        color.a = 1;
        float radius_pixels = kBevelRadius * plug_width_pixels;
        // Make the corners transparent
        color.rgba *= clamp(bevel * max(radius_pixels / 2, 1), 0, 1);
        return v.position;
      }
    )");

        auto spec_result = SkMeshSpecification::Make(attributes, 16, varyings, vs, fs);
        if (!spec_result.error.isEmpty()) {
          ERROR << "Error creating mesh specification: " << spec_result.error.c_str();
        } else {
          result->bounds = SkRect::MakeLTRB(casing_left, casing_top, casing_right, 0);
          Vec2 vertex_data[8] = {
              Vec2(casing_left, 0),          Vec2(0, 0), Vec2(casing_right, 0),          Vec2(1, 0),
              Vec2(casing_left, casing_top), Vec2(0, 1), Vec2(casing_right, casing_top), Vec2(1, 1),
          };
          result->vertex_buffer = SkMeshes::MakeVertexBuffer(vertex_data, sizeof(vertex_data));
          result->mesh_specification = spec_result.specification;
          result->paint.setColorFilter(color_filter);
          return result;
        }
        return nullptr;
      }();
    }
    if (state.pimpl->mesh) {
      state.pimpl->mesh->DrawState(canvas, state);
    }

    auto builder = SkVertices::Builder(
        SkVertices::kTriangleStrip_VertexMode, 4, 0,
        SkVertices::kHasTexCoords_BuilderFlag | SkVertices::kHasColors_BuilderFlag);
    constexpr Rect black_case_bounds =
        Rect::MakeCornerZero(kCasingWidth, kCasingHeight).MoveBy({-kCasingWidth / 2, 0});
    SkPoint* positions = builder.positions();
    positions[0] = black_case_bounds.BottomLeftCorner();
    positions[1] = black_case_bounds.BottomRightCorner();
    positions[2] = black_case_bounds.TopLeftCorner();
    positions[3] = black_case_bounds.TopRightCorner();

    SkPoint* tex_coords = builder.texCoords();
    tex_coords[0] = SkPoint::Make(0, 0);
    tex_coords[1] = SkPoint::Make(1, 0);
    tex_coords[2] = SkPoint::Make(0, 1);
    tex_coords[3] = SkPoint::Make(1, 1);

    // TODO: decide whether to use vertex colors or tint color filter
    SkColor* colors = builder.colors();
    colors[0] = state.arg.tint;
    colors[1] = state.arg.tint;
    colors[2] = state.arg.tint;
    colors[3] = state.arg.tint;

    SkPaint paint;
    paint.setColor("#111111"_color);
    canvas.drawVertices(builder.detach(), SkBlendMode::kScreen, paint);
  }

  canvas.restore();

  {  // Icon on the metal casing

    Vec2 icon_offset = connector_matrix.mapPoint(Vec2(0, kCasingHeight / 2));

    SkColor base_color = color::AdjustLightness(state.arg.tint, 30);
    SkColor bright_light = color::AdjustLightness(state.arg.light, 50);
    SkColor adjusted_color = color::AdjustLightness(base_color, state.lightness_pct);
    adjusted_color = color::MixColors(adjusted_color, bright_light, state.lightness_pct / 100);

    SkPaint icon_paint;
    icon_paint.setColor(adjusted_color);
    icon_paint.setAntiAlias(true);
    icon.paint = icon_paint;
    canvas.save();
    canvas.translate(icon_offset.x, icon_offset.y);
    canvas.scale(state.connector_scale, state.connector_scale);
    icon.draw(&canvas, 0, 0);

    // Draw blur
    if (state.lightness_pct > 1) {
      SkPaint glow_paint;
      glow_paint.setColor(state.arg.light);
      glow_paint.setAlphaf(state.lightness_pct / 100);
      float sigma = canvas.getLocalToDeviceAs3x3().mapRadius(0.5_mm);
      glow_paint.setMaskFilter(
          SkMaskFilter::MakeBlur(SkBlurStyle::kOuter_SkBlurStyle, sigma, false));
      glow_paint.setBlendMode(SkBlendMode::kScreen);
      icon.paint = glow_paint;
      icon.draw(&canvas, 0, 0);
    }
    canvas.restore();
  }

  {  // Rubber cable holder

    static sk_sp<SkMeshSpecification>& mesh_specification = []() -> sk_sp<SkMeshSpecification>& {
      auto vs = SkString(R"(
      Varyings main(const Attributes attrs) {
        Varyings v;
        v.position = attrs.position;
        v.uv = attrs.uv;
        v.tangent = normalize(attrs.tangent);
        return v;
      }
    )");
      auto fs = SkString(embedded::assets_cable_strain_reliever_frag_sksl.content);
      auto spec_result =
          SkMeshSpecification::Make(StrokeToMesh::kAttributes, sizeof(StrokeToMesh::VertexInfo),
                                    StrokeToMesh::kVaryings, vs, fs);
      if (!spec_result.error.isEmpty()) {
        ERROR << "Error creating mesh specification: " << spec_result.error.c_str();
        return resources::Hold<SkMeshSpecification>(nullptr);
      }
      return resources::Hold(spec_result.specification);
    }();

    float length = 15_mm * state.connector_scale;
    StrokeToCable mesh_builder;
    mesh_builder.start_offset = 0;
    mesh_builder.start_width = kCasingWidth * state.connector_scale;
    mesh_builder.end_offset = length;
    mesh_builder.end_width = (state.cable_width + 1_mm) * state.connector_scale;
    mesh_builder.Convert(p, length);
    if (mesh_builder.vertex_vector.empty()) {
      // Add two points on the left & right side of the connector - just so that we can build the
      // ellipse cap.
      Vec2 offset = Vec2::Polar(connector_dir + 90_deg, kCasingWidth / 2 * state.connector_scale);
      Vec2 tangent = Vec2::Polar(connector_dir, 1);
      mesh_builder.vertex_vector.push_back({
          .coords = cable_end + offset,
          .uv = Vec2(-1, 0),
          .tangent = tangent,
      });
      mesh_builder.vertex_vector.push_back({
          .coords = cable_end - offset,
          .uv = Vec2(1, 0),
          .tangent = tangent,
      });
    }
    // Add an ellipse cap at the end of the mesh
    if (mesh_builder.vertex_vector.size() >= 2) {
      auto left = mesh_builder.vertex_vector[mesh_builder.vertex_vector.size() - 2];
      auto right = mesh_builder.vertex_vector[mesh_builder.vertex_vector.size() - 1];
      Vec2 middle = (left.coords + right.coords) / 2;
      Vec2 left_to_right = right.coords - left.coords;
      Vec2 tangent = Normalize(left.tangent);
      float width = Length(left_to_right);
      float height = width / 8;
      constexpr int n_steps = 10;
      for (int i = 0; i < n_steps; ++i) {
        float t = (float)i / n_steps;
        auto mat = SkMatrix::RotateDeg(-t * 90);
        auto mat2 = SkMatrix::RotateDeg(t * 90);
        t = 1 - (1 - t) * (1 - t);
        float step_width = sqrt(1 - t * t);
        float step_height = height * t;
        mesh_builder.vertex_vector.push_back({
            .coords = middle - left_to_right * step_width / 2 + tangent * step_height,
            .uv = Vec2(-step_width, left.uv.y + step_height),
            .tangent = mat.mapPoint(tangent),
        });
        mesh_builder.bounds.ExpandToInclude(mesh_builder.vertex_vector.back().coords);
        mesh_builder.vertex_vector.push_back({
            .coords = middle + left_to_right * step_width / 2 + tangent * step_height,
            .uv = Vec2(step_width, left.uv.y + step_height),
            .tangent = mat2.mapPoint(tangent),
        });
        mesh_builder.bounds.ExpandToInclude(mesh_builder.vertex_vector.back().coords);
      }
      mesh_builder.vertex_vector.push_back({
          .coords = middle + tangent * height,
          .uv = Vec2(0, left.uv.y + height),
          .tangent = SkMatrix::RotateDeg(90).mapPoint(tangent),
      });
      mesh_builder.bounds.ExpandToInclude(mesh_builder.vertex_vector.back().coords);

      auto vertex_buffer = mesh_builder.BuildBuffer();
      auto mesh_result =
          SkMesh::Make(mesh_specification, SkMesh::Mode::kTriangleStrip, vertex_buffer,
                       mesh_builder.vertex_vector.size(), 0, nullptr, {}, mesh_builder.bounds);
      if (!mesh_result.error.isEmpty()) {
        ERROR << "Error creating mesh: " << mesh_result.error.c_str();
      } else {
        SkPaint paint;
        paint.setColor(0xffffffff);
        paint.setAntiAlias(true);
        paint.setColorFilter(color_filter);
        canvas.drawMesh(mesh_result.mesh, nullptr, paint);
      }
    }

    float length_limit = 15_mm * state.connector_scale;
    length = 0;

    SkPath::Iter iter(p, false);
    SkPath::Verb verb;
    Vec2 last_point = cable_end;
    Vec2 normal = Vec2::Polar(connector_dir - 90_deg, 1);
    do {
      SkPoint points[4];
      verb = iter.next(points);
      bool limit_reached = false;
      if (SkPath::kConic_Verb == verb) {
        float weight = iter.conicWeight();
        float angle = acosf(weight) * 2 * 180 / M_PI;
        int n_steps = ceil(angle * 2 / 5);
        last_point = points[0];
        for (int step = 0; step <= n_steps; step++) {
          float t = (float)step / n_steps;
          Vec2 point = conic(points[0], points[1], points[2], weight, t);
          float delta_length = Length(point - last_point);
          if (length + delta_length >= length_limit) {
            t = (float)(step - 1 + (length_limit - length) / delta_length) / n_steps;
            point = conic(points[0], points[1], points[2], weight, t);
            length = length_limit;
            limit_reached = true;
          } else {
            length += delta_length;
          }
          Vec2 tangent = -conic_tangent(points[0], points[1], points[2], weight, t);
          normal = Rotate90DegreesClockwise(tangent) / Length(tangent);
          last_point = point;
          if (limit_reached) {
            break;
          }
        }
      } else if (SkPath::kMove_Verb == verb) {
        // pass
      } else if (SkPath::kLine_Verb == verb) {
        Vec2 diff = points[1] - points[0];
        float segment_length = Length(diff);
        diff = diff / std::max(segment_length, 0.00001f);

        int n_steps = 1;
        for (int step = 0; step <= n_steps; ++step) {
          float t = (float)step / n_steps;

          float delta_length = step ? segment_length / n_steps : 0;
          if (length + delta_length >= length_limit) {
            t = (float)(step - 1 + (length_limit - length) / delta_length) / n_steps;
            length = length_limit;
            limit_reached = true;
          } else {
            length += delta_length;
          }

          last_point = points[0] * (1 - t) + points[1] * t;
          normal = Rotate90DegreesClockwise(diff);
          if (limit_reached) {
            break;
          }
        }
      } else if (SkPath::kCubic_Verb == verb) {
        Vec2 p0 = points[0];
        Vec2 p1 = points[1];
        Vec2 p2 = points[2];
        Vec2 p3 = points[3];
        constexpr int n_steps = 1;
        last_point = p0;
        for (int step = 1; step <= n_steps; step++) {
          float t = (float)step / n_steps;
          Vec2 point = p0 * powf(1 - t, 3) + p1 * 3 * powf(1 - t, 2) * t +
                       p2 * 3 * (1 - t) * t * t + p3 * powf(t, 3);

          float delta_length = Length(point - last_point);
          if (length + delta_length >= length_limit) {
            t = (float)(step - 1 + (length_limit - length) / delta_length) / n_steps;
            point = p0 * powf(1 - t, 3) + p1 * 3 * powf(1 - t, 2) * t + p2 * 3 * (1 - t) * t * t +
                    p3 * powf(t, 3);
            length = length_limit;
            limit_reached = true;
          } else {
            length += delta_length;
          }

          Vec2 tangent = p0 * -3 * powf(1 - t, 2) + p1 * (3 * powf(1 - t, 2) - 6 * t * (1 - t)) +
                         p2 * (6 * t * (1 - t) - 3 * t * t) + p3 * 3 * powf(t, 2);
          normal = Rotate90DegreesClockwise(tangent) / Length(tangent);
          last_point = point;
          if (limit_reached) {
            break;
          }
        }
      }
    } while (SkPath::kDone_Verb != verb && length < length_limit);

    SkPaint paint;
    paint.setColor(0xffff0000);

    Vec2 top_offset = normal * CosineInterpolate(kCasingWidth / 2, 1.5_mm, length / length_limit);
    Vec2 top_tangent = Rotate90DegreesCounterClockwise(normal);
    Vec2 base_offset =
        Vec2::Polar(connector_dir - 90_deg, kCasingWidth / 2 * state.connector_scale);
    auto top = last_point;
    auto base = cable_end;
    auto base_tangent = Vec2::Polar(connector_dir, 1);
    auto top_left = top - top_offset;
    auto top_right = top + top_offset;
    auto base_left = base - base_offset;
    auto base_right = base + base_offset;
    float vertical_control_point_distance_left = std::min(length, Length(base_left - top_left) / 2);
    float vertical_control_point_distance_right =
        std::min(length, Length(base_right - top_right) / 2);
    Vec2 positions[12] = {
        top_left,
        top_left + top_tangent * 1_mm,
        top_right + top_tangent * 1_mm,
        top_right,
        top_right - top_tangent * vertical_control_point_distance_right,
        base_right + base_tangent * vertical_control_point_distance_right,
        base_right,
        base + base_offset / 3,
        base - base_offset / 3,
        base_left,
        base_left + base_tangent * vertical_control_point_distance_left,
        top_left - top_tangent * vertical_control_point_distance_left,
    };

    SkColor colors[4] = {
        "#00ff00"_color,
        "#ffff00"_color,
        "#ff0000"_color,
        "#000000"_color,
    };

    canvas.drawPatch(&positions[0].sk, colors, nullptr, SkBlendMode::kDstOver, paint);
  }

  if constexpr (kDebugCable) {  // Draw the arcline
    if (state.arcline) {
      SkPath cable_path = state.arcline->ToPath(false);
      SkPaint arcline_paint;
      arcline_paint.setColor("#eee19d"_color);
      // arcline_paint.setAlphaf(.5);
      arcline_paint.setStrokeWidth(0.0005);
      arcline_paint.setStyle(SkPaint::kStroke_Style);
      arcline_paint.setAntiAlias(true);
      arcline_paint.setBlendMode(SkBlendMode::kDifference);
      canvas.drawPath(cable_path, arcline_paint);

      Vec<Vec2> anchors;
      Vec<SinCos> true_anchor_dir;
      PopulateAnchors(anchors, true_anchor_dir, *state.arcline);
      SkPath anchor_shape;
      anchor_shape.moveTo(1_mm, 0);
      anchor_shape.lineTo(0.5_mm, 0.5_mm);
      anchor_shape.lineTo(0.5_mm, 0.2_mm);
      anchor_shape.lineTo(-1_mm, 0.2_mm);
      anchor_shape.lineTo(-1_mm, -0.2_mm);
      anchor_shape.lineTo(0.5_mm, -0.2_mm);
      anchor_shape.lineTo(0.5_mm, -0.5_mm);
      anchor_shape.close();
      SkPaint anchor_paint;
      anchor_paint.setColor("#ff00ff"_color);
      anchor_paint.setBlendMode(SkBlendMode::kDifference);
      for (int i = 0; i < anchors.size(); i++) {
        auto saved_matrix = canvas.getTotalMatrix();
        canvas.translate(anchors[i].x, anchors[i].y);
        canvas.concat(true_anchor_dir[i].ToMatrix());
        canvas.drawPath(anchor_shape, anchor_paint);

        canvas.setMatrix(saved_matrix);
      }
    }
  }

  if constexpr (kDebugCable) {  // Draw the cable sections as a series of straight lines
    SkPaint cross_paint;
    cross_paint.setColor(0xffff8800);
    cross_paint.setAntiAlias(true);
    cross_paint.setStrokeWidth(0.0005);
    cross_paint.setStyle(SkPaint::kStroke_Style);

    auto& font = GetFont();
    auto& chain = state.sections;
    SkPaint chain_paint;
    chain_paint.setColor(0xff0088ff);
    chain_paint.setAntiAlias(true);
    chain_paint.setStrokeWidth(0.00025);
    chain_paint.setStyle(SkPaint::kStroke_Style);
    for (int i = 0; i < chain.size(); ++i) {
      Vec2 line_offset = Vec2::Polar(chain[i].dir, kStep / 4);
      canvas.drawLine(chain[i].pos - line_offset, chain[i].pos + line_offset, chain_paint);
      canvas.save();
      Str i_str = std::to_string(i);
      canvas.translate(chain[i].pos.x, chain[i].pos.y);
      font.DrawText(canvas, i_str, SkPaint());
      canvas.restore();
    }
  }
}

// This function has some nice code for drawing connections between rounded rectangles.
// Keeping this for potential usage in the future
void DrawArrow(SkCanvas& canvas, const SkPath& from_shape, const SkPath& to_shape) {
  static const SkPath kConnectionArrowShape = PathFromSVG(kConnectionArrowShapeSVG);
  SkColor color = "#6e4521"_color;
  SkPaint line_paint;
  line_paint.setAntiAlias(true);
  line_paint.setStyle(SkPaint::kStroke_Style);
  line_paint.setStrokeWidth(0.0005);
  line_paint.setColor(color);
  SkPaint arrow_paint;
  arrow_paint.setAntiAlias(true);
  arrow_paint.setStyle(SkPaint::kFill_Style);
  arrow_paint.setColor(color);
  SkRRect from_rrect, to_rrect;
  bool from_is_rrect = from_shape.isRRect(&from_rrect);
  bool to_is_rrect = to_shape.isRRect(&to_rrect);

  // Find an area where the start of a connection can freely move.
  SkRect from_inner;
  if (from_is_rrect) {
    SkVector radii = from_rrect.getSimpleRadii();
    from_inner = from_rrect.rect().makeInset(radii.x(), radii.y());
  } else {
    Vec2 from_center = from_shape.getBounds().center();
    from_inner = SkRect::MakeXYWH(from_center.x, from_center.y, 0, 0);
  }
  // Find an area where the end of a connection can freely move.
  SkRect to_inner;
  if (to_is_rrect) {
    SkVector radii = to_rrect.getSimpleRadii();
    to_inner = to_rrect.rect().makeInset(radii.x(), radii.y());
  } else {
    Vec2 to_center = to_shape.getBounds().center();
    to_inner = SkRect::MakeXYWH(to_center.x, to_center.y, 0, 0);
  }
  to_inner.sort();
  from_inner.sort();

  Vec2 from, to;
  // Set the vertical positions of the connection endpoints.
  float left = std::max(from_inner.left(), to_inner.left());
  float right = std::min(from_inner.right(), to_inner.right());
  if (left <= right) {
    from.x = to.x = (left + right) / 2;
  } else if (from_inner.right() < to_inner.left()) {
    from.x = from_inner.right();
    to.x = to_inner.left();
  } else {
    from.x = from_inner.left();
    to.x = to_inner.right();
  }
  // Set the horizontal positions of the connection endpoints.
  float top = std::max(from_inner.top(), to_inner.top());
  float bottom = std::min(from_inner.bottom(), to_inner.bottom());
  if (bottom >= top) {
    from.y = to.y = (top + bottom) / 2;
  } else if (from_inner.bottom() < to_inner.top()) {
    from.y = from_inner.bottom();
    to.y = to_inner.top();
  } else {
    from.y = from_inner.top();
    to.y = to_inner.bottom();
  }
  // Find polar coordinates of the connection.
  Vec2 delta = to - from;
  float degrees = atan(delta) * 180 / M_PI;
  float end = Length(delta);
  float start = 0;
  if (from_is_rrect) {
    start = std::min(start + from_rrect.getSimpleRadii().fX, end);
  }
  if (to_is_rrect) {
    end = std::max(start, end - to_rrect.getSimpleRadii().fX);
  }
  float line_end = std::max(start, end + kConnectionArrowShape.getBounds().centerX());
  // Draw the connection.
  canvas.save();
  canvas.translate(from.x, from.y);
  canvas.rotate(degrees);
  if (start < line_end) {
    canvas.drawLine(start, 0, line_end, 0, line_paint);
  }
  canvas.translate(end, 0);
  canvas.drawPath(kConnectionArrowShape, arrow_paint);
  canvas.restore();
}

CablePhysicsSimulation::CablePhysicsSimulation(Location& loc, Argument& arg, Vec2AndDir start)
    : dispenser_v(0), location(loc), arg(arg) {
  sections.emplace_back(CableSection{
      .pos = start.pos,
      .vel = Vec2(0, 0),
      .acc = Vec2(0, 0),
      .dir = start.dir.Opposite(),
      .true_dir_offset = 0_deg,
      .distance = 0,
      .next_dir_delta = 0_deg,
  });  // plug
  sections.emplace_back(CableSection{
      .pos = start.pos,
      .vel = Vec2(0, 0),
      .acc = Vec2(0, 0),
      .dir = start.dir.Opposite(),
      .true_dir_offset = 0_deg,
      .distance = 0,
      .next_dir_delta = 0_deg,
  });  // dispenser
  steel_insert_hidden.period = 0.5s;
  steel_insert_hidden.half_life = 0.2s;
}

CablePhysicsSimulation::~CablePhysicsSimulation() {}

}  // namespace automat::gui