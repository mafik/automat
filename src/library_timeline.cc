// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_timeline.hh"

#include <include/core/SkBlendMode.h>
#include <include/core/SkBlurTypes.h>
#include <include/core/SkColor.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <tracy/Tracy.hpp>

#include "../build/generated/embedded.hh"
#include "animation.hh"
#include "arcline.hh"
#include "argument.hh"
#include "base.hh"
#include "color.hh"
#include "font.hh"
#include "key_button.hh"
#include "library_mouse.hh"
#include "math.hh"
#include "number_text_field.hh"
#include "pointer.hh"
#include "random.hh"
#include "root_widget.hh"
#include "segment_tree.hh"
#include "sincos.hh"
#include "status.hh"
#include "svg.hh"
#include "textures.hh"
#include "time.hh"
#include "ui_button.hh"
#include "ui_constants.hh"
#include "ui_shape_widget.hh"

using namespace automat::ui;
using namespace std;

namespace automat::library {

constexpr float kScrewRadius = 1_mm;
constexpr float kScrewMargin =
    2_mm;  // margin from the screw position to the edge of the plastic front panel
constexpr float kWoodWidth = 4_mm;

constexpr float kPlasticCornerRadius = kScrewRadius + kScrewMargin;
constexpr float kWoodenCaseCornerRadius = kPlasticCornerRadius + kWoodWidth;

constexpr float kDisplayHeight = kLetterSize * 3 + 4 * 1_mm;
constexpr float kDisplayMargin = 2_mm;
constexpr float kDisplayWidth = 2.55_cm;

constexpr float kPlayButtonDiameter = kDisplayHeight;
constexpr float kPlayButtonRadius = kPlayButtonDiameter / 2;

constexpr float kRulerLength = (kDisplayWidth + kDisplayMargin + kPlayButtonRadius) * 2;
constexpr float kSideButtonMargin = 2_mm;
constexpr float kSideButtonDiameter = kMinimalTouchableSize;
constexpr float kSideButtonRadius = kSideButtonDiameter / 2;

constexpr float kPlasticWidth = kRulerLength + 2 * (kSideButtonDiameter + 2 * kSideButtonMargin);
constexpr float kWoodenCaseWidth = kPlasticWidth + 2 * kWoodWidth;

constexpr float kRulerHeight = kSideButtonDiameter / 2 + kSideButtonMargin;
constexpr float kMarginAroundTracks = 2_mm;

constexpr float kPlasticTop = 2 * kDisplayMargin + kDisplayHeight;

constexpr float kWindowWidth = kPlasticWidth - 2 * kDisplayMargin;

constexpr float kTrackMargin = 1_mm;
constexpr float kTrackHeight = 1_cm;
constexpr float kTrackWidth = kWindowWidth - 2 * kTrackMargin;

constexpr float kZoomRadius = 3_cm;
constexpr float kZoomVisible = (kWindowWidth - kRulerLength) / 2 - kTrackMargin;

constexpr SkColor kOrange = "#e24e1f"_color;

static constexpr Vec2 ZoomDialCenter(float window_height) {
  return {kWindowWidth / 2 + kZoomRadius - kZoomVisible, -window_height / 2};
}

static constexpr float WindowHeight(int num_tracks) {
  return kRulerHeight * 2 + kMarginAroundTracks * 2 + max(0, num_tracks - 1) * kTrackMargin +
         num_tracks * kTrackHeight;
}

// May return values outside of [0, num_tracks]!
static constexpr int TrackIndexFromY(float y) {
  return floorf((-y - kRulerHeight - kMarginAroundTracks + kTrackMargin / 2) /
                (kTrackHeight + kTrackMargin));
}

constexpr float kPlasticBottom = kDisplayMargin;

static Rect PlasticRect(int track_count) {
  return Rect(-kPlasticWidth / 2, -WindowHeight(track_count) - kPlasticBottom, kPlasticWidth / 2,
              kPlasticTop);
}

static Rect WoodenCaseRect(int track_count) { return PlasticRect(track_count).Outset(kWoodWidth); }

static SkRRect WoodenCaseRRect(int track_count) {
  return SkRRect::MakeRectXY(WoodenCaseRect(track_count).sk, kWoodenCaseCornerRadius,
                             kWoodenCaseCornerRadius);
}

static SkRRect PlasticRRect(int track_count) {
  return SkRRect::MakeRectXY(PlasticRect(track_count), kPlasticCornerRadius, kPlasticCornerRadius);
}

constexpr RRect kDisplayRRect = [] {
  float r = 1_mm;

  return RRect{.rect = Rect(-kDisplayWidth, 0, 0, kDisplayHeight)
                           .MoveBy({-kPlayButtonRadius - kDisplayMargin, kDisplayMargin}),
               .radii = {Vec2(r, r), Vec2(r, r), Vec2(r, r), Vec2(r, r)},
               .type = SkRRect::kSimple_Type};
}();

static auto rosewood_color = PersistentImage::MakeFromAsset(embedded::assets_rosewood_color_webp,
                                                            {
                                                                .tile_x = SkTileMode::kRepeat,
                                                                .tile_y = SkTileMode::kRepeat,
                                                            });

const SkPaint& WoodPaint() {
  static SkPaint wood_paint = ([&]() {
    SkPaint p;
    p.setColor("#805338"_color);
    p.setShader(rosewood_color.paint.getShader()->makeWithLocalMatrix(SkMatrix::RotateDeg(-5)));
    return p;
  })();
  return wood_paint;
}

const SkPaint kPlasticPaint = [] {
  SkPaint p;
  // p.setColor("#f0eae5"_color);
  SkPoint pts[2] = {{0, kPlasticTop}, {0, 0}};
  SkColor colors[3] = {"#f2ece8"_color, "#e0dbd8"_color};
  sk_sp<SkShader> gradient =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
  p.setShader(gradient);
  return p;
}();

const SkPaint kDisplayCurrentPaint = [] {
  SkPaint p;
  p.setColor(kOrange);
  return p;
}();

const SkPaint kDisplayTotalPaint = [] {
  SkPaint p;
  p.setColor("#4a4c3a"_color);
  return p;
}();

const SkPaint kDisplayRemainingPaint = [] {
  SkPaint p;
  p.setColor("#666a4d"_color);
  return p;
}();

const SkPaint kRulerPaint = [] {
  SkPaint p;
  p.setColor("#4e4e4e"_color);
  return p;
}();

const SkPaint kTrackPaint = [] {
  SkPaint p;
  // SkPoint pts[2] = {{0, 0}, {kTrackWidth, 0}};
  // SkColor colors[3] = {"#787878"_color, "#f3f3f3"_color, "#787878"_color};
  // sk_sp<SkShader> gradient =
  //     SkGradientShader::MakeLinear(pts, colors, nullptr, 3, SkTileMode::kClamp);
  // p.setShader(gradient);
  p.setColor("#d3d3d3"_color);
  return p;
}();

const SkPaint kWindowPaint = [] {
  SkPaint p;
  p.setColor("#1b1b1b"_color);
  return p;
}();

const SkPaint kTickPaint = [] {
  SkPaint p;
  p.setColor("#313131"_color);
  p.setStyle(SkPaint::kStroke_Style);
  return p;
}();

const SkPaint kBridgeHandlePaint = [] {
  SkPaint p;
  SkPoint pts[2] = {{0, -kRulerHeight - kMarginAroundTracks}, {0, -kRulerHeight}};
  SkColor colors[2] = {kOrange, "#f17149"_color};
  auto shader = SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
  p.setShader(shader);
  return p;
}();

const SkPaint kBridgeLinePaint = [] {
  SkPaint p;
  p.setColor(kOrange);
  p.setStyle(SkPaint::kStroke_Style);
  p.setStrokeWidth(1_mm);
  return p;
}();

const SkPaint kSignalPaint = [] {
  SkPaint p = kBridgeLinePaint;
  p.setStrokeWidth(0.5_mm);
  p.setAlpha(0x80);
  p.setBlendMode(SkBlendMode::kHardLight);
  return p;
}();

const SkPaint kOnOffPaint = [] {
  SkPaint p;
  p.setColor("#57dce4"_color);
  p.setStyle(SkPaint::kStroke_Style);
  p.setStrokeWidth(2_mm);
  p.setBlendMode(SkBlendMode::kMultiply);
  return p;
}();

constexpr float kVec2DisplayMargin = 0.5_mm;

const SkPaint kVec2Paint = [] {
  SkPaint p;
  p.setColor("#131c64"_color);
  p.setStyle(SkPaint::kStroke_Style);
  p.setStrokeWidth(kVec2DisplayMargin);
  p.setAlpha(0x50);
  p.setBlendMode(SkBlendMode::kMultiply);
  return p;
}();

const SkPaint kZoomPaint = [] {
  SkPaint p;
  p.setColor("#000000"_color);
  p.setAlphaf(0.5f);
  return p;
}();

const SkPaint kZoomTextPaint = [] {
  SkPaint p;
  p.setColor("#ffffff"_color);
  p.setAlphaf(0.9f);
  return p;
}();

const SkPaint kZoomTickPaint = [] {
  SkPaint p;
  p.setColor("#ffffff"_color);
  p.setAlphaf(0.9f);
  p.setStyle(SkPaint::kStroke_Style);
  return p;
}();

const SkMatrix kHorizontalFlip = SkMatrix::Scale(-1, 1);

static SkPath GetPausedPath() {
  static SkPath path = [] {
    SkPath path;
    path.addRect(-1.5_mm, -1.5_mm, -0.5_mm, 1.5_mm);
    path.addRect(0.5_mm, -1.5_mm, 1.5_mm, 1.5_mm);
    return path;
  }();
  return path;
}

static SkPath GetRecPath() {
  static SkPath path = [] {
    SkPath path;
    path.addCircle(0, 0, 2.5_mm);
    return path;
  }();
  return path;
}

static constexpr SkColor kTimelineButtonBackground = "#fdfcfb"_color;

TrackArgument::TrackArgument(StrView name) : icon(name, kKeyLetterSize, KeyFont()), name(name) {
  // interface = this;
}

PaintDrawable& TrackArgument::Icon() { return icon; }

void TrackArgument::CanConnect(Object& start, Atom& end, Status& status) const {}

Timeline::Timeline()
    : state(kPaused), timeline_length(0), paused{.playback_offset = 0s}, zoom(10) {}

OnOffTrack& Timeline::AddOnOffTrack(StrView name) {
  auto track_ptr = MAKE_PTR(OnOffTrack);
  auto& track = *track_ptr;
  AddTrack(std::move(track_ptr), name);
  return track;
}

Vec2Track& Timeline::AddVec2Track(StrView name) {
  auto track_ptr = MAKE_PTR(Vec2Track);
  auto& track = *track_ptr;
  AddTrack(std::move(track_ptr), name);
  return track;
}

Float64Track& Timeline::AddFloat64Track(StrView name) {
  auto track_ptr = MAKE_PTR(Float64Track);
  auto& track = *track_ptr;
  AddTrack(std::move(track_ptr), name);
  return track;
}

void Timeline::AddTrack(Ptr<TrackBase>&& track, StrView name) {
  track->timeline = this;
  auto arg = make_unique<TrackArgument>(name);
  arg->track = std::move(track);
  tracks.emplace_back(std::move(arg));
  if (auto h = here) {
    if (h->widget) h->widget->InvalidateConnectionWidgets(true, true);
  }
  WakeToys();
}

Timeline::Timeline(const Timeline& other) : Timeline() {
  tracks.reserve(other.tracks.size());
  for (const auto& track : other.tracks) {
    AddTrack(track->track->Clone().Cast<TrackBase>(), track->name);
  }
  WakeToys();
}

string_view Timeline::Name() const { return "Timeline"; }

Ptr<Object> Timeline::Clone() const { return MAKE_PTR(Timeline, *this); }

constexpr float kLcdFontSize = 1.5_mm;
static Font& LcdFont() {
  static unique_ptr<Font> font =
      Font::MakeV2(Font::MakeWeightVariation(Font::GetNotoSans(), 700), kLcdFontSize);
  return *font.get();
}

time::Duration Timeline::MaxTrackLength() const {
  auto max_track_length = timeline_length;
  if (state == kRecording) {
    max_track_length = max(max_track_length, time::SteadyNow() - recording.started_at);
  }
  for (const auto& track : tracks) {
    auto* track_base = track->track.Get();
    if (track_base->timestamps.empty()) {
      continue;
    }
    max_track_length = max(max_track_length, track_base->timestamps.back());
  }
  return max_track_length;
}

void TimelineCancelScheduled(Timeline& t) {
  if (auto h = t.here) {
    CancelScheduledAt(*h);
  }
}

// Schedule the timeline at the next timestamp AFTER `now`.
void TimelineScheduleNextAfter(Timeline& t, time::SteadyPoint now) {
  auto started_at = t.playing.started_at;
  time::SteadyPoint next_update = t.playing.started_at + time::Duration(t.MaxTrackLength());
  // We're not subtracting `started_at` from `now` because due to floating point accuracy
  // that might result in a timestamp before the current one (leading to infinite loop) or
  // after the next one (leading to skipped values).
  auto cmp = [started_at](time::SteadyPoint now, time::Duration timestamp) {
    return started_at + timestamp > now;
  };
  for (const auto& track : t.tracks) {
    auto* track_base = track->track.Get();
    int next_update_i =
        std::upper_bound(track_base->timestamps.begin(), track_base->timestamps.end(), now, cmp) -
        track_base->timestamps.begin();
    if (next_update_i < track_base->timestamps.size()) {
      auto next_update_point =
          t.playing.started_at + time::Duration(track_base->timestamps[next_update_i]);
      next_update = min(next_update, next_update_point);
    }
  }
  if (auto h = t.here) {
    ScheduleAt(*h, next_update);
  }
}

static void TimelineUpdateOutputs(Timeline& t, time::SteadyPoint started_at,
                                  time::SteadyPoint now) {
  for (auto& track : t.tracks) {
    t.InvalidateConnectionWidgets(track.get());
    auto* object = track->ObjectOrNull(t);
    if (object == nullptr) continue;
    auto* location = object->MyLocation();
    if (location == nullptr) continue;
    track->track->UpdateOutput(*location, started_at, now);
  }
}

static float BridgeOffsetX(float current_pos_ratio) {
  return -kRulerLength / 2 + kRulerLength * current_pos_ratio;
}

static float PosRatioFromBridgeOffsetX(float bridge_offset_x) {
  return (bridge_offset_x + kRulerLength / 2) / kRulerLength;
}

SkPath SplicerShape(int num_tracks, float current_pos_ratio) {
  static const SkPath splicer_shape = [] {
    float height = 5_mm;
    float width = 8_mm;

    float bottom = -height / 2;
    float top = height / 2;
    float middle_offset = width * 0.3f;
    float edge_offset = width * 0.5f;

    Vec2 bottom_center = {0, bottom};
    Vec2 top_center = {0, top};
    Vec2 left = {-middle_offset, 0};
    Vec2 right = {middle_offset, 0};
    Vec2 bottom_left = {-edge_offset, bottom};
    Vec2 bottom_right = {edge_offset, bottom};
    Vec2 top_left = {-edge_offset, top};
    Vec2 top_right = {edge_offset, top};
    float radius = 0.5_mm;

    SkPath path;
    // Start at bottom center
    path.moveTo(bottom_center);
    path.arcTo(bottom_right, (bottom_right + right) / 2, radius);
    path.arcTo(right, (right + top_right) / 2, radius);
    path.arcTo(top_right, top_center, radius);
    path.arcTo(top_left, (top_left + left) / 2, radius);
    path.arcTo(left, (left + bottom_left) / 2, radius);
    path.arcTo(bottom_left, bottom_center, radius);
    path.close();

    return path;
  }();

  float x = BridgeOffsetX(current_pos_ratio);

  float y = -(kMarginAroundTracks * 2 + kTrackHeight * num_tracks +
              kTrackMargin * max(0, num_tracks - 1) + kRulerHeight);
  return splicer_shape.makeTransform(SkMatrix::Translate(x, y));
}

SkPath kScissorsIcon = PathFromSVG(
    "m1.72 1.2a.96.65 52.31 00-.34.1.96.65 52.31 00.07 1.16.96.65 52.31 001.11.36.96.65 52.31 "
    "00-.07-1.16.96.65 52.31 00-.77-.46zm-3.44-.05a.65.96 37.69 00-.77.46.65.96 37.69 00-.07 "
    "1.16.65.96 37.69 001.11-.36.65.96 37.69 00.07-1.16.65.96 37.69 00-.34-.1zm4.18-5.06C2.34-2.78 "
    "1.5-.77.81.28c.41.03.97-.18 1.93.81a1.78 1.22 52.31 01.09.1c.01.01.02.02.03.03l.01.02a1.78 "
    "1.22 52.31 01.07.09 1.78 1.22 52.31 01.12 2.15A1.78 1.22 52.31 011 2.82a1.78 1.22 52.31 "
    "01-.06-.09l-.96-1.3-.92 1.25a1.22 1.78 37.69 01-.06.09 1.22 1.78 37.69 01-2.06.66 1.22 1.78 "
    "37.69 01.12-2.15 1.22 1.78 37.69 01.07-.1l.01-.01c.01-.02.02-.02.03-.03a1.22 1.78 37.69 "
    "01.09-.1c.91-.95 1.46-.8 1.86-.8-.67-1.08-1.47-3.01-1.58-4.1L-.02-.51l2.48-3.4z",
    SVGUnit_Millimeters);

SkPath kCancelIcon = PathFromSVG(
    "m-2.68-1.7c0 .1.7.85 1.55 1.7-.84.86-1.54 1.62-1.55 1.69-.01.18.76.98.98.98.09 0 .84-.72 "
    "1.69-1.56.87.84 1.63 1.56 1.7 1.56.18.01.99-.76.98-.98 0-.09-.72-.85-1.57-1.7.84-.86 "
    "1.55-1.61 1.57-1.69.05-.2-.73-.98-.98-.98-.1 0-.86.71-1.71 "
    "1.56-.85-.83-1.6-1.54-1.76-1.55-.16-.01-.89.73-.89.97z",
    SVGUnit_Millimeters);

SkPath kPlusIcon = PathFromSVG(
    "M-.6859-3.0901C-.5162-3.2598.5233-3.2668.6293-3.1466.7354-3.0264.7637-1.994.7778-.8061c1.2021-"
    "0 2.2415.0354 2.3122.1061.1768.1768.1768 1.2799 0 1.3859C3.0193.7283 "
    "1.987.7566.7849.7707.7849 1.9728.7566 3.0193.693 3.083c-.1485.1626-1.2657.1344-1.3859 "
    "0C-.7425 3.0335-.7707 "
    "1.987-.792.7778-1.987.7707-3.0264.7495-3.0901.6859-3.2456.5303-3.2244-.5798-3.0901-.7-3.0335-."
    "7425-2.0011-.7849-.799-.799c-0-1.2021.0354-2.2274.1061-2.2981Z",
    SVGUnit_Millimeters);

SkPath kMinusIcon = PathFromSVG(
    "M-3.09.69C-3.26.52-3.27-.52-3.15-.63S-1.99-.76.77-.78c1.2 0 2.25.02 2.31.09.17.15.14 1.26 0 "
    "1.38C3.03.74 1.99.77-.8.8-2 .8-3.03.76-3.1.69Z",
    SVGUnit_Millimeters);

SkPath BridgeShape(int num_tracks, float current_pos_ratio) {
  float bridge_offset_x = BridgeOffsetX(current_pos_ratio);

  float bottom_y = -(kMarginAroundTracks * 2 + kTrackHeight * num_tracks +
                     kTrackMargin * max(0, num_tracks - 1));

  float line_width = 0.5_mm;
  float line_gap = 1_mm;

  SkPath bridge_handle;
  bridge_handle.moveTo(0, kRulerHeight / 6);                              // top of the arrow
  bridge_handle.lineTo(kMinimalTouchableSize / 4, 0);                     // right of the arrow
  bridge_handle.lineTo(kMinimalTouchableSize / 2, 0);                     // top right
  bridge_handle.lineTo(kMinimalTouchableSize / 2, -kMarginAroundTracks);  // bottom right

  {  // right vertical line
    bridge_handle.lineTo(line_gap / 2 + line_width, -kMarginAroundTracks);
    bridge_handle.lineTo(line_gap / 2 + line_width, bottom_y);
    bridge_handle.lineTo(line_gap / 2, bottom_y);
    bridge_handle.lineTo(line_gap / 2, -kMarginAroundTracks);
  }

  {  // left vertical line
    bridge_handle.lineTo(-line_gap / 2, -kMarginAroundTracks);
    bridge_handle.lineTo(-line_gap / 2, bottom_y);
    bridge_handle.lineTo(-line_gap / 2 - line_width, bottom_y);
    bridge_handle.lineTo(-line_gap / 2 - line_width, -kMarginAroundTracks);
  }

  bridge_handle.lineTo(-kMinimalTouchableSize / 2, -kMarginAroundTracks);  // bottom left
  bridge_handle.lineTo(-kMinimalTouchableSize / 2, 0);                     // top left
  bridge_handle.lineTo(-kMinimalTouchableSize / 4, 0);                     // left of the arrow
  bridge_handle.close();
  bridge_handle.offset(bridge_offset_x, -kRulerHeight);

  return bridge_handle;
}

constexpr float kZoomTresholdsS[] = {
    0.001, 0.02, 0.1, 1, 20, 120, 3600,
};

constexpr float kZoomStepSizeS[] = {
    0.001, 0.001, 0.01, 0.1, 1, 10, 60,
};

constexpr Size kZoomLevelsCount = sizeof(kZoomTresholdsS) / sizeof(kZoomTresholdsS[0]);

static float NearestZoomTick(float zoom) {
  if (zoom < kZoomTresholdsS[0]) {
    return kZoomTresholdsS[0];
  }
  for (int i = 0; i < kZoomLevelsCount; ++i) {
    if (zoom < kZoomTresholdsS[i] + kZoomStepSizeS[i] / 2) {
      return roundf(zoom / kZoomStepSizeS[i]) * kZoomStepSizeS[i];
    }
  }
  return kZoomTresholdsS[kZoomLevelsCount - 1];
}

static float NextZoomTick(float zoom) {
  for (int i = 0; i < kZoomLevelsCount; ++i) {
    if (zoom < kZoomTresholdsS[i] - kZoomStepSizeS[i] / 2) {
      return zoom + kZoomStepSizeS[i];
    }
  }
  return zoom + kZoomStepSizeS[kZoomLevelsCount - 1];
}

static float PreviousZoomTick(float zoom) {
  for (int i = 0; i < kZoomLevelsCount; ++i) {
    if (zoom <= kZoomTresholdsS[i] + kZoomStepSizeS[i] / 2) {
      return zoom - kZoomStepSizeS[i];
    }
  }
  return zoom - kZoomStepSizeS[kZoomLevelsCount - 1];
}

SkPath WindowShape(int num_tracks) {
  ArcLine window = ArcLine({0, 0}, 0_deg);

  auto side_button_turn = ArcLine::TurnShift(-kSideButtonRadius - kSideButtonMargin,
                                             kSideButtonRadius + kSideButtonMargin);

  float top_line_dist = kWindowWidth / 2 - side_button_turn.distance_forward - kSideButtonRadius;
  window.MoveBy(top_line_dist);

  side_button_turn.Apply(window);
  window.MoveBy(kSideButtonRadius - kSideButtonMargin);
  window.TurnConvex(-90_deg, kSideButtonMargin);

  SinCos lower_turn_angle =
      SinCos::FromRadians(acos((kScrewMargin - kScrewRadius) / (kScrewRadius + 2 * kScrewMargin)));
  float lower_turn_dist =
      (float)lower_turn_angle.sin * (kScrewRadius + kScrewMargin * 2) + kScrewRadius;

  float window_height = WindowHeight(num_tracks);

  float vertical_dist =
      window_height - kSideButtonMargin - kSideButtonRadius - kSideButtonMargin - lower_turn_dist;
  window.MoveBy(vertical_dist);

  window.TurnConvex(-lower_turn_angle, kScrewMargin);
  window.TurnConvex(-90_deg + lower_turn_angle * 2, kScrewRadius + kScrewMargin);
  window.TurnConvex(-lower_turn_angle, kScrewMargin);

  window.MoveBy(kWindowWidth - lower_turn_dist * 2);

  window.TurnConvex(-lower_turn_angle, kScrewMargin);
  window.TurnConvex(-90_deg + lower_turn_angle * 2, kScrewRadius + kScrewMargin);
  window.TurnConvex(-lower_turn_angle, kScrewMargin);

  window.MoveBy(vertical_dist);

  window.TurnConvex(-90_deg, kSideButtonMargin);
  window.MoveBy(kSideButtonRadius - kSideButtonMargin);
  side_button_turn.ApplyNegative(window);

  return window.ToPath(true);
}

struct TimelineWidget;

struct SideButton : ui::Button {
  SideButton(TimelineWidget& parent) : Button((Widget*)&parent) {}
  TimelineWidget* GetTimelineWidget() { return (TimelineWidget*)(parent.get()); }
  SkColor ForegroundColor() const override { return "#404040"_color; }
  SkColor BackgroundColor() const override { return kTimelineButtonBackground; }
  SkRRect RRect() const override;
};

struct PrevButton : SideButton {
  PrevButton(TimelineWidget& parent) : SideButton(parent) {
    child = MakeShapeWidget(this, kNextShape, SK_ColorWHITE, &kHorizontalFlip);
    UpdateChildTransform();
  }
  void Activate(ui::Pointer&) override;
  StrView Name() const override { return "Prev Button"; }
};

struct NextButton : SideButton {
  NextButton(TimelineWidget& parent) : SideButton(parent) {
    child = MakeShapeWidget(this, kNextShape, SK_ColorWHITE);
    UpdateChildTransform();
  }
  void Activate(ui::Pointer&) override;
  StrView Name() const override { return "Next Button"; }
};

struct TimelineRunButton : ui::ToggleButton {
  WeakPtr<Timeline> timeline_weak;

