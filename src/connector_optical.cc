#include "connector_optical.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMesh.h>
#include <include/core/SkSamplingOptions.h>
#include <include/effects/SkGradientShader.h>
#include <include/effects/SkRuntimeEffect.h>

#include <cmath>
#include <numbers>

#include "../build/generated/embedded.hh"
#include "arcline.hh"
#include "color.hh"
#include "font.hh"
#include "gui_constants.hh"
#include "log.hh"
#include "math.hh"
#include "svg.hh"

using namespace maf;

namespace automat::gui {

constexpr bool kDebugCable = false;
constexpr float kCasingWidth = 0.008;
constexpr float kCasingHeight = 0.008;
constexpr float kStep = 0.005;
constexpr float kCrossSize = 0.001;
constexpr float kCableWidth = 0.002;

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
  ArcLine cable = ArcLine(start.pos, M_PI * 1.5);
  Vec2 delta = cable_end - start.pos;
  float distance = Length(delta);
  float turn_radius = std::max<float>(distance / 8, 0.01);

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
      cable.TurnBy(dir * M_PI / 2, turn_radius);
      x += turn_radius * dir;
      y += turn_radius;
      cable.TurnBy(dir * M_PI / 2, turn_radius);
      x += turn_radius * dir;
      y -= turn_radius;
      float move_up = cable_end.y - y;
      float move_down = -move_up;
      if (move_up > 0) {
        cable.MoveBy(move_up);
      }
      y = cable_end.y;
      cable.TurnBy(dir * M_PI / 2, turn_radius);
      x -= turn_radius * dir;
      y -= turn_radius;
      cable.MoveBy(dir * (x - cable_end.x) - turn_radius);
      cable.TurnBy(dir * M_PI / 2, turn_radius);
      if (move_down > 0) {
        cable.MoveBy(move_down);
      }
    } else {
      cable.TurnBy(horizontal_shift.first_turn_angle, turn_radius);
      if (move_side > 0) {
        cable.MoveBy(move_side);
      }
      vertical_shift.Apply(cable);
      if (move_side > 0) {
        cable.MoveBy(move_side);
      }
      cable.TurnBy(-horizontal_shift.first_turn_angle, turn_radius);
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

static ArcLine RoutCableStraight(Vec2AndDir start, Vec2 cable_end) {
  float radius = 1_cm;
  ArcLine cable = ArcLine(start.pos, start.dir);
  // check if the end point is on the left or right side of start (point + dir)
  Vec2 relative_end = cable_end - start.pos;
  float end_dir = atan(relative_end);
  float delta_dir = NormalizeAngle(end_dir - start.dir);

  float best_start_turn;
  float best_end_turn;
  float best_line_length;
  float best_total_length = HUGE_VALF;

  for (bool start_left : {false, true}) {
    Vec2 start_circle_center =
        start.pos + Vec2::Polar(start.dir + (start_left ? M_PI / 2 : -M_PI / 2), radius);
    for (bool end_left : {false, true}) {
      Vec2 end_circle_center = cable_end + Vec2(end_left ? radius : -radius, 0);
      Vec2 circle_diff = end_circle_center - start_circle_center;
      float circle_dist = Length(circle_diff);
      float circle_angle = atan(circle_diff);
      float line_dir;
      float line_length;
      if (start_left == end_left) {
        line_dir = circle_angle;
        line_length = circle_dist;
      } else if (circle_dist > 2 * radius) {
        line_length = sqrt(circle_dist * circle_dist - radius * radius * 4);
        line_dir = circle_angle + acosf(line_length / circle_dist) * (start_left ? 1 : -1);
      } else {
        continue;
      }

      float start_turn = line_dir - start.dir;
      constexpr float kEpsilon = 1e-6;
      while (start_left && start_turn < -kEpsilon) {
        start_turn += 2 * M_PI;
      }
      if (!start_left && start_turn > kEpsilon) {
        start_turn -= 2 * M_PI;
      }

      float end_turn = -M_PI / 2 - start_turn;
      while (end_left && end_turn < -kEpsilon) {
        end_turn += 2 * M_PI;
      }
      if (!end_left && end_turn > kEpsilon) {
        end_turn -= 2 * M_PI;
      }

      float total_length = fabsf(start_turn) * radius + line_length + fabsf(end_turn) * radius;
      if (total_length < best_total_length) {
        best_total_length = total_length;
        best_start_turn = start_turn;
        best_end_turn = end_turn;
        best_line_length = line_length;
      }
    }
  }

  // LOG << " start_turn:" << best_start_turn << " end_turn:" << best_end_turn
  //     << " line_length:" << best_line_length;

  cable.TurnBy(best_start_turn, radius);
  cable.MoveBy(best_line_length);
  cable.TurnBy(best_end_turn, radius);
  return cable;
}

static ArcLine RouteCable(Vec2AndDir start, Vec2 cable_end) {
  if (fabsf(start.dir + (float)M_PI / 2) < 0.000001) {
    return RouteCableDown(start, cable_end);
  } else {
    return RoutCableStraight(start, cable_end);
  }
}

// This function walks along the given arcline (from the end to its start) and adds
// an anchor every kStep distance. It populates the `anchors` and `anchor_tangents` vectors.
static void PopulateAnchors(Vec<Vec2>& anchors, Vec<float>& anchor_dir, const ArcLine& arcline) {
  auto it = ArcLine::Iterator(arcline);
  Vec2 dispenser = it.Position();
  float cable_length = it.AdvanceToEnd();
  Vec2 tail = it.Position();

  anchors.push_back(tail);
  anchor_dir.push_back(NormalizeAngle(it.Angle() + M_PI));
  for (float cable_pos = kStep; cable_pos < cable_length - kCableWidth / 2; cable_pos += kStep) {
    it.Advance(-kStep);
    anchors.push_back(it.Position());
    float dir = NormalizeAngle(it.Angle() + M_PI);
    anchor_dir.push_back(dir);
  }
  anchors.push_back(dispenser);
  anchor_dir.push_back(NormalizeAngle(it.Angle() + M_PI));
}

// Simulate the dispenser pulling in the cable. This function may remove some of the cable segments
// but will always leave at least two - starting & ending points.
//
// Returns true if the dispenser is active and pulling the cable in.
//
// WARNING: this function will adjust the length of the final cable segment (the one closest to the
// dispenser). Don't change it or visual glitches will occur.
static bool SimulateDispenser(OpticalConnectorState& state, float dt, Size anchor_count) {
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
    // Move chain[i] to |remaining| distance from dispenser
    state.sections[i].distance = remaining;
  } else {
    state.dispenser_v = 0;
    do {  // Add a new link if the last one is too far from the dispenser
      auto delta = state.sections[state.sections.size() - 2].pos - state.sections.back().pos;
      auto current_dist = Length(delta);
      constexpr float kExtendThreshold = kStep + kCableWidth / 2;
      if (current_dist > kExtendThreshold) {
        state.sections[state.sections.size() - 2].distance = kStep;
        auto new_it = state.sections.insert(
            state.sections.begin() + state.sections.size() - 1,
            OpticalConnectorState::CableSection{
                .pos = state.sections[state.sections.size() - 2].pos -
                       Vec2::Polar(state.sections.back().dir, kCableWidth / 2) -
                       delta / current_dist * kStep,
                .vel = Vec2(0, 0),
                .acc = Vec2(0, 0),
                .distance = current_dist - kStep,
            });
      } else if (state.sections.size() < anchor_count) {
        auto new_it = state.sections.insert(
            state.sections.begin() + state.sections.size() - 1,
            OpticalConnectorState::CableSection{
                .pos = state.sections.back().pos -
                       Vec2::Polar(state.sections.back().dir, kCableWidth / 2),
                .vel = Vec2(0, 0),
                .acc = Vec2(0, 0),
                .distance = kCableWidth / 2,
            });
        break;
      } else {
        break;
      }
    } while (state.sections.size() < anchor_count);
  }

  return pulling;
}

