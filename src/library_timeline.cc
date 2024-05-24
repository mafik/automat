#include "library_timeline.hh"

#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include <cmath>
#include <memory>

#include "arcline.hh"
#include "base.hh"
#include "font.hh"
#include "gui_button.hh"
#include "gui_constants.hh"
#include "gui_shape_widget.hh"
#include "library_macros.hh"
#include "svg.hh"
#include "time.hh"
#include "window.hh"

using namespace automat::gui;

namespace automat::library {

constexpr float kScrewRadius = 1_mm;
constexpr float kScrewMargin =
    2_mm;  // margin from the screw position to the edge of the plastic front panel
constexpr float kWoodWidth = 4_mm;

constexpr float kPlasticCornerRadius = kScrewRadius + kScrewMargin;
constexpr float kWoodenCaseCornerRadius = kPlasticCornerRadius + kWoodWidth;

constexpr float kDisplayHeight = kLetterSize * 3 + 4 * 1_mm;
constexpr float kDisplayMargin = 2_mm;
constexpr float kDisplayWidth = 2.5_cm;

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

static constexpr float WindowHeight(int num_tracks) {
  return kRulerHeight * 2 + kMarginAroundTracks * 2 + std::max(0, num_tracks - 1) * kTrackMargin +
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

const SkPaint kWoodPaint = []() {
  SkPaint p;
  p.setColor("#805338"_color);
  return p;
}();

const SkPaint kPlasticPaint = []() {
  SkPaint p;
  p.setColor("#ecede9"_color);
  return p;
}();

const SkPaint kDisplayPaint = []() {
  SkPaint p;
  p.setColor("#b4b992"_color);
  return p;
}();

const SkPaint kDisplayCurrentPaint = []() {
  SkPaint p;
  p.setColor("#e24e1f"_color);
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

const SkPaint kScrewPaint = []() {
  SkPaint p;
  p.setColor("#9b9994"_color);
  return p;
}();

const SkPaint kTrackPaint = []() {
  SkPaint p;
  SkPoint pts[2] = {{0, 0}, {kTrackWidth, 0}};
  SkColor colors[3] = {"#787878"_color, "#f3f3f3"_color, "#787878"_color};
  sk_sp<SkShader> gradient =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 3, SkTileMode::kClamp);
  p.setShader(gradient);
  return p;
}();

const SkPaint kRulerPaint = []() {
  SkPaint p;
  p.setColor("#4e4e4e"_color);
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
  p.setColor("#e24e1f"_color);
  return p;
}();

const SkPaint kBridgeLinePaint = []() {
  SkPaint p;
  p.setColor("#e24e1f"_color);
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
  p.setBlendMode(SkBlendMode::kColorBurn);
  return p;
}();

const SkMatrix kHorizontalFlip = SkMatrix::Scale(-1, 1);

DEFINE_PROTO(Timeline);

PrevButton::PrevButton()
    : gui::Button(MakeShapeWidget(kNextShape, 0xffffffff, &kHorizontalFlip), "#404040"_color),
      gui::CircularButtonMixin(kSideButtonRadius) {}

NextButton::NextButton()
    : gui::Button(MakeShapeWidget(kNextShape, 0xffffffff), "#404040"_color),
      gui::CircularButtonMixin(kSideButtonRadius) {}

Timeline::Timeline() : run_button(nullptr, kPlayButtonRadius), playback_offset(0) {
  run_button.color = "#e24e1f"_color;
}

Timeline::Timeline(const Timeline& other) : Timeline() {
  // Create some sample data:
  // - a track which switches on/off every second
  std::unique_ptr<OnOffTrack> track = std::make_unique<OnOffTrack>();
  for (int i = 0; i < 10; ++i) {
    track->timestamps.push_back(i);
  }
  tracks.emplace_back(std::move(track));
  // - a track which switches on/off every 5 seconds
  track = std::make_unique<OnOffTrack>();
  for (int i = 0; i < 2; ++i) {
    track->timestamps.push_back(i * 5);
  }
  tracks.emplace_back(std::move(track));
}

void Timeline::Relocate(Location* new_here) {
  LiveObject::Relocate(new_here);
  run_button.location = new_here;
}

string_view Timeline::Name() const { return "Timeline"; }

std::unique_ptr<Object> Timeline::Clone() const { return std::make_unique<Timeline>(*this); }

static Font& LcdFont() {
  static std::unique_ptr<Font> font = Font::Make(1.5, 700);
  return *font.get();
}

static time::T MaxTrackLength(const Timeline& timeline) {
  time::T max_track_length = 0;
  for (const auto& track : timeline.tracks) {
    max_track_length = std::max(max_track_length, track->timestamps.back());
  }
  return max_track_length;
}

static float CurrentPosRatio(const Timeline& timeline, time::point now) {
  time::T max_track_length = MaxTrackLength(timeline);
  if (max_track_length == 0) {
    return 0;
  } else if (timeline.currently_playing) {
    return (now - timeline.playback_started_at).count() / max_track_length;
  } else {
    return timeline.playback_offset / max_track_length;
  }
}

void SetPosRatio(Timeline& timeline, float pos_ratio) {
  pos_ratio = std::clamp(pos_ratio, 0.0f, 1.0f);
  if (timeline.currently_playing) {
    // TODO
  } else {
    time::T max_track_length = MaxTrackLength(timeline);
    timeline.playback_offset = pos_ratio * max_track_length;
  }
}

static float BridgeOffsetX(float current_pos_ratio) {
  return -kRulerLength / 2 + kRulerLength * current_pos_ratio;
}

static float PosRatioFromBridgeOffsetX(float bridge_offset_x) {
  return (bridge_offset_x + kRulerLength / 2) / kRulerLength;
}

SkPath BridgeShape(int num_tracks, float current_pos_ratio) {
  float bridge_offset_x = BridgeOffsetX(current_pos_ratio);

  float bottom_y = -(kMarginAroundTracks * 2 + kTrackHeight * num_tracks +
                     kTrackMargin * std::max(0, num_tracks - 1));

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
  DragBridgeAction(Timeline& timeline) : timeline(timeline) {}
  virtual void Begin(gui::Pointer& ptr) {
    float initial_x = ptr.PositionWithin(timeline).x;
    float initial_pos_ratio = CurrentPosRatio(timeline, ptr.window.actx.timer.now);
    float initial_bridge_x = BridgeOffsetX(initial_pos_ratio);
    press_offset_x = initial_x - initial_bridge_x;
  }
  virtual void Update(gui::Pointer& ptr) {
    float x = ptr.PositionWithin(timeline).x;
    float new_bridge_x = x - press_offset_x;
    SetPosRatio(timeline, PosRatioFromBridgeOffsetX(new_bridge_x));
  }
  virtual void End() {}
  virtual void DrawAction(gui::DrawContext&) {}
};

std::unique_ptr<Action> Timeline::ButtonDownAction(gui::Pointer& ptr, gui::PointerButton btn) {
  if (btn == gui::PointerButton::kMouseLeft) {
    auto bridge_shape =
        BridgeShape(tracks.size(), CurrentPosRatio(*this, ptr.window.actx.timer.now));
    auto pos = ptr.PositionWithin(*this);
    if (bridge_shape.contains(pos.x, pos.y)) {
      return std::unique_ptr<Action>(new DragBridgeAction(*this));
    }
  }
  return Object::ButtonDownAction(ptr, btn);
}

void Timeline::Draw(gui::DrawContext& dctx) const {
  auto& canvas = dctx.canvas;
  canvas.drawRRect(WoodenCaseRRect(*this), kWoodPaint);
  canvas.drawRRect(PlasticRRect(*this), kPlasticPaint);
  canvas.drawRRect(kDisplayRRect.sk, kDisplayPaint);

  constexpr float PI = std::numbers::pi;

  time::T max_track_length = MaxTrackLength(*this);
  float current_pos_ratio = CurrentPosRatio(*this, dctx.animation_context.timer.now);

  std::function<Str(time::T)> format_time;
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

  ArcLine signal_line = ArcLine({bridge_offset_x, -kRulerHeight}, M_PI_2);

  float x_behind_display = -kPlayButtonRadius - kDisplayMargin - kDisplayWidth - kDisplayMargin / 2;
  auto turn_shift = ArcLine::TurnShift(bridge_offset_x - x_behind_display, kDisplayMargin / 2);

  signal_line.MoveBy(kRulerHeight + kDisplayMargin / 2 - turn_shift.distance_forward / 2);
  turn_shift.Apply(signal_line);
  signal_line.MoveBy(kLetterSize * 2 + 1_mm * 3 + kDisplayMargin / 2 -
                     turn_shift.distance_forward / 2);
  signal_line.TurnBy(-M_PI_2, kDisplayMargin / 2);

  // signal_line.TurnBy(M_PI_2, kDisplayMargin / 2);
  auto signal_path = signal_line.ToPath(false);
  canvas.drawPath(signal_path, kSignalPaint);

  ArcLine window = ArcLine({0, 0}, 0);

  auto side_button_turn = ArcLine::TurnShift(-kSideButtonRadius - kSideButtonMargin,
                                             kSideButtonRadius + kSideButtonMargin);

  float top_line_dist = kWindowWidth / 2 - side_button_turn.distance_forward - kSideButtonRadius;
  window.MoveBy(top_line_dist);

  side_button_turn.Apply(window);
  window.MoveBy(kSideButtonRadius - kSideButtonMargin);
  window.TurnBy(-PI / 2, kSideButtonMargin);

  float lower_turn_angle = acos((kScrewMargin - kScrewRadius) / (kScrewRadius + 2 * kScrewMargin));
  float lower_turn_dist = sin(lower_turn_angle) * (kScrewRadius + kScrewMargin * 2) + kScrewRadius;

  float window_height = WindowHeight(tracks.size());

  float vertical_dist =
      window_height - kSideButtonMargin - kSideButtonRadius - kSideButtonMargin - lower_turn_dist;
  window.MoveBy(vertical_dist);

  window.TurnBy(-lower_turn_angle, kScrewMargin);
  window.TurnBy(-PI / 2 + 2 * lower_turn_angle, kScrewRadius + kScrewMargin);
  window.TurnBy(-lower_turn_angle, kScrewMargin);

  window.MoveBy(kWindowWidth - lower_turn_dist * 2);

  window.TurnBy(-lower_turn_angle, kScrewMargin);
  window.TurnBy(-PI / 2 + 2 * lower_turn_angle, kScrewRadius + kScrewMargin);
  window.TurnBy(-lower_turn_angle, kScrewMargin);

  window.MoveBy(vertical_dist);

  window.TurnBy(-PI / 2, kSideButtonMargin);
  window.MoveBy(kSideButtonRadius - kSideButtonMargin);
  side_button_turn.ApplyNegative(window);

  auto window_path = window.ToPath(true);

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
    // bridge_offset corresponds to playback_offset
    // each 1 cm corresponds to 1 second
    // so at the middle we will have...
    float distance_to_seconds = 100;  // 1 cm = 1 second

    float track_width = MaxTrackLength(*this) / distance_to_seconds;

    // at time 0 the first tick is at -kRulerWidth / 2
    // at time 0 the last tick is at -kRulerWidth / 2 + track_width
    // at time END the first tick is at kRulerWidth / 2 - track_width
    // at time END the last tick is at kRulerWidth / 2

    float first_tick_x0 = -kRulerLength / 2;
    float first_tick_x1 = kRulerLength / 2 - track_width;

    float first_tick_x = std::lerp(first_tick_x0, first_tick_x1, current_pos_ratio);
    float last_tick_x = first_tick_x + track_width;

    float middle_time = playback_offset + bridge_offset_x * distance_to_seconds;
    float left_time = middle_time - kWindowWidth / 2 * distance_to_seconds;
    float right_time = middle_time + kWindowWidth / 2 * distance_to_seconds;

    float tick_every_s = 0.1;
    float tick_every_x = tick_every_s / distance_to_seconds;

    int first_i = (-kWindowWidth / 2 - first_tick_x) / tick_every_x;
    first_i = std::max(0, first_i);

    int last_i = (kWindowWidth / 2 - first_tick_x) / tick_every_x;
    last_i = std::min<int>(last_i, (last_tick_x - first_tick_x) / tick_every_x);

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

  canvas.restore();

  // Screws
  canvas.drawCircle({kPlasticWidth / 2 - kScrewMargin - kScrewRadius,
                     -WindowHeight(tracks.size()) - kDisplayMargin + kScrewMargin + kScrewRadius},
                    kScrewRadius, kScrewPaint);
  canvas.drawCircle({-kPlasticWidth / 2 + kScrewMargin + kScrewRadius,
                     -WindowHeight(tracks.size()) - kDisplayMargin + kScrewMargin + kScrewRadius},
                    kScrewRadius, kScrewPaint);
  canvas.drawCircle(
      {kPlasticWidth / 2 - kScrewMargin - kScrewRadius, kPlasticTop - kScrewMargin - kScrewRadius},
      kScrewRadius, kScrewPaint);
  canvas.drawCircle(
      {-kPlasticWidth / 2 + kScrewMargin + kScrewRadius, kPlasticTop - kScrewMargin - kScrewRadius},
      kScrewRadius, kScrewPaint);

  DrawChildren(dctx);

  auto bridge_shape = BridgeShape(tracks.size(), current_pos_ratio);
  canvas.drawPath(bridge_shape, kBridgeHandlePaint);
}

SkPath Timeline::Shape() const {
  auto r = WoodenCaseRRect(*this);
  return SkPath::RRect(r);
}

void Timeline::Args(std::function<void(Argument&)> cb) {}

ControlFlow Timeline::VisitChildren(gui::Visitor& visitor) {
  Widget* arr[] = {&run_button, &prev_button, &next_button};
  if (visitor(arr) == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  Widget* tracks_arr[tracks.size()];
  for (size_t i = 0; i < tracks.size(); ++i) {
    tracks_arr[i] = tracks[i].get();
  }
  if (visitor({tracks_arr, tracks.size()}) == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  return ControlFlow::Continue;
}

SkMatrix Timeline::TransformToChild(const Widget& child, animation::Context& actx) const {
  if (&child == &run_button) {
    return SkMatrix::Translate(kPlayButtonRadius, -kDisplayMargin);
  } else if (&child == &prev_button) {
    return SkMatrix::Translate(kPlasticWidth / 2 - kSideButtonMargin, kSideButtonRadius);
  } else if (&child == &next_button) {
    return SkMatrix::Translate(-kPlasticWidth / 2 + kSideButtonMargin + kSideButtonDiameter,
                               kSideButtonRadius);
  } else if (auto* track = dynamic_cast<const TrackBase*>(&child)) {
    float distance_to_seconds = 100;  // 1 cm = 1 second
    float track_width = MaxTrackLength(*this) / distance_to_seconds;

    float current_pos_ratio = CurrentPosRatio(*this, actx.timer.now);

    float track_offset_x0 = kRulerLength / 2;
    float track_offset_x1 = track_width - kRulerLength / 2;

    float track_offset_x = std::lerp(track_offset_x0, track_offset_x1, current_pos_ratio);

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

SkPath TrackBase::Shape() const {
  float distance_to_seconds = 100;  // 1 cm = 1 second
  Rect rect = Rect(0, -kTrackHeight / 2, timestamps.back() / distance_to_seconds, kTrackHeight / 2);
  return SkPath::Rect(rect.sk);
}
void TrackBase::Draw(gui::DrawContext& dctx) const {
  auto& canvas = dctx.canvas;
  canvas.drawPath(Shape(), kTrackPaint);
}

void OnOffTrack::Draw(gui::DrawContext& dctx) const {
  TrackBase::Draw(dctx);
  for (int i = 0; i + 1 < timestamps.size(); i += 2) {
    float distance_to_seconds = 100;  // 1 cm = 1 second
    auto start = timestamps[i];
    auto end = timestamps[i + 1];
    // if (start > kTrackWidth * distance_to_seconds) {
    //   break;
    // }
    // if (end > kTrackWidth * distance_to_seconds) {
    //   end = kTrackWidth * distance_to_seconds;
    // }

    dctx.canvas.drawLine({(float)(start / distance_to_seconds), 0},
                         {(float)(end / distance_to_seconds), 0}, kOnOffPaint);
  }
}

}  // namespace automat::library