  std::unique_ptr<ui::Button> rec_button;
  mutable ui::Button* last_on_widget = nullptr;

  TimelineRunButton(Widget* parent, WeakPtr<Timeline> timeline)
      : ui::ToggleButton(parent), timeline_weak(std::move(timeline)) {
    on = std::make_unique<ColoredButton>(
        this, GetPausedPath(),
        ColoredButtonArgs{.fg = kTimelineButtonBackground,
                          .bg = kOrange,
                          .radius = kPlayButtonRadius,
                          .on_click = [this](ui::Pointer& p) { Activate(p); }});
    off = std::make_unique<ColoredButton>(
        this, kPlayShape,
        ColoredButtonArgs{.fg = kOrange,
                          .bg = kTimelineButtonBackground,
                          .radius = kPlayButtonRadius,
                          .on_click = [this](ui::Pointer& p) { Activate(p); }});
    rec_button = std::make_unique<ColoredButton>(
        this, GetRecPath(),
        ColoredButtonArgs{.fg = kTimelineButtonBackground,
                          .bg = color::kParrotRed,
                          .radius = kPlayButtonRadius,
                          .on_click = [this](ui::Pointer& p) { Activate(p); }});
  }
  ui::Button* OnWidget() override {
    auto timeline = timeline_weak.Lock();
    if (timeline) {
      auto lock = std::lock_guard(timeline->mutex);
      if (timeline->state == Timeline::kRecording) {
        last_on_widget = rec_button.get();
      } else if (timeline->state == Timeline::kPlaying) {
        last_on_widget = on.get();
      }
    }
    if (last_on_widget == nullptr) {
      last_on_widget = on.get();
    }
    return last_on_widget;
  }
  bool Filled() const override {
    auto timeline = timeline_weak.Lock();
    if (timeline) {
      auto lock = std::lock_guard(timeline->mutex);
      return timeline->running.IsOn();
    }
    return false;
  }
  void Activate(ui::Pointer&) {
    auto timeline = timeline_weak.Lock();
    if (!timeline) return;
    auto lock = std::lock_guard(timeline->mutex);
    switch (timeline->state) {
      case Timeline::kPlaying:
        timeline->running.Cancel();
        break;
      case Timeline::kPaused:
        timeline->run.ScheduleRun(*timeline);
        break;
      case Timeline::kRecording:
        timeline->StopRecording();
        break;
    }
  }
  StrView Name() const override { return "Timeline Run Button"; }
};

struct DragZoomAction;

struct SpliceAction : Action {
  TrackedPtr<TimelineWidget> timeline_widget;
  time::Duration splice_to;
  bool snapped = false;
  bool cancel = true;
  ui::Pointer::IconOverride resize_icon;
  SpliceAction(ui::Pointer&, TimelineWidget&);
  ~SpliceAction();
  void Update() override;
};

static time::Duration DistanceToSeconds(float zoom) {
  return time::FromSeconds(zoom / kWindowWidth);
}

time::Duration Timeline::CurrentOffset(time::SteadyPoint now) const {
  switch (state) {
    case Timeline::kPlaying:
      return now - playing.started_at;
    case Timeline::kPaused:
      return paused.playback_offset;
    case Timeline::kRecording:
      return now - recording.started_at;
  }
}

struct TimelineWidget : Object::Toy {
  std::unique_ptr<TimelineRunButton> run_button;
  std::unique_ptr<PrevButton> prev_button;
  std::unique_ptr<NextButton> next_button;

