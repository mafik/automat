// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_timer.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPathUtils.h>
#include <include/effects/SkGradientShader.h>
#include <include/pathops/SkPathOps.h>

#include <cmath>
#include <memory>

#include "animation.hh"
#include "argument.hh"
#include "base.hh"
#include "drag_action.hh"
#include "font.hh"
#include "library_macros.hh"
#include "math.hh"
#include "number_text_field.hh"
#include "pointer.hh"
#include "status.hh"
#include "tasks.hh"
#include "time.hh"

using namespace maf;

namespace automat::library {

using time::Duration;
using time::SteadyClock;
using time::SteadyPoint;

DEFINE_PROTO(TimerDelay);

static constexpr float kOuterRadius = 0.02;
static constexpr SkRect kOuterOval =
    SkRect::MakeXYWH(-kOuterRadius, -kOuterRadius, 2 * kOuterRadius, 2 * kOuterRadius);
static constexpr float kSoftEdgeWidth = 0.0005;

constexpr static float r0 = kOuterRadius;
constexpr static float r1 = kOuterRadius - kSoftEdgeWidth;
constexpr static float r2 = r1 - 3 * kSoftEdgeWidth;
constexpr static float r3 = r2 - kSoftEdgeWidth;
constexpr static float r4 = r3 - kSoftEdgeWidth;  // outer edge of white watch face
constexpr static float r4_b = r4 * 0.9;
constexpr static float r5 = kSoftEdgeWidth * 3;
constexpr static float r6 = r5 - kSoftEdgeWidth;
constexpr static float kTextWidth = r4;

static constexpr SkRect kDialOval =
    SkRect::MakeXYWH(-r4, -r4, r4 * 2, r4 * 2);  // outer edge of dial
constexpr static float kTickOuterRadius = r4 * 0.95;
constexpr static float kTickMajorLength = r4 * 0.05;
constexpr static float kTickMinorLength = r4 * 0.025;

static constexpr time::Duration kHandPeriod = 0.1s;

void TimerDelay::OnTimerNotification(Location& here2, time::SteadyPoint) { Done(here2); }

// How long it takes for the timer dial to rotate once.
static Duration RangeDuration(TimerDelay::Range range) {
  switch (range) {
    case TimerDelay::Range::Milliseconds:
      return 1s;
    case TimerDelay::Range::Seconds:
      return 60s;
    case TimerDelay::Range::Minutes:
      return 60min;
    case TimerDelay::Range::Hours:
      return 12h;
    case TimerDelay::Range::Days:
      return 7 * 24h;
    default:
      return 1s;
  }
}

static int TickCount(TimerDelay::Range range) {
  switch (range) {
    case TimerDelay::Range::Milliseconds:
      return 1000;
    case TimerDelay::Range::Seconds:
      return 60;
    case TimerDelay::Range::Minutes:
      return 60;
    case TimerDelay::Range::Hours:
      return 12;
    case TimerDelay::Range::Days:
      return 7;
    default:
      return 100;
  }
}

static int MajorTickCount(TimerDelay::Range range) {
  switch (range) {
    case TimerDelay::Range::Milliseconds:
      return 10;
    case TimerDelay::Range::Seconds:
      return 12;
    case TimerDelay::Range::Minutes:
      return 12;
    case TimerDelay::Range::Hours:
      return 4;
    case TimerDelay::Range::Days:
      return 7;
    default:
      return 10;
  }
}

static const char* RangeName(TimerDelay::Range range) {
  switch (range) {
    case TimerDelay::Range::Milliseconds:
      return "milliseconds";
    case TimerDelay::Range::Seconds:
      return "seconds";
    case TimerDelay::Range::Minutes:
      return "minutes";
    case TimerDelay::Range::Hours:
      return "hours";
    case TimerDelay::Range::Days:
      return "days";
    default:
      return "???";
  }
}

static void UpdateTextField(TimerDelay& timer) {
  auto n =
      timer.duration.value.count() / RangeDuration(timer.range).count() * TickCount(timer.range);
  timer.text_field->SetNumber(n);
}

static void SetDuration(TimerDelay& timer, Duration new_duration) {
  if (IsRunning(timer)) {
    if (auto h = timer.here.lock()) {
      RescheduleAt(*h, timer.start_time + timer.duration.value, timer.start_time + new_duration);
    }
  }

  timer.duration.value = new_duration;
  UpdateTextField(timer);
}

static void PropagateDurationOutwards(TimerDelay& timer) {
  if (auto h = timer.here.lock()) {
    NoSchedulingGuard guard(*h);
    auto duration_obj = timer.duration_arg.GetLocation(*h);
    if (duration_obj.ok && duration_obj.location) {
      duration_obj.location->SetNumber(timer.duration.value.count() * TickCount(timer.range) /
                                       RangeDuration(timer.range).count());
    }
  }
}

TimerDelay::TimerDelay() : text_field(std::make_shared<gui::NumberTextField>(kTextWidth)) {
  range_dial.velocity = 0;
  range_dial.value = 1;

  duration_handle_rotation.speed = 100;
  text_field->argument = &duration_arg;
  duration_arg.field = &duration;
  SetDuration(*this, 10s);
}

TimerDelay::TimerDelay(const TimerDelay& other) : TimerDelay() {
  range_dial.velocity = other.range_dial.velocity;
  range_dial.value = other.range_dial.value;
  SetDuration(*this, other.duration.value);
}

string_view TimerDelay::Name() const { return "Delay"; }

std::shared_ptr<Object> TimerDelay::Clone() const { return std::make_shared<TimerDelay>(*this); }

static sk_sp<SkShader> MakeGradient(SkPoint a, SkPoint b, SkColor color_a, SkColor color_b) {
  SkPoint pts[2] = {a, b};
  SkColor colors[2] = {color_a, color_b};
  return SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kMirror);
}