void SimulateCablePhysics(float dt, OpticalConnectorState& state, Vec2AndDir dispenser,
                          Optional<Vec2> end) {
  Optional<Vec2> cable_end;
  if (end) {
    cable_end = Vec2(end->x, end->y + kCasingHeight);
  }
  if (state.stabilized && Length(dispenser.pos - state.stabilized_start) < 0.0001) {
    if (cable_end.has_value() == state.stabilized_end.has_value() &&
        (!cable_end.has_value() || Length(*cable_end - *state.stabilized_end) < 0.0001)) {
      return;
    }
  }

  auto& chain = state.sections;
  if (cable_end) {
    chain.front().pos = *cable_end;
  }
  chain.back().pos = dispenser.pos;

  if (cable_end) {  // Create the arcline & pull the cable towards it
    state.arcline = RouteCable(dispenser, *cable_end);
  } else {
    state.arcline.reset();
  }

  Vec<Vec2> anchors;
  Vec<float> true_anchor_dir;
  if (state.arcline) {
    PopulateAnchors(anchors, true_anchor_dir, *state.arcline);
  }

  for (auto& link : chain) {
    link.acc = Vec2(0, 0);
  }

  // Dispenser pulling the chain in. The chain is pulled in when there are fewer anchors than cable
  // segments.
  bool dispenser_active = SimulateDispenser(state, dt, anchors.size());

  float numerical_anchor_dir[anchors.size()];

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
  if (Length(chain[chain.size() - 1].pos - chain[chain.size() - 2].pos) > kDistanceEpsilon &&
      chain[chain.size() - 2].distance > kDistanceEpsilon) {
    chain[chain.size() - 1].dir = atan(chain[chain.size() - 1].pos - chain[chain.size() - 2].pos);
  } else {
    chain[chain.size() - 1].dir = NormalizeAngle(dispenser.dir + M_PI);  // M_PI / 2;
  }
  if (Length(chain[1].pos - chain[0].pos) > kDistanceEpsilon &&
      chain[0].distance > kDistanceEpsilon) {
    chain[0].dir = atan(chain[1].pos - chain[0].pos);
  } else {
    chain[0].dir = M_PI / 2;
  }
  for (int i = 1; i < chain.size() - 1; i++) {
    chain[i].dir = atan(chain[i + 1].pos - chain[i - 1].pos);
  }

  // Copy over the alignment of the anchors to the chain links.
  float total_anchor_distance = 0;
  for (int i = 0; i < chain.size(); ++i) {
    int ai = anchor_i[i];
    int prev_ai = i > 0 ? anchor_i[i - 1] : -1;
    int next_ai = i < chain.size() - 1 ? anchor_i[i + 1] : -1;

    if (ai != -1 && prev_ai != -1 && next_ai != -1) {
      numerical_anchor_dir[ai] = atan(anchors[next_ai] - anchors[prev_ai]);
    } else if (ai != -1 && prev_ai != -1) {
      numerical_anchor_dir[ai] = atan(anchors[ai] - anchors[prev_ai]);
    } else if (ai != -1 && next_ai != -1) {
      numerical_anchor_dir[ai] = atan(anchors[next_ai] - anchors[ai]);
    } else if (ai != -1) {
      numerical_anchor_dir[ai] = M_PI / 2;
    }
    float true_dir_offset;
    if (ai != -1) {
      float distance_mm = Length(anchors[ai] - chain[i].pos) * 1000;
      total_anchor_distance += distance_mm;
      true_dir_offset = NormalizeAngle(true_anchor_dir[ai] - chain[i].dir);
      true_dir_offset = std::lerp(true_dir_offset, 0, std::min<float>(distance_mm, 1));
      chain[i].true_dir_offset = true_dir_offset;
    } else {
      chain[i].true_dir_offset *= expf(-dt * 10);
    }
    if (ai != -1 && prev_ai != -1) {
      chain[i].prev_dir_delta = atan(anchors[prev_ai] - anchors[ai]) - numerical_anchor_dir[ai];
    } else {
      chain[i].prev_dir_delta = M_PI;
    }
    if (ai != -1 && next_ai != -1) {
      chain[i].next_dir_delta = atan(anchors[next_ai] - anchors[ai]) - numerical_anchor_dir[ai];
    } else {
      chain[i].next_dir_delta = 0;
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
    chain.front().true_dir_offset = NormalizeAngle(M_PI / 2 - chain.front().dir);
  }
  chain.back().true_dir_offset = NormalizeAngle(dispenser.dir + M_PI - chain.back().dir);

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
      chain.front().true_dir_offset = 0;
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

      OpticalConnectorState::CableSection cN;
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

        float a_dir_offset = b.prev_dir_delta;
        float c_dir_offset = b.next_dir_delta;
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
}

Vec2 OpticalConnectorState::PlugTopCenter() const { return sections.front().pos; }

Vec2 OpticalConnectorState::PlugBottomCenter() const {
  return sections.front().pos -
         Vec2::Polar(sections.front().dir + sections.front().true_dir_offset, kCasingHeight);
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

static sk_sp<SkImage> MakeImageFromAsset(fs::VFile& asset) {
  auto& content = asset.content;
  auto data = SkData::MakeWithoutCopy(content.data(), content.size());
  auto image = SkImages::DeferredFromEncodedData(data);
  return image;
}

static sk_sp<SkImage>& CableWeaveColor() {
  static auto image =
      MakeImageFromAsset(embedded::assets_cable_weave_color_webp)->withDefaultMipmaps();
  return image;
}

static sk_sp<SkImage>& CableWeaveNormal() {
  static auto image =
      MakeImageFromAsset(embedded::assets_cable_weave_normal_webp)->withDefaultMipmaps();
  return image;
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
        int n_steps = ceil(angle / 5);
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

        int n_steps = IsConstantWidth() ? 1 : std::max<int>(1, ceil(segment_length / 0.002));
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
  float GetWidth() const override { return kCableWidth; }
  bool IsConstantWidth() const override { return true; }
};

struct StrokeToStrainReliever : StrokeToMesh {
  static constexpr float kLength = 15_mm;
  static constexpr float kTopWidth = kCableWidth + 1_mm;
  static constexpr float kBottomWidth = kCasingWidth;

  float GetWidth() const override {
    // Interpolate between kTopWidth & kBottomWidth in a sine-like fashion
    float a = length / kLength * std::numbers::pi;  // scale position to [0, pi]
    float t = cos(a) * 0.5 + 0.5;                   // map cos to [0, 1] range
    return t * kBottomWidth + (1 - t) * kTopWidth;
  }
};

static void DrawCable(DrawContext& ctx, OpticalConnectorState& state, SkPath& path) {
  auto& canvas = ctx.canvas;
  Rect clip = canvas.getLocalClipBounds();
  Rect path_bounds = path.getBounds().makeOutset(kCableWidth / 2, kCableWidth / 2);
  if (!clip.sk.intersects(path_bounds.sk)) {
    return;
  }
  // TODO: adjust the tesselation density based on the zoom level

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

      const float kCableWidth = 0.002;

      uniform shader cable_weave_color;
      uniform shader cable_weave_normal;

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

        vec2 texCoord = vec2(-angle / PI, v.uv.y / kCableWidth / 2) * 512;

        vec3 normalTanSpace = normalize(cable_weave_normal.eval(texCoord).yxz * 2 - 1 + vec3(0, 0, 0.5)); // already in tangent space
        normalTanSpace.x = -normalTanSpace.x;
        vec3 lightDirTanSpace = normalize(TBN_inv * lightDir);
        vec3 viewDirTanSpace = normalize(TBN_inv * vec3(0, 0, 1));

        vec3 normal = normalize(TBN * normalTanSpace);

        color.rgba = cable_weave_color.eval(texCoord).rgba;
        color.rgb = color.rgb * 4;
        float light = max(dot(normalTanSpace, lightDirTanSpace), 0);
        vec3 ambient = vec3(0.1, 0.1, 0.2);
        color.rgb = light * color.rgb + ambient * color.rgb;

        color.rgb += pow(length(normal.xy), 8) * vec3(0.9, 0.9, 0.9) * 0.5; // rim lighting

        color.rgb += pow(max(dot(reflect(-lightDirTanSpace, normalTanSpace), viewDirTanSpace), 0), 10) * vec3(0.4, 0.4, 0.35);
        return v.position;
      }
    )");
  auto spec_result = SkMeshSpecification::Make(
      StrokeToMesh::kAttributes, sizeof(StrokeToMesh::VertexInfo), StrokeToMesh::kVaryings, vs, fs);
  if (!spec_result.error.isEmpty()) {
    ERROR << "Error creating mesh specification: " << spec_result.error.c_str();
    return;
  }

  StrokeToCable stroke_to_cable;
  stroke_to_cable.Convert(path);

  if (stroke_to_cable.vertex_vector.empty()) {
    return;
  }

  auto vertex_buffer = stroke_to_cable.BuildBuffer();
  sk_sp<SkShader> cable_weave_color = CableWeaveColor()->makeShader(
      SkTileMode::kRepeat, SkTileMode::kRepeat,
      SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear));
  sk_sp<SkShader> cable_weave_normal = CableWeaveNormal()->makeRawShader(
      SkTileMode::kRepeat, SkTileMode::kRepeat,
      SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear));
  SkMesh::ChildPtr children[] = {cable_weave_color, cable_weave_normal};
  auto mesh_result = SkMesh::Make(spec_result.specification, SkMesh::Mode::kTriangleStrip,
                                  vertex_buffer, stroke_to_cable.vertex_vector.size(), 0, nullptr,
                                  {children, 2}, stroke_to_cable.bounds);
  if (!mesh_result.error.isEmpty()) {
    ERROR << "Error creating mesh: " << mesh_result.error.c_str();
    return;
  }
  SkPaint default_paint;
  default_paint.setColor(0xffffffff);
  default_paint.setAntiAlias(true);
  canvas.drawMesh(mesh_result.mesh, nullptr, default_paint);
}