  SpliceAction* splice_action = nullptr;
  DragZoomAction* drag_zoom_action = nullptr;

  Vec<std::unique_ptr<ui::Widget>> track_widgets;

  float zoom = 10;
  float splice_x = 0;
  animation::SpringV2<float> splice_wiggle;
  float bridge_wiggle_s = 0;
  bool bridge_snapped = false;

  time::Duration max_track_length;     // populated on Tick
  time::Duration current_offset_raw;   // populated on Tick
  time::Duration current_offset;       // populated on Tick
  float current_pos_ratio;             // populated on Tick
  time::Duration distance_to_seconds;  // populated on Tick

  TimelineWidget(ui::Widget* parent, Object& object)
      : Object::Toy(parent, object),
        run_button(new TimelineRunButton(this, static_cast<Timeline&>(object).AcquireWeakPtr())),
        prev_button(new PrevButton(*this)),
        next_button(new NextButton(*this)) {
    run_button->local_to_parent = SkM44::Translate(-kPlayButtonRadius, kDisplayMargin);
    prev_button->local_to_parent =
        SkM44::Translate(-kPlasticWidth / 2 + kSideButtonMargin, -kSideButtonRadius);
    next_button->local_to_parent = SkM44::Translate(
        kPlasticWidth / 2 - kSideButtonMargin - kSideButtonDiameter, -kSideButtonRadius);
    if (auto timeline = this->LockObject<Timeline>()) {
      auto lock = std::lock_guard(timeline->mutex);
      zoom = timeline->zoom;
    }
  }

  Optional<time::Duration> SnapToTrack(Timeline& timeline_locked, Vec2 pos,
                                       time::Duration time_at_x = time::kDurationGuard) const {
    auto track_index = TrackIndexFromY(pos.y);
    if (track_index >= 0 && track_index < timeline_locked.tracks.size()) {
      auto& track = timeline_locked.tracks[track_index];
      auto& timestamps = track->track->timestamps;
      if (time_at_x == time::kDurationGuard) {
        time_at_x = TimeAtX(pos.x);
      }
      auto last = upper_bound(timestamps.begin(), timestamps.end(), time_at_x);
      int right_i = last - timestamps.begin();
      auto closest_dist = time::kDurationInfinity;
      auto closest_t = time::kDurationGuard;
      if (right_i >= 0 && right_i < timestamps.size()) {
        auto right_dist = abs(time_at_x - timestamps[right_i]);
        if (right_dist < closest_dist) {
          closest_dist = right_dist;
          closest_t = timestamps[right_i];
        }
      }
      int left_i = right_i - 1;
      if (left_i >= 0 && left_i < timestamps.size()) {
        auto left_dist = abs(time_at_x - timestamps[left_i]);
        if (left_dist < closest_dist) {
          closest_dist = left_dist;
          closest_t = timestamps[left_i];
        }
      }
      if (closest_dist < 1_mm * distance_to_seconds) {
        return closest_t;
      }
    }
    return nullopt;
  }

  Optional<time::Duration> SnapToBottomRuler(
      Vec2 pos, time::Duration time_at_x = time::kDurationGuard) const {
    int num_tracks = track_widgets.size();
    float window_height = WindowHeight(num_tracks);
    if (pos.y > -window_height && pos.y < -window_height + kRulerHeight) {
      double zoom_log = log10(zoom / 200.0);
      auto tick_duration = time::FloatDuration(pow(10, ceil(zoom_log)));
      if (time_at_x == time::kDurationGuard) {
        time_at_x = TimeAtX(pos.x);
      }
      if (time_at_x >= 0s && time_at_x < max_track_length) {
        return time::Defloat(round(time_at_x / tick_duration) * tick_duration);
      }
    }
    return nullopt;
  }

  time::Duration TimeAtX(float x) const {
    // Find the time at the center of the timeline
    auto center_t0 = time::Defloat(kRulerLength * distance_to_seconds) / 2;
    auto center_t1 = max_track_length - center_t0;
    auto center_t = center_t0 + time::Defloat((center_t1 - center_t0) * current_pos_ratio);
    return center_t + time::Defloat(x * distance_to_seconds);
  }

  float XAtTime(time::Duration t) const {
    // The original formula came from finding the time when current_pos_ratio was 0 and 1 and
    // lerp-ing between them.
    //
    // Keeping it around for reference since the math may be hard to decipher otherwise.
    // auto center_t0 = time::Defloat(kRulerLength * distance_to_seconds) / 2;
    // auto center_t1 = max_track_length - center_t0;
    // auto center_t = center_t0 + time::Defloat((center_t1 - center_t0) * current_pos_ratio);
    // return (t - center_t) / time::FloatDuration(distance_to_seconds);

    return (t - max_track_length * current_pos_ratio) / distance_to_seconds +
           kRulerLength * (current_pos_ratio - 0.5f);
  }

  void SetPosRatio(Timeline& timeline_locked, float pos_ratio, time::SteadyPoint now) {
    pos_ratio = clamp(pos_ratio, 0.0f, 1.0f);
    auto max_track_length = timeline_locked.MaxTrackLength();
    if (timeline_locked.state == Timeline::kPlaying) {
      TimelineCancelScheduled(timeline_locked);
      timeline_locked.playing.started_at = now - time::Defloat(pos_ratio * max_track_length);
      TimelineUpdateOutputs(timeline_locked, timeline_locked.playing.started_at, now);
      TimelineScheduleNextAfter(timeline_locked, now);
    } else if (timeline_locked.state == Timeline::kPaused) {
      timeline_locked.paused.playback_offset = time::Defloat(pos_ratio * max_track_length);
    }
    WakeAnimationResponsively(timeline_locked, now);
  }

  void SetOffset(Timeline& timeline_locked, time::Duration offset, time::SteadyPoint now) {
    offset = clamp<time::Duration>(offset, 0s, timeline_locked.MaxTrackLength());
    if (timeline_locked.state == Timeline::kPlaying) {
      TimelineCancelScheduled(timeline_locked);
      timeline_locked.playing.started_at = now - time::Duration(offset);
      TimelineUpdateOutputs(timeline_locked, timeline_locked.playing.started_at, now);
      TimelineScheduleNextAfter(timeline_locked, now);
    } else if (timeline_locked.state == Timeline::kPaused) {
      timeline_locked.paused.playback_offset = offset;
    }
    WakeAnimationResponsively(timeline_locked, now);
  }

  void AdjustOffset(Timeline& timeline_locked, time::Duration offset, time::SteadyPoint now) {
    if (timeline_locked.state == Timeline::kPlaying) {
      TimelineCancelScheduled(timeline_locked);
      timeline_locked.playing.started_at -= time::Duration(offset);
      timeline_locked.playing.started_at = min(timeline_locked.playing.started_at, now);
      TimelineUpdateOutputs(timeline_locked, timeline_locked.playing.started_at, now);
      TimelineScheduleNextAfter(timeline_locked, now);
    } else if (timeline_locked.state == Timeline::kPaused) {
      timeline_locked.paused.playback_offset = clamp<time::Duration>(
          timeline_locked.paused.playback_offset + offset, 0s, timeline_locked.MaxTrackLength());
    }
    WakeAnimationResponsively(timeline_locked, now);
  }

  void AddMissingTrackWidgets(Timeline& timeline_locked) {
    auto& tracks = timeline_locked.tracks;
    for (size_t i = track_widgets.size(); i < tracks.size(); ++i) {
      track_widgets.push_back(tracks[i]->track->MakeToy(this));
    }
  }

  void PullTimelineState(Timeline& timeline_locked, time::SteadyPoint now) {
    // update current_offset
    current_offset_raw = timeline_locked.CurrentOffset(now);
    current_offset = clamp<time::Duration>(current_offset_raw + time::FromSeconds(bridge_wiggle_s),
                                           0s, max_track_length);
    max_track_length = timeline_locked.MaxTrackLength();
    if (max_track_length == 0s) {
      current_pos_ratio = 1;
    } else {
      current_pos_ratio = current_offset / time::FloatDuration(max_track_length);
    }
    distance_to_seconds = DistanceToSeconds(zoom);
    AddMissingTrackWidgets(timeline_locked);
    UpdateChildTransform(now);
  }

  void WakeAnimationResponsively(Timeline& timeline_locked, time::SteadyPoint now) {
    PullTimelineState(timeline_locked, now);
    timeline_locked.WakeToys();  // wakes this & other widgets
    for (auto& t : track_widgets) {
      t->WakeAnimation();
    }
    // Only the children of the current timeline need to be updated responsively.
    for (auto& w : track_widgets) {
      w->RedrawThisFrame();
    }
  }

  void UpdateChildTransform(time::SteadyPoint now) {
    float track_width = max_track_length / time::FloatDuration(distance_to_seconds);

    float track_offset_x0 = kRulerLength / 2;
    float track_offset_x1 = track_width - kRulerLength / 2;

    float track_offset_x = lerp(track_offset_x0, track_offset_x1, current_pos_ratio);

    for (size_t i = 0; i < track_widgets.size(); ++i) {
      track_widgets[i]->local_to_parent =
          SkM44::Translate(-track_offset_x, -kRulerHeight - kMarginAroundTracks - kTrackHeight / 2 -
                                                i * (kTrackMargin + kTrackHeight));
    }
  }
  animation::Phase Tick(time::Timer& timer) override {
    auto timeline = LockObject<Timeline>();
    if (!timeline) return animation::Finished;
    auto lock = std::lock_guard(timeline->mutex);

    auto phase = animation::Finished;
    if ((timeline->state == Timeline::kPlaying) || (timeline->state == Timeline::kRecording)) {
      phase |= animation::Animating;
    }
    phase |= animation::ExponentialApproach(timeline->zoom, timer.d, 0.1, zoom);
    phase |= splice_wiggle.SpringTowards(0, timer.d, 0.3, 0.1);
    if (splice_action) {
      phase |= animation::Animating;
      splice_x = XAtTime(splice_action->splice_to) + splice_wiggle.value;
    }
    phase |= animation::ExponentialApproach(0, timer.d, 0.05, bridge_wiggle_s);

    if (phase == animation::Animating) {
      WakeAnimationResponsively(*timeline, timer.now);
    } else {
      PullTimelineState(*timeline, timer.now);
    }
    return phase;
  }
  void Draw(SkCanvas& canvas) const override {
    int track_count = track_widgets.size();
    auto wood_case_rrect = WoodenCaseRRect(track_count);
    SkPath wood_case_path = SkPath::RRect(wood_case_rrect);

    {  // Wooden case, light & shadow
      canvas.save();
      canvas.clipRRect(wood_case_rrect, true);
      canvas.drawPaint(WoodPaint());

      SkPaint outer_shadow;
      outer_shadow.setMaskFilter(SkMaskFilter::MakeBlur(kOuter_SkBlurStyle, 1_mm));
      SkPoint pts[2] = {{0, kPlasticTop + kWoodWidth},
                        {0, kPlasticTop + kWoodWidth - kWoodenCaseCornerRadius}};
      SkColor colors[2] = {"#aa6048"_color, "#2d1f1b"_color};

      outer_shadow.setShader(
          SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp));

      wood_case_path.toggleInverseFillType();
      canvas.drawPath(wood_case_path, outer_shadow);

      canvas.restore();
    }

    {  // Inset in the wooden case
      SkPaint inset_shadow;
      SkRRect inset_rrect = PlasticRRect(track_count);
      inset_rrect.outset(0.2_mm, 0.2_mm);
      inset_shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.2_mm));
      SkPoint pts[2] = {{0, inset_rrect.getBounds().fTop + inset_rrect.getSimpleRadii().y()},
                        {0, inset_rrect.getBounds().fTop}};
      SkColor colors[2] = {"#2d1f1b"_color, "#aa6048"_color};