enum DrawRingMode {
  kRingAliased,
  kRingAntiAliased,
  kRingBlurred,
  kRingInset,
};

static void DrawRing(SkCanvas& canvas, float outer_r, float inner_r, SkColor top_left,
                     SkColor bottom_right, DrawRingMode mode = kRingAntiAliased) {
  SkPaint paint;
  SkPoint top_left_pos = SkPoint::Make(-outer_r * M_SQRT2 / 2, outer_r * M_SQRT2 / 2);
  SkPoint bottom_right_pos = SkPoint::Make(outer_r * M_SQRT2 / 2, -outer_r * M_SQRT2 / 2);
  paint.setShader(MakeGradient(top_left_pos, bottom_right_pos, top_left, bottom_right));
  if (mode == kRingAntiAliased) {
    paint.setAntiAlias(true);
  }
  // paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.0001f, true));
  float radius;
  if (mode == kRingBlurred) {
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, (outer_r - inner_r) / 4));
  }
  if (mode == kRingInset) {
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, (outer_r - inner_r) / 4));
    canvas.save();
    canvas.clipRRect(
        SkRRect::MakeOval(SkRect::MakeXYWH(-outer_r, -outer_r, outer_r * 2, outer_r * 2)), true);
    outer_r += (outer_r - inner_r);
  }
  if (inner_r > 0) {
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(outer_r - inner_r);
    radius = (outer_r + inner_r) / 2;
  } else {
    paint.setStyle(SkPaint::kFill_Style);
    radius = outer_r;
  }
  canvas.drawCircle(SkPoint::Make(0, 0), radius, paint);
  if (mode == kRingInset) {
    canvas.restore();
  }
}

constexpr static float kHandWidth = 0.0004;
constexpr static float kHandLength = r4 * 0.8;

static float HandBaseDegrees(const TimerDelay& timer) {
  if (IsRunning(timer)) {
    Duration elapsed = SteadyClock::now() - timer.start_time;
    return 90 - 360 * elapsed.count() / RangeDuration(timer.range).count();
  } else {
    return 90;
  }
}

