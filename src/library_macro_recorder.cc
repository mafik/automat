#include "library_macro_recorder.hh"

#include <include/core/SkColor.h>
#include <include/core/SkPaint.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradientShader.h>
#include <modules/svg/include/SkSVGDOM.h>

#include <cstdint>

#include "../build/generated/embedded.hh"
#include "gui_constants.hh"
#include "keyboard.hh"
#include "library_key_presser.hh"
#include "library_macros.hh"
#include "library_timeline.hh"
#include "log.hh"
#include "math.hh"
#include "svg.hh"
#include "textures.hh"
#include "time.hh"
#include "window.hh"

namespace automat::library {

using namespace automat::gui;
using namespace std;

DEFINE_PROTO(MacroRecorder);

const char kMacroRecorderShapeSVG[] = R"(m 3.78,-48.4
c 0,-0.58 0.49,-0.76 0.7,-0.76 0.6,0 2.62,0.04 2.62,0.04 0,0 3.06,-0.82 14.29,-0.82 11.22,0 15.12,0.75 15.12,0.75 0,0 2.17,0.03 2.69,0.03 0.46,0 0.75,0.41 0.75,0.62
l -0.02,22.69 0.65,0.77 -0.51,0.05
c 0.93,1 3.91,5.67 3.45,6.1 -0.28,0.26 -0.72,-0.3 -0.91,-0.06 -0.13,0.21 1.77,4.6 0.88,5.9 -0.29,0.42 -0.86,0 -0.88,0.48 -0.37,7.53 -3.59,11.03 -4.34,11.19 -0.09,-0.13 -0.17,-0.35 -0.17,-0.35 0,0 -0.83,1.21 -2.5,1.3 -1.67,0.1 -27.23,0.07 -28.13,0.01 -1.48,-0.1 -2.45,-1.67 -2.45,-1.67 0,0 -0.5,2.05 -1.24,2.03 -2.94,-4.1 -2.8,-12.41 -2.64,-13.19 -2.07,-0.62 -0.06,-5.09 0.28,-5.51 -0.44,-0.04 -1.31,0.06 -1.34,-0.49 -0.03,-0.54 1.43,-3.42 3.47,-5.58 -0.03,-0.14 -0.64,-0.08 -0.65,-0.3 -0.02,-0.41 0.86,-1.08 0.86,-1.08
z)";

constexpr float kEyeRadius = 8.8_mm / 2;
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

