// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_timeline.hh"

#include <include/core/SkBlendMode.h>
#include <include/core/SkBlurTypes.h>
#include <include/core/SkColor.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include <algorithm>
#include <cmath>
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
#include "gui_button.hh"
#include "gui_constants.hh"
#include "gui_shape_widget.hh"
#include "key_button.hh"
#include "library_mouse_move.hh"
#include "math.hh"
#include "number_text_field.hh"
#include "pointer.hh"
#include "random.hh"
#include "root_widget.hh"
#include "sincos.hh"
#include "status.hh"
#include "svg.hh"
#include "textures.hh"
#include "time.hh"

using namespace automat::gui;
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

static Rect PlasticRect(const Timeline& t) {
  return Rect(-kPlasticWidth / 2, -WindowHeight(t.tracks.size()) - kPlasticBottom,
              kPlasticWidth / 2, kPlasticTop);
}

static Rect WoodenCaseRect(const Timeline& t) { return PlasticRect(t).Outset(kWoodWidth); }

static SkRRect WoodenCaseRRect(const Timeline& t) {
  return SkRRect::MakeRectXY(WoodenCaseRect(t).sk, kWoodenCaseCornerRadius,
                             kWoodenCaseCornerRadius);
}

static SkRRect PlasticRRect(const Timeline& t) {
  return SkRRect::MakeRectXY(PlasticRect(t), kPlasticCornerRadius, kPlasticCornerRadius);
}