static SkPath HandPath(const TimerDelay& timer) {
  float base_degrees = HandBaseDegrees(timer);
  float end_degrees = timer.hand_degrees.value;
  float twist_degrees = end_degrees - base_degrees;
  animation::WrapModulo(twist_degrees, 0, 360);

  if (fabs(twist_degrees) < 1) {
    SkPath path;
    auto end_point = SkMatrix::RotateDeg(end_degrees).mapXY(kHandLength, 0);
    path.lineTo(end_point);
    return path;
  }

  float R = kHandLength / ((twist_degrees / 360) * 2 * M_PI);

  SkPath path;

  auto end_point =
      SkMatrix::Translate(R, 0).postRotate(twist_degrees).postTranslate(-R, 0).mapXY(0, 0);

  path.rArcTo(R, R, 0, SkPath::kSmall_ArcSize,
              twist_degrees > 0 ? SkPathDirection::kCW : SkPathDirection::kCCW, end_point.x(),
              end_point.y());
  path.transform(SkMatrix::RotateDeg(base_degrees - 90));
  return path;
}

const static SkPaint kHandPaint = []() {
  SkPaint paint;
  paint.setColor(0xffd93f2a);
  paint.setAntiAlias(true);
  paint.setStrokeWidth(kHandWidth);
  paint.setStyle(SkPaint::kStroke_Style);
  return paint;
}();

static void DrawHand(gui::DrawContext& ctx, const TimerDelay& timer) {
  auto& canvas = ctx.canvas;

  SkPath path = HandPath(timer);

  canvas.save();
  canvas.translate(0.001, -0.001);
  SkPaint shadow_paint;
  shadow_paint.setColor(0xff46464d);
  shadow_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.0005f, true));
  shadow_paint.setStyle(SkPaint::kStroke_Style);
  shadow_paint.setStrokeWidth(kHandWidth);
  canvas.drawPath(path, shadow_paint);
  canvas.restore();

  canvas.drawPath(path, kHandPaint);
}

constexpr static float kStartPusherAxleWidth = 0.003;
constexpr static float kStartPusherAxleLength = 0.001;
constexpr static SkRect kStartPusherAxleBox =
    SkRect::MakeXYWH(-kStartPusherAxleWidth / 2, kOuterRadius - 0.0003, kStartPusherAxleWidth,
                     kStartPusherAxleLength + 0.0006);
static SkRRect kStartPusherAxle =
    SkRRect::MakeRectXY(kStartPusherAxleBox, kStartPusherAxleWidth / 2, 0.0002);

constexpr static float kStartPusherWidth = 0.005;
constexpr static float kStartPusherHeight = 0.004;
constexpr static SkRect kStartPusherBox =
    SkRect::MakeXYWH(-kStartPusherWidth / 2, kOuterRadius + kStartPusherAxleLength,
                     kStartPusherWidth, kStartPusherHeight);
static SkRRect kStartPusher =
    SkRRect::MakeRectXY(kStartPusherBox, kStartPusherWidth / 2, kStartPusherHeight / 12);

constexpr static float kSmallAxleWidth = 0.002;
constexpr static float kSmallAxleLength = 0.001;
constexpr static SkRect kSmallAxleBox = SkRect::MakeXYWH(
    -kSmallAxleWidth / 2, kOuterRadius - 0.0003, kSmallAxleWidth, kSmallAxleLength + 0.0006);
static SkRRect kSmallAxle = SkRRect::MakeRectXY(kSmallAxleBox, kSmallAxleWidth / 2, 0.0002);

constexpr static float kSmallPusherWidth = 0.004;
constexpr static float kSmallPusherHeight = 0.002;
constexpr static SkRect kSmallPusherBox = SkRect::MakeXYWH(
    -kSmallPusherWidth / 2, kOuterRadius + kSmallAxleLength, kSmallPusherWidth, kSmallPusherHeight);
static SkRRect kSmallPusher = SkRRect::MakeRectXY(kSmallPusherBox, 0.0002, 0.0002);

