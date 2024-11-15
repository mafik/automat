// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_timeline.hh"

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
#include "library_macros.hh"
#include "math.hh"
#include "number_text_field.hh"
#include "pointer.hh"
#include "sincos.hh"
#include "status.hh"
#include "svg.hh"
#include "textures.hh"
#include "time.hh"
#include "window.hh"

using namespace automat::gui;
using namespace std;
using namespace maf;

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
constexpr float kZoomVisible = kRulerHeight + kMarginAroundTracks / 2;

constexpr SkColor kOrange = "#e24e1f"_color;

static constexpr Vec2 ZoomDialCenter(float window_height) {
  return {kWindowWidth / 4, -window_height - kZoomRadius + kZoomVisible};
}

static constexpr float WindowHeight(int num_tracks) {
  return kRulerHeight * 2 + kMarginAroundTracks * 2 + max(0, num_tracks - 1) * kTrackMargin +
         num_tracks * kTrackHeight;
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

static sk_sp<SkImage> RosewoodColor(DrawContext& ctx) {
  return MakeImageFromAsset(embedded::assets_rosewood_color_webp, &ctx);
}

const SkPaint& WoodPaint(DrawContext& ctx) {
  static animation::PerDisplay<SkPaint> wood_paint;
  if (wood_paint.Find(ctx.display) == nullptr) {
    SkPaint p;
    p.setColor("#805338"_color);
    auto s = kWoodenCaseWidth / 512 / 2;
    p.setShader(RosewoodColor(ctx)
                    ->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat,
                                 SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear))
                    ->makeWithLocalMatrix(SkMatrix::Scale(s, s).postRotate(-85)));
    wood_paint[ctx.display] = p;
  }
  return wood_paint[ctx.display];
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

DEFINE_PROTO(Timeline);

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
          make_shared<ColoredButton>(
              GetPausedPath(),
              ColoredButtonArgs{.fg = kTimelineButtonBackground,
                                .bg = kOrange,
                                .radius = kPlayButtonRadius,
                                .on_click = [this](gui::Pointer& p) { Activate(p); }}),
          make_shared<ColoredButton>(
              kPlayShape, ColoredButtonArgs{.fg = kOrange,
                                            .bg = kTimelineButtonBackground,
                                            .radius = kPlayButtonRadius,
                                            .on_click = [this](gui::Pointer& p) { Activate(p); }})),
      timeline(timeline),
      rec_button(make_shared<ColoredButton>(
          GetRecPath(), ColoredButtonArgs{.fg = kTimelineButtonBackground,
                                          .bg = color::kParrotRed,
                                          .radius = kPlayButtonRadius,
                                          .on_click = [this](gui::Pointer& p) { Activate(p); }})) {}

void TimelineRunButton::Activate(gui::Pointer& p) {
  LOG << "TimelineRunButton::Activate";
  switch (timeline->state) {
    case Timeline::kPlaying:
      LOG << "Timeline is playing - Cancelling it!";
      timeline->Cancel();
      if (auto h = timeline->here.lock()) {
        h->long_running = nullptr;
      }
      break;
    case Timeline::kPaused:
      LOG << "Timeline is paused - Scheduling it!";
      if (auto h = timeline->here.lock()) {
        h->ScheduleRun();
      }
      break;
    case Timeline::kRecording:
      LOG << "Timeline is recording - Stopping!";
      timeline->StopRecording();
      break;
  }
}

shared_ptr<gui::Button>& TimelineRunButton::OnWidget() {
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
  bool filled = timeline->state == Timeline::kRecording;
  if (auto h = timeline->here.lock()) {
    filled |= (h->run_task && h->run_task->scheduled) || (h->long_running != nullptr);
  }
  return filled;
}

Timeline::Timeline()
    : run_button(make_shared<TimelineRunButton>(this)),
      prev_button(make_shared<PrevButton>()),
      next_button(make_shared<NextButton>()),
      state(kPaused),
      paused{.playback_offset = 0},
      zoom(10) {}

