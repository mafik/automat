#include "connector_optical.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkMaskFilter.h>
#include <include/effects/SkGradientShader.h>
#include <include/effects/SkRuntimeEffect.h>

#include <cmath>

#include "arcline.hh"
#include "font.hh"
#include "math.hh"
#include "svg.hh"

using namespace maf;

namespace automat::gui {

constexpr float kCasingWidth = 0.008;
constexpr float kCasingHeight = 0.008;
constexpr bool kDebugCable = false;
constexpr float kStep = 0.005;
constexpr float kCrossSize = 0.001;

ArcLine RouteCable(Vec2 start, Vec2 cable_end) {
  ArcLine cable = ArcLine(start, M_PI * 1.5);
  Vec2 cable_middle = (start + cable_end) / 2;
  Vec2 delta = cable_middle - start;
  float distance = Length(delta);
  float turn_radius = std::max<float>(distance / 4, 0.01);

  auto horizontal_shift = ArcLine::TurnShift(delta.x * 2, turn_radius);
  float move_down = -delta.y - horizontal_shift.distance_forward / 2;
  if (move_down < 0) {
    auto vertical_shift =
        ArcLine::TurnShift(cable_end.x < start.x ? move_down * 2 : -move_down * 2, turn_radius);

    float move_side = (horizontal_shift.move_between_turns - vertical_shift.distance_forward) / 2;
    if (move_side < 0) {
      // If there is not enough space to route the cable in the middle, we will route it around the
      // objects.
      float x = start.x;
      float y = start.y;
      float dir;
      if (start.x > cable_end.x) {
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

// This function walks along the given arcline (from the end to its start) and adds
// an anchor every kStep distance. It populates the `anchors` and `anchor_tangents` vectors.
static void PopulateAnchors(Vec<Vec2>& anchors, Vec<float>& anchor_tangents,
                            const ArcLine& arcline) {
  auto it = ArcLine::Iterator(arcline);
  Vec2 dispenser = it.Position();
  float cable_length = it.AdvanceToEnd();
  Vec2 tail = it.Position();

  anchors.push_back(tail);
  anchor_tangents.push_back(M_PI / 2);
  for (float cable_pos = kStep; cable_pos < cable_length; cable_pos += kStep) {
    it.Advance(-kStep);
    anchors.push_back(it.Position());
    anchor_tangents.push_back(it.Angle() + M_PI);
  }
  anchors.push_back(dispenser);
  anchor_tangents.push_back(M_PI / 2);
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
      auto desired_dist = state.sections[state.sections.size() - 2].distance;
      if (current_dist > kStep) {
        state.sections[state.sections.size() - 2].distance = kStep;
        auto new_it = state.sections.insert(
            state.sections.begin() + state.sections.size() - 1,
            OpticalConnectorState::CableSection{
                .pos = state.sections[state.sections.size() - 2].pos - delta / current_dist * kStep,
                .vel = Vec2(0, 0),
                .acc = Vec2(0, 0),
                .distance = current_dist - kStep,
            });
      } else {
        break;
      }
    } while (state.sections.size() < anchor_count);
  }

  return pulling;
}

void SimulateCablePhysics(float dt, OpticalConnectorState& state, Vec2 start, Optional<Vec2> end) {
  Optional<Vec2> cable_end;
  if (end) {
    cable_end = Vec2(end->x, end->y + kCasingHeight);
  }

  auto& dispenser = start;
  auto& chain = state.sections;
  if (cable_end) {
    chain.front().pos = *cable_end;
  }
  chain.back().pos = start;

  for (auto& link : chain) {
    link.acc = Vec2(0, 0);
  }

  Vec<Vec2> anchors;
  Vec<float> anchor_tangents;

  if (cable_end) {  // Create the arcline & pull the cable towards it
    state.arcline = RouteCable(start, *cable_end);
    PopulateAnchors(anchors, anchor_tangents, *state.arcline);
  } else {
    state.arcline.reset();
  }

  // Dispenser pulling the chain in. The chain is pulled in when there are fewer anchors than cable
  // segments.
  bool dispenser_active = SimulateDispenser(state, dt, anchors.size());

  float anchor_dir[anchors.size()];
  if (anchors.size() > 0) {
    anchor_dir[0] = M_PI / 2;
    anchor_dir[anchors.size() - 1] = M_PI / 2;
  }

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

  // Copy over the alignment of the anchors to the chain links.
  for (int i = 0; i < chain.size(); ++i) {
    int ai = anchor_i[i];
    int prev_ai = i > 0 ? anchor_i[i - 1] : -1;
    int next_ai = i < chain.size() - 1 ? anchor_i[i + 1] : -1;
    if (ai != -1 && prev_ai != -1 && next_ai != -1) {
      anchor_dir[ai] = atan(anchors[next_ai] - anchors[prev_ai]);
    } else if (ai != -1) {
      anchor_dir[ai] = M_PI / 2;
    }
    if (ai != -1 && prev_ai != -1) {
      chain[i].prev_dir_delta = atan(anchors[prev_ai] - anchors[ai]) - anchor_dir[ai];
    } else {
      chain[i].prev_dir_delta = M_PI;
    }
    if (ai != -1 && next_ai != -1) {
      chain[i].next_dir_delta = atan(anchors[next_ai] - anchors[ai]) - anchor_dir[ai];
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

  // Move chain links towards anchors (more at the end of the cable)
  for (int i = 0; i < chain.size(); i++) {
    int ai = anchor_i[i];
    if (ai == -1) {
      continue;
    }
    // LERP the cable section towards its anchor point. More at the end of the cable.
    Vec2 new_pos =
        chain[i].pos + (anchors[ai] - chain[i].pos) * expf(-dt * 6000.0f * i / anchors.size());
    chain[i].vel += (new_pos - chain[i].pos) / dt;
    chain[i].pos = new_pos;

    // Also apply a force towards the anchor. This is unrelated to LERP-ing above.
    chain[i].acc += (anchors[ai] - chain[i].pos) * 3e2;
  }

  if (cable_end) {
    chain[0].dir = M_PI / 2;
  } else {
    if (Length(chain[1].pos - chain[0].pos) > 1e-6) {
      chain[0].dir = atan(chain[1].pos - chain[0].pos);
    } else {
      chain[0].dir = M_PI / 2;
    }
  }
  for (int i = 1; i < chain.size() - 1; i++) {
    chain[i].dir = atan(chain[i + 1].pos - chain[i - 1].pos);
  }
  chain[chain.size() - 1].dir = M_PI / 2;

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
    } else {
      chain.front().vel *= expf(-20 * dt);
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
      chain.back().pos = dispenser;

      OpticalConnectorState::CableSection a0, cN;
      a0.pos = chain[0].pos - Vec2::Polar(chain.front().dir, kStep);
      a0.distance = kStep;
      a0.next_dir_delta = 0;
      chain.back().distance = kStep;
      cN.pos = chain.back().pos + Vec2::Polar(chain.back().dir, kStep);
      cN.prev_dir_delta = M_PI;

      int start = 0;
      int end = chain.size();
      int inc = 1;
      if (iter % 2) {
        start = chain.size() - 1;
        end = -1;
        inc = -1;
      }

      for (int i = start; i != end; i += inc) {
        auto& a = i == 0 ? a0 : chain[i - 1];
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
      chain.back().pos = dispenser;
    }
  }
}

Vec2 OpticalConnectorState::PlugTopCenter() const { return sections.front().pos; }

Vec2 OpticalConnectorState::PlugBottomCenter() const {
  return sections.front().pos - Vec2::Polar(sections.front().dir, kCasingHeight);
}

void DrawOpticalConnector(DrawContext& ctx, OpticalConnectorState& state) {
  auto& canvas = ctx.canvas;
  auto& actx = ctx.animation_context;

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

  SkPaint cross_paint;
  cross_paint.setColor(0xffff8800);
  cross_paint.setAntiAlias(true);
  cross_paint.setStrokeWidth(0.0005);
  cross_paint.setStyle(SkPaint::kStroke_Style);

  auto& font = GetFont();
  auto& chain = state.sections;

  if constexpr (kDebugCable) {  // Draw the chain as a series of straight lines
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

  if constexpr (!kDebugCable) {  // Draw the chain as a bezier curve
    SkPaint cable_paint;
    cable_paint.setStyle(SkPaint::kStroke_Style);
    cable_paint.setStrokeWidth(0.002);
    cable_paint.setAntiAlias(true);
    cable_paint.setColor(0xff111111);

    SkPath p;
    p.moveTo(chain[0].pos);
    for (int i = 1; i < chain.size(); i++) {
      Vec2 p1 = chain[i - 1].pos + Vec2::Polar(chain[i - 1].dir, kStep / 3);
      Vec2 p2 = chain[i].pos - Vec2::Polar(chain[i].dir, kStep / 3);
      p.cubicTo(p1, p2, chain[i].pos);
    }
    p.setIsVolatile(true);
    canvas.drawPath(p, cable_paint);
    SkPaint cable_paint2;
    cable_paint2.setStyle(SkPaint::kStroke_Style);
    cable_paint2.setStrokeWidth(0.002);
    cable_paint2.setAntiAlias(true);
    cable_paint2.setColor(0xff444444);
    cable_paint2.setMaskFilter(
        SkMaskFilter::MakeBlur(SkBlurStyle::kInner_SkBlurStyle, 0.0005, true));
    canvas.drawPath(p, cable_paint2);
  }

  canvas.save();
  Vec2 cable_end = state.PlugTopCenter();
  SkMatrix transform = SkMatrix::Translate(cable_end);
  transform.preRotate(state.sections.front().dir * 180 / M_PI - 90);
  transform.preTranslate(0, -kCasingHeight);
  canvas.concat(transform);

  float casing_left = -kCasingWidth / 2;
  float casing_right = kCasingWidth / 2;
  float casing_top = kCasingHeight;

  {  // Black metal casing
    SkPaint black_metal_paint;
    SkPoint pts[2] = {Vec2(-0.004, 0), Vec2(0.004, 0)};
    SkColor colors[5] = {0xff626262, 0xff000000, 0xff181818, 0xff0d0d0d, 0xff5e5e5e};
    float pos[5] = {0, 0.1, 0.5, 0.9, 1};
    sk_sp<SkShader> gradient =
        SkGradientShader::MakeLinear(pts, colors, pos, 5, SkTileMode::kClamp);
    black_metal_paint.setShader(gradient);
    SkRect black_metal_rect = SkRect::MakeLTRB(-0.004, 0, +0.004, +0.008);
    canvas.drawRect(black_metal_rect, black_metal_paint);
  }

  {  // Steel insert
    SkRect steel_rect = SkRect::MakeLTRB(-0.003, -0.001, 0.003, 0);

    // Fill with black - this will only stay around borders
    SkPaint black;
    black.setColor(0xff000000);
    canvas.drawRect(steel_rect, black);

    // Fill with steel-like gradient
    SkPaint steel_paint;
    SkPoint pts[2] = {Vec2(-0.003, 0), Vec2(0.003, 0)};
    SkColor colors[2] = {0xffe6e6e6, 0xff949494};
    sk_sp<SkShader> gradient =
        SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
    steel_paint.setShader(gradient);
    steel_paint.setMaskFilter(
        SkMaskFilter::MakeBlur(SkBlurStyle::kInner_SkBlurStyle, 0.0001, true));
    steel_paint.setColor(0xff000000);
    canvas.drawRect(steel_rect, steel_paint);
  }

  {  // Rubber cable holder
    constexpr float kRubberWidth = 0.003;
    constexpr float kRubberHeight = 0.015;
    constexpr float kLowerCpOffset = kRubberHeight * 0.3;
    constexpr float kUpperCpOffset = kRubberHeight * 0.6;
    constexpr float kTopCpOffset = kRubberWidth * 0.2;

    Vec2 pts[6];
    Vec2& left = pts[0];
    Vec2& left_cp1 = pts[1];
    Vec2& left_cp2 = pts[2];
    Vec2& right = pts[3];
    Vec2& right_cp1 = pts[4];
    Vec2& right_cp2 = pts[5];
    int sleeve_i = 3;
    SkMatrix inverse;
    if (sleeve_i < state.sections.size() && transform.invert(&inverse)) {
      auto& p = state.sections[sleeve_i];
      Vec2 side_offset = Vec2::Polar(p.dir + M_PI / 2, kRubberWidth / 2);
      Vec2 upper_cp_offset = Vec2::Polar(p.dir + M_PI, kUpperCpOffset);
      Vec2 top_cp_offset = Vec2::Polar(p.dir, kTopCpOffset);
      left = p.pos + side_offset;
      left_cp1 = left + upper_cp_offset;
      left_cp2 = left + top_cp_offset;
      right = p.pos - side_offset;
      right_cp1 = right + top_cp_offset;
      right_cp2 = right + upper_cp_offset;
      inverse.mapPoints(&pts[0].sk, 6);
    } else {
      float sleeve_left = -kRubberWidth / 2;
      float sleeve_right = kRubberWidth / 2;
      float sleeve_top = kCasingHeight + kRubberHeight;
      left = Vec2(sleeve_left, sleeve_top);
      left_cp1 = Vec2(sleeve_left, sleeve_top - kUpperCpOffset);
      left_cp2 = Vec2(sleeve_left, sleeve_top + kTopCpOffset);
      right = Vec2(sleeve_right, sleeve_top);
      right_cp1 = Vec2(sleeve_right, sleeve_top + kTopCpOffset);
      right_cp2 = Vec2(sleeve_right, sleeve_top - kUpperCpOffset);
    }
    Vec2 bottom_left = Vec2(casing_left, casing_top);
    Vec2 bottom_left_cp = bottom_left + Vec2(0, kLowerCpOffset);
    Vec2 bottom_right = Vec2(casing_right, casing_top);
    Vec2 bottom_right_cp = bottom_right + Vec2(0, kLowerCpOffset);
    SkPath rubber_path;
    rubber_path.moveTo(bottom_left);                      // bottom left
    rubber_path.cubicTo(bottom_left_cp, left_cp1, left);  // upper left
    rubber_path.cubicTo(left_cp2, right_cp1, right);      // upper right
    rubber_path.cubicTo(right_cp2, bottom_right_cp,
                        bottom_right);  // bottom right
    rubber_path.close();

    SkPaint dark_flat;
    dark_flat.setAntiAlias(true);
    dark_flat.setColor(0xff151515);
    canvas.drawPath(rubber_path, dark_flat);

    SkPaint lighter_inside;
    lighter_inside.setAntiAlias(false);
    lighter_inside.setMaskFilter(
        SkMaskFilter::MakeBlur(SkBlurStyle::kInner_SkBlurStyle, 0.0010, true));
    lighter_inside.setColor(0xff2a2a2a);
    canvas.drawPath(rubber_path, lighter_inside);
  }

  {  // Icon on the metal casing
    SkPath path = PathFromSVG(kNextShape);
    path.offset(0, 0.004);
    SkPaint icon_paint;
    icon_paint.setColor(0xff808080);
    icon_paint.setAntiAlias(true);
    canvas.drawPath(path, icon_paint);
  }

  canvas.restore();
}

/*
// This function has some nice code for drawing connections between rounded rectangles.
// Keeping this for potential usage in the future
void DrawConnection(SkCanvas& canvas, const SkPath& from_shape, const SkPath& to_shape) {
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
  SkVector delta = to - from;
  float degrees = 180 * std::atan2(delta.y(), delta.x()) / std::numbers::pi;
  float end = delta.length();
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
*/

OpticalConnectorState::OpticalConnectorState(Vec2 start) : dispenser_v(0) {
  sections.emplace_back(CableSection{
      .pos = start,
      .vel = Vec2(0, 0),
      .acc = Vec2(0, 0),
      .dir = M_PI / 2,
      .distance = 0,
      .next_dir_delta = 0,
  });  // plug
  sections.emplace_back(CableSection{
      .pos = start,
      .vel = Vec2(0, 0),
      .acc = Vec2(0, 0),
      .dir = M_PI / 2,
      .distance = 0,
      .next_dir_delta = 0,
  });  // dispenser
}
}  // namespace automat::gui