static void DrawPusher(SkCanvas& canvas, const SkRRect& axle, const SkRRect& pusher,
                       SkColor pusher_gradient[3]) {
  SkPath pusher_axle_path = SkPath::RRect(axle);

  SkPaint pusher_axle_paint;
  pusher_axle_paint.setAntiAlias(true);
  SkPoint axle_pts[2] = {{axle.rect().fLeft, 0}, {axle.rect().fRight, 0}};
  SkColor axle_colors[] = {0xffb0b0b0, 0xff383739, 0xffa8a9ab, 0xff000000, 0xff4d4b4f};
  float positions[] = {0, 0.1, 0.3, 0.6, 1};
  pusher_axle_paint.setShader(
      SkGradientShader::MakeLinear(axle_pts, axle_colors, positions, 5, SkTileMode::kClamp));

  canvas.drawPath(pusher_axle_path, pusher_axle_paint);

  SkPath pusher_path = SkPath::RRect(pusher);

  SkPaint pusher_paint;
  SkPoint btn_pts[2] = {{0, 0}, {0.0004, 0}};
  SkColor btn_colors[] = {0xff707070, 0xff303030, 0xff000000};
  float btn_positions[] = {0, 0.3, 1};
  pusher_paint.setShader(
      SkGradientShader::MakeLinear(btn_pts, btn_colors, btn_positions, 3, SkTileMode::kMirror));
  pusher_paint.setAntiAlias(true);
  canvas.drawPath(pusher_path, pusher_paint);

  SkPoint btn_pts2[2] = {{pusher.rect().fLeft, 0}, {pusher.rect().fRight, 0}};

  pusher_paint.setShader(
      SkGradientShader::MakeLinear(btn_pts2, pusher_gradient, nullptr, 3, SkTileMode::kClamp));
  canvas.drawPath(pusher_path, pusher_paint);
}

SkPaint kDurationPaint = []() {
  SkPaint paint;
  paint.setColor(0xff23a9f2);
  paint.setAntiAlias(true);
  return paint;
}();

static SkPoint DurationHandlePos(const TimerDelay& timer) {
  float s = sin(timer.duration_handle_rotation);
  float c = cos(timer.duration_handle_rotation);
  return SkPoint::Make(c * r3, s * r3);
}

static SkPath DurationHandlePath(const TimerDelay& timer) {
  static const SkPath path = []() {
    SkPath path;
    path.moveTo(kTickOuterRadius, 0);
    float start_angle = atan2(0.001, r4) / M_PI * 180;
    float handle_angle = 20;
    path.arcTo(kDialOval, start_angle, handle_angle / 2 - start_angle, false);
    path.arcTo(kOuterOval.makeOutset(0.0005, 0.0005), handle_angle / 2, -handle_angle, false);
    path.arcTo(kDialOval, -handle_angle / 2, handle_angle / 2 - start_angle, false);
    path.close();
    return path;
  }();
  return path.makeTransform(SkMatrix::RotateRad(timer.duration_handle_rotation));
}

static void DrawDial(SkCanvas& canvas, TimerDelay::Range range, time::Duration duration) {
  int range_max = TickCount(range);
  int tick_count = TickCount(range);
  int major_tick_count = MajorTickCount(range);
  // Draw duration
  float duration_angle = -duration.count() / RangeDuration(range).count() * 360;
  if (duration_angle < -360) {
    duration_angle = -360;
  }
  canvas.drawArc(SkRect::MakeXYWH(-r4, -r4, r4 * 2, r4 * 2), 90, duration_angle, true,
                 kDurationPaint);

  // Draw ticks
  SkPaint tick_paint;
  tick_paint.setColor(0xff121215);
  tick_paint.setAntiAlias(true);
  float circumference = 2 * M_PI * r4;
  float minor_tick_w = std::min<float>(circumference / tick_count / 2, 0.0003);
  float major_tick_w = 2 * minor_tick_w;
  SkRect major_tick = SkRect::MakeXYWH(-major_tick_w / 2, kTickOuterRadius - kTickMajorLength,
                                       major_tick_w, kTickMajorLength);
  SkRect minor_tick = SkRect::MakeXYWH(-minor_tick_w / 2, kTickOuterRadius - kTickMinorLength,
                                       minor_tick_w, kTickMinorLength);
  static auto font = gui::Font::Make(2);
  float text_r = r4 * 0.8;
  for (int i = 1; i <= major_tick_count; ++i) {
    float a = (float)i / major_tick_count;
    canvas.save();
    canvas.rotate(360.f * a);
    canvas.drawRect(major_tick, tick_paint);
    canvas.restore();
    auto text = f("%d", i * range_max / major_tick_count);
    canvas.save();
    float s = sin(a * M_PI * 2);
    float w = font->MeasureText(text);
    canvas.translate(s * (text_r - w / 4) - w / 2, cos(a * M_PI * 2) * text_r - 0.002 / 2);
    font->DrawText(canvas, text, SkPaint());
    canvas.restore();
  }
  for (int i = 0; i < tick_count; ++i) {
    if (i * major_tick_count % tick_count == 0) {
      continue;
    }
    canvas.save();
    canvas.rotate(360.f * i / tick_count);
    canvas.drawRect(minor_tick, tick_paint);
    canvas.restore();
  }

  auto range_name = RangeName(range);
  auto range_name_width = font->MeasureText(range_name);
  canvas.save();
  canvas.translate(-range_name_width / 2, text_r * 0.5 - 0.002 / 2);
  font->DrawText(canvas, RangeName(range), SkPaint());
  canvas.restore();
}