MacroRecorder::MacroRecorder() : record_button(this) {}
MacroRecorder::~MacroRecorder() {
  if (keylogging) {
    keylogging->Release();
    keylogging = nullptr;
  }
}
string_view MacroRecorder::Name() const { return "Macro Recorder"; }
std::unique_ptr<Object> MacroRecorder::Clone() const { return std::make_unique<MacroRecorder>(); }
void MacroRecorder::Draw(gui::DrawContext& dctx) const {
  auto& animation_state = animation_state_ptr[dctx.animation_context];
  auto& image = MacroRecorderFrontColor();
  auto& canvas = dctx.canvas;

  if (keylogging) {
    animation_state.eye_speed.target = 1;
  } else {
    animation_state.eye_speed.target = 0;
  }
  animation_state.eye_speed.speed = 5;
  animation_state.eye_speed.Tick(dctx.animation_context);
  animation_state.eye_rotation -= dctx.animation_context.timer.d * 360 * animation_state.eye_speed;
  if (animation_state.eye_rotation < 0) {
    animation_state.eye_rotation += 360;
  }

  {
    auto sharingan = SharinganColor();

    auto local_to_window = TransformUp(dctx.path, dctx.animation_context);

    auto top_window = (Window*)dctx.path[0];

    auto main_pointer_screen = GetMainPointerScreenPos();

    auto size = sharingan->containerSize();
    float s = 0.9 * kEyeRadius * 2 / size.height();

    auto DrawEye = [&](Vec2 center, animation::Spring<Vec2>& googly) {
      Rect bounds = Rect::MakeCenterWH(center, kEyeRadius * 2, kEyeRadius * 2);
      SkPaint white_eye_paint = SkPaint();
      white_eye_paint.setColor(SK_ColorWHITE);
      canvas.drawRect(bounds, white_eye_paint);

      auto eye_window = local_to_window.mapPoint(center.sk);
      auto eye_screen = WindowToScreen(eye_window);
      auto eye_delta = main_pointer_screen - eye_screen;
      auto eye_dir = Normalize(eye_delta);
      float z = local_to_window.mapRadius(kEyeRadius * 2) * top_window->display_pixels_per_meter;
      auto eye_dist_3d = Length(Vec3(eye_delta.x, eye_delta.y, z));
      auto eye_dist_2d = Length(eye_delta);

      float dist = eye_dist_2d / eye_dist_3d;

      googly.target.x = eye_dir.x * dist;
      googly.target.y = -eye_dir.y * dist;
      googly.Tick(dctx.animation_context);

      Vec2 pos = center + googly.value * kEyeRadius * 0.5;
      canvas.save();
      canvas.translate(pos.x, pos.y);
      canvas.scale(s, s);

      float h_angle = atan(googly.value) * 180 / -M_PI;
      float squeeze_3d = 1 - Length(googly.value) / 3;

      canvas.rotate(-h_angle);
      canvas.scale(squeeze_3d, 1);
      canvas.rotate(h_angle);
      canvas.rotate(animation_state.eye_rotation);
      canvas.translate(-size.width() / 2, -size.height() / 2);

      sharingan->render(&canvas);
      canvas.restore();

      SkColor colors[] = {"#00000000"_color, "#00000010"_color, "#00000080"_color};
      float colors_pos[] = {0, 0.6, 1};
      SkPaint eye_shadow_paint;
      eye_shadow_paint.setShader(SkGradientShader::MakeRadial(center, kEyeRadius, colors,
                                                              colors_pos, 3, SkTileMode::kClamp));
      canvas.drawRect(bounds, eye_shadow_paint);
    };
    DrawEye(kLeftEyeCenter, animation_state.googly_left);
    DrawEye(kRightEyeCenter, animation_state.googly_right);
  }

  {
    canvas.save();
    float s = 5_cm / image->height();
    canvas.translate(0, 5_cm);
    canvas.scale(s, -s);
    canvas.drawImage(MacroRecorderFrontColor(), 0, 0, kDefaultSamplingOptions);
    canvas.restore();
  }

  DrawChildren(dctx);

  // SkPaint outline;
  // outline.setStyle(SkPaint::kStroke_Style);
  // canvas.drawPath(record_button.Shape(), outline);

  // outline.setStyle(SkPaint::kStroke_Style);
  // canvas.drawPath(record_button.child->Shape(), outline);
}

static void PositionBelow(Location& origin, Location& below) {
  Rect origin_shape = origin.object->Shape().getBounds();
  Rect below_shape = below.object->Shape().getBounds();
  below.position = origin_shape.BottomCenter() + origin.position - below_shape.TopCenter();
  Machine* m = origin.ParentAs<Machine>();
  Size origin_index = SIZE_MAX;
  Size below_index = SIZE_MAX;
  for (Size i = 0; i < m->locations.size(); i++) {
    if (m->locations[i].get() == &origin) {
      origin_index = i;
      if (below_index != SIZE_MAX) {
        break;
      }
    }
    if (m->locations[i].get() == &below) {
      below_index = i;
      if (origin_index != SIZE_MAX) {
        break;
      }
    }
  }
  if (origin_index > below_index) {
    std::swap(m->locations[origin_index], m->locations[below_index]);
  }
}

static void AnimateGrowFrom(Location& source, Location& grown) {
  for (auto* window : gui::windows) {
    auto& animation_state = grown.animation_state[window->actx];
    animation_state.scale.target = 1;
    animation_state.scale.speed = 10;
    animation_state.scale.value = 0.5;
    Vec2 source_center = source.object->Shape().getBounds().center() + source.position;
    Vec2 grown_center = grown.object->Shape().getBounds().center() + grown.position;
    animation_state.position_offset.value = source_center - grown_center;
    animation_state.position_offset.target = Vec2(0, 0);
    animation_state.position_offset.acceleration = 400;
    animation_state.position_offset.friction = 40;
    animation_state.transparency.value = 1;
    animation_state.transparency.speed = 5;
  }
}

static Timeline* FindTimeline(MacroRecorder& macro_recorder) {
  auto machine = macro_recorder.here->ParentAs<Machine>();
  if (machine == nullptr) {
    FATAL << "MacroRecorder must be a child of a Machine";
    return nullptr;
  }
  auto timeline = (Timeline*)macro_recorder.here->Nearby([](Location& loc) -> void* {
    if (auto timeline = loc.As<Timeline>()) {
      return timeline;
    }
    return nullptr;
  });
  return timeline;
}

