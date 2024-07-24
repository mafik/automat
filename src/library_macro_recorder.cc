#include "library_macro_recorder.hh"

#include <include/core/SkColor.h>
#include <include/core/SkPaint.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradientShader.h>
#include <modules/svg/include/SkSVGDOM.h>

#include <cstdint>

#include "../build/generated/embedded.hh"
#include "animation.hh"
#include "argument.hh"
#include "color.hh"
#include "connector_optical.hh"
#include "keyboard.hh"
#include "library_key_presser.hh"
#include "library_macros.hh"
#include "library_timeline.hh"
#include "log.hh"
#include "math.hh"
#include "sincos.hh"
#include "svg.hh"
#include "textures.hh"
#include "time.hh"
#include "widget.hh"
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

constexpr float kEyeRadius = 9_mm / 2;
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

static Timeline* FindTimeline(const MacroRecorder& macro_recorder) {
  if (macro_recorder.here == nullptr) {
    return nullptr;
  }
  auto machine = macro_recorder.here->ParentAs<Machine>();
  if (machine == nullptr) {
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
string_view MacroRecorder::Name() const { return "Macro Recorder"; }
std::unique_ptr<Object> MacroRecorder::Clone() const { return std::make_unique<MacroRecorder>(); }
void MacroRecorder::Draw(gui::DrawContext& dctx) const {
  auto& animation_state = animation_state_ptr[dctx.display];
  auto& image = MacroRecorderFrontColor();
  auto& canvas = dctx.canvas;

  if (keylogging) {
    animation_state.eye_speed.target = 1;
  } else {
    animation_state.eye_speed.target = 0;
  }
  animation_state.eye_speed.speed = 5;
  animation_state.eye_speed.Tick(dctx.display);
  if (keylogging) {
    animation_state.eyes_open.target = 1;
    animation_state.eyes_open.speed = 5;
  } else if (animation_state.pointers_over > 0) {
    animation_state.eyes_open.target = 0.8;
    animation_state.eyes_open.speed = 5;
  } else {
    animation_state.eyes_open.target = 0;
    animation_state.eyes_open.speed = 5;
  }
  animation_state.eyes_open.Tick(dctx.display);
  animation_state.eye_rotation -= dctx.display.timer.d * 360 * animation_state.eye_speed;
  if (animation_state.eye_rotation < 0) {
    animation_state.eye_rotation += 360;
  }

  Timeline* timeline = FindTimeline(*this);
  animation_state.timeline_cable_width.target =
      (animation_state.pointers_over > 0 && timeline != nullptr) ? 2_mm : 0;
  animation_state.timeline_cable_width.Tick(dctx.display);
  animation_state.timeline_cable_width.speed = 5;

  if (animation_state.timeline_cable_width > 0.01_mm && timeline) {
    Vec2AndDir start = {.pos = Vec2(2.2_cm, 1_mm), .dir = -90_deg};

    Vec<Vec2AndDir> ends = {};
    timeline->ConnectionPositions(ends);
    Path up_path = {timeline->here->ParentAs<Widget>(), timeline->here, timeline};
    auto matrix = TransformUp(up_path, &dctx.display);
    Path down_path = {here->ParentAs<Widget>(), here, const_cast<MacroRecorder*>(this)};
    matrix.postConcat(TransformDown(down_path, &dctx.display));
    for (auto& end : ends) {
      end.pos = matrix.mapPoint(end.pos);
    }

    auto arcline = RouteCable(dctx, start, ends);
    auto color =
        SkColorSetA(color::kParrotRed, 255 * animation_state.timeline_cable_width.value / 2_mm);
    auto color_filter = color::MakeTintFilter(color, 30);
    auto path = arcline.ToPath(false);
    DrawCable(dctx, path, color_filter, CableTexture::Smooth,
              animation_state.timeline_cable_width.value);
  }

  {
    auto sharingan = SharinganColor();

    auto local_to_window = TransformUp(dctx.path, &dctx.display);

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

      googly.period = 0.5s;
      googly.half_life = 0.2s;
      googly.target.x = eye_dir.x * dist;
      googly.target.y = -eye_dir.y * dist;
      googly.Tick(dctx.display);

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

      if (animation_state.eyes_open < 0.999) {
        float eyelid_offset =
            animation_state.eyes_open * -kEyeRadius / (animation_state.eyes_open - 1.01);
        SkPath eyelid;
        if (eyelid_offset < 0.1_mm) {
          eyelid = SkPath::Rect(bounds);
        } else {
          Vec2 cp_top = bounds.Center() + Vec2(0, eyelid_offset);
          Vec2 cp_bottom = bounds.Center() - Vec2(0, eyelid_offset);
          float eyelid_r = Length(cp_top - bounds.LeftCenter()) * kEyeRadius / eyelid_offset;
          eyelid.moveTo(bounds.LeftCenter());
          eyelid.arcTo(cp_top, bounds.RightCenter(), eyelid_r);
          eyelid.arcTo(cp_bottom, bounds.LeftCenter(), eyelid_r);
          eyelid.lineTo(bounds.BottomLeftCorner());
          eyelid.lineTo(bounds.BottomRightCorner());
          eyelid.lineTo(bounds.TopRightCorner());
          eyelid.lineTo(bounds.TopLeftCorner());
          eyelid.close();
        }

        SkPaint eyelid_paint;
        SkColor colors[] = {"#353940"_color, "#131519"_color, "#070708"_color};
        float colors_pos[] = {0, 0.6, 1};
        eyelid_paint.setShader(
            SkGradientShader::MakeRadial(bounds.Center() + Vec2(0, kEyeRadius / 2), kEyeRadius,
                                         colors, colors_pos, 3, SkTileMode::kClamp));
        canvas.drawPath(eyelid, eyelid_paint);
      }

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
  // canvas.drawPath(record_button.Shape(nullptr), outline);

  // outline.setStyle(SkPaint::kStroke_Style);
  // canvas.drawPath(record_button.child->Shape(nullptr), outline);
}

static void PositionBelow(Location& origin, Location& below) {
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
  for (auto* display : animation::displays) {
    auto& animation_state = grown.GetAnimationState(*display);
    animation_state.scale.value = 0.5;
    Vec2 source_center = source.object->Shape(nullptr).getBounds().center() + source.position;
    animation_state.position.value = source_center;
    animation_state.transparency.value = 1;
  }
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
    Rect timeline_bounds = timeline->Shape(nullptr).getBounds();
    loc.position = macro_recorder.here->position + Vec2(2.2_cm, 0) - timeline_bounds.TopCenter();
    PositionBelow(*macro_recorder.here, loc);
    AnimateGrowFrom(*macro_recorder.here, loc);
  }
  if (macro_recorder.keylogging && timeline->state != Timeline::State::kRecording) {
    timeline->BeginRecording();
  }
  return timeline;
}

SkPath MacroRecorder::Shape(animation::Display*) const { return MacroRecorderShape(); }
LongRunning* MacroRecorder::OnRun(Location& here) {
  if (keylogging == nullptr) {
    auto timeline = FindOrCreateTimeline(*this);
    timeline->BeginRecording();
    keylogging = &gui::keyboard->BeginKeylogging(*this);
  }
  return this;
}
void MacroRecorder::Cancel() {
  if (keylogging) {
    if (auto timeline = FindTimeline(*this)) {
      timeline->StopRecording();
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
  Str key_name = Str(ToStr(key));
  for (int i = 0; i < timeline->tracks.size(); i++) {
    if (timeline->track_args[i]->name == key_name) {
      track_index = i;
      break;
    }
  }

  if (track_index == -1) {
    if (!down && timeline->tracks.empty()) {
      // Timeline is empty and the key is released. Do nothing.
      return;
    }
    auto& new_track = timeline->AddOnOffTrack(key_name);
    if (!down) {
      // If the key is released then we should assume that it was pressed before the recording and
      // the pressed section should start at 0.
      new_track.timestamps.push_back(0);
    }
    track_index = timeline->tracks.size() - 1;
    Location& key_presser_loc = machine->Create<KeyPresser>();
    KeyPresser* key_presser = key_presser_loc.As<KeyPresser>();
    key_presser->SetKey(key);
    Rect key_presser_shape = key_presser_loc.object->Shape(nullptr).getBounds();
    Vec2AndDir arg_start = timeline->here->ArgStart(nullptr, *timeline->track_args.back());

    // Pick the position that allows the cable to come in most horizontally (left to right).
    Vec2 best_connector_pos = key_presser_shape.TopCenter();
    SinCos best_connector_angle = 90_deg;
    Vec<Vec2AndDir> connector_positions;
    key_presser->ConnectionPositions(connector_positions);
    for (auto& pos : connector_positions) {
      if (fabs(pos.dir.ToRadians()) < fabs(best_connector_angle.ToRadians())) {
        best_connector_pos = pos.pos;
        best_connector_angle = pos.dir;
      }
    }

    key_presser_loc.position = arg_start.pos + Vec2(3_cm, 0) - best_connector_pos;
    AnimateGrowFrom(*macro_recorder.here, key_presser_loc);
    timeline->here->ConnectTo(key_presser_loc, key_name);
  }

  // Append the current timestamp to that track
  OnOffTrack* track = dynamic_cast<OnOffTrack*>(timeline->tracks[track_index].get());
  if (track == nullptr) {
    ERROR << "Track is not an OnOffTrack";
    return;
  }

  auto& ts = track->timestamps;
  time::T t = (time::SteadyNow() - timeline->recording.started_at).count();

  size_t next_i = std::lower_bound(ts.begin(), ts.end(), t) - ts.begin();

  // LOG << "key " << ToStr(key) << " " << (down ? "down" : "up") << " while in "
  //     << (next_i % 2 ? "filled" : "empty") << " section"
  //     << (isnan(track->on_at) ? "" : " with on_at") << ", at frame " << frame << " (" << t <<
  //     ")";

  // How recording over existing tracks works:
  // - if either start or end of a key-down section touches another section, that section is
  // adjusted to cover the new key-down section
  // - otherwise a new section is added and any overlapping sections are removed
  if (down) {
    if (next_i % 2) {
      // Key is pressed down while in a filled section. Move the start time to `t`.
      ts[next_i - 1] = t;
      track->on_at = t;
    } else {
      // Key is pressed while in an empty section. Mark current time as `on_at` and continue.
      if (next_i == ts.size()) {
        ts.push_back(t);
      }
      track->on_at = t;
    }
  } else {
    if (next_i % 2) {
      // filled section
      if (isnan(track->on_at)) {
        // This shouldn't happen but in some edge cases it might. We just adjust the end time of
        // the current section to `t`.
        if (next_i < ts.size()) {
          ts[next_i] = t;
        } else {
          ts.push_back(t);
        }
      } else {
        // Replace the current section with one
        auto first_it = std::lower_bound(ts.begin(), ts.end(), track->on_at);
        size_t first_i = first_it - ts.begin();
        auto second_it = ts.begin() + next_i;
        size_t second_i = next_i;

        // LOG << "first_i: " << first_i << " second_i: " << second_i << " ts.size(): " <<
        // ts.size();

        // Example starting state:
        // 0  1  2  3  4  5  6
        // ---###---###---###---
        // ts idxs: 0 1 2 3 4 5
        // ts vals: 1 2 3 4 5 6
        //
        // Test case 1 (spanning 3 existing sections):
        // Let's say on_at is 1 and t is 5.5
        // We want to remove stuff between indexes 1 (inclusive) and 5 (exclusive).
        // Then we overwrite the value 1 (idx 0) with 1 and value 6 (idx 1) 5.5.
        // lower_bound (on_at) = 0
        // upper_bound (on_at) = 1
        // lower_bound (t) = 5
        //
        // Test case 2 (spanning 2 existing sections):
        // Let's say on_at is 1 and t is 2.5
        // We want to remove stuff between indexes 1 (inclusive) and 3 (exclusive).
        // Then we overwrite the value 1 (idx 0) with 1 and value 4 (idx 1) with 2.5.
        // lower_bound (on_at) = 0
        // upper_bound (on_at) = 1
        // lower_bound (t) = 2
        //
        // Test case 3 (starting empty, ending in the next section):
        // Let's say on_at is at 2.5 and t is 3.5
        // We don't want to remove anything.
        // We overwrite the value 3 (idx 2) with 2.5 and value 4 (idx 3) with 3.5.
        // lower_bound (on_at) = 2
        // upper_bound (on_at) = 2
        // lower_bound (t) = 3
        //
        // Test case 4 (starting empty, spanning one section and ending in another):
        // Let's say on_at is at 2.5 and t is 5.5
        // We want to remove stuff between indexes 3 (inclusive) and 5 (exclusive).
        // We overwrite the value 3 (idx 2) with 2.5 and value 6 (idx 3) with 5.5.
        // lower_bound (on_at) = 2
        // upper_bound (on_at) = 2
        // lower_bound (t) = 5

        first_it++;
        if (second_it > first_it) {
          first_it = ts.erase(first_it, second_it);
        }
        if (first_i < ts.size()) {
          ts[first_i] = track->on_at;
        } else {
          ts.push_back(track->on_at);
        }
        if (first_i + 1 < ts.size()) {
          ts[first_i + 1] = t;
        } else {
          ts.push_back(t);
        }
        track->on_at = NAN;
      }
    } else {
      // empty section
      if (isnan(track->on_at)) {
        // This shouldn't happen but in some edge cases it might. We just adjust the end time of
        // the previous section to `t`.
        if (next_i > 0) {
          ts[next_i - 1] = t;  // DONE!
        }
      } else {
        // Key released in an empty section.
        auto first_it = std::lower_bound(ts.begin(), ts.end(), track->on_at);
        auto second_it = ts.begin() + next_i;
        if (second_it > first_it) {
          ts.erase(first_it, second_it);
        }
        ts.insert(first_it, {track->on_at, t});
        track->on_at = NAN;
      }
    }
  }
}

void MacroRecorder::KeyloggerKeyDown(gui::Key key) { RecordKeyEvent(*this, key.physical, true); }
void MacroRecorder::KeyloggerKeyUp(gui::Key key) { RecordKeyEvent(*this, key.physical, false); }

SkMatrix MacroRecorder::TransformToChild(const Widget& child, animation::Display*) const {
  if (&child == &record_button) {
    return SkMatrix::Translate(-17.5_mm, -3.2_mm);
  }
  return SkMatrix::I();
}
bool MacroRecorder::IsOn() const { return keylogging != nullptr; }
void MacroRecorder::On() { here->long_running = OnRun(*here); }
void MacroRecorder::Off() {
  Cancel();
  here->long_running = nullptr;
}
void MacroRecorder::PointerOver(gui::Pointer&, animation::Display& d) {
  animation_state_ptr[d].pointers_over++;
}
void MacroRecorder::PointerLeave(gui::Pointer&, animation::Display& d) {
  animation_state_ptr[d].pointers_over--;
}
}  // namespace automat::library