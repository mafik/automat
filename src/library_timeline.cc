#include "library_timeline.hh"

#include <include/core/SkRRect.h>

#include "arcline.hh"
#include "base.hh"
#include "gui_constants.hh"
#include "include/core/SkPath.h"
#include "library_macros.hh"

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
constexpr float kDisplayWidth = 1.5 * kDisplayHeight;

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

constexpr float kTrackHeight = 1_cm;
constexpr float kTrackMargin = 1_mm;

constexpr float kPlasticTop = 2 * kDisplayMargin + kDisplayHeight;

constexpr float kWindowWidth = kPlasticWidth - 2 * kDisplayMargin;

static constexpr float WindowHeight(int num_tracks) {
  return kRulerHeight * 2 + kMarginAroundTracks * 2 + std::max(0, num_tracks - 1) * kTrackMargin;
}

constexpr float kPlasticBottom = kDisplayMargin;

static Rect PlasticRect(const Timeline& t) {
  return Rect(-kPlasticWidth / 2, -WindowHeight(0) - kPlasticBottom, kPlasticWidth / 2,
              kPlasticTop);
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

const SkPaint kScrewPaint = []() {
  SkPaint p;
  p.setColor("#9b9994"_color);
  return p;
}();

DEFINE_PROTO(Timeline);

Timeline::Timeline() : run_button(nullptr, kPlayButtonRadius) {}

Timeline::Timeline(const Timeline& other) : run_button(nullptr, kPlayButtonRadius) {}

void Timeline::Relocate(Location* new_here) {
  LiveObject::Relocate(new_here);
  run_button.location = new_here;
}

string_view Timeline::Name() const { return "Timeline"; }

std::unique_ptr<Object> Timeline::Clone() const { return std::make_unique<Timeline>(*this); }

void Timeline::Draw(gui::DrawContext& dctx) const {
  auto& canvas = dctx.canvas;
  canvas.drawRRect(WoodenCaseRRect(*this), kWoodPaint);
  canvas.drawRRect(PlasticRRect(*this), kPlasticPaint);
  canvas.drawRRect(kDisplayRRect.sk, kDisplayPaint);

  constexpr float PI = std::numbers::pi;

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

  float vertical_dist =
      WindowHeight(0) - kSideButtonMargin - kSideButtonRadius - kSideButtonMargin - lower_turn_dist;
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
  canvas.drawPath(window_path, SkPaint());

  canvas.drawCircle({kPlasticWidth / 2 - kSideButtonMargin - kSideButtonRadius, 0},
                    kSideButtonRadius, SkPaint());
  canvas.drawCircle({-kPlasticWidth / 2 + kSideButtonMargin + kSideButtonRadius, 0},
                    kSideButtonRadius, SkPaint());

  // Screws
  canvas.drawCircle({kPlasticWidth / 2 - kScrewMargin - kScrewRadius,
                     -WindowHeight(0) - kDisplayMargin + kScrewMargin + kScrewRadius},
                    kScrewRadius, kScrewPaint);
  canvas.drawCircle({-kPlasticWidth / 2 + kScrewMargin + kScrewRadius,
                     -WindowHeight(0) - kDisplayMargin + kScrewMargin + kScrewRadius},
                    kScrewRadius, kScrewPaint);
  canvas.drawCircle(
      {kPlasticWidth / 2 - kScrewMargin - kScrewRadius, kPlasticTop - kScrewMargin - kScrewRadius},
      kScrewRadius, kScrewPaint);
  canvas.drawCircle(
      {-kPlasticWidth / 2 + kScrewMargin + kScrewRadius, kPlasticTop - kScrewMargin - kScrewRadius},
      kScrewRadius, kScrewPaint);

  // Play button
  DrawChildren(dctx);
}

SkPath Timeline::Shape() const {
  auto r = WoodenCaseRRect(*this);
  return SkPath::RRect(r);
}

void Timeline::Args(std::function<void(Argument&)> cb) {}

ControlFlow Timeline::VisitChildren(gui::Visitor& visitor) {
  Widget* arr[] = {&run_button};
  if (visitor(arr) == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  return ControlFlow::Continue;
}
SkMatrix Timeline::TransformToChild(const Widget& child, animation::Context&) const {
  if (&child == &run_button) {
    return SkMatrix::Translate(kPlayButtonRadius, -kDisplayMargin);
  }
  return SkMatrix::I();
}

}  // namespace automat::library