      inset_shadow.setShader(
          SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp));
      canvas.drawRRect(inset_rrect, inset_shadow);
    }

    canvas.drawRRect(PlasticRRect(track_count), kPlasticPaint);

    NumberTextField::DrawBackground(canvas, kDisplayRRect.sk);
    // canvas.drawRRect(kDisplayRRect.sk, kDisplayPaint);

    constexpr float PI = numbers::pi;

    float current_pos_ratio =
        max_track_length == 0s ? 1 : current_offset / time::FloatDuration(max_track_length);

    function<Str(time::Duration)> format_time;
    if (max_track_length > 1h) {
      format_time = [](time::Duration t) {
        double t_s = time::ToSeconds(t);
        t_s = round(t_s * 1000) / 1000;
        int hours = t_s / 3600;
        t_s -= hours * 3600;
        int minutes = t_s / 60;
        t_s -= minutes * 60;
        int seconds = t_s;
        t_s -= seconds;
        int milliseconds = round(t_s * 1000);
        return f("{:02d}:{:02d}:{:02d}.{:03d} s", hours, minutes, seconds, milliseconds);
      };
    } else if (max_track_length > 1min) {
      format_time = [](time::Duration t) {
        double t_s = time::ToSeconds(t);
        t_s = round(t_s * 1000) / 1000;
        int minutes = t_s / 60;
        t_s -= minutes * 60;
        int seconds = t_s;
        t_s -= seconds;
        int milliseconds = round(t_s * 1000);
        return f("{:02d}:{:02d}.{:03d} s", minutes, seconds, milliseconds);
      };
    } else if (max_track_length >= 10s) {
      format_time = [](time::Duration t) {
        double t_s = time::ToSeconds(t);
        t_s = round(t_s * 1000) / 1000;
        int seconds = t_s;
        t_s -= seconds;
        int milliseconds = round(t_s * 1000);
        return f("{:02d}.{:03d} s", seconds, milliseconds);
      };
    } else {
      format_time = [](time::Duration t) {
        double t_s = time::ToSeconds(t);
        t_s = round(t_s * 1000) / 1000;
        int seconds = t_s;
        t_s -= seconds;
        int milliseconds = round(t_s * 1000);
        return f("{}.{:03d} s", seconds, milliseconds);
      };
    }

    Str total_text = format_time(max_track_length);
    Str current_text = format_time(current_offset);
    Str remaining_text = format_time(max_track_length - current_offset);

    auto& lcd_font = LcdFont();
    auto& font = GetFont();

    float left_column_width = lcd_font.MeasureText("Remaining");

    float text_width = left_column_width + 1_mm + font.MeasureText(total_text);

    canvas.save();
    canvas.translate(-kPlayButtonRadius - kDisplayMargin - kDisplayWidth + 1_mm,
                     kDisplayMargin + kLetterSize * 2 + 1_mm * 3);
    canvas.scale((kDisplayWidth - 2_mm) / text_width, 1);

    lcd_font.DrawText(canvas, "Current", kDisplayCurrentPaint);

    canvas.translate(left_column_width + 1_mm, 0);
    font.DrawText(canvas, current_text, kDisplayCurrentPaint);
    canvas.translate(-left_column_width - 1_mm, 0);

    canvas.translate(0, -kLetterSize - 1_mm);
    lcd_font.DrawText(canvas, "Total", kDisplayTotalPaint);

    canvas.translate(left_column_width + 1_mm, 0);
    font.DrawText(canvas, total_text, kDisplayTotalPaint);
    canvas.translate(-left_column_width - 1_mm, 0);

    canvas.translate(0, -kLetterSize - 1_mm);
    lcd_font.DrawText(canvas, "Remaining", kDisplayRemainingPaint);

    canvas.translate(left_column_width + 1_mm, 0);
    font.DrawText(canvas, remaining_text, kDisplayRemainingPaint);
    canvas.translate(-left_column_width - 1_mm, 0);

    canvas.restore();

    float bridge_offset_x = BridgeOffsetX(current_pos_ratio);

    ArcLine signal_line = ArcLine({bridge_offset_x, -kRulerHeight}, 90_deg);

    float x_behind_display =
        -kPlayButtonRadius - kDisplayMargin - kDisplayWidth - kDisplayMargin / 2;
    auto turn_shift = ArcLine::TurnShift(bridge_offset_x - x_behind_display, kDisplayMargin / 2);

    signal_line.MoveBy(kRulerHeight + kDisplayMargin / 2 - turn_shift.distance_forward / 2);
    turn_shift.Apply(signal_line);
    signal_line.MoveBy(kLetterSize * 2 + 1_mm * 3 + kDisplayMargin / 2 -
                       turn_shift.distance_forward / 2);
    signal_line.TurnConvex(-90_deg, kDisplayMargin / 2);

    // signal_line.TurnBy(M_PI_2, kDisplayMargin / 2);
    auto signal_path = signal_line.ToPath(false);
    canvas.drawPath(signal_path, kSignalPaint);

    float window_height = WindowHeight(track_count);
    auto window_path = WindowShape(track_count);

    canvas.save();
    canvas.clipPath(window_path, true);

    // Ruler
    canvas.drawPaint(kWindowPaint);

    Rect top_bar = Rect(-kWindowWidth / 2, -kRulerHeight, kWindowWidth / 2, 0);
    canvas.drawRect(top_bar, kRulerPaint);

    float ruler_pixels = canvas.getTotalMatrix().mapRadius(kRulerLength);

    int step;
    if (ruler_pixels < 20) {
      step = 10;
    } else if (ruler_pixels < 200) {
      step = 5;
    } else {
      step = 1;
    }

    for (int i = 0; i <= 100; i += step) {
      float x = kRulerLength * i / 100 - kRulerLength / 2;
      float h;
      if (i % 10 == 0) {
        h = kRulerHeight / 2;
      } else if (i % 5 == 0) {
        h = kRulerHeight / 3;
      } else {
        h = kRulerHeight / 4;
      }
      canvas.drawLine(x, -kRulerHeight, x, -kRulerHeight + h, kTickPaint);
    }

    Rect bottom_bar =
        Rect(-kWindowWidth / 2, -window_height, kWindowWidth / 2, -window_height + kRulerHeight);
    canvas.drawRect(bottom_bar, kRulerPaint);

    canvas.drawLine(bridge_offset_x, -kRulerHeight, bridge_offset_x, 0, kSignalPaint);

    // Bottom ticks
    {
      float track_width = max_track_length / time::FloatDuration(distance_to_seconds);

      // at time 0 the first tick is at -kRulerWidth / 2
      // at time 0 the last tick is at -kRulerWidth / 2 + track_width
      // at time END the first tick is at kRulerWidth / 2 - track_width
      // at time END the last tick is at kRulerWidth / 2

      float first_tick_x0 = -kRulerLength / 2;
      float first_tick_x1 = kRulerLength / 2 - track_width;

      float first_tick_x = lerp(first_tick_x0, first_tick_x1, current_pos_ratio);
      float last_tick_x = first_tick_x + track_width;

      float tick_every_x = 0.1s / time::FloatDuration(distance_to_seconds);

      int first_i = (-kWindowWidth / 2 - first_tick_x) / tick_every_x;
      first_i = max(0, first_i);

      int last_i = (kWindowWidth / 2 - first_tick_x) / tick_every_x;
      last_i = min<int>(last_i, (last_tick_x - first_tick_x) / tick_every_x);

      for (int i = first_i; i <= last_i; ++i) {
        float x = first_tick_x + i * tick_every_x;
        float h = kRulerHeight / 4;
        if (i % 10 == 0) {
          h *= 2;
        }
        canvas.drawLine(x, -window_height + kRulerHeight, x, -window_height + kRulerHeight - h,
                        kTickPaint);
      }
    }

    DrawChildrenSpan(canvas,
                     WidgetPtrSpan(const_cast<Vec<std::unique_ptr<ui::Widget>>&>(track_widgets)));

    bool draw_bridge_hairline = true;

    if (splice_action) {
      auto rect = Rect(splice_x, -window_height + kRulerHeight + kMarginAroundTracks,
                       bridge_offset_x, -kRulerHeight - kMarginAroundTracks);
      if (splice_x < bridge_offset_x) {
        // deleting
        SkPaint delete_paint;
        SkPoint pts[2] = {rect.TopLeftCorner(), rect.TopRightCorner()};
        constexpr int n = 10;
        SkColor colors[n] = {};
        float pos[n] = {};
        float w = rect.Width();
        float cx = w / 2;
        float r = 1_cm;
        SkColor shadow_color = "#801010"_color;
        for (int i = 0; i < n; ++i) {
          float a1 = i / (n - 1.f) * 2;
          float a2 = (n - 1 - i) / (n - 1.f) * 2;
          float a = min(a1, a2);
          colors[i] = SkColorSetA(shadow_color, lerp(255, 128, a));
          float t = i / (n - 1.f);
          // Don't try to unterstand this - it doesn't make sense - but looks ok.
          if (i <= n / 2) {
            pos[i] = t - (1 - a1) * r / w;
          } else {
            pos[i] = t + (1 - a2) * r / w;
          }
          pos[i] = clamp<float>(pos[i], 0, 1);
        }
        delete_paint.setShader(
            SkGradientShader::MakeLinear(pts, colors, pos, n, SkTileMode::kClamp));
        delete_paint.setBlendMode(SkBlendMode::kColorBurn);

        canvas.drawRect(rect, delete_paint);

        canvas.save();
        canvas.clipRect(rect);

        {  // Draw dust being sucked in
          auto current_time = time::SecondsSinceEpoch();
          int n_particles = 10;
          float particle_lifetime_s = 1;

          auto particle_absolute = current_time * n_particles;
          for (int i = 0; i < n_particles; ++i) {
            auto particle_i = floor(particle_absolute) - i;
            auto particle_start = particle_i / n_particles;
            auto particle_end = particle_start + particle_lifetime_s;
            auto particle_a = (current_time - particle_start) / particle_lifetime_s;

            // Fast fade in/out, slow in the middle
            particle_a = acos(1 - 2 * particle_a) / numbers::pi;

            float y = SeededFloat(rect.top, rect.bottom, particle_i);
            SkPaint paint;
            paint.setColor("#ffffff"_color);
            paint.setAlphaf(lerp(1, 0, particle_a));
            paint.setStrokeWidth(1_mm);
            paint.setStyle(SkPaint::kStrokeAndFill_Style);
            float x0;
            float x1;
            if ((uint64_t)particle_i & 1) {
              x0 = rect.left;
              x1 = rect.left + particle_a * w / 2;
            } else {
              x0 = rect.right;
              x1 = rect.right - particle_a * w / 2;
            }
            canvas.drawLine(x0, y, x1, y, paint);
          }
        }

        canvas.restore();

        // Two black vertical lines around the deleted region
        SkPaint delete_line_paint;
        delete_line_paint.setStyle(SkPaint::kStroke_Style);
        delete_line_paint.setStrokeWidth(0.2_mm);
        canvas.drawLine(rect.TopLeftCorner(), rect.BottomLeftCorner(), delete_line_paint);
        canvas.drawLine(rect.TopRightCorner(), rect.BottomRightCorner(), delete_line_paint);
        draw_bridge_hairline = false;
      } else {
        // Stretch the right part of the timeline
        rect.left = bridge_offset_x;
        rect.right = splice_x;
        Rect save_rect =
            Rect(bridge_offset_x, -window_height + kRulerHeight, kWindowWidth / 2, -kRulerHeight);
        SkPaint stretch_paint;
        stretch_paint.setImageFilter(SkImageFilters::MatrixTransform(
            SkMatrix::Translate(splice_x - bridge_offset_x, 0), kDefaultSamplingOptions,
            SkImageFilters::Crop(save_rect.sk, SkTileMode::kClamp, nullptr)));
        stretch_paint.setColor("#808080"_color);

        auto rec = SkCanvas::SaveLayerRec(&save_rect.sk, &stretch_paint,
                                          SkCanvas::kInitWithPrevious_SaveLayerFlag);
        canvas.save();
        canvas.clipRect(save_rect);
        canvas.saveLayer(rec);
        canvas.restore();
        canvas.restore();

        SkPaint new_paint;
        new_paint.setColor("#d1ffd1"_color);
        new_paint.setAlphaf(0.5);
        canvas.drawRect(rect, new_paint);
      }
    }

    canvas.restore();  // unclip

    // Screws
    auto DrawScrew = [&](float x, float y) {
      SkPaint inner_paint;
      inner_paint.setAntiAlias(true);
      inner_paint.setStyle(SkPaint::kStroke_Style);
      inner_paint.setStrokeWidth(0.1_mm);
      SkPoint pts[2] = {{x, y - kScrewRadius}, {x, y + kScrewRadius}};
      SkColor colors[2] = {"#615954"_color, "#fbf9f3"_color};
      auto inner_gradient =
          SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
      inner_paint.setShader(inner_gradient);

      SkPaint outer_paint;
      outer_paint.setAntiAlias(true);
      outer_paint.setStyle(SkPaint::kStroke_Style);
      outer_paint.setStrokeWidth(0.1_mm);
      SkColor outer_colors[2] = {"#fbf9f3"_color, "#615954"_color};
      auto outer_gradient =
          SkGradientShader::MakeLinear(pts, outer_colors, nullptr, 2, SkTileMode::kClamp);
      outer_paint.setShader(outer_gradient);

      canvas.drawCircle(x, y, kScrewRadius - 0.05_mm, inner_paint);
      canvas.drawCircle(x, y, kScrewRadius + 0.05_mm, outer_paint);
    };

    DrawScrew(kPlasticWidth / 2 - kScrewMargin - kScrewRadius,
              -WindowHeight(track_count) - kDisplayMargin + kScrewMargin + kScrewRadius);
    DrawScrew(-kPlasticWidth / 2 + kScrewMargin + kScrewRadius,
              -WindowHeight(track_count) - kDisplayMargin + kScrewMargin + kScrewRadius);
    DrawScrew(kPlasticWidth / 2 - kScrewMargin - kScrewRadius,
              kPlasticTop - kScrewMargin - kScrewRadius);
    DrawScrew(-kPlasticWidth / 2 + kScrewMargin + kScrewRadius,
              kPlasticTop - kScrewMargin - kScrewRadius);

    Widget* arr[] = {run_button.get(), prev_button.get(), next_button.get()};
    DrawChildrenSpan(canvas, arr);

    canvas.save();
    canvas.clipPath(window_path, true);

    {  // Window shadow
      SkPaint paint;
      paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 5_mm));
      window_path.toggleInverseFillType();
      canvas.drawPath(window_path, paint);
    }
    {  // Bridge
      float x = BridgeOffsetX(current_pos_ratio);
      float bottom_y = -(kMarginAroundTracks * 2 + kTrackHeight * track_count +
                         kTrackMargin * max(0, track_count - 1));
      if (draw_bridge_hairline) {
        SkPaint hairline;
        hairline.setColor(kBridgeLinePaint.getColor());
        hairline.setStyle(SkPaint::kStroke_Style);
        hairline.setAntiAlias(true);
        canvas.drawLine({x, -kRulerHeight}, {x, bottom_y - kRulerHeight}, hairline);
      }

      auto bridge_shape = BridgeShape(track_count, current_pos_ratio);

      canvas.save();

      canvas.clipPath(bridge_shape);
      canvas.drawPaint(kBridgeHandlePaint);

      SkPoint pts2[2] = {{x, 0}, {x + 0.4_mm, 0}};
      SkColor colors2[2] = {"#cb532d"_color, "#809d3312"_color};
      auto shader2 = SkGradientShader::MakeLinear(pts2, colors2, nullptr, 2, SkTileMode::kMirror);
      SkPaint wavy_paint;
      wavy_paint.setShader(shader2);
      wavy_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.5_mm));
      Rect wavy_rect = Rect(x - kMinimalTouchableSize / 2, -kRulerHeight - kMarginAroundTracks,
                            x + kMinimalTouchableSize / 2, -kRulerHeight);
      wavy_rect = wavy_rect.Outset(-0.5_mm);
      canvas.drawRect(wavy_rect.sk, wavy_paint);

      SkPaint bridge_stroke_paint;
      bridge_stroke_paint.setColor("#5d1e0a"_color);
      bridge_stroke_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.2_mm));
      bridge_shape.toggleInverseFillType();

      canvas.drawPath(bridge_shape, bridge_stroke_paint);

      canvas.restore();
    }
    {  // Splicer
      canvas.save();
      const static SkPaint kSplicerPaint = [] {
        SkPaint paint = kBridgeHandlePaint;
        // paint.setColor("#5d1e0a"_color);
        paint.setImageFilter(
            SkImageFilters::DropShadow(0, 0, 0.2_mm, 0.2_mm, "#000000"_color, nullptr));
        return paint;
      }();
      auto splicer_shape = SplicerShape(track_count, current_pos_ratio);
      canvas.drawPath(splicer_shape, kSplicerPaint);

      canvas.clipPath(splicer_shape);

      SkPaint splicer_stroke_paint;
      splicer_stroke_paint.setColor("#5d1e0a"_color);
      splicer_stroke_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.2_mm));
      splicer_shape.toggleInverseFillType();
      canvas.drawPath(splicer_shape, splicer_stroke_paint);

      canvas.restore();

      SkPaint icon_paint;
      icon_paint.setColor("#ffffff"_color);
      canvas.save();
      canvas.translate(bridge_offset_x, -window_height + kRulerHeight);
      canvas.scale(0.5, 0.5);
      auto icon = kScissorsIcon;
      if (splice_action) {
        if (splice_action->cancel) {
          icon = kCancelIcon;
        } else {
          auto true_splice_x = XAtTime(splice_action->splice_to);
          if (true_splice_x > bridge_offset_x) {
            icon = kPlusIcon;
          } else {
            icon = kMinusIcon;
          }
        }
      }
      canvas.drawPath(icon, icon_paint);
      canvas.restore();
    }
    {  // Zoom dial

      auto TickAngle = [](float tick0, float tick1) {
        return ((tick1 - tick0) / (tick1 + tick0)) * .5f;
      };

      float nearest_tick = NearestZoomTick(zoom);
      float next_tick, previous_tick;
      if (nearest_tick > zoom) {
        next_tick = nearest_tick;
        previous_tick = PreviousZoomTick(nearest_tick);
      } else {
        next_tick = NextZoomTick(nearest_tick);
        previous_tick = nearest_tick;
      }
      float ratio = (zoom - previous_tick) / (next_tick - previous_tick);
      float zero_dir = numbers::pi;
      float angle0 = lerp(0, -TickAngle(previous_tick, next_tick), ratio) + zero_dir;

      auto zoom_center = ZoomDialCenter(window_height);
      canvas.drawCircle(zoom_center, kZoomRadius, kZoomPaint);
      canvas.save();
      float zoom_text_width = lcd_font.MeasureText("ZOOM");
      Vec2 zoom_text_pos =
          Vec2::Polar(zero_dir, kZoomRadius - 0.5_mm - kZoomVisible / 2) + zoom_center;
      canvas.translate(zoom_text_pos.x - zoom_text_width / 2, zoom_text_pos.y);
      lcd_font.DrawText(canvas, "ZOOM", kZoomTextPaint);
      canvas.restore();

      Str current_zoom_text;
      if (zoom < 1) {
        current_zoom_text = f("{} ms", (int)roundf(zoom * 1000));
      } else {
        current_zoom_text = f("{:.1f} s", zoom);
      }
      {
        float text_width = lcd_font.MeasureText(current_zoom_text);
        canvas.save();
        canvas.translate(zoom_text_pos.x - text_width / 2, zoom_text_pos.y - kLcdFontSize - 1_mm);
        lcd_font.DrawText(canvas, current_zoom_text, kZoomTextPaint);
        canvas.restore();
      }

      float line_start = kZoomRadius - 1_mm;
      float line_end = kZoomRadius;

      float angle = angle0;
      float tick = previous_tick;

      while (tick <= 3600) {
        Vec2 p0 = Vec2::Polar(angle, line_start) + zoom_center;
        Vec2 p1 = Vec2::Polar(angle, line_end) + zoom_center;
        if (p1.y < -window_height || p1.x > kWindowWidth / 2) {
          break;
        }
        canvas.drawLine(p0.x, p0.y, p1.x, p1.y, kZoomTickPaint);
        float next = NextZoomTick(tick);
        angle += TickAngle(tick, next);
        tick = next;
      }
      angle = angle0;
      tick = previous_tick;
      while (angle >= 0.001) {
        Vec2 p0 = Vec2::Polar(angle, line_start) + zoom_center;
        Vec2 p1 = Vec2::Polar(angle, line_end) + zoom_center;
        if (p1.y < -window_height || p1.x > kWindowWidth / 2) {
          break;
        }
        canvas.drawLine(p0.x, p0.y, p1.x, p1.y, kZoomTickPaint);
        float prev = PreviousZoomTick(tick);
        angle -= TickAngle(prev, tick);
        tick = prev;
      }
    }

    canvas.restore();  // unclip
  }
  SkPath Shape() const override {
    auto r = WoodenCaseRRect(track_widgets.size());
    return SkPath::RRect(r);
  }
  bool CenteredAtZero() const override { return true; }
  void FillChildren(Vec<Widget*>& children) override {
    children.reserve(3 + track_widgets.size());
    children.push_back(run_button.get());
    children.push_back(prev_button.get());
    children.push_back(next_button.get());
    children.reserve(children.size() + track_widgets.size());
    for (auto& track_widget : track_widgets) {
      children.push_back(track_widget.get());
    }
  }
  std::unique_ptr<Action> FindAction(ui::Pointer&, ui::ActionTrigger) override;
  Vec2AndDir ArgStart(const Argument& arg, ui::Widget* coordinate_space = nullptr) override {
    auto timeline = LockObject<Timeline>();
    if (timeline) {
      auto lock = std::lock_guard(timeline->mutex);
      for (int i = 0; i < timeline->tracks.size(); ++i) {
        if (timeline->tracks[i].get() == &arg) {
          Vec2AndDir pos_dir = {
              .pos = {kPlasticWidth / 2, -kRulerHeight - kMarginAroundTracks - kTrackHeight / 2 -
                                             i * (kTrackMargin + kTrackHeight)},
              .dir = 0_deg,
          };
          if (coordinate_space) {
            auto m = TransformBetween(*this, *coordinate_space);
            pos_dir.pos = m.mapPoint(pos_dir.pos);
          }
          return pos_dir;
        }
      }
    }
    return Object::Toy::ArgStart(arg, coordinate_space);
  }
};