struct TextDrawable : PaintDrawable {
  Str text;
  float width;
  constexpr static float kLetterSize = kKeyLetterSize;
  TextDrawable(StrView text) : text(text) { width = KeyFont().MeasureText(text); }

  void onDraw(SkCanvas* canvas) override {
    canvas->translate(-width / 2, -kLetterSize / 2);
    KeyFont().DrawText(*canvas, text, paint);
  }
  SkRect onGetBounds() override {
    return SkRect::MakeXYWH(-width / 2, -kLetterSize / 2, width, kLetterSize);
  }
};

struct TrackArgument : Argument {
  TextDrawable icon;
  TrackArgument(StrView name) : Argument(name, Argument::kOptional), icon(name) {}
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
  auto track = make_unique<OnOffTrack>();
  track->timeline = this;
  tracks.emplace_back(std::move(track));
  AddTrackArg(*this, tracks.size() - 1, name);
  if (auto h = here.lock()) {
    h->InvalidateConnectionWidgets();
  }
  return *dynamic_cast<OnOffTrack*>(tracks.back().get());
}

Timeline::Timeline(const Timeline& other) : Timeline() {
  tracks.reserve(other.tracks.size());
  for (const auto& track : other.tracks) {
    tracks.emplace_back(dynamic_pointer_cast<TrackBase>(track->Clone()));
  }
  track_args.reserve(other.track_args.size());
  for (int i = 0; i < other.track_args.size(); ++i) {
    AddTrackArg(*this, i, other.track_args[i]->name);
  }
}

string_view Timeline::Name() const { return "Timeline"; }

shared_ptr<Object> Timeline::Clone() const { return make_shared<Timeline>(*this); }