animation::Phase TimerDelay::Draw(gui::DrawContext& ctx) const {
  auto& canvas = ctx.canvas;

  auto phase = IsRunning(*this) ? animation::Animating : animation::Finished;
  phase |= start_pusher_depression.Tick(ctx.display);
  phase |= left_pusher_depression.Tick(ctx.display);
  phase |= right_pusher_depression.Tick(ctx.display);

  int range_end = (int)Range::EndGuard;
  animation::WrapModulo(range_dial.value, (float)range, range_end);
  phase |= range_dial.SpringTowards((float)range, ctx.DeltaT(), 0.4, 0.05);

  double circles;
  duration_handle_rotation.target =
      M_PI * 2.5 - modf(duration.value.count() / RangeDuration(range).count(), &circles) * 2 * M_PI;
  duration_handle_rotation.target =
      modf(duration_handle_rotation.target / (2 * M_PI), &circles) * 2 * M_PI;
  animation::WrapModulo(duration_handle_rotation.value, duration_handle_rotation.target, M_PI * 2);
  duration_handle_rotation.Tick(ctx.display);

  if (hand_draggers) {
    // do nothing...
  } else {
    float hand_target;
    if (IsRunning(*this)) {
      auto base_degrees = HandBaseDegrees(*this);
      hand_target = base_degrees;
    } else {
      hand_target = 90;
    }
    animation::WrapModulo(hand_degrees.value, hand_target, 360);
    phase |= hand_degrees.SpringTowards(hand_target, ctx.DeltaT(), kHandPeriod.count(), 0.05);
  }

  DrawRing(canvas, r4, r5, 0xffcfd0cf, 0xffc9c9cb);  // white watch face

  canvas.save();
  canvas.clipRRect(SkRRect::MakeOval(SkRect::MakeXYWH(-r4, -r4, r4 * 2, r4 * 2)), false);
  float fract = range_dial - roundf(range_dial);
  // canvas.rotate(-180 * (range_dial - roundf(range_dial)), 0, r4);
  // canvas.translate(-r4 * 2 * fract, 0);
  if (fabs(fract) > 0.01) {
    SkMatrix m;
    m.setPerspX(-20 * fract);
    m.postScale(1 - fabs(fract), 1);
    m.postTranslate(-r4 * 2 * fract, 0);
    m.postRotate(-90 * (range_dial - roundf(range_dial)), 0, 0);
    m.normalizePerspective();
    canvas.concat(m);
  }
  DrawDial(canvas, (Range)(((int)roundf(range_dial) + range_end) % range_end), duration.value);
  canvas.restore();

  DrawChildren(ctx);

  DrawRing(canvas, r4, r4_b, 0x46000000, 0xe1ffffff, kRingInset);  // shadow over white watch face

  canvas.save();
  canvas.translate(0.001, -0.001);
  DrawRing(canvas, r5, 0, 0xff46464d, 0xff46464d, kRingBlurred);  // black pin shadow
  canvas.restore();

  DrawRing(canvas, r0, r2, 0xfff6f6f0, 0xff6a6a71);              // white case
  DrawRing(canvas, r0, r1, 0xfff7f4f2, 0xff5e5f65, kRingInset);  // white case soft edge

  {  // Draw pusher
    SkColor colors1[] = {0x20ffffff, 0x15000000, 0xA0000000};
    auto start_pusher =
        kStartPusher.makeOffset(0, -start_pusher_depression * kStartPusherAxleLength);
    DrawPusher(canvas, kStartPusherAxle, start_pusher, colors1);
    canvas.save();
    canvas.rotate(45);
    SkColor colors2[] = {0xD0000000, 0x40000000, 0xD0000000};
    auto left_pusher = kSmallPusher.makeOffset(0, -left_pusher_depression * kSmallAxleLength);
    DrawPusher(canvas, kSmallAxle, left_pusher, colors2);
    canvas.rotate(-90);
    SkColor colors3[] = {0x40FFFFFF, 0x40000000, 0xFF000000};
    auto right_pusher = kSmallPusher.makeOffset(0, -right_pusher_depression * kSmallAxleLength);
    DrawPusher(canvas, kSmallAxle, right_pusher, colors3);
    canvas.restore();
  }

  DrawRing(canvas, r2, r3, 0xff878682, 0xff020302);  // black metal band outer edge
  DrawRing(canvas, r3, r4, 0xff080604, 0xffe2e2e1);  // black metal band inner edge

  DrawHand(ctx, *this);

  DrawRing(canvas, r5, 0, 0xff25272e, 0xff0d0b0f);               // black pin fill
  DrawRing(canvas, r5, r6, 0xff7e7d7a, 0xff05070b, kRingInset);  // black pin soft outer edge

  auto duration_handle_matrix = SkMatrix::RotateRad(duration_handle_rotation);
  SkPath duration_path_rotated = DurationHandlePath(*this);

  SkPaint duration_handle_paint;
  SkPoint quad[4];
  duration_path_rotated.getBounds().toQuad(quad);
  quad[1] = ClampLength(quad[1], kTickOuterRadius, kOuterRadius);
  quad[3] = ClampLength(quad[3], kTickOuterRadius, kOuterRadius);
  duration_handle_paint.setShader(MakeGradient(duration_handle_matrix.mapXY(0, 0),
                                               duration_handle_matrix.mapXY(0, 0.0005), 0xff404040,
                                               0xff202020));

  SkPaint highlight_paint;
  highlight_paint.setShader(MakeGradient(quad[3], quad[1], 0xff404040, 0xff202020));
  highlight_paint.setStyle(SkPaint::kStroke_Style);
  highlight_paint.setStrokeWidth(0.001);
  highlight_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.0002));

  canvas.save();
  canvas.clipPath(duration_path_rotated, true);
  canvas.drawPaint(duration_handle_paint);
  canvas.drawPath(duration_path_rotated, highlight_paint);
  canvas.restore();
  return phase;
}