void PrevButton::Activate(ui::Pointer& ptr) {
  Button::Activate(ptr);
  if (auto* timeline_widget = GetTimelineWidget()) {
    if (auto timeline = timeline_widget->LockObject<Timeline>()) {
      auto lock = std::lock_guard(timeline->mutex);
      timeline_widget->SetPosRatio(*timeline, 0, ptr.root_widget.timer.now);
    }
  }
}

void NextButton::Activate(ui::Pointer& ptr) {
  Button::Activate(ptr);
  if (auto* timeline_widget = GetTimelineWidget()) {
    if (auto timeline = timeline_widget->LockObject<Timeline>()) {
      auto lock = std::lock_guard(timeline->mutex);
      timeline_widget->SetPosRatio(*timeline, 1, ptr.root_widget.timer.now);
    }
  }
}

struct DragBridgeAction : Action {
  TrackedPtr<TimelineWidget> timeline_widget;
  float press_offset_x;
  DragBridgeAction(ui::Pointer& pointer, TimelineWidget& timeline_widget_ref)
      : Action(pointer), timeline_widget(&timeline_widget_ref) {
    float initial_x = pointer.PositionWithin(timeline_widget_ref).x;
    float initial_pos_ratio = timeline_widget_ref.current_pos_ratio;
    float initial_bridge_x = BridgeOffsetX(initial_pos_ratio);
    press_offset_x = initial_x - initial_bridge_x;
    timeline_widget_ref.bridge_snapped = false;
  }
  ~DragBridgeAction() override {
    if (timeline_widget) {
      timeline_widget->bridge_snapped = false;
    }
  }
  void Update() override {
    if (!timeline_widget) return;
    auto timeline = timeline_widget->LockObject<Timeline>();
    if (!timeline) return;
    auto now = pointer.root_widget.timer.now;
    Vec2 pos = pointer.PositionWithin(*timeline_widget);
    pos.x -= press_offset_x;
    auto current_offset = timeline_widget->current_offset;
    auto time_at_x =
        time::Defloat(PosRatioFromBridgeOffsetX(pos.x) * timeline_widget->max_track_length);
    auto lock = std::lock_guard(timeline->mutex);
    if (auto snapped_time = timeline_widget->SnapToTrack(*timeline, pos, time_at_x)) {
      timeline_widget->bridge_wiggle_s = time::ToSeconds(current_offset - *snapped_time);
      timeline_widget->bridge_snapped = true;
      time_at_x = *snapped_time;
    } else if (auto snapped_time = timeline_widget->SnapToBottomRuler(pos, time_at_x)) {
      timeline_widget->bridge_wiggle_s = time::ToSeconds(current_offset - *snapped_time);
      timeline_widget->bridge_snapped = true;
      time_at_x = *snapped_time;
    } else {
      if (timeline_widget->bridge_snapped) {
        timeline_widget->bridge_wiggle_s = time::ToSeconds(current_offset - time_at_x);
      }
      timeline_widget->bridge_snapped = false;
    }
    timeline_widget->SetOffset(*timeline, time_at_x, now);
  }
};

struct DragTimelineAction : Action {
  TrackedPtr<TimelineWidget> timeline_widget;
  time::Duration initial_bridge_offset;
  float initial_x;
  DragTimelineAction(ui::Pointer& pointer, TimelineWidget& timeline_widget_ref)
      : Action(pointer), timeline_widget(&timeline_widget_ref) {
    Vec2 pos = pointer.PositionWithin(timeline_widget_ref);
    initial_bridge_offset = timeline_widget_ref.current_offset;
    initial_x = pos.x;
    timeline_widget_ref.bridge_snapped = false;
  }
  ~DragTimelineAction() override {
    if (timeline_widget) {
      timeline_widget->bridge_snapped = false;
    }
  }
  void Update() override {
    if (!timeline_widget) return;
    auto timeline = timeline_widget->LockObject<Timeline>();
    if (!timeline) return;
    Vec2 pos = pointer.PositionWithin(*timeline_widget);
    float x = pos.x;
    auto distance_to_seconds = timeline_widget->distance_to_seconds;  // [s]
    auto max_track_length = timeline_widget->max_track_length;
    auto denominator = max_track_length - kRulerLength * distance_to_seconds;  // [s]

    time::FloatDuration scaling_factor;
    if (abs(denominator) > 0.0001s) {
      scaling_factor = distance_to_seconds / denominator * max_track_length;
    } else {
      scaling_factor = 0s;
    }

    auto time_at_x = initial_bridge_offset - time::Defloat((x - initial_x) * scaling_factor);
    auto current_offset = timeline_widget->current_offset;
    auto lock = std::lock_guard(timeline->mutex);
    if (auto snapped_time = timeline_widget->SnapToTrack(*timeline, pos, time_at_x)) {
      time_at_x = *snapped_time;
      timeline_widget->bridge_wiggle_s = time::ToSeconds(current_offset - *snapped_time);
      timeline_widget->bridge_snapped = true;
    } else if (auto snapped_time = timeline_widget->SnapToBottomRuler(pos, time_at_x)) {
      time_at_x = *snapped_time;
      timeline_widget->bridge_wiggle_s = time::ToSeconds(current_offset - *snapped_time);
      timeline_widget->bridge_snapped = true;
    } else {
      if (timeline_widget->bridge_snapped) {
        timeline_widget->bridge_wiggle_s = time::ToSeconds(current_offset - time_at_x);
      }
      timeline_widget->bridge_snapped = false;
    }
    timeline_widget->SetOffset(*timeline, time_at_x, pointer.root_widget.timer.now);
  }
};

struct DragZoomAction : Action {
  TrackedPtr<TimelineWidget> timeline_widget;
  float last_y;
  DragZoomAction(ui::Pointer& pointer, TimelineWidget& timeline_widget_ref)
      : Action(pointer), timeline_widget(&timeline_widget_ref) {
    timeline_widget_ref.drag_zoom_action = this;
    last_y = pointer.PositionWithin(timeline_widget_ref).y;
  }
  ~DragZoomAction() override {
    if (timeline_widget) {
      timeline_widget->drag_zoom_action = nullptr;
      auto timeline = timeline_widget->LockObject<Timeline>();
      if (timeline) {
        auto lock = std::lock_guard(timeline->mutex);
        timeline->zoom = NearestZoomTick(timeline->zoom);
        timeline_widget->WakeAnimationResponsively(*timeline, time::SteadyNow());
      }
    }
  }
  void Update() override {
    if (!timeline_widget) return;
    float y = pointer.PositionWithin(*timeline_widget).y;
    float delta_y = y - last_y;
    last_y = y;
    float factor = expf(delta_y * 60);
    auto timeline = timeline_widget->LockObject<Timeline>();
    if (timeline) {
      timeline->zoom *= factor;
      timeline_widget->zoom *= factor;
      timeline->zoom = clamp(timeline->zoom, 0.001f, 3600.0f);
      timeline_widget->zoom = clamp(timeline_widget->zoom, 0.001f, 3600.0f);
      timeline_widget->WakeAnimationResponsively(*timeline, pointer.root_widget.timer.now);
    }
  }
};

