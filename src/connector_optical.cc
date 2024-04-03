#include "connector_optical.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkMaskFilter.h>
#include <include/effects/SkGradientShader.h>

#include "arcline.hh"
#include "log.hh"
#include "svg.hh"

using namespace maf;

namespace automat::gui {

constexpr float kCasingWidth = 0.008;
constexpr float kCasingHeight = 0.008;

ArcLine RouteCable(Vec2 start, Vec2 end) {
  ArcLine cable = ArcLine(start, M_PI * 1.5);
  float casing_top = end.y + kCasingHeight;
  Vec2 cable_end = Vec2(end.x, casing_top);
  Vec2 cable_middle = (start + cable_end) / 2;
  Vec2 delta = cable_middle - start;
  float distance = Length(delta);
  float turn_radius = std::max<float>(distance / 4, 0.01);

  auto horizontal_shift = ArcLine::TurnShift(delta.x * 2, turn_radius);
  float move_down = -delta.y - horizontal_shift.distance_forward / 2;
  if (move_down < 0) {
    auto vertical_shift =
        ArcLine::TurnShift(end.x < start.x ? move_down * 2 : -move_down * 2, turn_radius);

    float move_side = (horizontal_shift.move_between_turns - vertical_shift.distance_forward) / 2;
    if (move_side < 0) {
      // If there is not enough space to route the cable in the middle, we will route it around the
      // objects.
      float x = start.x;
      float y = start.y;
      float dir;
      if (start.x > end.x) {
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

void DrawOpticalConnector(DrawContext& ctx, OpticalConnectorState& state, Vec2 start, Vec2 end) {
  auto& canvas = ctx.canvas;
  auto& actx = ctx.animation_context;

  float casing_left = end.x - kCasingWidth / 2;
  float casing_right = end.x + kCasingWidth / 2;
  float casing_top = end.y + kCasingHeight;

  SkPaint cable_paint;
  cable_paint.setColor(SK_ColorBLACK);
  cable_paint.setAlphaf(.5);
  cable_paint.setStrokeWidth(0.0005);
  cable_paint.setStyle(SkPaint::kStroke_Style);
  cable_paint.setAntiAlias(true);

  ArcLine cable = RouteCable(start, end);

  auto cable_path = cable.ToPath(false);

  {  // Black metal casing
    SkPaint black_metal_paint;
    SkPoint pts[2] = {end + Vec2(-0.004, 0), end + Vec2(0.004, 0)};
    SkColor colors[5] = {0xff626262, 0xff000000, 0xff181818, 0xff0d0d0d, 0xff5e5e5e};
    float pos[5] = {0, 0.1, 0.5, 0.9, 1};
    sk_sp<SkShader> gradient =
        SkGradientShader::MakeLinear(pts, colors, pos, 5, SkTileMode::kClamp);
    black_metal_paint.setShader(gradient);
    SkRect black_metal_rect = SkRect::MakeLTRB(end.x - 0.004, end.y, end.x + 0.004, end.y + 0.008);
    canvas.drawRect(black_metal_rect, black_metal_paint);
  }

  {  // Steel insert
    SkRect steel_rect = SkRect::MakeLTRB(end.x - 0.003, end.y - 0.001, end.x + 0.003, end.y);

    // Fill with black - this will only stay around borders
    SkPaint black;
    black.setColor(0xff000000);
    canvas.drawRect(steel_rect, black);

    // Fill with steel-like gradient
    SkPaint steel_paint;
    SkPoint pts[2] = {end + Vec2(-0.003, 0), end + Vec2(0.003, 0)};
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
    constexpr float kRubberWidth = 0.002;
    constexpr float kRubberHeight = 0.016;
    constexpr float kLowerCpOffset = kRubberHeight * 0.3;
    constexpr float kUpperCpOffset = kRubberHeight * 0.7;
    constexpr float kTopCpOffset = kRubberWidth * 0.2;

    float sleeve_left = end.x - kRubberWidth / 2;
    float sleeve_right = end.x + kRubberWidth / 2;
    float sleeve_top = casing_top + kRubberHeight;
    SkPath rubber_path;
    rubber_path.moveTo(casing_left, casing_top);
    rubber_path.cubicTo(casing_left, casing_top + kLowerCpOffset, sleeve_left,
                        sleeve_top - kUpperCpOffset, sleeve_left, sleeve_top);
    rubber_path.cubicTo(sleeve_left, sleeve_top + kTopCpOffset, sleeve_right,
                        sleeve_top + +kTopCpOffset, sleeve_right, sleeve_top);
    rubber_path.cubicTo(sleeve_right, sleeve_top - kUpperCpOffset, casing_right,
                        casing_top + kLowerCpOffset, casing_right, casing_top);
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
    path.offset(end.x, end.y + 0.004);
    SkPaint icon_paint;
    icon_paint.setColor(0xff808080);
    icon_paint.setAntiAlias(true);
    canvas.drawPath(path, icon_paint);
  }

  canvas.drawPath(cable_path, cable_paint);

  auto it = ArcLine::Iterator(cable);
  float cable_length = it.AdvanceToEnd();

  SkPaint cross_paint;
  cross_paint.setColor(0xffff8800);
  cross_paint.setAntiAlias(true);
  cross_paint.setStrokeWidth(0.0005);
  cross_paint.setStyle(SkPaint::kStroke_Style);

  float desired_advance = 0.005;
  while (true) {
    Vec2 pos = it.Position();
    constexpr float kCrossSize = 0.0005;
    SkPath cross;
    cross.moveTo(pos.x - kCrossSize, pos.y - kCrossSize);
    cross.lineTo(pos.x + kCrossSize, pos.y + kCrossSize);
    cross.moveTo(pos.x + kCrossSize, pos.y - kCrossSize);
    cross.lineTo(pos.x - kCrossSize, pos.y + kCrossSize);
    canvas.drawPath(cross, cross_paint);
    float actual_advance = -it.Advance(-desired_advance);
    if (desired_advance - actual_advance > 1e-5) {
      pos = it.Position();
      canvas.drawCircle(pos.x, pos.y, kCrossSize, cross_paint);
      break;
    }
  }
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

}  // namespace automat::gui