constexpr float kLcdFontSize = 1.5_mm;
static Font& LcdFont() {
  static unique_ptr<Font> font = Font::Make(kLcdFontSize * 1000, 700);
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

static float CurrentPosRatio(const Timeline& timeline, time::SteadyPoint now) {
  time::T max_track_length = timeline.MaxTrackLength();
  if (max_track_length == 0) {
    return 1;
  }
  switch (timeline.state) {
    case Timeline::kPlaying:
      return (now - timeline.playing.started_at).count() / max_track_length;
    case Timeline::kPaused:
      return timeline.paused.playback_offset / max_track_length;
    case Timeline::kRecording:
      return (now - timeline.recording.started_at).count() / max_track_length;
  }
}

void TimelineCancelScheduledAt(Timeline& t) {
  if (auto h = t.here.lock()) {
    CancelScheduledAt(*h);
  }
}

void TimelineScheduleAt(Timeline& t, time::SteadyPoint now) {
  time::SteadyPoint next_update = t.playing.started_at + time::Duration(t.MaxTrackLength());
  for (const auto& track : t.tracks) {
    for (time::T timestamp : track->timestamps) {
      auto timestamp_abs = t.playing.started_at + time::Duration(timestamp);
      if (timestamp_abs <= now) {
        continue;
      }
      next_update = min(next_update, timestamp_abs);
      break;
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

static time::T CurrentOffset(Timeline& timeline, time::SteadyPoint now) {
  switch (timeline.state) {
    case Timeline::kPlaying:
      return (now - timeline.playing.started_at).count();
    case Timeline::kPaused:
      return timeline.paused.playback_offset;
    case Timeline::kRecording:
      return (now - timeline.recording.started_at).count();
  }
}

void OffsetPosRatio(Timeline& timeline, time::T offset, time::SteadyPoint now) {
  if (timeline.state == Timeline::kPlaying) {
    TimelineCancelScheduledAt(timeline);
    timeline.playing.started_at -= time::Duration(offset);
    timeline.playing.started_at = min(timeline.playing.started_at, now);
    if (auto h = timeline.here.lock()) {
      TimelineUpdateOutputs(*h, timeline, timeline.playing.started_at, now);
    }
    TimelineScheduleAt(timeline, now);
  } else if (timeline.state == Timeline::kPaused) {
    timeline.paused.playback_offset =
        clamp<time::T>(timeline.paused.playback_offset + offset, 0, timeline.MaxTrackLength());
  }
  timeline.InvalidateDrawCache();
}

void SetPosRatio(Timeline& timeline, float pos_ratio, time::SteadyPoint now) {
  pos_ratio = clamp(pos_ratio, 0.0f, 1.0f);
  time::T max_track_length = timeline.MaxTrackLength();
  if (timeline.state == Timeline::kPlaying) {
    TimelineCancelScheduledAt(timeline);
    timeline.playing.started_at = now - time::Duration(pos_ratio * max_track_length);
    if (auto h = timeline.here.lock()) {
      TimelineUpdateOutputs(*h, timeline, timeline.playing.started_at, now);
    }
    TimelineScheduleAt(timeline, now);
  } else if (timeline.state == Timeline::kPaused) {
    timeline.paused.playback_offset = pos_ratio * max_track_length;
  }
  timeline.InvalidateDrawCache();
}

void NextButton::Activate(gui::Pointer& ptr) {
  Button::Activate(ptr);
  if (auto timeline = Closest<Timeline>(ptr.hover)) {
    SetPosRatio(*timeline, 1, ptr.window.display.timer.now);
  }
}

void PrevButton::Activate(gui::Pointer& ptr) {
  Button::Activate(ptr);
  if (Timeline* timeline = Closest<Timeline>(ptr.hover)) {
    SetPosRatio(*timeline, 0, ptr.window.display.timer.now);
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

time::T TimeAtX(const Timeline& timeline, float x, time::SteadyPoint now = time::kZeroSteady) {
  if (now == time::kZeroSteady) {
    now = time::SteadyNow();
  }
  // Find the time at the center of the timeline
  float distance_to_seconds = DistanceToSeconds(timeline);
  float current_pos_ratio = CurrentPosRatio(timeline, now);
  float track_width = timeline.MaxTrackLength();

  float center_t0 = kRulerLength / 2 * distance_to_seconds;
  float center_t1 = track_width - kRulerLength / 2 * distance_to_seconds;
  float center_t = lerp(center_t0, center_t1, current_pos_ratio);
  return center_t + x * distance_to_seconds;
}

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

struct DragBridgeAction : Action {
  float press_offset_x;
  Timeline& timeline;
  DragBridgeAction(gui::Pointer& pointer, Timeline& timeline)
      : Action(pointer), timeline(timeline) {}
  virtual void Begin() {
    float initial_x = pointer.PositionWithin(timeline).x;
    float initial_pos_ratio = CurrentPosRatio(timeline, pointer.window.display.timer.now);
    float initial_bridge_x = BridgeOffsetX(initial_pos_ratio);
    press_offset_x = initial_x - initial_bridge_x;
  }
  virtual void Update() {
    float x = pointer.PositionWithin(timeline).x;
    float new_bridge_x = x - press_offset_x;
    SetPosRatio(timeline, PosRatioFromBridgeOffsetX(new_bridge_x),
                pointer.window.display.timer.now);
  }
  virtual void End() {}
};

struct DragTimelineAction : Action {
  Timeline& timeline;
  float last_x;
  DragTimelineAction(gui::Pointer& pointer, Timeline& timeline)
      : Action(pointer), timeline(timeline) {}
  virtual void Begin() { last_x = pointer.PositionWithin(timeline).x; }
  virtual void Update() {
    float x = pointer.PositionWithin(timeline).x;
    float delta_x = x - last_x;
    last_x = x;
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
    OffsetPosRatio(timeline, -delta_x * scaling_factor, pointer.window.display.timer.now);
  }
  virtual void End() {}
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
  float last_x;
  DragZoomAction(gui::Pointer& pointer, Timeline& timeline) : Action(pointer), timeline(timeline) {}
  virtual void Begin() { last_x = pointer.PositionWithin(timeline).x; }
  virtual void Update() {
    float x = pointer.PositionWithin(timeline).x;
    float delta_x = x - last_x;
    last_x = x;
    float factor = expf(delta_x * -30);
    timeline.zoom.value *= factor;
    timeline.zoom.target *= factor;
    timeline.zoom.value = clamp(timeline.zoom.value, 0.001f, 3600.0f);
    timeline.zoom.target = clamp(timeline.zoom.target, 0.001f, 3600.0f);
    timeline.InvalidateDrawCache();
    for (auto& track : timeline.tracks) {
      track->InvalidateDrawCache();
    }
  }
  virtual void End() {
    timeline.zoom.target = NearestZoomTick(timeline.zoom.target);
    timeline.InvalidateDrawCache();
    for (auto& track : timeline.tracks) {
      track->InvalidateDrawCache();
    }
  }
};

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

unique_ptr<Action> Timeline::FindAction(gui::Pointer& ptr, gui::ActionTrigger btn) {
  if (btn == gui::PointerButton::Left) {
    auto bridge_shape =
        BridgeShape(tracks.size(), CurrentPosRatio(*this, ptr.window.display.timer.now));
    auto window_shape = WindowShape(tracks.size());
    auto pos = ptr.PositionWithin(*this);
    if (bridge_shape.contains(pos.x, pos.y)) {
      return unique_ptr<Action>(new DragBridgeAction(ptr, *this));
    } else if (window_shape.contains(pos.x, pos.y)) {
      if (pos.y < -kRulerHeight) {
        if (LengthSquared(pos - ZoomDialCenter(WindowHeight(tracks.size()))) <
            kZoomRadius * kZoomRadius) {
          return unique_ptr<Action>(new DragZoomAction(ptr, *this));
        } else {
          return unique_ptr<Action>(new DragTimelineAction(ptr, *this));
        }
      } else {
        SetPosRatio(*this, PosRatioFromBridgeOffsetX(pos.x), ptr.window.display.timer.now);
        return unique_ptr<Action>(new DragBridgeAction(ptr, *this));
      }
    }
  }
  return Object::FindAction(ptr, btn);
}

animation::Phase Timeline::Draw(gui::DrawContext& dctx) const {
  auto& canvas = dctx.canvas;

  auto wood_case_rrect = WoodenCaseRRect(*this);
  SkPath wood_case_path = SkPath::RRect(wood_case_rrect);
  auto phase = state == kPaused ? animation::Finished : animation::Animating;

  {  // Wooden case, light & shadow
    canvas.save();
    canvas.clipRRect(wood_case_rrect);
    canvas.drawPaint(WoodPaint(dctx));

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

  NumberTextField::DrawBackground(dctx, kDisplayRRect.sk);
  // canvas.drawRRect(kDisplayRRect.sk, kDisplayPaint);

  constexpr float PI = numbers::pi;

  phase |= zoom.Tick(dctx.display);

  time::T max_track_length = MaxTrackLength();
  float current_pos_ratio = CurrentPosRatio(*this, dctx.display.timer.now);

  function<Str(time::T)> format_time;
  if (max_track_length > 3600) {
    format_time = [](time::T t) {
      int hours = t / 3600;
      t -= hours * 3600;
      int minutes = t / 60;
      t -= minutes * 60;
      int seconds = t;
      t -= seconds;
      int milliseconds = t * 1000;
      return f("%02d:%02d:%02d.%03d s", hours, minutes, seconds, milliseconds);
    };
  } else if (max_track_length > 60) {
    format_time = [](time::T t) {
      int minutes = t / 60;
      t -= minutes * 60;
      int seconds = t;
      t -= seconds;
      int milliseconds = t * 1000;
      return f("%02d:%02d.%03d s", minutes, seconds, milliseconds);
    };
  } else if (max_track_length >= 10) {
    format_time = [](time::T t) {
      int seconds = t;
      t -= seconds;
      int milliseconds = t * 1000;
      return f("%02d.%03d s", seconds, milliseconds);
    };
  } else {
    format_time = [](time::T t) {
      int seconds = t;
      t -= seconds;
      int milliseconds = t * 1000;
      return f("%d.%03d s", seconds, milliseconds);
    };
  }

  Str total_text = format_time(max_track_length);
  Str current_text = format_time(current_pos_ratio * max_track_length);
  Str remaining_text = format_time((1 - current_pos_ratio) * max_track_length);

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

  shared_ptr<Widget> tracks_arr[tracks.size()];  // TODO: awful, fix this
  for (size_t i = 0; i < tracks.size(); ++i) {
    tracks_arr[i] = tracks[i];
  }
  phase |= DrawChildrenSpan(dctx, SpanOfArr(tracks_arr, tracks.size()));

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

  shared_ptr<Widget> arr[] = {run_button, prev_button, next_button};
  phase |= DrawChildrenSpan(dctx, arr);

  canvas.save();
  canvas.clipPath(window_path, true);

  {  // Window shadow
    SkPaint paint;
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 5_mm));
    window_path.toggleInverseFillType();
    canvas.drawPath(window_path, paint);
  }
  {
    float x = BridgeOffsetX(current_pos_ratio);
    float bottom_y = -(kMarginAroundTracks * 2 + kTrackHeight * tracks.size() +
                       kTrackMargin * max(0, (int)tracks.size() - 1));

    SkPaint hairline;
    hairline.setColor(kBridgeLinePaint.getColor());
    hairline.setStyle(SkPaint::kStroke_Style);
    hairline.setAntiAlias(true);
    canvas.drawLine({x, -kRulerHeight}, {x, bottom_y - kRulerHeight}, hairline);

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
  {  // Zoom dial
    auto zoom_center = ZoomDialCenter(window_height);
    canvas.drawCircle(zoom_center, kZoomRadius, kZoomPaint);
    canvas.save();
    float zoom_text_width = lcd_font.MeasureText("ZOOM");
    canvas.translate(zoom_center.x - zoom_text_width / 2, -window_height + kMarginAroundTracks / 2);
    lcd_font.DrawText(canvas, "ZOOM", kZoomTextPaint);
    canvas.restore();

    auto DrawZoomText = [&](float angle_degrees, Str text) {
      float text_width = lcd_font.MeasureText(text);
      canvas.save();
      canvas.translate(zoom_center.x - text_width / 2, -window_height - kZoomRadius + kZoomVisible);
      canvas.rotate(angle_degrees);
      canvas.translate(0, kZoomRadius - kLcdFontSize - 2_mm);
      lcd_font.DrawText(canvas, text, kZoomTextPaint);
      canvas.restore();
    };

    Str current_zoom_text;
    if (zoom < 1) {
      current_zoom_text = f("%d ms", (int)roundf(zoom.value * 1000));
    } else {
      current_zoom_text = f("%.1f s", zoom.value);
    }
    DrawZoomText(0, current_zoom_text);

    float nearest_tick = NearestZoomTick(zoom.value);
    float next_tick, previous_tick;
    if (nearest_tick > zoom.value) {
      next_tick = nearest_tick;
      previous_tick = PreviousZoomTick(nearest_tick);
    } else {
      next_tick = NextZoomTick(nearest_tick);
      previous_tick = nearest_tick;
    }

    auto tick_angle = [](float tick0, float tick1) {
      return ((tick1 - tick0) / (tick1 + tick0)) * .5f;
    };

    float ratio = (zoom.value - previous_tick) / (next_tick - previous_tick);
    float angle0 = lerp(0, tick_angle(previous_tick, next_tick), ratio) + numbers::pi / 2;

    float line_start = kZoomRadius - 1_mm;
    float line_end = kZoomRadius;

    float angle = angle0;
    float tick = previous_tick;

    while (tick <= 3600) {
      Vec2 p0 = Vec2::Polar(angle, line_start) + zoom_center;
      Vec2 p1 = Vec2::Polar(angle, line_end) + zoom_center;
      if (p1.y < -window_height) {
        break;
      }
      canvas.drawLine(p0.x, p0.y, p1.x, p1.y, kZoomTickPaint);
      float next = NextZoomTick(tick);
      angle -= tick_angle(tick, next);
      tick = next;
    }
    angle = angle0;
    tick = previous_tick;
    while (angle >= 0.001) {
      Vec2 p0 = Vec2::Polar(angle, line_start) + zoom_center;
      Vec2 p1 = Vec2::Polar(angle, line_end) + zoom_center;
      if (p1.y < -window_height) {
        break;
      }
      canvas.drawLine(p0.x, p0.y, p1.x, p1.y, kZoomTickPaint);
      float prev = PreviousZoomTick(tick);
      angle += tick_angle(prev, tick);
      tick = prev;
    }
  }
  canvas.restore();  // unclip
  return phase;
}

SkPath Timeline::Shape(animation::Display*) const {
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
  return Object::ArgStart(arg);
}

ControlFlow Timeline::VisitChildren(gui::Visitor& visitor) {
  shared_ptr<Widget> arr[] = {run_button, prev_button, next_button};
  if (visitor(arr) == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  shared_ptr<Widget> tracks_arr[tracks.size()];
  for (size_t i = 0; i < tracks.size(); ++i) {
    tracks_arr[i] = tracks[i];
  }
  if (visitor({tracks_arr, tracks.size()}) == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  return ControlFlow::Continue;
}

SkMatrix Timeline::TransformToChild(const Widget& child, animation::Display* display) const {
  if (&child == run_button.get()) {
    return SkMatrix::Translate(kPlayButtonRadius, -kDisplayMargin);
  } else if (&child == prev_button.get()) {
    return SkMatrix::Translate(kPlasticWidth / 2 - kSideButtonMargin, kSideButtonRadius);
  } else if (&child == next_button.get()) {
    return SkMatrix::Translate(-kPlasticWidth / 2 + kSideButtonMargin + kSideButtonDiameter,
                               kSideButtonRadius);
  } else if (auto* track = dynamic_cast<const TrackBase*>(&child)) {
    float distance_to_seconds = DistanceToSeconds(*this);  // 1 cm = 1 second
    float track_width = MaxTrackLength() / distance_to_seconds;

    float current_pos_ratio =
        CurrentPosRatio(*this, display ? display->timer.now : time::SteadyNow());

    float track_offset_x0 = kRulerLength / 2;
    float track_offset_x1 = track_width - kRulerLength / 2;

    float track_offset_x = lerp(track_offset_x0, track_offset_x1, current_pos_ratio);

    for (size_t i = 0; i < tracks.size(); ++i) {
      if (tracks[i].get() == track) {
        return SkMatrix::Translate(track_offset_x, kRulerHeight + kMarginAroundTracks +
                                                       kTrackHeight / 2 +
                                                       i * (kTrackMargin + kTrackHeight));
      }
    }
  }
  return SkMatrix::I();
}

SkPath TrackBase::Shape(animation::Display*) const {
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
animation::Phase TrackBase::Draw(gui::DrawContext& dctx) const {
  auto& canvas = dctx.canvas;
  canvas.drawPath(Shape(nullptr), kTrackPaint);
  return animation::Finished;
}

animation::Phase OnOffTrack::Draw(gui::DrawContext& dctx) const {
  TrackBase::Draw(dctx);
  auto shape = Shape(nullptr);
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
    dctx.canvas.drawLine({start, 0}, {end, 0}, kOnOffPaint);
  };
  for (int i = 0; i + 1 < timestamps.size(); i += 2) {
    DrawSegment(timestamps[i], timestamps[i + 1]);
  }
  if (!isnan(on_at)) {
    switch (timeline->state) {
      case Timeline::kRecording:
        DrawSegment(on_at, (dctx.display.timer.now - timeline->recording.started_at).count());
        break;
      case Timeline::kPlaying:
      case Timeline::kPaused:
        // segment ends at the right edge of the track
        DrawSegment(on_at, timeline->MaxTrackLength());
        break;
    }
  }
  return animation::Finished;
}

void Timeline::Cancel() {
  if (state == kPlaying) {
    TimelineCancelScheduledAt(*this);
    state = kPaused;
    paused = {.playback_offset = (time::SteadyNow() - playing.started_at).count()};
    if (auto h = here.lock()) {
      TimelineUpdateOutputs(*h, *this, time::SteadyPoint{},
                            time::SteadyPoint{} + time::Duration(paused.playback_offset));
    }
    run_button->InvalidateDrawCache();
  }
}

LongRunning* Timeline::OnRun(Location& here) {
  LOG << "Timeline::OnRun";
  if (state != kPaused) {
    return nullptr;
  }
  if (paused.playback_offset >= MaxTrackLength()) {
    paused.playback_offset = 0;
  }
  state = kPlaying;
  time::SteadyPoint now = time::SteadyNow();
  playing = {.started_at = now - time::Duration(paused.playback_offset)};
  TimelineUpdateOutputs(here, *this, playing.started_at, now);
  TimelineScheduleAt(*this, now);
  run_button->InvalidateDrawCache();
  return this;
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
  run_button->InvalidateDrawCache();
}

void Timeline::StopRecording() {
  if (state != Timeline::kRecording) {
    return;
  }
  timeline_length = MaxTrackLength();
  paused = {.playback_offset =
                min((time::SteadyNow() - recording.started_at).count(), timeline_length)};
  state = Timeline::kPaused;
  run_button->InvalidateDrawCache();
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
      if (target.long_running) {
        target.long_running->Cancel();
        target.long_running = nullptr;
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
    run_button->InvalidateDrawCache();
  }
  TimelineUpdateOutputs(here, *this, playing.started_at, now);
  if (now < end_at) {
    TimelineScheduleAt(*this, now);
  }
}

std::unique_ptr<Action> TrackBase::FindAction(gui::Pointer& ptr, gui::ActionTrigger btn) {
  if (timeline) {
    return timeline->FindAction(ptr, btn);
  } else {
    return Object::FindAction(ptr, btn);
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

bool TrackBase::TryDeserializeField(Location& l, Deserializer& d, maf::Str& field_name) {
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
bool OnOffTrack::TryDeserializeField(Location& l, Deserializer& d, maf::Str& field_name) {
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

void TrackBase::DeserializeState(Location& l, Deserializer& d) {
  ERROR << "TrackBase::DeserializeState() not implemented";
}

void OnOffTrack::DeserializeState(Location& l, Deserializer& d) {
  ERROR << "OnOffTrack::DeserializeState() not implemented";
}

void Timeline::DeserializeState(Location& l, Deserializer& d) {
  Status status;
  for (auto key : ObjectView(d, status)) {
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
              } else {
                AppendErrorMessage(status) += f("Unknown track type: %s", track_type.c_str());
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
      double value = 0;
      d.Get(value, status);
      time::SteadyPoint now = time::SteadyNow();
      playing.started_at = now - time::Duration(value);
      // We're not updating the outputs because they should be deserialized in a proper state
      // TimelineUpdateOutputs(l, *this, playing.started_at, now);
      TimelineScheduleAt(*this, now);
      l.long_running = this;
    } else if (key == "recording") {
      state = kRecording;
      double value = 0;
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