ControlFlow TimerDelay::VisitChildren(gui::Visitor& visitor) {
  std::shared_ptr<Widget> arr[] = {text_field};
  if (visitor(arr) == ControlFlow::Stop) return ControlFlow::Stop;
  return ControlFlow::Continue;
}

SkPath TimerDelay::FieldShape(Object& field) const {
  if (&field == &duration) {
    auto transform = SkMatrix::Translate(-kTextWidth / 2, -gui::NumberTextField::kHeight);
    return text_field->Shape().makeTransform(transform);
  }
  return SkPath();
}

SkMatrix TimerDelay::TransformToChild(const Widget& child) const {
  if (&child == text_field.get()) {
    return SkMatrix::Translate(kTextWidth / 2, gui::NumberTextField::kHeight);
  }
  return SkMatrix::I();
}

SkPath TimerDelay::Shape() const {
  static SkPath shape = []() {
    SkPathBuilder path_builder;
    path_builder.addOval(kOuterOval);
    path_builder.addRect(kStartPusherAxleBox);
    path_builder.addRRect(kStartPusher);
    SkPath small_pusher = SkPath::RRect(kSmallPusher);
    small_pusher.addRRect(kSmallAxle);
    small_pusher.transform(SkMatrix::RotateDeg(45));
    path_builder.addPath(small_pusher);
    small_pusher.transform(SkMatrix::RotateDeg(-90));
    path_builder.addPath(small_pusher);
    SkPath path = path_builder.detach();
    SkPath simple_path;
    if (Simplify(path, &simple_path)) {
      return simple_path;
    } else {
      return path;
    }
  }();
  return shape;
}