SpliceAction::SpliceAction(ui::Pointer& pointer, TimelineWidget& timeline_widget_ref)
    : Action(pointer),
      timeline_widget(&timeline_widget_ref),
      resize_icon(pointer, ui::Pointer::kIconResizeHorizontal) {
  assert(timeline_widget_ref.splice_action == nullptr);
  timeline_widget_ref.splice_action = this;
  splice_to = timeline_widget_ref.current_offset;
  timeline_widget_ref.splice_wiggle.velocity -= 0.1;
  timeline_widget_ref.WakeAnimation();
}

SpliceAction::~SpliceAction() {
  if (!timeline_widget) return;
  timeline_widget->splice_action = nullptr;
  if (!cancel) {
    // Delete stuff between splice_to and current_offset
    auto current_offset = timeline_widget->current_offset;
    auto now = time::SteadyNow();
    auto timeline = timeline_widget->LockObject<Timeline>();
    if (timeline) {
      int num_tracks = timeline->tracks.size();
      for (int i = 0; i < num_tracks; ++i) {
        auto& track = timeline->tracks[i];
        track->track->Splice(current_offset, splice_to);
      }
      timeline->timeline_length += splice_to - current_offset;
      timeline_widget->AdjustOffset(*timeline, splice_to - current_offset, now);
    }
  }
}

void SpliceAction::Update() {
  if (!timeline_widget) return;
  auto pos = pointer.PositionWithin(*timeline_widget);
  int num_tracks = timeline_widget->track_widgets.size();
  float current_pos_ratio = timeline_widget->current_pos_ratio;
  float bridge_offset_x = BridgeOffsetX(current_pos_ratio);
  time::Duration new_splice_to;
  bool new_snapped = false;
  if (SplicerShape(num_tracks, current_pos_ratio).contains(pos.x, pos.y)) {
    new_splice_to = timeline_widget->TimeAtX(bridge_offset_x);
    new_snapped = true;
    cancel = true;
  } else {
    cancel = false;

    new_splice_to = timeline_widget->TimeAtX(pos.x);
    if (pos.x < bridge_offset_x) {
      if (auto timeline = timeline_widget->LockObject<Timeline>()) {
        auto lock = std::lock_guard(timeline->mutex);
        if (auto snapped_time = timeline_widget->SnapToTrack(*timeline, pos, new_splice_to)) {
          new_splice_to = *snapped_time;
          new_snapped = true;
        }
      }
    }
    if (auto snapped_time = timeline_widget->SnapToBottomRuler(pos)) {
      new_splice_to = *snapped_time;
      new_snapped = true;
    }
    new_splice_to = max<time::Duration>(0s, new_splice_to);
  }

  if (new_snapped || snapped) {
    time::FloatDuration distance_to_seconds = timeline_widget->distance_to_seconds;
    timeline_widget->splice_wiggle.value += (splice_to - new_splice_to) / distance_to_seconds;
  }
  snapped = new_snapped;
  splice_to = new_splice_to;
  timeline_widget->WakeAnimation();
}

std::unique_ptr<Action> TimelineWidget::FindAction(ui::Pointer& ptr, ui::ActionTrigger btn) {
  if (!IsIconified() && btn == ui::PointerButton::Left) {
    int n = track_widgets.size();
    auto splicer_shape = SplicerShape(n, current_pos_ratio);
    auto bridge_shape = BridgeShape(n, current_pos_ratio);
    auto window_shape = WindowShape(n);
    auto pos = ptr.PositionWithin(*this);
    if (splicer_shape.contains(pos.x, pos.y) && splice_action == nullptr) {
      return make_unique<SpliceAction>(ptr, *this);
    } else if (bridge_shape.contains(pos.x, pos.y)) {
      return make_unique<DragBridgeAction>(ptr, *this);
    } else if (window_shape.contains(pos.x, pos.y)) {
      if (pos.y < -kRulerHeight) {
        if (LengthSquared(pos - ZoomDialCenter(WindowHeight(n))) < kZoomRadius * kZoomRadius) {
          return make_unique<DragZoomAction>(ptr, *this);
        } else {
          return make_unique<DragTimelineAction>(ptr, *this);
        }
      } else {
        if (auto timeline = LockObject<Timeline>()) {
          auto lock = std::lock_guard(timeline->mutex);
          SetPosRatio(*timeline, PosRatioFromBridgeOffsetX(pos.x), ptr.root_widget.timer.now);
          return make_unique<DragBridgeAction>(ptr, *this);
        }
      }
    }
  }
  return Object::Toy::FindAction(ptr, btn);
}

std::unique_ptr<Object::Toy> Timeline::MakeToy(ui::Widget* parent) {
  return std::make_unique<TimelineWidget>(parent, *this);
}

void Timeline::Atoms(const function<void(Atom&)>& cb) {
  for (auto& track_arg : tracks) {
    cb(*track_arg);
  }
  cb(next_arg);
  cb(run);
  cb(running);
}

struct TrackBaseWidget : Object::Toy {
  using Object::Toy::Toy;

  // Many functions within TrackBaseWidget query the same information. This class:
  // - caches these results to avoid repeated lookups &
  // - provides a single place to define lookup logic for code reuse.
  struct Context {
    const TrackBaseWidget& widget;
    Context(const TrackBaseWidget& widget) : widget(widget) {}

    Ptr<TrackBase> track_ptr = nullptr;
    Ptr<TrackBase>& GetTrackPtr() {
      if (track_ptr == nullptr) {
        track_ptr = widget.LockObject<TrackBase>();
      }
      return track_ptr;
    }

    template <typename T>
    T& GetTrack() {
      return static_cast<T&>(*GetTrackPtr());
    }

    Timeline* timeline = nullptr;
    Timeline* GetTimeline() {
      if (timeline == nullptr) {
        timeline = GetTrackPtr()->timeline;
      }
      return timeline;
    }

    Optional<TimelineWidget*> timeline_widget = nullopt;
    TimelineWidget* GetTimelineWidget() {
      if (!timeline_widget) {
        timeline_widget = static_cast<TimelineWidget*>(widget.parent.get());
      }
      return *timeline_widget;
    }

    Optional<time::FloatDuration> distance_to_seconds = nullopt;
    time::FloatDuration& GetDistanceToSeconds() {
      if (!distance_to_seconds) {
        if (auto timeline_widget = GetTimelineWidget()) {
          distance_to_seconds = timeline_widget->distance_to_seconds;
        } else {
          distance_to_seconds = 100s;  // 1 cm = 1 second
        }
      }
      return *distance_to_seconds;
    }
  };
  SkPath Shape() const override {
    Context ctx(*this);
    return Shape(ctx);
  }
  SkPath Shape(Context& ctx) const {
    time::FloatDuration distance_to_seconds = ctx.GetDistanceToSeconds();
    auto* timeline_widget = ctx.GetTimelineWidget();
    auto* timeline = ctx.GetTimeline();
    auto lock = std::lock_guard(timeline->mutex);
    auto& track = ctx.GetTrack<TrackBase>();
    auto end_time = timeline ? timeline_widget->max_track_length : track.timestamps.back();
    Rect rect = Rect(0, -kTrackHeight / 2, end_time / distance_to_seconds, kTrackHeight / 2);
    if (timeline) {
      // Clip to the width of the timeline window
      rect.right =
          min<float>(rect.right, timeline_widget->TimeAtX(kWindowWidth / 2) / distance_to_seconds);
      rect.left =
          max<float>(rect.left, timeline_widget->TimeAtX(-kWindowWidth / 2) / distance_to_seconds);
    }
    return SkPath::Rect(rect.sk);
  }
  Optional<Rect> TextureBounds() const override {
    Context ctx(*this);
    auto* timeline_widget = ctx.GetTimelineWidget();
    if (timeline_widget == nullptr || timeline_widget->drag_zoom_action == nullptr) {
      return Object::Toy::TextureBounds();
    }
    return nullopt;
  }
  void Draw(SkCanvas& canvas) const override {
    Context ctx(*this);
    Draw(canvas, ctx);
  }
  void Draw(SkCanvas& canvas, Context& ctx) const { canvas.drawPath(Shape(ctx), kTrackPaint); }
  std::unique_ptr<Action> FindAction(ui::Pointer& ptr, ui::ActionTrigger btn) override {
    Context ctx(*this);
    if (auto* timeline_widget = ctx.GetTimelineWidget()) {
      return timeline_widget->FindAction(ptr, btn);
    } else {
      return Object::Toy::FindAction(ptr, btn);
    }
  }
};

struct OnOffTrackWidget : TrackBaseWidget {
  using TrackBaseWidget::TrackBaseWidget;
  void Draw(SkCanvas& canvas) const override {
    Context ctx(*this);
    auto* timeline_widget = ctx.GetTimelineWidget();
    auto* timeline = ctx.GetTimeline();
    auto& track = ctx.GetTrack<OnOffTrack>();
    auto& timestamps = track.timestamps;
    TrackBaseWidget::Draw(canvas, ctx);
    auto shape = Shape(ctx);
    Rect rect;
    shape.isRect(&rect.sk);
    time::FloatDuration distance_to_seconds = ctx.GetDistanceToSeconds();
    auto DrawSegment = [&](time::Duration start_t, time::Duration end_t) {
      float start = start_t / distance_to_seconds;
      float end = end_t / distance_to_seconds;
      if (end < rect.left || start > rect.right) {
        return;
      }
      start = max(start, rect.left);
      end = min(end, rect.right);
      canvas.drawLine({start, 0}, {end, 0}, kOnOffPaint);
    };
    for (int i = 0; i + 1 < timestamps.size(); i += 2) {
      DrawSegment(timestamps[i], timestamps[i + 1]);
    }
    if (track.on_at != time::kDurationGuard) {
      switch (timeline->state) {
        case Timeline::kRecording:
          DrawSegment(track.on_at, timeline_widget->current_offset);
          break;
        case Timeline::kPlaying:
        case Timeline::kPaused:
          // segment ends at the right edge of the track
          DrawSegment(track.on_at, timeline->MaxTrackLength());
          break;
      }
    }
  }
};

static int LowerBound(TrackBase& track, time::Duration t) {
  auto& timestamps = track.timestamps;
  return lower_bound(timestamps.begin(), timestamps.end(), t) - timestamps.begin();
}

static int UpperBound(TrackBase& track, time::Duration t) {
  auto& timestamps = track.timestamps;
  return upper_bound(timestamps.begin(), timestamps.end(), t) - timestamps.begin();
}