static Timeline* FindOrCreateTimeline(MacroRecorder& macro_recorder) {
  auto timeline = FindTimeline(macro_recorder);
  if (timeline == nullptr) {
    auto machine = macro_recorder.here->ParentAs<Machine>();
    if (machine == nullptr) {
      FATAL << "MacroRecorder must be a child of a Machine";
      return nullptr;
    }
    Location& loc = machine->Create<Timeline>();
    timeline = loc.As<Timeline>();
    // TODO: animate timeline creation
    PositionBelow(*macro_recorder.here, loc);
    AnimateGrowFrom(*macro_recorder.here, loc);
  }
  return timeline;
}

SkPath MacroRecorder::Shape() const { return MacroRecorderShape(); }
LongRunning* MacroRecorder::OnRun(Location& here) {
  if (keylogging == nullptr) {
    auto timeline = FindOrCreateTimeline(*this);
    // TODO: check if the nearby timeline already has some data and append new stuff at the end
    switch (timeline->state) {
      case Timeline::kPaused:
        timeline->state = Timeline::kRecording;
        timeline->recording.recording_started_at =
            time::SteadyNow() - time::Duration(timeline->paused.playback_offset);
        break;
      case Timeline::kRecording:
        // WTF? Maybe show an error?
        break;
      case Timeline::kPlaying:
        timeline->state = Timeline::kRecording;
        timeline->recording.recording_started_at = timeline->playing.playback_started_at;
        break;
    }
    keylogging = &gui::keyboard->BeginKeylogging(*this);
  }
  return this;
}
void MacroRecorder::Cancel() {
  if (keylogging) {
    if (auto timeline = FindTimeline(*this)) {
      timeline->state = Timeline::kPaused;
      timeline->paused.playback_offset = timeline->MaxTrackLength();
    }
    keylogging->Release();
    keylogging = nullptr;
  }
}

static void RecordKeyEvent(MacroRecorder& macro_recorder, AnsiKey key, bool down) {
  auto machine = macro_recorder.here->ParentAs<Machine>();
  if (machine == nullptr) {
    FATAL << "MacroRecorder must be a child of a Machine";
    return;
  }
  // Find the nearby timeline (or create one)
  auto timeline = FindOrCreateTimeline(macro_recorder);

  // Find a track which is attached to the given key
  int track_index = -1;
  Str key_name = "Key " + Str(ToStr(key));
  for (int i = 0; i < timeline->tracks.size(); i++) {
    if (timeline->track_args[i]->name == key_name) {
      track_index = i;
      break;
    }
  }

  if (track_index == -1) {
    // TODO: animate track creation
    timeline->AddOnOffTrack(key_name);
    track_index = timeline->tracks.size() - 1;
    // TODO: animate key presser creation
    Location& key_presser_loc = machine->Create<KeyPresser>();
    KeyPresser* key_presser = key_presser_loc.As<KeyPresser>();
    key_presser->SetKey(key);
    Rect key_presser_shape = key_presser_loc.object->Shape().getBounds();
    Vec2AndDir arg_start = timeline->here->ArgStart(nullptr, *timeline->track_args.back());
    key_presser_loc.position = arg_start.pos + Vec2(2_cm, 0) * track_index + Vec2(2_cm, -1.8_cm) -
                               key_presser_shape.TopCenter();
    timeline->here->ConnectTo(key_presser_loc, key_name);
  }

  bool is_down = timeline->tracks[track_index]->timestamps.size() % 2 == 1;
  if (is_down == down) {
    return;
  }

  // Append the current timestamp to that track
  OnOffTrack* track = dynamic_cast<OnOffTrack*>(timeline->tracks[track_index].get());
  if (track == nullptr) {
    ERROR << "Track is not an OnOffTrack";
    return;
  }

  track->timestamps.push_back(
      (time::SteadyNow() - timeline->recording.recording_started_at).count());
}

void MacroRecorder::KeyloggerKeyDown(gui::Key key) {
  LOG << "Key down: " << ToStr(key.physical);
  RecordKeyEvent(*this, key.physical, true);
}
void MacroRecorder::KeyloggerKeyUp(gui::Key key) {
  LOG << "Key up: " << ToStr(key.physical);
  RecordKeyEvent(*this, key.physical, false);
}

SkMatrix MacroRecorder::TransformToChild(const Widget& child, animation::Context&) const {
  if (&child == &record_button) {
    return SkMatrix::Translate(-17.5_mm, -3.2_mm);
  }
  return SkMatrix::I();
}
bool MacroRecorder::IsOn() const { return keylogging != nullptr; }
void MacroRecorder::On() { OnRun(*here); }
void MacroRecorder::Off() { Cancel(); }
}  // namespace automat::library