struct DragDurationHandleAction : Action {
  TimerDelay& timer;
  DragDurationHandleAction(gui::Pointer& pointer, TimerDelay& timer)
      : Action(pointer), timer(timer) {}
  virtual void Begin() {}
  virtual void Update() {
    auto pos = pointer.PositionWithin(timer);
    auto tick_count = TickCount(timer.range);
    double angle = atan2(pos.sk.y(), pos.sk.x());

    // Rescale to [0, 1] with 0 & 1 at the top of the dial
    double new_duration = (1.25 - angle / (2 * M_PI));
    if (new_duration < 0) {
      new_duration += 1;
    } else if (new_duration > 1) {
      new_duration -= 1;
    }
    {  // Snap to nearest tick
      new_duration *= tick_count;
      new_duration -= 0.5;
      if (new_duration <= 0) {
        new_duration += tick_count;
      }
      new_duration = ceilf(new_duration);
      new_duration /= tick_count;
    }
    new_duration *= RangeDuration(timer.range).count();

    SetDuration(timer, time::Duration(new_duration));
    PropagateDurationOutwards(timer);
  }
  virtual void End() {}
};

void TimerDelay::Updated(Location& here, Location& updated) {
  auto result = duration_arg.GetObject(here);
  if (!result.ok) {
    return;
  }
  std::string duration_str = result.object->GetText();
  double n = std::stod(duration_str);
  Duration d = Duration(n * RangeDuration(range).count() / TickCount(range));
  SetDuration(*this, d);
}

struct DragHandAction : Action {
  std::weak_ptr<TimerDelay> timer_weak;
  DragHandAction(gui::Pointer& pointer, std::shared_ptr<TimerDelay> timer)
      : Action(pointer), timer_weak(timer) {
    ++timer->hand_draggers;
  }
  virtual void Begin() {}
  virtual void Update() {
    auto timer_shared = timer_weak.lock();
    if (!timer_shared) {
      return;
    }
    Vec2 pos = gui::TransformDown(*timer_shared, nullptr).mapPoint(pointer.pointer_position);
    timer_shared->hand_degrees.value = atan(pos) * 180 / M_PI;
    timer_shared->InvalidateDrawCache();
  }
  virtual void End() {}
  ~DragHandAction() {
    if (auto timer = timer_weak.lock()) {
      --timer->hand_draggers;
      timer->InvalidateDrawCache();
    }
  }
};

std::unique_ptr<Action> TimerDelay::FindAction(gui::Pointer& pointer, gui::ActionTrigger btn) {
  if (btn == gui::PointerButton::Left) {
    auto pos = pointer.PositionWithin(*this);
    auto duration_handle_path = DurationHandlePath(*this);
    if (duration_handle_path.contains(pos.x, pos.y)) {
      return std::make_unique<DragDurationHandleAction>(pointer, *this);
    }
    if (kStartPusherBox.contains(pos.x, pos.y)) {
      start_pusher_depression.value = 1;
      if (auto h = here.lock()) {
        if (IsRunning(*this)) {
          Cancel();
          h->long_running = nullptr;
        } else {
          h->ScheduleRun();
        }
        return nullptr;
      }
    }
    auto left_rot = SkMatrix::RotateDeg(-45).mapPoint(pos.sk);
    auto right_rot = SkMatrix::RotateDeg(45).mapPoint(pos.sk);
    if (kSmallPusherBox.contains(left_rot.x(), left_rot.y())) {
      range = Range(((int)range + (int)Range::EndGuard - 1) % (int)Range::EndGuard);
      left_pusher_depression.value = 1;
      UpdateTextField(*this);
      PropagateDurationOutwards(*this);
      InvalidateDrawCache();
      return nullptr;
    }
    if (kSmallPusherBox.contains(right_rot.x(), right_rot.y())) {
      range = Range(((int)range + 1) % (int)Range::EndGuard);
      right_pusher_depression.value = 1;
      UpdateTextField(*this);
      PropagateDurationOutwards(*this);
      InvalidateDrawCache();
      return nullptr;
    }

    SkPath hand_path = HandPath(*this);
    SkPath hand_outline;  // Hand is just a straight line so we have to "widen" it
    skpathutils::FillPathWithPaint(hand_path, kHandPaint, &hand_outline);
    if (hand_outline.contains(pos.x, pos.y)) {
      return std::make_unique<DragHandAction>(pointer, SharedPtr<TimerDelay>());
    }
  }
  return Object::FindAction(pointer, btn);
}