void DrawOpticalConnector(DrawContext& ctx, OpticalConnectorState& state) {
  auto& canvas = ctx.canvas;
  auto& actx = ctx.animation_context;

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
  DrawCable(ctx, state, p);

  canvas.save();
  Vec2 cable_end = state.PlugTopCenter();
  SkMatrix transform = SkMatrix::Translate(cable_end);
  float connector_dir = state.arcline
                            ? M_PI / 2
                            : state.sections.front().dir + state.sections.front().true_dir_offset;
  transform.preRotate(connector_dir * 180 / M_PI - 90);
  transform.preTranslate(0, -kCasingHeight);
  canvas.concat(transform);

  constexpr float casing_left = -kCasingWidth / 2;
  constexpr float casing_right = kCasingWidth / 2;
  constexpr float casing_top = kCasingHeight;

  struct MeshWithUniforms {
    SkMesh mesh;
    sk_sp<SkData> uniforms;

    MeshWithUniforms(Size uniforms_size) { uniforms = SkData::MakeUninitialized(uniforms_size); }

    virtual void UpdateUniforms(SkCanvas&) = 0;

    void Draw(SkCanvas& canvas) {
      UpdateUniforms(canvas);
      SkPaint default_paint;
      default_paint.setColor(0xffffffff);
      canvas.drawMesh(mesh, nullptr, default_paint);
    }
  };

  {  // Steel insert
    static const Rect kSteelRect = Rect(-3_mm, -1_mm, 3_mm, 1_mm);

    struct SteelInsert : MeshWithUniforms {
      SteelInsert() : MeshWithUniforms(4) {}
      void UpdateUniforms(SkCanvas& canvas) override {
        float& plug_width_pixels = *(float*)uniforms->data();
        plug_width_pixels = canvas.getTotalMatrix().mapRadius(kSteelRect.Width());
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
        float plug_width_pixels = canvas.getTotalMatrix().mapRadius(kSteelRect.Width());
        result.uniforms = SkData::MakeWithCopy(&plug_width_pixels, sizeof(plug_width_pixels));
        auto vertex_buffer = SkMeshes::MakeVertexBuffer(vertex_data, sizeof(vertex_data));
        auto mesh_result =
            SkMesh::Make(spec_result.specification, SkMesh::Mode::kTriangleStrip, vertex_buffer, 4,
                         0, result.uniforms, SkSpan<SkMesh::ChildPtr>(), kSteelRect.sk);
        if (!mesh_result.error.isEmpty()) {
          ERROR << "Error creating mesh: " << mesh_result.error.c_str();
        } else {
          result.mesh = std::move(mesh_result.mesh);
          return result;
        }
      }
      return std::nullopt;
    }();
    if (mesh) {
      canvas.save();
      canvas.translate(0, 2_mm * state.steel_insert_hidden);
      mesh->Draw(canvas);
      canvas.restore();
    }
  }

  {  // Black metal casing
    struct BlackCasing : MeshWithUniforms {
      BlackCasing() : MeshWithUniforms(4) {}
      void UpdateUniforms(SkCanvas& canvas) override {
        float& plug_width_pixels = *(float*)uniforms->data();
        plug_width_pixels = canvas.getTotalMatrix().mapRadius(kCasingWidth);
      }
    };

    static Optional<BlackCasing> mesh = [&]() -> Optional<BlackCasing> {
      BlackCasing result;
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
      Varyings main(const Attributes attrs) {
        Varyings v;
        v.position = attrs.position;
        v.uv = attrs.uv;
        v.light = attrs.uv.y;
        return v;
      }
    )");
      auto fs = SkString(R"(
      const float kCaseSideRadius = 0.12;
      // NOTE: fix this once Skia supports array initializers here
      const vec3 kCaseBorderDarkColor = vec3(5) / 255; // subtle dark contour
      const vec3 kCaseBorderReflectionColor = vec3(0x36, 0x39, 0x3c) / 255; // canvas reflection
      const vec3 kCaseSideDarkColor = vec3(0x14, 0x15, 0x16) / 255; // darker metal between reflections
      const vec3 kCaseSideLightColor = vec3(0x2a, 0x2c, 0x2f) / 255; // side-light reflection
      const vec3 kCaseFrontColor = vec3(0x15, 0x16, 0x1a) / 255; // front color
      const float kBorderDarkWidth = 0.2;
      const float kCaseSideDarkH = 0.4;
      const float kCaseSideLightH = 0.8;
      const float kCaseFrontH = 1;
      const vec3 kTopLightColor = vec3(0x32, 0x34, 0x39) / 255 - kCaseFrontColor;
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
        SkRect bounds = SkRect::MakeLTRB(casing_left, casing_top, casing_right, 0);
        Vec2 vertex_data[8] = {
            Vec2(casing_left, 0),          Vec2(0, 0), Vec2(casing_right, 0),          Vec2(1, 0),
            Vec2(casing_left, casing_top), Vec2(0, 1), Vec2(casing_right, casing_top), Vec2(1, 1),
        };
        float plug_width_pixels = 1;
        result.uniforms = SkData::MakeWithCopy(&plug_width_pixels, sizeof(plug_width_pixels));
        auto vertex_buffer = SkMeshes::MakeVertexBuffer(vertex_data, sizeof(vertex_data));
        auto mesh_result =
            SkMesh::Make(spec_result.specification, SkMesh::Mode::kTriangleStrip, vertex_buffer, 4,
                         0, result.uniforms, SkSpan<SkMesh::ChildPtr>(), bounds);
        if (!mesh_result.error.isEmpty()) {
          ERROR << "Error creating mesh: " << mesh_result.error.c_str();
        } else {
          result.mesh = std::move(mesh_result.mesh);
          return result;
        }
      }
      return std::nullopt;
    }();
    if (mesh) {
      mesh->Draw(canvas);
    }
  }

  {  // Icon on the metal casing
    SkPath path = PathFromSVG(kNextShape);
    path.offset(0, 0.004);

    SkColor base_color = "#808080"_color;
    float lightness_pct = exp(-(actx.timer.now - state.last_activity).count() * 10) * 100;
    SkColor bright_light = "#fcfef7"_color;
    SkColor adjusted_color = color::AdjustLightness(base_color, lightness_pct);
    adjusted_color = color::MixColors(adjusted_color, bright_light, lightness_pct / 100);

    SkPaint icon_paint;
    icon_paint.setColor(adjusted_color);
    icon_paint.setAntiAlias(true);
    canvas.drawPath(path, icon_paint);

    // Draw blur
    if (lightness_pct > 1) {
      SkPaint glow_paint;
      glow_paint.setColor("#ef9f37"_color);
      glow_paint.setAlphaf(lightness_pct / 100);
      glow_paint.setMaskFilter(
          SkMaskFilter::MakeBlur(SkBlurStyle::kOuter_SkBlurStyle, 0.5_mm, true));
      glow_paint.setBlendMode(SkBlendMode::kScreen);
      canvas.drawPath(path, glow_paint);
    }
  }

  canvas.restore();

  {  // Rubber cable holder
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
      return;
    }

    StrokeToStrainReliever mesh_builder;
    mesh_builder.Convert(p, StrokeToStrainReliever::kLength);
    if (mesh_builder.vertex_vector.empty()) {
      // Add two points on the left & right side of the connector - just so that we can build the
      // ellipse cap.
      mesh_builder.vertex_vector.push_back({
          .coords = cable_end + Vec2(kCasingWidth / 2, 0),
          .uv = Vec2(-1, 0),
          .tangent = Vec2(0, 1),
      });
      mesh_builder.vertex_vector.push_back({
          .coords = cable_end + Vec2(-kCasingWidth / 2, 0),
          .uv = Vec2(1, 0),
          .tangent = Vec2(0, 1),
      });
    }
    // Add an ellipse cap at the end of the mesh
    if (mesh_builder.vertex_vector.size() >= 2) {
      auto& left = mesh_builder.vertex_vector[mesh_builder.vertex_vector.size() - 2];
      auto& right = mesh_builder.vertex_vector[mesh_builder.vertex_vector.size() - 1];
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
          SkMesh::Make(spec_result.specification, SkMesh::Mode::kTriangleStrip, vertex_buffer,
                       mesh_builder.vertex_vector.size(), 0, nullptr, {}, mesh_builder.bounds);
      if (!mesh_result.error.isEmpty()) {
        ERROR << "Error creating mesh: " << mesh_result.error.c_str();
      } else {
        SkPaint default_paint;
        default_paint.setColor(0xffffffff);
        default_paint.setAntiAlias(true);
        canvas.drawMesh(mesh_result.mesh, nullptr, default_paint);
      }
    }
  }

  if constexpr (kDebugCable) {  // Draw the arcline
    if (state.arcline) {
      SkPath cable_path = state.arcline->ToPath(false);
      SkPaint arcline_paint;
      arcline_paint.setColor(SK_ColorBLACK);
      arcline_paint.setAlphaf(.5);
      arcline_paint.setStrokeWidth(0.0005);
      arcline_paint.setStyle(SkPaint::kStroke_Style);
      arcline_paint.setAntiAlias(true);
      canvas.drawPath(cable_path, arcline_paint);
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
      Str i_str = ::ToStr(i);
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
  SkColor color = 0xff6e4521;
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

OpticalConnectorState::OpticalConnectorState(Location& loc, Vec2AndDir start)
    : dispenser_v(0), location(loc) {
  sections.emplace_back(CableSection{
      .pos = start.pos,
      .vel = Vec2(0, 0),
      .acc = Vec2(0, 0),
      .dir = NormalizeAngle(start.dir + M_PI),
      .true_dir_offset = 0,
      .distance = 0,
      .next_dir_delta = 0,
  });  // plug
  sections.emplace_back(CableSection{
      .pos = start.pos,
      .vel = Vec2(0, 0),
      .acc = Vec2(0, 0),
      .dir = NormalizeAngle(start.dir + M_PI),
      .true_dir_offset = 0,
      .distance = 0,
      .next_dir_delta = 0,
  });  // dispenser
  loc.next_observers.insert(this);
  steel_insert_hidden.acceleration = 400;
  steel_insert_hidden.friction = 40;
}

OpticalConnectorState::~OpticalConnectorState() { location.next_observers.erase(this); }

void OpticalConnectorState::OnNextActivated(Location& source) { last_activity = time::SystemNow(); }

}  // namespace automat::gui