#include "library_macro_recorder.hh"

#include <include/core/SkPaint.h>
#include <modules/svg/include/SkSVGDOM.h>

#include "../build/generated/embedded.hh"
#include "gui_constants.hh"
#include "library_macros.hh"
#include "svg.hh"
#include "textures.hh"
#include "window.hh"

namespace automat::library {

using namespace automat::gui;

DEFINE_PROTO(MacroRecorder);

const char kMacroRecorderShapeSVG[] = R"(m 3.78,-48.4
c 0,-0.58 0.49,-0.76 0.7,-0.76 0.6,0 2.62,0.04 2.62,0.04 0,0 3.06,-0.82 14.29,-0.82 11.22,0 15.12,0.75 15.12,0.75 0,0 2.17,0.03 2.69,0.03 0.46,0 0.75,0.41 0.75,0.62
l -0.02,22.69 0.65,0.77 -0.51,0.05
c 0.93,1 3.91,5.67 3.45,6.1 -0.28,0.26 -0.72,-0.3 -0.91,-0.06 -0.13,0.21 1.77,4.6 0.88,5.9 -0.29,0.42 -0.86,0 -0.88,0.48 -0.37,7.53 -3.59,11.03 -4.34,11.19 -0.09,-0.13 -0.17,-0.35 -0.17,-0.35 0,0 -0.83,1.21 -2.5,1.3 -1.67,0.1 -27.23,0.07 -28.13,0.01 -1.48,-0.1 -2.45,-1.67 -2.45,-1.67 0,0 -0.5,2.05 -1.24,2.03 -2.94,-4.1 -2.8,-12.41 -2.64,-13.19 -2.07,-0.62 -0.06,-5.09 0.28,-5.51 -0.44,-0.04 -1.31,0.06 -1.34,-0.49 -0.03,-0.54 1.43,-3.42 3.47,-5.58 -0.03,-0.14 -0.64,-0.08 -0.65,-0.3 -0.02,-0.41 0.86,-1.08 0.86,-1.08
z)";

constexpr float kEyeRadius = 8.685_mm / 2;
const Vec2 kLeftEyeCenter = {12.8875_mm, 30.6385_mm};
const Vec2 kRightEyeCenter = {30.2075_mm, 30.6385_mm};

static SkPath& MacroRecorderShape() {
  static auto path = PathFromSVG(kMacroRecorderShapeSVG, SVGUnit_Millimeters);
  return path;
}

static sk_sp<SkImage>& MacroRecorderFrontColor() {
  static auto image =
      MakeImageFromAsset(embedded::assets_macro_recorder_front_color_webp)->withDefaultMipmaps();
  return image;
}

static sk_sp<SkSVGDOM>& SharinganColor() {
  static auto dom = SVGFromAsset(embedded::assets_sharingan_color_svg.content);
  return dom;
}

MacroRecorder::MacroRecorder() {}
string_view MacroRecorder::Name() const { return "Macro Recorder"; }
std::unique_ptr<Object> MacroRecorder::Clone() const { return std::make_unique<MacroRecorder>(); }
void MacroRecorder::Draw(gui::DrawContext& dctx) const {
  auto& image = MacroRecorderFrontColor();
  auto& canvas = dctx.canvas;
  SkPaint white_eye_paint = SkPaint();
  white_eye_paint.setColor(SK_ColorWHITE);
  Rect left_eye = Rect::MakeCenterWH(kLeftEyeCenter, kEyeRadius * 2, kEyeRadius * 2);
  canvas.drawRect(left_eye, white_eye_paint);
  Rect right_eye = Rect::MakeCenterWH(kRightEyeCenter, kEyeRadius * 2, kEyeRadius * 2);
  canvas.drawRect(right_eye, white_eye_paint);

  {
    auto sharingan = SharinganColor();

    auto local_to_window = TransformUp(dctx.path, dctx.animation_context);

    auto top_window = (Window*)dctx.path[0];

    auto main_pointer_screen = GetMainPointerScreenPos();

    auto size = sharingan->containerSize();
    float s = 0.9 * kEyeRadius * 2 / size.height();
    auto screen_diameter = Length(MainScreenSizePixels());

    auto DrawEye = [&](Vec2 center) {
      auto eye_window = local_to_window.mapPoint(center.sk);
      auto eye_screen = WindowToScreen(eye_window);
      auto eye_delta = main_pointer_screen - eye_screen;
      auto eye_dir = Normalize(eye_delta);
      float z = local_to_window.mapRadius(kEyeRadius * 2) * top_window->display_pixels_per_meter;
      auto eye_dist_3d = Length(Vec3(eye_delta.x, eye_delta.y, z));
      auto eye_dist_2d = Length(eye_delta);

      float dist = eye_dist_2d / eye_dist_3d * kEyeRadius * 0.5;

      canvas.save();
      canvas.translate(center.x + eye_dir.x * dist, center.y - eye_dir.y * dist);
      canvas.scale(s, s);
      canvas.translate(-size.width() / 2, -size.height() / 2);
      sharingan->render(&canvas);
      canvas.restore();
    };
    DrawEye(kLeftEyeCenter);
    DrawEye(kRightEyeCenter);
  }

  {
    canvas.save();
    float s = 5_cm / image->height();
    canvas.translate(0, 5_cm);
    canvas.scale(s, -s);
    canvas.drawImage(MacroRecorderFrontColor(), 0, 0, kDefaultSamplingOptions);
    canvas.restore();
  }
  // SkPaint outline;
  // outline.setStyle(SkPaint::kStroke_Style);
  // canvas.drawPath(Shape(), outline);
}
SkPath MacroRecorder::Shape() const { return MacroRecorderShape(); }
LongRunning* MacroRecorder::OnRun(Location& here) { return this; }
void MacroRecorder::Cancel() {}

}  // namespace automat::library