void TimerDelay::Fields(std::function<void(Object&)> cb) { cb(duration); }

void TimerDelay::Args(std::function<void(Argument&)> cb) {
  cb(duration_arg);
  cb(next_arg);
}

LongRunning* TimerDelay::OnRun(Location& here) {
  start_time = time::SteadyClock::now();
  ScheduleAt(here, start_time + duration.value);
  InvalidateDrawCache();
  return this;
}

void TimerDelay::Cancel() {
  if (auto h = here.lock()) {
    CancelScheduledAt(*h, start_time + duration.value);
  }
  InvalidateDrawCache();
}

DurationArgument::DurationArgument() : LiveArgument("duration", Argument::kOptional) {
  requirements.push_back([](Location* location, Object* object, std::string& error) {
    if (object == nullptr) {
      error = "Duration argument must be set.";
    }
    Str text = object->GetText();
    char* endptr = nullptr;
    double val = strtod(text.c_str(), &endptr);
    if (endptr == text.c_str() || endptr == nullptr) {
      error = "Duration argument must be a number.";
    }
  });
  tint = "#6e4521"_color;
}

StrView ToStr(TimerDelay::Range r) {
  switch (r) {
    using enum TimerDelay::Range;
    case Milliseconds:
      return "milliseconds";
    case Seconds:
      return "seconds";
    case Minutes:
      return "minutes";
    case Hours:
      return "hours";
    case Days:
      return "days";
    default:
      return "unknown";
  }
}
TimerDelay::Range TimerRangeFromStr(StrView str, Status& status) {
  using enum TimerDelay::Range;
  if (str == "milliseconds") return Milliseconds;
  if (str == "seconds") return Seconds;
  if (str == "minutes") return Minutes;
  if (str == "hours") return Hours;
  if (str == "days") return Days;
  AppendErrorMessage(status) += "Unknown value for timer range: " + Str(str);
  return Seconds;
}

void TimerDelay::SerializeState(Serializer& writer, const char* key) const {
  writer.Key(key);
  writer.StartObject();
  writer.Key("range");
  auto range_str = ToStr(range);
  writer.String(range_str.data(), range_str.size());
  writer.Key("duration_seconds");
  writer.Double(duration.value.count());
  if (IsRunning(*this)) {
    writer.Key("running");
    writer.Double((time::SteadyNow() - start_time).count());
  }
  writer.EndObject();
}
void TimerDelay::DeserializeState(Location& l, Deserializer& d) {
  Status status;
  // TODO: handle deserialization into a running timer
  for (auto& key : ObjectView(d, status)) {
    if (key == "running") {
      double value = 0;
      d.Get(value, status);
      l.long_running = this;
      start_time = time::SteadyNow() - Duration(value);
    } else if (key == "duration_seconds") {
      double value;
      d.Get(value, status);
      if (OK(status)) {
        duration.value = Duration(value);
      }
    } else if (key == "range") {
      Str value;
      d.Get(value, status);
      if (OK(status)) {
        range = TimerRangeFromStr(value, status);
      }
    }
  }
  UpdateTextField(*this);
  if (l.long_running) {
    ScheduleAt(l, start_time + duration.value);
  }

  if (!OK(status)) {
    l.ReportError("Failed to deserialize TimerDelay: " + status.ToStr());
  }
}
}  // namespace automat::library