constexpr RRect kDisplayRRect = []() {
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

const SkPaint kPlasticPaint = []() {
  SkPaint p;
  // p.setColor("#f0eae5"_color);
  SkPoint pts[2] = {{0, kPlasticTop}, {0, 0}};
  SkColor colors[3] = {"#f2ece8"_color, "#e0dbd8"_color};
  sk_sp<SkShader> gradient =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
  p.setShader(gradient);
  return p;
}();

const SkPaint kDisplayCurrentPaint = []() {
  SkPaint p;
  p.setColor(kOrange);
  return p;
}();

const SkPaint kDisplayTotalPaint = []() {
  SkPaint p;
  p.setColor("#4a4c3a"_color);
  return p;
}();

const SkPaint kDisplayRemainingPaint = []() {
  SkPaint p;
  p.setColor("#666a4d"_color);
  return p;
}();

const SkPaint kRulerPaint = []() {
  SkPaint p;
  p.setColor("#4e4e4e"_color);
  return p;
}();

const SkPaint kTrackPaint = []() {
  SkPaint p;
  // SkPoint pts[2] = {{0, 0}, {kTrackWidth, 0}};
  // SkColor colors[3] = {"#787878"_color, "#f3f3f3"_color, "#787878"_color};
  // sk_sp<SkShader> gradient =
  //     SkGradientShader::MakeLinear(pts, colors, nullptr, 3, SkTileMode::kClamp);
  // p.setShader(gradient);
  p.setColor("#d3d3d3"_color);
  return p;
}();

const SkPaint kWindowPaint = []() {
  SkPaint p;
  p.setColor("#1b1b1b"_color);
  return p;
}();

const SkPaint kTickPaint = []() {
  SkPaint p;
  p.setColor("#313131"_color);
  p.setStyle(SkPaint::kStroke_Style);
  return p;
}();

const SkPaint kBridgeHandlePaint = []() {
  SkPaint p;
  SkPoint pts[2] = {{0, -kRulerHeight - kMarginAroundTracks}, {0, -kRulerHeight}};
  SkColor colors[2] = {kOrange, "#f17149"_color};
  auto shader = SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
  p.setShader(shader);
  return p;
}();

const SkPaint kBridgeLinePaint = []() {
  SkPaint p;
  p.setColor(kOrange);
  p.setStyle(SkPaint::kStroke_Style);
  p.setStrokeWidth(1_mm);
  return p;
}();

const SkPaint kSignalPaint = []() {
  SkPaint p = kBridgeLinePaint;
  p.setStrokeWidth(0.5_mm);
  p.setAlpha(0x80);
  p.setBlendMode(SkBlendMode::kHardLight);
  return p;
}();

const SkPaint kOnOffPaint = []() {
  SkPaint p;
  p.setColor("#57dce4"_color);
  p.setStyle(SkPaint::kStroke_Style);
  p.setStrokeWidth(2_mm);
  p.setBlendMode(SkBlendMode::kMultiply);
  return p;
}();

const SkPaint kZoomPaint = []() {
  SkPaint p;
  p.setColor("#000000"_color);
  p.setAlphaf(0.5f);
  return p;
}();

const SkPaint kZoomTextPaint = []() {
  SkPaint p;
  p.setColor("#ffffff"_color);
  p.setAlphaf(0.9f);
  return p;
}();

const SkPaint kZoomTickPaint = []() {
  SkPaint p;
  p.setColor("#ffffff"_color);
  p.setAlphaf(0.9f);
  p.setStyle(SkPaint::kStroke_Style);
  return p;
}();

const SkMatrix kHorizontalFlip = SkMatrix::Scale(-1, 1);

PrevButton::PrevButton()
    : SideButton(MakeShapeWidget(kNextShape, SK_ColorWHITE, &kHorizontalFlip)) {}

NextButton::NextButton() : SideButton(MakeShapeWidget(kNextShape, SK_ColorWHITE)) {}

static SkPath GetPausedPath() {
  static SkPath path = []() {
    SkPath path;
    path.addRect(-1.5_mm, -1.5_mm, -0.5_mm, 1.5_mm);
    path.addRect(0.5_mm, -1.5_mm, 1.5_mm, 1.5_mm);
    return path;
  }();
  return path;
}

static SkPath GetRecPath() {
  static SkPath path = []() {
    SkPath path;
    path.addCircle(0, 0, 2.5_mm);
    return path;
  }();
  return path;
}

static constexpr SkColor kTimelineButtonBackground = "#fdfcfb"_color;
SkColor SideButton::ForegroundColor() const { return "#404040"_color; }
SkColor SideButton::BackgroundColor() const { return kTimelineButtonBackground; }

TimelineRunButton::TimelineRunButton(Timeline* timeline)
    : gui::ToggleButton(
          MakePtr<ColoredButton>(
              GetPausedPath(),
              ColoredButtonArgs{.fg = kTimelineButtonBackground,
                                .bg = kOrange,
                                .radius = kPlayButtonRadius,
                                .on_click = [this](gui::Pointer& p) { Activate(p); }}),
          MakePtr<ColoredButton>(
              kPlayShape, ColoredButtonArgs{.fg = kOrange,
                                            .bg = kTimelineButtonBackground,
                                            .radius = kPlayButtonRadius,
                                            .on_click = [this](gui::Pointer& p) { Activate(p); }})),
      timeline(timeline),
      rec_button(MakePtr<ColoredButton>(
          GetRecPath(), ColoredButtonArgs{.fg = kTimelineButtonBackground,
                                          .bg = color::kParrotRed,
                                          .radius = kPlayButtonRadius,
                                          .on_click = [this](gui::Pointer& p) { Activate(p); }})) {}

void TimelineRunButton::Activate(gui::Pointer& p) {
  switch (timeline->state) {
    case Timeline::kPlaying:
      timeline->Cancel();
      break;
    case Timeline::kPaused:
      if (auto h = timeline->here.lock()) {
        h->ScheduleRun();
      }
      break;
    case Timeline::kRecording:
      timeline->StopRecording();
      break;
  }
}

void TimelineRunButton::FixParents() {
  rec_button->parent = off->parent = on->parent = AcquirePtr();
  on->FixParents();
  off->FixParents();
  rec_button->FixParents();
}

void TimelineRunButton::ForgetParents() {
  parent = nullptr;
  rec_button->ForgetParents();
  on->ForgetParents();
  off->ForgetParents();
}

Ptr<gui::Button>& TimelineRunButton::OnWidget() {
  if (timeline->state == Timeline::kRecording) {
    last_on_widget = &rec_button;
  } else if (timeline->state == Timeline::kPlaying) {
    last_on_widget = &on;
  } else if (last_on_widget == nullptr) {
    last_on_widget = &on;
  }
  return *last_on_widget;
}

bool TimelineRunButton::Filled() const {
  bool filled = timeline->state == Timeline::kRecording || timeline->IsRunning();
  if (auto h = timeline->here.lock()) {
    filled |= (h->run_task && h->run_task->scheduled);
  }
  return filled;
}

Timeline::Timeline()
    : run_button(MakePtr<TimelineRunButton>(this)),
      prev_button(MakePtr<PrevButton>()),
      next_button(MakePtr<NextButton>()),
      state(kPaused),
      paused{.playback_offset = 0},
      zoom(10) {
  run_button->local_to_parent = SkM44::Translate(-kPlayButtonRadius, kDisplayMargin);
  prev_button->local_to_parent =
      SkM44::Translate(-kPlasticWidth / 2 + kSideButtonMargin, -kSideButtonRadius);
  next_button->local_to_parent = SkM44::Translate(
      kPlasticWidth / 2 - kSideButtonMargin - kSideButtonDiameter, -kSideButtonRadius);
}

struct TrackArgument : Argument {
  TextDrawable icon;
  TrackArgument(StrView name)
      : Argument(name, Argument::kOptional), icon(name, kKeyLetterSize, KeyFont()) {}
  PaintDrawable& Icon() override { return icon; }
};

static void AddTrackArg(Timeline& t, int track_number, StrView track_name) {
  auto arg = make_unique<TrackArgument>(track_name);
  arg->field = t.tracks[track_number].get();
  arg->tint = "#17aeb7"_color;
  arg->light = "#17aeb7"_color;
  t.track_args.emplace_back(std::move(arg));
}

OnOffTrack& Timeline::AddOnOffTrack(StrView name) {
  auto track_ptr = MakePtr<OnOffTrack>();
  auto& track = *track_ptr;
  AddTrack(std::move(track_ptr), name);
  return track;
}

Vec2Track& Timeline::AddVec2Track(StrView name) {
  auto track_ptr = MakePtr<Vec2Track>();
  auto& track = *track_ptr;
  AddTrack(std::move(track_ptr), name);
  return track;
}

void Timeline::AddTrack(Ptr<TrackBase>&& track, StrView name) {
  track->timeline = this;
  track->parent = this->AcquirePtr();
  tracks.emplace_back(std::move(track));
  AddTrackArg(*this, tracks.size() - 1, name);
  if (auto h = here.lock()) {
    h->InvalidateConnectionWidgets(true, true);
  }
  UpdateChildTransform(time::SteadyNow());
}

Timeline::Timeline(const Timeline& other) : Timeline() {
  tracks.reserve(other.tracks.size());
  for (const auto& track : other.tracks) {
    tracks.emplace_back(track->Clone().Cast<TrackBase>());
  }
  track_args.reserve(other.track_args.size());
  for (int i = 0; i < other.track_args.size(); ++i) {
    AddTrackArg(*this, i, other.track_args[i]->name);
  }
  UpdateChildTransform(time::SteadyNow());
}

string_view Timeline::Name() const { return "Timeline"; }

Ptr<Object> Timeline::Clone() const { return MakePtr<Timeline>(*this); }

constexpr float kLcdFontSize = 1.5_mm;
static Font& LcdFont() {
  static unique_ptr<Font> font =
      Font::MakeV2(Font::MakeWeightVariation(Font::GetNotoSans(), 700), kLcdFontSize);
  return *font.get();
}

time::T Timeline::MaxTrackLength() const {
  time::T max_track_length = timeline_length;
  if (state == kRecording) {
    max_track_length = max(max_track_length, (time::SteadyNow() - recording.started_at).count());
  }
  for (const auto& track : tracks) {
    if (track->timestamps.empty()) {
      continue;
    }
    max_track_length = max(max_track_length, track->timestamps.back());
  }
  return max_track_length;
}

static float CurrentPosRatio(const Timeline& timeline) {
  time::T max_track_length = timeline.MaxTrackLength();
  if (max_track_length == 0) {
    return 1;
  }
  switch (timeline.state) {
    case Timeline::kPlaying:
      return (timeline.playing.now - timeline.playing.started_at).count() / max_track_length;
    case Timeline::kPaused:
      return timeline.paused.playback_offset / max_track_length;
    case Timeline::kRecording:
      return (timeline.recording.now - timeline.recording.started_at).count() / max_track_length;
  }
}

void TimelineCancelScheduled(Timeline& t) {
  if (auto h = t.here.lock()) {
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
  auto cmp = [started_at](time::SteadyPoint now, time::T timestamp) {
    return started_at + time::Duration(timestamp) > now;
  };
  for (const auto& track : t.tracks) {
    int next_update_i =
        std::upper_bound(track->timestamps.begin(), track->timestamps.end(), now, cmp) -
        track->timestamps.begin();
    if (next_update_i < track->timestamps.size()) {
      auto next_update_point =
          t.playing.started_at + time::Duration(track->timestamps[next_update_i]);
      next_update = min(next_update, next_update_point);
    }
  }
  if (auto h = t.here.lock()) {
    ScheduleAt(*h, next_update);
  }
}

static void TimelineUpdateOutputs(Location& here, Timeline& t, time::SteadyPoint started_at,
                                  time::SteadyPoint now) {
  for (int i = 0; i < t.tracks.size(); ++i) {
    auto obj_result = t.track_args[i]->GetObject(here);
    if (obj_result.location != nullptr && obj_result.object != nullptr) {
      t.tracks[i]->UpdateOutput(*obj_result.location, started_at, now);
    }
    t.track_args[i]->InvalidateConnectionWidgets(here);
  }
}

static time::T CurrentOffset(const Timeline& timeline, time::SteadyPoint now) {
  switch (timeline.state) {
    case Timeline::kPlaying:
      return (now - timeline.playing.started_at).count();
    case Timeline::kPaused:
      return timeline.paused.playback_offset;
    case Timeline::kRecording:
      return (now - timeline.recording.started_at).count();
  }
}

static void SetOffset(Timeline& timeline, time::T offset, time::SteadyPoint now) {
  offset = clamp<time::T>(offset, 0, timeline.MaxTrackLength());
  if (timeline.state == Timeline::kPlaying) {
    TimelineCancelScheduled(timeline);
    timeline.playing.started_at = now - time::Duration(offset);
    if (auto h = timeline.here.lock()) {
      TimelineUpdateOutputs(*h, timeline, timeline.playing.started_at, now);
    }
    TimelineScheduleNextAfter(timeline, now);
  } else if (timeline.state == Timeline::kPaused) {
    timeline.paused.playback_offset = offset;
  }
  timeline.WakeAnimation();
}

void AdjustOffset(Timeline& timeline, time::T offset, time::SteadyPoint now) {
  if (timeline.state == Timeline::kPlaying) {
    TimelineCancelScheduled(timeline);
    timeline.playing.started_at -= time::Duration(offset);
    timeline.playing.started_at = min(timeline.playing.started_at, now);
    if (auto h = timeline.here.lock()) {
      TimelineUpdateOutputs(*h, timeline, timeline.playing.started_at, now);
    }
    TimelineScheduleNextAfter(timeline, now);
  } else if (timeline.state == Timeline::kPaused) {
    timeline.paused.playback_offset =
        clamp<time::T>(timeline.paused.playback_offset + offset, 0, timeline.MaxTrackLength());
  }
  timeline.WakeAnimation();
}

void SetPosRatio(Timeline& timeline, float pos_ratio, time::SteadyPoint now) {
  pos_ratio = clamp(pos_ratio, 0.0f, 1.0f);
  time::T max_track_length = timeline.MaxTrackLength();
  if (timeline.state == Timeline::kPlaying) {
    TimelineCancelScheduled(timeline);
    timeline.playing.started_at = now - time::Duration((time::T)(pos_ratio * max_track_length));
    if (auto h = timeline.here.lock()) {
      TimelineUpdateOutputs(*h, timeline, timeline.playing.started_at, now);
    }
    TimelineScheduleNextAfter(timeline, now);
  } else if (timeline.state == Timeline::kPaused) {
    timeline.paused.playback_offset = pos_ratio * max_track_length;
  }
  timeline.WakeAnimation();
}

void NextButton::Activate(gui::Pointer& ptr) {
  Button::Activate(ptr);
  if (auto timeline = Closest<Timeline>(*ptr.hover)) {
    SetPosRatio(*timeline, 1, ptr.root_widget.timer.now);
  }
}

void PrevButton::Activate(gui::Pointer& ptr) {
  Button::Activate(ptr);
  if (Timeline* timeline = Closest<Timeline>(*ptr.hover)) {
    SetPosRatio(*timeline, 0, ptr.root_widget.timer.now);
  }
}

static float BridgeOffsetX(float current_pos_ratio) {
  return -kRulerLength / 2 + kRulerLength * current_pos_ratio;
}

static float PosRatioFromBridgeOffsetX(float bridge_offset_x) {
  return (bridge_offset_x + kRulerLength / 2) / kRulerLength;
}

static float DistanceToSeconds(const Timeline& timeline) {
  return timeline.zoom.value / kWindowWidth;
}

time::T TimeAtX(const Timeline& timeline, float x) {
  // Find the time at the center of the timeline
  float distance_to_seconds = DistanceToSeconds(timeline);
  float current_pos_ratio = CurrentPosRatio(timeline);
  float track_width = timeline.MaxTrackLength();

  float center_t0 = kRulerLength / 2 * distance_to_seconds;
  float center_t1 = track_width - kRulerLength / 2 * distance_to_seconds;
  float center_t = lerp(center_t0, center_t1, current_pos_ratio);
  return center_t + x * distance_to_seconds;
}

static float XAtTime(const Timeline& timeline, time::T t) {
  float distance_to_seconds = DistanceToSeconds(timeline);
  float current_pos_ratio = CurrentPosRatio(timeline);
  float track_width = timeline.MaxTrackLength();

  float center_t0 = kRulerLength / 2 * distance_to_seconds;
  float center_t1 = track_width - kRulerLength / 2 * distance_to_seconds;
  float center_t = lerp(center_t0, center_t1, current_pos_ratio);

  return (t - center_t) / distance_to_seconds;
}

SkPath SplicerShape(int num_tracks, float current_pos_ratio) {
  static const SkPath splicer_shape = []() {
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

static Optional<time::T> SnapToTrack(Timeline& timeline, Vec2 pos, time::T time_at_x = NAN,
                                     float distance_to_seconds = NAN) {
  auto track_index = TrackIndexFromY(pos.y);
  if (track_index >= 0 && track_index < timeline.tracks.size()) {
    auto& track = timeline.tracks[track_index];
    auto& timestamps = track->timestamps;
    if (isnan(time_at_x)) {
      time_at_x = TimeAtX(timeline, pos.x);
    }
    auto last = upper_bound(timestamps.begin(), timestamps.end(), time_at_x);
    int right_i = last - timestamps.begin();
    time::T closest_dist = INFINITY;
    time::T closest_t = NAN;
    if (right_i >= 0 && right_i < timestamps.size()) {
      time::T right_dist = fabs(time_at_x - timestamps[right_i]);
      if (right_dist < closest_dist) {
        closest_dist = right_dist;
        closest_t = timestamps[right_i];
      }
    }
    int left_i = right_i - 1;
    if (left_i >= 0 && left_i < timestamps.size()) {
      time::T left_dist = fabs(time_at_x - timestamps[left_i]);
      if (left_dist < closest_dist) {
        closest_dist = left_dist;
        closest_t = timestamps[left_i];
      }
    }
    if (isnan(distance_to_seconds)) {
      distance_to_seconds = DistanceToSeconds(timeline);
    }
    if (closest_dist < 1_mm * distance_to_seconds) {
      return closest_t;
    }
  }
  return nullopt;
}

static Optional<time::T> SnapToBottomRuler(Timeline& timeline, Vec2 pos, time::T time_at_x = NAN) {
  int num_tracks = timeline.tracks.size();
  float window_height = WindowHeight(num_tracks);
  if (pos.y > -window_height && pos.y < -window_height + kRulerHeight) {
    float zoom = timeline.zoom.target;
    double zoom_log = log10(zoom / 200.0);
    double tick_mult = pow(10, ceil(zoom_log));
    if (isnan(time_at_x)) {
      time_at_x = TimeAtX(timeline, pos.x);
    }
    if (time_at_x >= 0 && time_at_x < timeline.timeline_length) {
      return round(time_at_x / tick_mult) * tick_mult;
    }
  }
  return nullopt;
}

struct DragBridgeAction : Action {
  float press_offset_x;
  Timeline& timeline;
  DragBridgeAction(gui::Pointer& pointer, Timeline& timeline)
      : Action(pointer), timeline(timeline) {
    float initial_x = pointer.PositionWithin(timeline).x;
    float initial_pos_ratio = CurrentPosRatio(timeline);
    float initial_bridge_x = BridgeOffsetX(initial_pos_ratio);
    press_offset_x = initial_x - initial_bridge_x;
    timeline.bridge_snapped = false;
  }
  ~DragBridgeAction() override { timeline.bridge_snapped = false; }
  void Update() override {
    auto now = pointer.root_widget.timer.now;
    Vec2 pos = pointer.PositionWithin(timeline);
    pos.x -= press_offset_x;
    auto current_offset = CurrentOffset(timeline, now);
    auto time_at_x = PosRatioFromBridgeOffsetX(pos.x) * timeline.MaxTrackLength();
    if (auto snapped_time = SnapToTrack(timeline, pos, time_at_x)) {
      timeline.bridge_wiggle_s += current_offset - *snapped_time;
      timeline.bridge_snapped = true;
      time_at_x = *snapped_time;
    } else if (auto snapped_time = SnapToBottomRuler(timeline, pos, time_at_x)) {
      timeline.bridge_wiggle_s += current_offset - *snapped_time;
      timeline.bridge_snapped = true;
      time_at_x = *snapped_time;
    } else {
      if (timeline.bridge_snapped) {
        timeline.bridge_wiggle_s += current_offset - time_at_x;
      }
      timeline.bridge_snapped = false;
    }
    SetOffset(timeline, time_at_x, now);
  }
};

struct DragTimelineAction : Action {
  Timeline& timeline;
  time::T initial_bridge_offset;
  float initial_x;
  DragTimelineAction(gui::Pointer& pointer, Timeline& timeline)
      : Action(pointer), timeline(timeline) {
    Vec2 pos = pointer.PositionWithin(timeline);
    initial_bridge_offset = CurrentOffset(timeline, pointer.root_widget.timer.now);
    initial_x = pos.x;
    timeline.bridge_snapped = false;
  }
  ~DragTimelineAction() override { timeline.bridge_snapped = false; }
  void Update() override {
    auto now = pointer.root_widget.timer.now;
    Vec2 pos = pointer.PositionWithin(timeline);
    float x = pos.x;
    float distance_to_seconds = DistanceToSeconds(timeline);
    float max_track_length = timeline.MaxTrackLength();
    float denominator = max_track_length - kRulerLength * distance_to_seconds;

    float scaling_factor;
    if (fabs(denominator) > 0.0001) {
      scaling_factor = distance_to_seconds * max_track_length /
                       (max_track_length - kRulerLength * distance_to_seconds);
    } else {
      scaling_factor = 0;
    }

    auto time_at_x = initial_bridge_offset - (x - initial_x) * scaling_factor;
    auto current_offset = CurrentOffset(timeline, now);
    if (auto snapped_time = SnapToTrack(timeline, pos, time_at_x, distance_to_seconds)) {
      time_at_x = *snapped_time;
      timeline.bridge_wiggle_s += current_offset - *snapped_time;
      timeline.bridge_snapped = true;
    } else if (auto snapped_time = SnapToBottomRuler(timeline, pos, time_at_x)) {
      time_at_x = *snapped_time;
      timeline.bridge_wiggle_s += current_offset - *snapped_time;
      timeline.bridge_snapped = true;
    } else {
      if (timeline.bridge_snapped) {
        timeline.bridge_wiggle_s += current_offset - time_at_x;
      }
      timeline.bridge_snapped = false;
    }
    SetOffset(timeline, time_at_x, now);
  }
};

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

struct DragZoomAction : Action {
  Timeline& timeline;
  float last_y;
  DragZoomAction(gui::Pointer& pointer, Timeline& timeline);
  ~DragZoomAction() override;
  void Update() override;
};
DragZoomAction::DragZoomAction(gui::Pointer& pointer, Timeline& timeline)
    : Action(pointer), timeline(timeline) {
  timeline.drag_zoom_action = this;
  last_y = pointer.PositionWithin(timeline).y;
}
DragZoomAction::~DragZoomAction() {
  timeline.drag_zoom_action = nullptr;
  timeline.zoom.target = NearestZoomTick(timeline.zoom.target);
  timeline.WakeAnimation();
  for (auto& track : timeline.tracks) {
    track->WakeAnimation();
  }
}
void DragZoomAction::Update() {
  float y = pointer.PositionWithin(timeline).y;
  float delta_y = y - last_y;
  last_y = y;
  float factor = expf(delta_y * 60);
  timeline.zoom.value *= factor;
  timeline.zoom.target *= factor;
  timeline.zoom.value = clamp(timeline.zoom.value, 0.001f, 3600.0f);
  timeline.zoom.target = clamp(timeline.zoom.target, 0.001f, 3600.0f);
  timeline.WakeAnimation();
  for (auto& track : timeline.tracks) {
    track->WakeAnimation();
  }
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

SpliceAction::SpliceAction(gui::Pointer& pointer, Timeline& timeline)
    : Action(pointer),
      timeline(timeline),
      resize_icon(pointer, gui::Pointer::kIconResizeHorizontal) {
  assert(timeline.splice_action == nullptr);
  timeline.splice_action = this;
  splice_to = CurrentOffset(timeline, time::SteadyNow());
  timeline.splice_wiggle.velocity -= 0.1;
  timeline.WakeAnimation();
}
SpliceAction::~SpliceAction() {
  auto now = time::SteadyNow();
  timeline.splice_action = nullptr;
  auto current_offset = CurrentOffset(timeline, now);

  if (!cancel) {
    // Delete stuff between splice_to and current_offset
    int num_tracks = timeline.tracks.size();
    for (int i = 0; i < num_tracks; ++i) {
      auto& track = timeline.tracks[i];
      track->Splice(current_offset, splice_to);
      track->WakeAnimation();
    }
    timeline.timeline_length += splice_to - current_offset;
    AdjustOffset(timeline, splice_to - current_offset, now);
  }
  timeline.WakeAnimation();
}

void SpliceAction::Update() {
  auto pos = pointer.PositionWithin(timeline);
  int num_tracks = timeline.tracks.size();
  float current_pos_ratio = CurrentPosRatio(timeline);
  float bridge_offset_x = BridgeOffsetX(current_pos_ratio);
  time::T new_splice_to;
  bool new_snapped = false;
  if (SplicerShape(num_tracks, current_pos_ratio).contains(pos.x, pos.y)) {
    new_splice_to = TimeAtX(timeline, bridge_offset_x);
    new_snapped = true;
    cancel = true;
  } else {
    cancel = false;

    new_splice_to = TimeAtX(timeline, pos.x);
    if (pos.x < bridge_offset_x) {
      if (auto snapped_time = SnapToTrack(timeline, pos, new_splice_to)) {
        new_splice_to = *snapped_time;
        new_snapped = true;
      }
    }
    if (auto snapped_time = SnapToBottomRuler(timeline, pos)) {
      new_splice_to = *snapped_time;
      new_snapped = true;
    }
    new_splice_to = max<time::T>(0, new_splice_to);
  }

  if (new_snapped || snapped) {
    float distance_to_seconds = DistanceToSeconds(timeline);
    timeline.splice_wiggle.value += (splice_to - new_splice_to) / distance_to_seconds;
  }
  snapped = new_snapped;
  splice_to = new_splice_to;
  timeline.WakeAnimation();
}

unique_ptr<Action> Timeline::FindAction(gui::Pointer& ptr, gui::ActionTrigger btn) {
  if (btn == gui::PointerButton::Left) {
    float current_pos_ratio = CurrentPosRatio(*this);
    int n = tracks.size();
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
        SetPosRatio(*this, PosRatioFromBridgeOffsetX(pos.x), ptr.root_widget.timer.now);
        return make_unique<DragBridgeAction>(ptr, *this);
      }
    }
  }
  return Object::FallbackWidget::FindAction(ptr, btn);
}

animation::Phase Timeline::Tick(time::Timer& timer) {
  auto phase = animation::Finished;
  if (state == kPlaying) {
    playing.now = time::SteadyNow();
    phase |= animation::Animating;
  } else if (state == kRecording) {
    recording.now = time::SteadyNow();
    phase |= animation::Animating;
  }
  phase |= zoom.Tick(timer);
  if (splice_action) {
    phase |= animation::Animating;
  }
  phase |= splice_wiggle.SpringTowards(0, timer.d, 0.3, 0.1);
  phase |= animation::ExponentialApproach(0, timer.d, 0.05, bridge_wiggle_s);
  UpdateChildTransform(timer.now);
  return phase;
}

void Timeline::Draw(SkCanvas& canvas) const {
  auto wood_case_rrect = WoodenCaseRRect(*this);
  SkPath wood_case_path = SkPath::RRect(wood_case_rrect);

  {  // Wooden case, light & shadow
    canvas.save();
    canvas.clipRRect(wood_case_rrect);
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
    SkRRect inset_rrect = PlasticRRect(*this);
    inset_rrect.outset(0.2_mm, 0.2_mm);
    inset_shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.2_mm));
    SkPoint pts[2] = {{0, inset_rrect.getBounds().fTop + inset_rrect.getSimpleRadii().y()},
                      {0, inset_rrect.getBounds().fTop}};
    SkColor colors[2] = {"#2d1f1b"_color, "#aa6048"_color};

    inset_shadow.setShader(
        SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp));
    canvas.drawRRect(inset_rrect, inset_shadow);
  }

  canvas.drawRRect(PlasticRRect(*this), kPlasticPaint);

  NumberTextField::DrawBackground(canvas, kDisplayRRect.sk);
  // canvas.drawRRect(kDisplayRRect.sk, kDisplayPaint);

  constexpr float PI = numbers::pi;

  time::T max_track_length = MaxTrackLength();
  time::T current_offset = clamp<time::T>(CurrentOffset(*this, time::SteadyNow()) + bridge_wiggle_s,
                                          0, max_track_length);
  float current_pos_ratio = max_track_length == 0 ? 1 : current_offset / max_track_length;

  function<Str(time::T)> format_time;
  if (max_track_length > 3600) {
    format_time = [](time::T t) {
      t = round(t * 1000) / 1000;
      int hours = t / 3600;
      t -= hours * 3600;
      int minutes = t / 60;
      t -= minutes * 60;
      int seconds = t;
      t -= seconds;
      int milliseconds = round(t * 1000);
      return f("{:02d}:{:02d}:{:02d}.{:03d} s", hours, minutes, seconds, milliseconds);
    };
  } else if (max_track_length > 60) {
    format_time = [](time::T t) {
      t = round(t * 1000) / 1000;
      int minutes = t / 60;
      t -= minutes * 60;
      int seconds = t;
      t -= seconds;
      int milliseconds = round(t * 1000);
      return f("{:02d}:{:02d}.{:03d} s", minutes, seconds, milliseconds);
    };
  } else if (max_track_length >= 10) {
    format_time = [](time::T t) {
      t = round(t * 1000) / 1000;
      int seconds = t;
      t -= seconds;
      int milliseconds = round(t * 1000);
      return f("{:02d}.{:03d} s", seconds, milliseconds);
    };
  } else {
    format_time = [](time::T t) {
      t = round(t * 1000) / 1000;
      int seconds = t;
      t -= seconds;
      int milliseconds = round(t * 1000);
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

  float x_behind_display = -kPlayButtonRadius - kDisplayMargin - kDisplayWidth - kDisplayMargin / 2;
  auto turn_shift = ArcLine::TurnShift(bridge_offset_x - x_behind_display, kDisplayMargin / 2);

  signal_line.MoveBy(kRulerHeight + kDisplayMargin / 2 - turn_shift.distance_forward / 2);
  turn_shift.Apply(signal_line);
  signal_line.MoveBy(kLetterSize * 2 + 1_mm * 3 + kDisplayMargin / 2 -
                     turn_shift.distance_forward / 2);
  signal_line.TurnConvex(-90_deg, kDisplayMargin / 2);

  // signal_line.TurnBy(M_PI_2, kDisplayMargin / 2);
  auto signal_path = signal_line.ToPath(false);
  canvas.drawPath(signal_path, kSignalPaint);

  float window_height = WindowHeight(tracks.size());

  auto window_path = WindowShape(tracks.size());

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
    float distance_to_seconds = DistanceToSeconds(*this);
    float track_width = MaxTrackLength() / distance_to_seconds;

    // at time 0 the first tick is at -kRulerWidth / 2
    // at time 0 the last tick is at -kRulerWidth / 2 + track_width
    // at time END the first tick is at kRulerWidth / 2 - track_width
    // at time END the last tick is at kRulerWidth / 2

    float first_tick_x0 = -kRulerLength / 2;
    float first_tick_x1 = kRulerLength / 2 - track_width;

    float first_tick_x = lerp(first_tick_x0, first_tick_x1, current_pos_ratio);
    float last_tick_x = first_tick_x + track_width;

    float tick_every_s = 0.1;
    float tick_every_x = tick_every_s / distance_to_seconds;

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

  Ptr<Widget> tracks_arr[tracks.size()];  // TODO: awful, fix this
  for (size_t i = 0; i < tracks.size(); ++i) {
    tracks_arr[i] = tracks[i];
  }

  DrawChildrenSpan(canvas, SpanOfArr(tracks_arr, tracks.size()));

  bool draw_bridge_hairline = true;

  if (splice_action) {
    float splice_x = XAtTime(*this, splice_action->splice_to) + splice_wiggle.value;
    auto rect = Rect(splice_x, -window_height + kRulerHeight + kMarginAroundTracks, bridge_offset_x,
                     -kRulerHeight - kMarginAroundTracks);
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
      delete_paint.setShader(SkGradientShader::MakeLinear(pts, colors, pos, n, SkTileMode::kClamp));
      delete_paint.setBlendMode(SkBlendMode::kColorBurn);

      canvas.drawRect(rect, delete_paint);

      canvas.save();
      canvas.clipRect(rect);

      {  // Draw dust being sucked in
        time::T current_time = time::SteadyNow().time_since_epoch().count();
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
    auto inner_gradient = SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
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
            -WindowHeight(tracks.size()) - kDisplayMargin + kScrewMargin + kScrewRadius);
  DrawScrew(-kPlasticWidth / 2 + kScrewMargin + kScrewRadius,
            -WindowHeight(tracks.size()) - kDisplayMargin + kScrewMargin + kScrewRadius);
  DrawScrew(kPlasticWidth / 2 - kScrewMargin - kScrewRadius,
            kPlasticTop - kScrewMargin - kScrewRadius);
  DrawScrew(-kPlasticWidth / 2 + kScrewMargin + kScrewRadius,
            kPlasticTop - kScrewMargin - kScrewRadius);

  Ptr<Widget> arr[] = {run_button, prev_button, next_button};
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
    float bottom_y = -(kMarginAroundTracks * 2 + kTrackHeight * tracks.size() +
                       kTrackMargin * max(0, (int)tracks.size() - 1));
    if (draw_bridge_hairline) {
      SkPaint hairline;
      hairline.setColor(kBridgeLinePaint.getColor());
      hairline.setStyle(SkPaint::kStroke_Style);
      hairline.setAntiAlias(true);
      canvas.drawLine({x, -kRulerHeight}, {x, bottom_y - kRulerHeight}, hairline);
    }

    auto bridge_shape = BridgeShape(tracks.size(), current_pos_ratio);

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
    const static SkPaint kSplicerPaint = []() {
      SkPaint paint = kBridgeHandlePaint;
      // paint.setColor("#5d1e0a"_color);
      paint.setImageFilter(
          SkImageFilters::DropShadow(0, 0, 0.2_mm, 0.2_mm, "#000000"_color, nullptr));
      return paint;
    }();
    auto splicer_shape = SplicerShape(tracks.size(), current_pos_ratio);
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
        auto true_splice_x = XAtTime(*this, splice_action->splice_to);
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

    float nearest_tick = NearestZoomTick(zoom.value);
    float next_tick, previous_tick;
    if (nearest_tick > zoom.value) {
      next_tick = nearest_tick;
      previous_tick = PreviousZoomTick(nearest_tick);
    } else {
      next_tick = NextZoomTick(nearest_tick);
      previous_tick = nearest_tick;
    }
    float ratio = (zoom.value - previous_tick) / (next_tick - previous_tick);
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
      current_zoom_text = f("{} ms", (int)roundf(zoom.value * 1000));
    } else {
      current_zoom_text = f("{:.1f} s", zoom.value);
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

SkPath Timeline::Shape() const {
  auto r = WoodenCaseRRect(*this);
  return SkPath::RRect(r);
}

void Timeline::Args(function<void(Argument&)> cb) {
  for (auto& track_arg : track_args) {
    cb(*track_arg);
  }
  cb(next_arg);
}

Vec2AndDir Timeline::ArgStart(const Argument& arg) {
  for (int i = 0; i < tracks.size(); ++i) {
    if (track_args[i].get() != &arg) {
      continue;
    }
    return {
        .pos = {kPlasticWidth / 2, -kRulerHeight - kMarginAroundTracks - kTrackHeight / 2 -
                                       i * (kTrackMargin + kTrackHeight)},
        .dir = 0_deg,
    };
  }
  return Object::FallbackWidget::ArgStart(arg);
}

void Timeline::FillChildren(Vec<Ptr<Widget>>& children) {
  children.reserve(3 + tracks.size());
  children.push_back(run_button);
  children.push_back(prev_button);
  children.push_back(next_button);
  for (size_t i = 0; i < tracks.size(); ++i) {
    children.push_back(tracks[i]);
  }
}

void Timeline::UpdateChildTransform(time::SteadyPoint now) {
  float distance_to_seconds = DistanceToSeconds(*this);  // 1 cm = 1 second
  auto max_track_length = MaxTrackLength();
  float track_width = max_track_length / distance_to_seconds;

  float current_pos_ratio =
      clamp<time::T>((CurrentOffset(*this, now) + bridge_wiggle_s) / max_track_length, 0, 1);

  float track_offset_x0 = kRulerLength / 2;
  float track_offset_x1 = track_width - kRulerLength / 2;

  float track_offset_x = lerp(track_offset_x0, track_offset_x1, current_pos_ratio);

  for (size_t i = 0; i < tracks.size(); ++i) {
    tracks[i]->local_to_parent =
        SkM44::Translate(-track_offset_x, -kRulerHeight - kMarginAroundTracks - kTrackHeight / 2 -
                                              i * (kTrackMargin + kTrackHeight));
  }
}

SkPath TrackBase::Shape() const {
  float distance_to_seconds;
  if (timeline) {
    distance_to_seconds = DistanceToSeconds(*timeline);
  } else {
    distance_to_seconds = 100;  // 1 cm = 1 second
  }
  time::T end_time = timeline ? timeline->MaxTrackLength() : timestamps.back();
  Rect rect = Rect(0, -kTrackHeight / 2, end_time / distance_to_seconds, kTrackHeight / 2);
  if (timeline) {
    // Clip to the width of the timeline window
    rect.right = min(rect.right, (float)TimeAtX(*timeline, kWindowWidth / 2) / distance_to_seconds);
    rect.left = max(rect.left, (float)TimeAtX(*timeline, -kWindowWidth / 2) / distance_to_seconds);
  }
  return SkPath::Rect(rect.sk);
}

Optional<Rect> TrackBase::TextureBounds() const {
  if (timeline == nullptr || timeline->drag_zoom_action == nullptr) {
    return FallbackWidget::TextureBounds();
  }
  return nullopt;
}

void TrackBase::Draw(SkCanvas& canvas) const { canvas.drawPath(Shape(), kTrackPaint); }

void OnOffTrack::Draw(SkCanvas& canvas) const {
  TrackBase::Draw(canvas);
  auto shape = Shape();
  Rect rect;
  shape.isRect(&rect.sk);
  float distance_to_seconds = DistanceToSeconds(*timeline);
  auto DrawSegment = [&](time::T start_t, time::T end_t) {
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
  if (!isnan(on_at)) {
    switch (timeline->state) {
      case Timeline::kRecording:
        DrawSegment(on_at, (timeline->recording.now - timeline->recording.started_at).count());
        break;
      case Timeline::kPlaying:
      case Timeline::kPaused:
        // segment ends at the right edge of the track
        DrawSegment(on_at, timeline->MaxTrackLength());
        break;
    }
  }
}

void Vec2Track::Draw(SkCanvas& canvas) const {
  TrackBase::Draw(canvas);
  auto shape = Shape();
  Rect rect;
  shape.isRect(&rect.sk);
  float s_per_m = DistanceToSeconds(*timeline);  // s / m
  auto left_t = rect.left * s_per_m;
  auto right_t = rect.right * s_per_m;
  auto left_i = lower_bound(timestamps.begin(), timestamps.end(), left_t) - timestamps.begin();
  auto right_i = upper_bound(timestamps.begin(), timestamps.end(), right_t) - timestamps.begin();

  float px_per_meter = canvas.getLocalToDeviceAs3x3().mapVector(1, 0).length();  // px / m
  float m_per_px = 1.f / px_per_meter;                                           // m / px
  float pixel_t = s_per_m * m_per_px;                                            // s / px
  int i = left_i;
  int segments = 0;
  // Draw the timestamps as horizontal lines (segments). Each timestamp is a 1-pixel long segment.
  // Multiple timestamps whose segments overlap are combined into a single segment.
  while (i < right_i) {
    int start_i = i;
    float start_t = timestamps[i];    // start time of a line segment
    float end_t = start_t + pixel_t;  // end time of a line segment
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
    canvas.drawLine(start_t / s_per_m, 0, end_t / s_per_m, 0, kOnOffPaint);
  }
  // LOG << "Drawn " << segments << " segments showing " << right_i - left_i << " / "
  //     << timestamps.size() << " points";
}
void Vec2Track::Splice(time::T current_offset, time::T splice_to) {
  double delta = splice_to - current_offset;
  auto [current_offset_ge, current_offset_g] =
      equal_range(timestamps.begin(), timestamps.end(), current_offset);
  if (delta < 0) {
    auto [splice_to_ge, splice_to_g] = equal_range(timestamps.begin(), timestamps.end(), splice_to);
    int begin = splice_to_ge - timestamps.begin();
    int end = current_offset_g - timestamps.begin();
    // Fold the motion into a single event.
    for (int i = begin + 1; i < end; ++i) {
      values[begin] += values[i];
    }
    if (begin < values.size()) {
      timestamps[begin] = splice_to;
    }
    int count = end - begin - 1;
    if (count > 0) {
      for (int i = begin + 1; i < timestamps.size() - count; ++i) {
        timestamps[i] = timestamps[i + count] + delta;
        values[i] = values[i + count] + delta;
      }
      timestamps.resize(timestamps.size() - count);
      values.resize(values.size() - count);
    }
  } else if (delta > 0) {
    for (auto it = current_offset_ge; it != timestamps.end(); ++it) {
      *it += delta;
    }
  }
}
void Vec2Track::UpdateOutput(Location& target, time::SteadyPoint started_at,
                             time::SteadyPoint now) {
  auto cmp = [started_at](time::T timestamp, time::SteadyPoint now) {
    return started_at + time::Duration(timestamp) < now;
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

void Timeline::OnCancel() {
  if (state == kPlaying) {
    TimelineCancelScheduled(*this);
    state = kPaused;
    paused = {.playback_offset = (time::SteadyNow() - playing.started_at).count()};
    if (auto h = here.lock()) {
      TimelineUpdateOutputs(*h, *this, time::SteadyPoint{},
                            time::SteadyPoint{} + time::Duration(paused.playback_offset));
    }
    run_button->WakeAnimation();
  }
}

void Timeline::OnRun(Location& here, RunTask& run_task) {
  ZoneScopedN("Timeline");
  if (state != kPaused) {
    return;
  }
  if (paused.playback_offset >= MaxTrackLength()) {
    paused.playback_offset = 0;
  }
  state = kPlaying;
  time::SteadyPoint now = time::SteadyNow();
  playing = {.started_at = now - time::Duration(paused.playback_offset)};
  TimelineUpdateOutputs(here, *this, playing.started_at, now);
  TimelineScheduleNextAfter(*this, now);
  run_button->WakeAnimation();
  WakeAnimation();
  BeginLongRunning(here, run_task);
}

void Timeline::BeginRecording() {
  switch (state) {
    case Timeline::kPaused:
      state = Timeline::kRecording;
      recording.now = time::SteadyNow();
      recording.started_at = recording.now - time::Duration(paused.playback_offset);
      break;
    case Timeline::kRecording:
      // WTF? Maybe show an error?
      break;
    case Timeline::kPlaying:
      state = Timeline::kRecording;
      recording.started_at = playing.started_at;
      recording.now = playing.now;
      break;
  }
  run_button->WakeAnimation();
  WakeAnimation();
}

void Timeline::StopRecording() {
  if (state != Timeline::kRecording) {
    return;
  }
  timeline_length = MaxTrackLength();
  paused = {.playback_offset =
                min((time::SteadyNow() - recording.started_at).count(), timeline_length)};
  state = Timeline::kPaused;
  run_button->WakeAnimation();
  WakeAnimation();
}

void OnOffTrack::Splice(time::T current_offset, time::T splice_to) {
  double delta = splice_to - current_offset;
  auto [current_offset_ge, current_offset_g] =
      equal_range(timestamps.begin(), timestamps.end(), current_offset);
  if (delta < 0) {
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
  } else if (delta > 0) {
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
  if (auto runnable = dynamic_cast<Runnable*>(target.object.get())) {
    if (on) {
      target.ScheduleRun();
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
    Done(here);
    run_button->WakeAnimation();
  }
  TimelineUpdateOutputs(here, *this, playing.started_at, now);
  if (now < end_at) {
    TimelineScheduleNextAfter(*this, now);
  }
}

std::unique_ptr<Action> TrackBase::FindAction(gui::Pointer& ptr, gui::ActionTrigger btn) {
  if (timeline) {
    return timeline->FindAction(ptr, btn);
  } else {
    return Object::FallbackWidget::FindAction(ptr, btn);
  }
}

bool OnOffTrack::IsOn() const {
  if (timeline->state == Timeline::kPaused) {
    return false;
  }
  auto now = time::SteadyNow();
  auto current_offset = CurrentOffset(*timeline, now);

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

void TrackBase::SerializeState(Serializer& writer, const char* key) const {
  // Using nullptr as a guard value to reduce JSON nesting
  if (key != nullptr) {
    writer.Key(key);
    writer.StartObject();
  }
  writer.Key("timestamps");
  writer.StartArray();
  for (auto t : timestamps) {
    writer.Double(t);
  }
  writer.EndArray();
  if (key != nullptr) {
    writer.EndObject();
  }
}

void OnOffTrack::SerializeState(Serializer& writer, const char* key) const {
  // Using nullptr as a guard value to reduce JSON nesting
  if (key != nullptr) {
    writer.Key(key);
    writer.StartObject();
  }
  writer.Key("type");
  writer.String("on/off");
  TrackBase::SerializeState(writer, nullptr);
  if (!isnan(on_at)) {
    writer.Key("on_at");
    writer.Double(on_at);
  }
  if (key != nullptr) {
    writer.EndObject();
  }
}

void Vec2Track::SerializeState(Serializer& writer, const char* key) const {
  // Using nullptr as a guard value to reduce JSON nesting
  if (key != nullptr) {
    writer.Key(key);
    writer.StartObject();
  }
  writer.Key("type");
  writer.String("vec2");
  TrackBase::SerializeState(writer, nullptr);
  writer.Key("values");
  writer.StartArray();
  for (const auto& v : values) {
    writer.StartArray();
    writer.Double(v.x);
    writer.Double(v.y);
    writer.EndArray();
  }
  writer.EndArray();
  if (key != nullptr) {
    writer.EndObject();
  }
}

void Timeline::SerializeState(Serializer& writer, const char* key) const {
  writer.Key(key);
  writer.StartObject();

  writer.Key("tracks");
  writer.StartArray();
  for (int i = 0; i < tracks.size(); ++i) {
    writer.StartObject();
    writer.Key("name");
    writer.String(track_args[i]->name.c_str());
    tracks[i]->SerializeState(writer, nullptr);
    writer.EndObject();
  }
  writer.EndArray();
  writer.Key("zoom");
  writer.Double(zoom.value);
  writer.Key("length");
  writer.Double(timeline_length);

  switch (state) {
    case kPaused:
      writer.Key("paused");
      writer.Double(paused.playback_offset);
      break;
    case kPlaying:
      writer.Key("playing");
      writer.Double((time::SteadyNow() - playing.started_at).count());
      break;
    case kRecording:
      writer.Key("recording");
      writer.Double((time::SteadyNow() - recording.started_at).count());
      break;
  }

  writer.EndObject();
}

bool TrackBase::TryDeserializeField(Location& l, Deserializer& d, Str& field_name) {
  if (field_name == "timestamps") {
    timestamps.clear();
    Status status;
    for (int i : ArrayView(d, status)) {
      double t;
      d.Get(t, status);
      if (OK(status)) {
        timestamps.push_back(t);
      }
    }
    if (!OK(status)) {
      l.ReportError(status.ToStr());
    }
    return true;
  }
  return false;
}
bool OnOffTrack::TryDeserializeField(Location& l, Deserializer& d, Str& field_name) {
  if (field_name == "on_at") {
    Status status;
    d.Get(on_at, status);
    if (!OK(status)) {
      l.ReportError(status.ToStr());
    }
    return true;
  }
  return TrackBase::TryDeserializeField(l, d, field_name);
}
bool Vec2Track::TryDeserializeField(Location& l, Deserializer& d, Str& field_name) {
  if (field_name == "values") {
    values.clear();
    Status status;
    for (int i : ArrayView(d, status)) {
      Vec2 v;
      for (int i : ArrayView(d, status)) {
        if (i == 0) {
          d.Get(v.x, status);
        } else if (i == 1) {
          d.Get(v.y, status);
        }
      }
      if (OK(status)) {
        values.push_back(v);
      }
    }
    if (!OK(status)) {
      l.ReportError(status.ToStr());
    }
    return true;
  }
  return TrackBase::TryDeserializeField(l, d, field_name);
}

void TrackBase::DeserializeState(Location& l, Deserializer& d) {
  ERROR << "TrackBase::DeserializeState() not implemented";
}

void OnOffTrack::DeserializeState(Location& l, Deserializer& d) {
  ERROR << "OnOffTrack::DeserializeState() not implemented";
}

void Vec2Track::DeserializeState(Location& l, Deserializer& d) {
  ERROR << "Vec2Track::DeserializeState() not implemented";
}

void Timeline::DeserializeState(Location& l, Deserializer& d) {
  Status status;
  for (auto& key : ObjectView(d, status)) {
    if (key == "tracks") {
      for (auto elem : ArrayView(d, status)) {
        Str track_name = "";
        Str track_type = "";
        TrackBase* track = nullptr;
        for (auto track_key : ObjectView(d, status)) {
          if (track_key == "name") {
            d.Get(track_name, status);
          } else if (track_key == "type") {
            d.Get(track_type, status);
          } else {
            if (track == nullptr) {
              if (track_type == "on/off") {
                track = &AddOnOffTrack(track_name);
              } else if (track_type == "vec2") {
                track = &AddVec2Track(track_name);
              } else {
                AppendErrorMessage(status) += f("Unknown track type: {}", track_type);
              }
            }
            if (track) {
              track->TryDeserializeField(l, d, track_key);
            }
          }
        }
      }
    } else if (key == "zoom") {
      d.Get(zoom.value, status);
      zoom.target = zoom.value;
    } else if (key == "length") {
      d.Get(timeline_length, status);
    } else if (key == "paused") {
      state = kPaused;
      d.Get(paused.playback_offset, status);
    } else if (key == "playing") {
      state = kPlaying;
      time::T value = 0;
      d.Get(value, status);
      time::SteadyPoint now = time::SteadyNow();
      playing.started_at = now - time::Duration(value);
      // We're not updating the outputs because they should be deserialized in a proper state
      // TimelineUpdateOutputs(l, *this, playing.started_at, now);
      TimelineScheduleNextAfter(*this, now);
      BeginLongRunning(l, l.GetRunTask());
    } else if (key == "recording") {
      state = kRecording;
      time::T value = 0;
      d.Get(value, status);
      recording.started_at = time::SteadyNow() - time::Duration(value);
    }
  }
  if (!OK(status)) {
    l.ReportError(status.ToStr());
  }
}

SkRRect SideButton::RRect() const {
  SkRect oval = SkRect::MakeXYWH(0, 0, 2 * kSideButtonRadius, 2 * kSideButtonRadius);
  return SkRRect::MakeOval(oval);
}
}  // namespace automat::library