struct Vec2TrackWidget : TrackBaseWidget {
  using TrackBaseWidget::TrackBaseWidget;
  void Draw(SkCanvas& canvas) const override {
    Context ctx(*this);
    auto& track = ctx.GetTrack<Vec2Track>();
    auto& timestamps = track.timestamps;
    auto& values = track.values;
    auto* timeline_widget = ctx.GetTimelineWidget();
    auto* timeline = ctx.GetTimeline();
    TrackBaseWidget::Draw(canvas, ctx);
    auto shape = Shape(ctx);
    Rect rect;
    shape.isRect(&rect.sk);
    time::FloatDuration s_per_m = timeline_widget->distance_to_seconds;  // s / m
    auto left_t = time::Defloat(rect.left * s_per_m);
    auto right_t = time::Defloat(rect.right * s_per_m);
    auto left_i = LowerBound(track, left_t);
    auto right_i = UpperBound(track, right_t);

    float px_per_meter = canvas.getLocalToDeviceAs3x3().mapVector(1, 0).length();  // [px / m]
    float m_per_px = 1.f / px_per_meter;                                           // [m / px]
    auto pixel_t = time::Defloat(s_per_m * m_per_px);                              // [s / px]

    {  // draw horizontal segments

      int i = left_i;
      int segments = 0;
      // Draw the timestamps as horizontal lines (segments). Each timestamp is a 1-pixel long
      // segment. Multiple timestamps whose segments overlap are combined into a single segment.
      while (i < right_i) {
        int start_i = i;
        auto start_t = timestamps[i];    // start time of a line segment
        auto end_t = start_t + pixel_t;  // end time of a line segment
        // keep expanding end_t until a gap of at least one pixel is found
        while (true) {
          // find the first timestep that's outside of the current [start_t, end_t] segment
          int leftmost_i_outside_segment =
              upper_bound(timestamps.begin() + i, timestamps.begin() + right_i, end_t) -
              timestamps.begin();
          if (leftmost_i_outside_segment >= right_i) {
            i = right_i;
            end_t = std::min(end_t, right_t);
            break;
          }
          // take one step back to get the last timestamp that's within the current end_t
          int rightmost_i_inside_segment = leftmost_i_outside_segment - 1;
          if (rightmost_i_inside_segment == i) {
            // one-pixel gap found, exit to draw the segment (and also advance i for next iteration)
            ++i;
            break;
          }
          i = rightmost_i_inside_segment;
          end_t = timestamps[i] + pixel_t;
        }
        ++segments;
        canvas.drawLine(start_t / s_per_m, (kTrackHeight - kVec2Paint.getStrokeWidth()) / 2,
                        end_t / s_per_m, (kTrackHeight - kVec2Paint.getStrokeWidth()) / 2,
                        kVec2Paint);
        canvas.drawLine(start_t / s_per_m, -(kTrackHeight - kVec2Paint.getStrokeWidth()) / 2,
                        end_t / s_per_m, -(kTrackHeight - kVec2Paint.getStrokeWidth()) / 2,
                        kVec2Paint);
      }
    }

    {  // draw displays
      constexpr float kDisplayHeight = kTrackHeight - kVec2DisplayMargin * 2;
      constexpr float kDisplayMinWidth = kDisplayHeight;
      auto max_track_length = timeline->MaxTrackLength();
      auto current_t = timeline_widget->current_offset_raw;
      int current_i = LowerBound(track, current_t);
      auto display_min_t = time::Defloat(kDisplayMinWidth * s_per_m);
      struct Vec2Display {
        time::Duration start_t, end_t;
        int start_i, end_i;  // end_i is not inclusive
      };
      Vec2Display root;
      root.start_t = 0s;
      root.end_t = max_track_length;
      root.start_i = LowerBound(track, root.start_t);
      root.end_i = UpperBound(track, root.end_t);
      // Snap the analysis window to the actual timestamps that exist in the track.
      root.start_t = timestamps[root.start_i];
      root.end_t = timestamps[root.end_i - 1];
      auto timestamp_gaps = vector<time::Duration>(timestamps.size() - 1);
      for (int i = 0; i < timestamps.size() - 1; ++i) {
        timestamp_gaps[i] = timestamps[i + 1] - timestamps[i];
      }
      auto gap_tree = SegmentTree(timestamp_gaps.size(), [&](int left, int right) {
        return timestamp_gaps[right] > timestamp_gaps[left];
      });
      for (int i = 0; i < timestamps.size() - 1; ++i) {
        gap_tree.Update(i);
      }
      std::vector<Vec2Display> displays_to_split;
      std::vector<Vec2Display> displays_to_draw;
      if (root.start_i < root.end_i) {
        displays_to_split.push_back(root);
      }
      constexpr bool kDebugSplitting = false;
      while (!displays_to_split.empty()) {
        auto display = displays_to_split.back();
        displays_to_split.pop_back();
        if (display.start_t > right_t + display_min_t / 2) {
          continue;
        }
        if (display.end_t < left_t - display_min_t / 2) {
          continue;
        }

        auto earliest_split_t = display.start_t + display_min_t;
        auto latest_split_t = display.end_t - display_min_t;
        if (earliest_split_t >= latest_split_t) {
          {  // re-center the displays that contain very few points
            auto first_point = timestamps[display.start_i];
            auto last_point = timestamps[display.end_i - 1];
            if (last_point - first_point < display_min_t) {
              if (display_min_t >= max_track_length) {
                display.start_t = 0s;
                display.end_t = max_track_length;
              } else {
                auto center = clamp((first_point + last_point) / 2, display_min_t / 2,
                                    max_track_length - display_min_t / 2);
                display.start_t = center - display_min_t / 2;
                display.end_t = center + display_min_t / 2;
              }
            }
          }
          displays_to_draw.push_back(display);
          continue;
        }
        // the lowest index that can be moved into the next display
        int earliest_split_i = UpperBound(track, earliest_split_t);
        auto initial_gap_t =
            std::min(latest_split_t, timestamps[earliest_split_i]) - earliest_split_t;

        int gap_i = -1;
        time::Duration gap_t = time::kDurationGuard;

        // the lowest index that must belong to the next display
        int latest_split_i = LowerBound(track, latest_split_t) - 1;
        auto final_gap_t = latest_split_t - timestamps[latest_split_i];
        if (earliest_split_i < latest_split_i) {
          gap_i = gap_tree.Query(earliest_split_i, latest_split_i - 1);
          gap_t = timestamp_gaps[gap_i];
        }

        if constexpr (kDebugSplitting) {
          LOG << "Splitting display from " << time::ToSeconds(display.start_t) << " to "
              << time::ToSeconds(display.end_t);
          LOG << "  earliest split at " << time::ToSeconds(earliest_split_t) << " < ["
              << earliest_split_i << "]=" << time::ToSeconds(timestamps[earliest_split_i]);
          LOG << "  latest split at " << time::ToSeconds(latest_split_t) << " [" << latest_split_i
              << "]=" << time::ToSeconds(timestamps[latest_split_i]);
          LOG << "  found gap with duration " << time::ToSeconds(gap_t) << " [" << gap_i
              << "]=" << time::ToSeconds(timestamps[gap_i]);
          LOG << "  initial gap duration " << time::ToSeconds(initial_gap_t) << " ["
              << earliest_split_i << "]=" << time::ToSeconds(timestamps[earliest_split_i]);
          LOG << "  final gap duration " << time::ToSeconds(final_gap_t) << " [" << latest_split_i
              << "]=" << time::ToSeconds(timestamps[latest_split_i]);
        }

        if (gap_t >= initial_gap_t && gap_t >= final_gap_t) {
          Vec2Display left = {.start_t = display.start_t,
                              .end_t = timestamps[gap_i],
                              .start_i = display.start_i,
                              .end_i = gap_i + 1};
          Vec2Display right = {.start_t = timestamps[gap_i + 1],
                               .end_t = display.end_t,
                               .start_i = gap_i + 1,
                               .end_i = display.end_i};
          displays_to_split.push_back(left);
          displays_to_split.push_back(right);
          if constexpr (kDebugSplitting) LOG << "  chose split at gap";
        } else if (initial_gap_t > final_gap_t) {
          Vec2Display left = {.start_t = display.start_t,
                              .end_t = earliest_split_t,
                              .start_i = display.start_i,
                              .end_i = earliest_split_i};
          Vec2Display right = {.start_t = std::min(latest_split_t, timestamps[earliest_split_i]),
                               .end_t = display.end_t,
                               .start_i = earliest_split_i,
                               .end_i = display.end_i};
          displays_to_split.push_back(left);
          displays_to_split.push_back(right);
          if constexpr (kDebugSplitting) LOG << "  chose split at earliest split";
        } else {
          Vec2Display left = {.start_t = display.start_t,
                              .end_t = timestamps[latest_split_i],
                              .start_i = display.start_i,
                              .end_i = latest_split_i + 1};
          Vec2Display right = {.start_t = latest_split_t,
                               .end_t = display.end_t,
                               .start_i = latest_split_i + 1,
                               .end_i = display.end_i};
          displays_to_split.push_back(left);
          displays_to_split.push_back(right);
          if constexpr (kDebugSplitting) LOG << "  chose split at latest split";
        }
      }

      for (auto& display : displays_to_draw) {
        auto display_start_t = display.start_t;
        auto display_end_t = display.end_t;
        int start_i = display.start_i;
        int end_i = display.end_i;

        auto display_duration = display_end_t - display_start_t;
        float display_width = display_duration / s_per_m;  // division as double (doesn't floor)
        SkPath trail;
        trail.moveTo(0, 0);
        Vec2 cursor = {};
        for (int i = start_i; i < end_i; ++i) {
          cursor += Vec2(values[i].x, -values[i].y);
          trail.lineTo(cursor.x, cursor.y);
        }
        Rect bounds = trail.getBounds().makeOutset(1, 1);

        auto rect = Rect(display_start_t / s_per_m + kVec2DisplayMargin / 2, -kDisplayHeight / 2,
                         display_end_t / s_per_m - kVec2DisplayMargin / 2, kDisplayHeight / 2);
        auto clip = RRect::MakeSimple(rect, std::min(kVec2DisplayMargin, rect.Width() / 2));
        if (end_i > start_i) {
          canvas.save();
          canvas.clipRRect(clip.sk, true);
          canvas.translate(rect.CenterX(), rect.CenterY());
          float display_height_px =
              canvas.getLocalToDeviceAs3x3().mapVector(kDisplayHeight, 0).length();
          float scale = 10 * kDisplayHeight / display_height_px;
          if (auto scale_right = display_width / 2 / bounds.right; scale_right < scale) {
            scale = scale_right;
          }
          if (auto scale_left = -display_width / 2 / bounds.left; scale_left < scale) {
            scale = scale_left;
          }
          if (auto scale_top = kDisplayHeight / 2 / bounds.top; scale_top < scale) {
            scale = scale_top;
          }
          if (auto scale_bottom = -kDisplayHeight / 2 / bounds.bottom; scale_bottom < scale) {
            scale = scale_bottom;
          }
          scale *= 0.9;  // 10% margin
          canvas.scale(scale, scale);

          auto matrix = canvas.getLocalToDeviceAs3x3();
          SkMatrix inverse;
          (void)matrix.invert(&inverse);

          SkVector dpd[2] = {SkVector(1, 0), SkVector(0, 1)};
          inverse.mapVectors(dpd);
          SkPaint display_paint;
          display_paint.setShader(mouse::GetPixelGridRuntimeEffect().makeShader(
              SkData::MakeWithCopy((void*)&dpd, sizeof(dpd)), nullptr, 0));
          canvas.drawPaint(display_paint);

          SkPaint trail_paint;
          trail_paint.setColor("#131c64"_color);
          trail_paint.setStyle(SkPaint::kStroke_Style);
          SkPaint border_paint;
          border_paint.setColor("#888888"_color);
          border_paint.setStyle(SkPaint::kStroke_Style);
          if (dpd[0].x() < 1) {
            trail_paint.setStrokeWidth(1);
            trail_paint.setStrokeCap(SkPaint::kSquare_Cap);
            trail_paint.setStrokeJoin(SkPaint::kMiter_Join);
            trail_paint.setStrokeMiter(2);
          }
          canvas.drawPath(trail, trail_paint);

          if (current_i >= start_i && current_i < end_i) {
            Vec2 previous_point = trail.getPoint(current_i - start_i);
            Vec2 current_point = trail.getPoint(current_i - start_i + 1);
            auto delta = current_point - previous_point;
            auto length = Length(delta);
            float outset = max(1_mm / scale, 1.5f);
            auto outline_rect = Rect(previous_point.x, previous_point.y, previous_point.x + length,
                                     previous_point.y)
                                    .Outset(outset);
            auto outline_rrect = RRect::MakeSimple(outline_rect, outset);
            SkMatrix transform;
            transform.setSinCos(delta.y / length, delta.x / length, previous_point.x,
                                previous_point.y);
            auto outline_path = SkPath::RRect(outline_rrect.sk).makeTransform(transform);
            SkPaint outline_paint;
            outline_paint.setColor(kOrange);
            if (timestamps[current_i] == current_t) {
              outline_paint.setStyle(SkPaint::kStrokeAndFill_Style);
              canvas.drawPath(outline_path, outline_paint);
              canvas.drawLine(previous_point, current_point, trail_paint);
            } else {
              outline_paint.setStyle(SkPaint::kStroke_Style);
              canvas.drawPath(outline_path, outline_paint);
            }
          }

          canvas.restore();
          canvas.drawRRect(clip.sk, border_paint);
        }
      }
    }
  }
};

struct Float64TrackWidget : TrackBaseWidget {
  animation::SpringV2<float> y_max;
  animation::SpringV2<float> y_min;
  SkPath trail;

  using TrackBaseWidget::TrackBaseWidget;

  animation::Phase Tick(time::Timer& t) override {
    auto phase = animation::Finished;
    Context ctx(*this);
    auto& track = ctx.GetTrack<Float64Track>();
    auto* timeline_widget = ctx.GetTimelineWidget();
    auto& timestamps = track.timestamps;
    auto& values = track.values;

    Vec<double> absolute_values;
    {  // precompute absolute_values
      absolute_values.reserve(values.size());
      double y = 0;
      for (int i = 0; i < values.size(); ++i) {
        y += values[i];
        absolute_values.emplace_back(y);
      }
    }

    time::FloatDuration s_per_m = timeline_widget->distance_to_seconds;  // s / m
    auto shape = Shape(ctx);
    Rect shape_rect;
    shape.isRect(&shape_rect.sk);
    auto left_t = time::Defloat(shape_rect.left * s_per_m);
    auto right_t = time::Defloat(shape_rect.right * s_per_m);
    auto left_i = LowerBound(track, left_t);
    auto right_i = UpperBound(track, right_t);

    SkPathBuilder trail_builder;
    double last_y = left_i > 0 ? absolute_values[left_i - 1] : 0;
    double y_min_target = last_y, y_max_target = last_y;
    trail_builder.moveTo(shape_rect.left, last_y);
    for (int i = left_i; i < right_i; ++i) {
      double x = timestamps[i] / s_per_m;
      trail_builder.lineTo(x, last_y);
      double y = absolute_values[i];
      y_min_target = std::min(y_min_target, y);
      y_max_target = std::max(y_max_target, y);
      trail_builder.lineTo(x, y);
      last_y = y;
    }
    trail_builder.lineTo(shape_rect.right, last_y);

    phase |= y_min.SineTowards(y_min_target, t.d, 0.5);
    phase |= y_max.SineTowards(y_max_target, t.d, 0.5);

    double trail_height = y_max - y_min;
    double trail_middle = (y_max + y_min) / 2;

    SkMatrix m = SkMatrix::Translate(0, -trail_middle);
    trail_height = std::max(1., trail_height);
    if (trail_height > 0) {
      m.postScale(1, (kTrackHeight - 1_mm) / trail_height);
    }
    trail_builder.transform(m);
    trail = trail_builder.detach();

    return phase;
  }

  void Draw(SkCanvas& canvas) const override {
    Context ctx(*this);
    TrackBaseWidget::Draw(canvas, ctx);

    SkPaint trail_paint;
    trail_paint.setColor("#131c64"_color);
    trail_paint.setStyle(SkPaint::kStroke_Style);
    SkPaint border_paint;
    border_paint.setColor("#888888"_color);
    border_paint.setStyle(SkPaint::kStroke_Style);
    canvas.drawPath(trail, trail_paint);
  }
};

std::unique_ptr<Object::Toy> OnOffTrack::MakeToy(ui::Widget* parent) {
  auto ret = std::make_unique<OnOffTrackWidget>(parent, *this);
  return ret;
}

std::unique_ptr<Object::Toy> Vec2Track::MakeToy(ui::Widget* parent) {
  auto ret = std::make_unique<Vec2TrackWidget>(parent, *this);
  return ret;
}

std::unique_ptr<Object::Toy> Float64Track::MakeToy(ui::Widget* parent) {
  auto ret = std::make_unique<Float64TrackWidget>(parent, *this);
  return ret;
}

template <typename T>
static void TrackSplice(Vec<time::Duration>& timestamps, Vec<T>& values,
                        time::Duration current_offset, time::Duration splice_to) {
  auto delta = splice_to - current_offset;
  auto [current_offset_ge, current_offset_g] =
      equal_range(timestamps.begin(), timestamps.end(), current_offset);
  if (delta < 0s) {
    // splice_to < current_offset
    auto [splice_to_ge, splice_to_g] = equal_range(timestamps.begin(), timestamps.end(), splice_to);
    bool begin_has_event = splice_to_ge != splice_to_g;
    bool end_has_event = current_offset_ge != current_offset_g;
    // Boundary events are usually kept except when there are two of them.
    // If that's the case then they're both deleted.
    bool delete_boundaries = begin_has_event && end_has_event;
    auto begin_it = delete_boundaries ? splice_to_ge : splice_to_g;
    auto end_it = delete_boundaries ? current_offset_g : current_offset_ge;
    int begin = begin_it - timestamps.begin();
    int end = end_it - timestamps.begin();
    if (begin != end) {
      timestamps.erase(begin_it, end_it);
      values.erase(values.begin() + begin, values.begin() + end);
    }
    for (int i = begin; i < timestamps.size(); ++i) {
      // `delta` is negative so this actually shifts subsequent events back in time.
      timestamps[i] += delta;
    }
  } else if (delta > 0s) {
    for (auto it = current_offset_ge; it != timestamps.end(); ++it) {
      *it += delta;
    }
  }
}

void Vec2Track::Splice(time::Duration current_offset, time::Duration splice_to) {
  TrackSplice(timestamps, values, current_offset, splice_to);
}

void Float64Track::Splice(time::Duration current_offset, time::Duration splice_to) {
  TrackSplice(timestamps, values, current_offset, splice_to);
}

void Vec2Track::UpdateOutput(Location& target, time::SteadyPoint started_at,
                             time::SteadyPoint now) {
  auto cmp = [started_at](time::Duration timestamp, time::SteadyPoint now) {
    return started_at + timestamp < now;
  };
  int next_update_i =
      std::lower_bound(timestamps.begin(), timestamps.end(), now, cmp) - timestamps.begin();
  if (next_update_i == timestamps.size()) {
    return;
  }
  auto next_update_at = started_at + time::Duration(timestamps[next_update_i]);
  if (next_update_at != now) {
    return;
  }
  auto* mouse_move = target.As<MouseMove>();
  if (mouse_move) {
    mouse_move->OnMouseMove(values[next_update_i]);
  }
}
void Float64Track::UpdateOutput(Location& target, time::SteadyPoint started_at,
                                time::SteadyPoint now) {
  auto cmp = [started_at](time::Duration timestamp, time::SteadyPoint now) {
    return started_at + timestamp < now;
  };
  int next_update_i =
      std::lower_bound(timestamps.begin(), timestamps.end(), now, cmp) - timestamps.begin();
  if (next_update_i == timestamps.size()) {
    return;
  }
  auto next_update_at = started_at + time::Duration(timestamps[next_update_i]);
  if (next_update_at != now) {
    return;
  }
  auto* sink = target.As<SinkRelativeFloat64>();
  if (sink) {
    sink->OnRelativeFloat64(values[next_update_i]);
  }
}

static void WakeRunButton(Timeline& timeline) {
  timeline.ForEachToy([](ui::RootWidget& root_widget, ui::Widget& widget) {
    TimelineWidget& timeline_widget = static_cast<TimelineWidget&>(widget);
    timeline_widget.run_button->WakeAnimation();
  });
}

void Timeline::Running::OnCancel() {
  auto& t = GetTimeline();
  if (t.state == kPlaying) {
    TimelineCancelScheduled(t);
    t.state = kPaused;
    t.paused = {.playback_offset = time::SteadyNow() - t.playing.started_at};
    TimelineUpdateOutputs(t, time::SteadyPoint{},
                          time::SteadyPoint{} + time::Duration(t.paused.playback_offset));
    WakeRunButton(t);
  }
}

void Timeline::Run::OnRun(std::unique_ptr<RunTask>& run_task) {
  ZoneScopedN("Timeline");
  auto& t = GetTimeline();
  if (t.state != kPaused) {
    return;
  }
  if (t.paused.playback_offset >= t.MaxTrackLength()) {
    t.paused.playback_offset = 0s;
  }
  t.state = kPlaying;
  time::SteadyPoint now = time::SteadyNow();
  t.playing = {.started_at = now - time::Duration(t.paused.playback_offset)};
  TimelineUpdateOutputs(t, t.playing.started_at, now);
  TimelineScheduleNextAfter(t, now);
  WakeRunButton(t);
  t.WakeToys();
  t.running.BeginLongRunning(std::move(run_task));
}

void Timeline::BeginRecording() {
  switch (state) {
    case Timeline::kPaused:
      state = Timeline::kRecording;
      recording.started_at = time::SteadyNow() - time::Duration(paused.playback_offset);
      break;
    case Timeline::kRecording:
      // WTF? Maybe show an error?
      break;
    case Timeline::kPlaying:
      state = Timeline::kRecording;
      recording.started_at = playing.started_at;
      break;
  }
  WakeRunButton(*this);
  WakeToys();
}

void Timeline::StopRecording() {
  if (state != Timeline::kRecording) {
    return;
  }
  timeline_length = MaxTrackLength();
  paused = {.playback_offset = min(time::SteadyNow() - recording.started_at, timeline_length)};
  state = Timeline::kPaused;
  WakeRunButton(*this);
  WakeToys();
}

void OnOffTrack::Splice(time::Duration current_offset, time::Duration splice_to) {
  auto delta = splice_to - current_offset;
  auto [current_offset_ge, current_offset_g] =
      equal_range(timestamps.begin(), timestamps.end(), current_offset);
  if (delta < 0s) {
    auto [splice_to_ge, splice_to_g] = equal_range(timestamps.begin(), timestamps.end(), splice_to);
    int begin = splice_to_ge - timestamps.begin();
    int end = current_offset_g - timestamps.begin();
    int count = end - begin;
    if (count & 1) {
      timestamps[begin] = splice_to;
      begin += 1;
      count &= ~1;
    }
    for (int i = begin; i < timestamps.size() - count; ++i) {
      timestamps[i] = timestamps[i + count] + delta;
    }
    timestamps.resize(timestamps.size() - count);
  } else if (delta > 0s) {
    for (auto it = current_offset_ge; it != timestamps.end(); ++it) {
      *it += delta;
    }
  }
}

void OnOffTrack::UpdateOutput(Location& target, time::SteadyPoint started_at,
                              time::SteadyPoint now) {
  int i = 0;
  for (; i < timestamps.size(); ++i) {
    if (started_at + time::Duration(timestamps[i]) > now) {
      break;
    }
  }
  i = i - 1;
  bool on = i % 2 == 0;
  if (timeline->state != Timeline::kPlaying) {
    on = false;
  }
  if (auto on_off = (OnOff*)(*target.object)) {
    if (on) {
      on_off->TurnOn();
    } else {
      on_off->TurnOff();
    }
  } else if (auto runnable = dynamic_cast<Runnable*>(target.object.get())) {
    if (on) {
      runnable->ScheduleRun(*target.object);
    } else {
      if (auto long_running = target.object->AsLongRunning(); long_running->IsRunning()) {
        long_running->Cancel();
      }
    }
  } else {
    ERROR << "Target is not runnable!";
  }
}

void Timeline::OnTimerNotification(Location& here, time::SteadyPoint now) {
  auto end_at = playing.started_at + time::Duration(MaxTrackLength());
  if (now >= end_at) {
    state = kPaused;
    paused = {.playback_offset = MaxTrackLength()};
    running.Done(*this);
    WakeRunButton(*this);
  }
  TimelineUpdateOutputs(*this, playing.started_at, now);
  if (now < end_at) {
    TimelineScheduleNextAfter(*this, now);
  }
}

bool OnOffTrack::IsOn() const {
  if (timeline->state == Timeline::kPaused) {
    return false;
  }
  auto now = time::SteadyNow();
  auto current_offset = timeline->CurrentOffset(now);

  int i = 0;
  for (; i < timestamps.size(); ++i) {
    if (timestamps[i] > current_offset) {
      break;
    }
  }
  i = i - 1;
  bool on = i % 2 == 0;
  return on;
}

void TrackBase::SerializeState(ObjectSerializer& writer) const {
  writer.Key("timestamps");
  writer.StartArray();
  for (auto t : timestamps) {
    writer.Double(time::ToSeconds(t));
  }
  writer.EndArray();
}

void OnOffTrack::SerializeState(ObjectSerializer& writer) const {
  TrackBase::SerializeState(writer);
  if (on_at != time::kDurationGuard) {
    writer.Key("on_at");
    writer.Double(time::ToSeconds(on_at));
  }
}

void Vec2Track::SerializeState(ObjectSerializer& writer) const {
  TrackBase::SerializeState(writer);
  writer.Key("values");
  writer.StartArray();
  for (const auto& v : values) {
    writer.StartArray();
    writer.Double(v.x);
    writer.Double(v.y);
    writer.EndArray();
  }
  writer.EndArray();
}

void Float64Track::SerializeState(ObjectSerializer& writer) const {
  TrackBase::SerializeState(writer);
  writer.Key("values");
  writer.StartArray();
  for (const auto& v : values) {
    writer.Double(v);
  }
  writer.EndArray();
}

void Timeline::SerializeState(ObjectSerializer& writer) const {
  writer.Key("tracks");
  writer.StartObject();
  for (int i = 0; i < tracks.size(); ++i) {
    writer.Key(tracks[i]->name.c_str());
    writer.StartObject();
    writer.Key("type");
    auto type = tracks[i]->track->Name();
    writer.String(type.data(), type.size());
    tracks[i]->track->SerializeState(writer);
    writer.EndObject();
  }
  writer.EndObject();
  writer.Key("zoom");
  writer.Double(zoom);
  writer.Key("length");
  writer.Double(time::ToSeconds(timeline_length));

  switch (state) {
    case kPaused:
      writer.Key("paused");
      writer.Double(time::ToSeconds(paused.playback_offset));
      break;
    case kPlaying:
      writer.Key("playing");
      writer.Double(time::ToSeconds(time::SteadyNow() - playing.started_at));
      break;
    case kRecording:
      writer.Key("recording");
      writer.Double(time::ToSeconds(time::SteadyNow() - recording.started_at));
      break;
  }
}

bool TrackBase::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "timestamps") {
    timestamps.clear();
    Status status;
    for (int i : ArrayView(d, status)) {
      double t;
      d.Get(t, status);
      if (OK(status)) {
        timestamps.push_back(time::FromSeconds(t));
      }
    }
    if (!OK(status)) {
      ReportError(status.ToStr());
    }
    return true;
  }
  return false;
}

bool OnOffTrack::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "on_at") {
    Status status;
    double t;
    d.Get(t, status);
    on_at = time::FromSeconds(t);
    if (!OK(status)) {
      ReportError(status.ToStr());
    }
    return true;
  }
  return TrackBase::DeserializeKey(d, key);
}

bool Vec2Track::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "values") {
    values.clear();
    Status status;
    for (int i : ArrayView(d, status)) {
      Vec2 v;
      for (int j : ArrayView(d, status)) {
        if (j == 0) {
          d.Get(v.x, status);
        } else if (j == 1) {
          d.Get(v.y, status);
        }
      }
      if (OK(status)) {
        values.push_back(v);
      }
    }
    if (!OK(status)) {
      ReportError(status.ToStr());
    }
    return true;
  }
  return TrackBase::DeserializeKey(d, key);
}

bool Float64Track::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "values") {
    values.clear();
    Status status;
    for (int i : ArrayView(d, status)) {
      double v;
      d.Get(v, status);
      if (OK(status)) {
        values.push_back(v);
      }
    }
    if (!OK(status)) {
      ReportError(status.ToStr());
    }
    return true;
  }
  return TrackBase::DeserializeKey(d, key);
}

bool Timeline::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  here = MyLocation();
  if (key == "tracks") {
    for (auto& track_name : ObjectView(d, status)) {
      Str track_type = "";
      TrackBase* track = nullptr;
      for (auto& track_key : ObjectView(d, status)) {
        if (track_key == "type") {
          d.Get(track_type, status);
          if (OK(status)) {
            if (track_type == "On/Off Track") {
              track = &AddOnOffTrack(track_name);
            } else if (track_type == "Vec2 Track") {
              track = &AddVec2Track(track_name);
            } else if (track_type == "Float64 Track") {
              track = &AddFloat64Track(track_name);
            } else {
              AppendErrorMessage(status) += f("Unknown track type: {}", track_type);
            }
          }
        } else {
          if (track) {
            track->DeserializeKey(d, track_key);
          } else {
            d.Skip();
          }
        }
      }
    }
  } else if (key == "zoom") {
    d.Get(zoom, status);
  } else if (key == "length") {
    double t;
    d.Get(t, status);
    timeline_length = time::FromSeconds(t);
  } else if (key == "paused") {
    state = kPaused;
    double t;
    d.Get(t, status);
    paused.playback_offset = time::FromSeconds(t);
  } else if (key == "playing") {
    state = kPlaying;
    double value = 0;
    d.Get(value, status);
    time::SteadyPoint now = time::SteadyNow();
    playing.started_at = now - time::FromSeconds(value);
    // We're not updating the outputs because they should be deserialized in a proper state
    // TimelineUpdateOutputs(l, *this, playing.started_at, now);
    TimelineScheduleNextAfter(*this, now);
    running.BeginLongRunning(std::make_unique<RunTask>(AcquireWeakPtr(), &run));
  } else if (key == "recording") {
    state = kRecording;
    double value = 0;
    d.Get(value, status);
    auto duration_double_s = std::chrono::duration<double>(value);
    recording.started_at =
        time::SteadyNow() - std::chrono::duration_cast<time::Duration>(duration_double_s);
  } else {
    return false;
  }
  if (!OK(status)) {
    ReportError(status.ToStr());
  }
  return true;
}

SkRRect SideButton::RRect() const {
  SkRect oval = SkRect::MakeXYWH(0, 0, 2 * kSideButtonRadius, 2 * kSideButtonRadius);
  return SkRRect::MakeOval(oval);
}
}  // namespace automat::library
