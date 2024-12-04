// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_macro_recorder.hh"

#include <include/core/SkColor.h>
#include <include/core/SkPaint.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradientShader.h>
#include <include/gpu/GrDirectContext.h>
#include <modules/svg/include/SkSVGDOM.h>

#include "animation.hh"
#include "argument.hh"
#include "audio.hh"
#include "color.hh"
#include "embedded.hh"
#include "gui_connection_widget.hh"
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
using namespace maf;

DEFINE_PROTO(MacroRecorder);

const char kMacroRecorderShapeSVG[] =
    R"(m3.78-48.4c0-.58.49-.76.7-.76.6 0 2.62.04 2.62.04 0 0 3.06-.82 14.29-.82 11.22 0 15.12.75 15.12.75 0 0 2.17.03 2.69.03.46 0 .75.41.75.62l-.02 22.69.65.77-.51.05c.93 1 3.91 5.67 3.45 6.1-.28.26-.72-.3-.91-.06-.13.21 1.77 4.6.88 5.9-.29.42-.86 0-.88.48-.37 7.53-3.59 11.03-4.34 11.19-.09-.13-.17-.35-.17-.35 0 .97-.9 2.07-1.9 2.07-1.7 0-27.1 0-28.9 0-.9 0-2.2-1.2-2.3-2.3-.15.17-.5 2.05-1.24 2.03-2.94-4.1-2.8-12.41-2.64-13.19-2.07-.62-.06-5.09.28-5.51-.44-.04-1.31.06-1.34-.49-.03-.54 1.43-3.42 3.47-5.58-.03-.14-.64-.08-.65-.3-.02-.41.86-1.08.86-1.08z)";

constexpr float kEyeRadius = 9_mm / 2;
const Vec2 kLeftEyeCenter = {13_mm, 30.9_mm};
const Vec2 kRightEyeCenter = {30.2_mm, 30.9_mm};

constexpr float kHeight = 5_cm;

static SkPath& MacroRecorderShape() {
  static auto path = []() {
    auto path = PathFromSVG(kMacroRecorderShapeSVG, SVGUnit_Millimeters);
    auto bounds = path.getBounds();
    // scale to kHeight
    float scale = kHeight / bounds.height();
    path.transform(SkMatrix::Scale(scale, scale));
    // align bottom edge to 0
    bounds = path.getBounds();
    path.transform(SkMatrix::Translate(0.25_mm, -bounds.fTop));

    return path;
  }();
  return path;
}

static auto macro_recorder_front_color = PersistentImage::MakeFromAsset(
    embedded::assets_macro_recorder_front_color_webp, {.height = kHeight});

static sk_sp<SkSVGDOM>& SharinganColor() {
  static auto dom = SVGFromAsset(embedded::assets_sharingan_color_svg.content);
  return dom;
}

MacroRecorder::MacroRecorder() : record_button(make_shared<GlassRunButton>(this)) {
  record_button->local_to_parent = SkM44::Translate(17.5_mm, 3.2_mm);
}
MacroRecorder::~MacroRecorder() {
  if (keylogging) {
    keylogging->Release();
    keylogging = nullptr;
  }
}

// We will provide a prototype of the Timeline - to be created if no timeline can be found.
// We will also specify here that it should search for any Timeline objects nearby (with some
// radius).
Argument timeline_arg = []() {
  Argument arg("Timeline", Argument::kRequiresObject);
  arg.RequireInstanceOf<Timeline>();
  arg.autoconnect_radius = 10_cm;
  arg.tint = color::kParrotRed;
  arg.style = Argument::Style::Cable;
  return arg;
}();

void MacroRecorder::Args(std::function<void(Argument&)> cb) { cb(timeline_arg); }
std::shared_ptr<Object> MacroRecorder::ArgPrototype(const Argument& arg) {
  if (&arg == &timeline_arg) {
    return Timeline::proto;
  }
  return nullptr;
}

string_view MacroRecorder::Name() const { return "Macro Recorder"; }
std::shared_ptr<Object> MacroRecorder::Clone() const {
  auto clone = std::make_shared<MacroRecorder>();
  clone->animation_state = animation_state;
  clone->animation_state.pointers_over = 0;
  return clone;
}

animation::Phase MacroRecorder::Tick(time::Timer& timer) {
  auto phase = keylogging ? animation::Animating : animation::Finished;

  phase |= animation::ExponentialApproach(keylogging ? 1 : 0, timer.d, 0.2,
                                          animation_state.eye_rotation_speed);

  animation_state.eye_rotation -= timer.d * 360 * animation_state.eye_rotation_speed;
  if (animation_state.eye_rotation < 0) {
    animation_state.eye_rotation += 360;
  }

  float eyes_open_target;
  if (keylogging) {
    eyes_open_target = 1;
  } else if (animation_state.pointers_over > 0) {
    eyes_open_target = 0.8;
  } else {
    eyes_open_target = 0;
  }

  phase |=
      animation::ExponentialApproach(eyes_open_target, timer.d, 0.2, animation_state.eyes_open);

  auto local_to_window = TransformUp(*this);
  auto main_pointer_screen = GetMainPointerScreenPos();
  auto top_window = dynamic_cast<gui::Window*>(&RootWidget());

  auto UpdateEye = [&](Vec2 center, animation::SpringV2<Vec2>& googly) -> animation::Phase {
    auto eye_window = local_to_window.mapPoint(center.sk);
    auto eye_screen = WindowPxToScreen(eye_window);
    auto eye_delta = main_pointer_screen - eye_screen;
    auto eye_dir = Normalize(eye_delta);
    float z = local_to_window.mapRadius(kEyeRadius * 2);
    auto eye_dist_3d = Length(Vec3(eye_delta.x, eye_delta.y, z));
    auto eye_dist_2d = Length(eye_delta);

    float dist = eye_dist_2d / eye_dist_3d;

    Vec2 target = Vec2(eye_dir.x * dist, -eye_dir.y * dist);
    return googly.SpringTowards(target, timer.d, 0.5, 0.2);
  };
  phase |= UpdateEye(kLeftEyeCenter, animation_state.googly_left);
  phase |= UpdateEye(kRightEyeCenter, animation_state.googly_right);

  return phase;
}

#pragma region Draw
void MacroRecorder::Draw(SkCanvas& canvas) const {
  {  // Draw the eyes
    auto sharingan = SharinganColor();

    auto size = sharingan->containerSize();
    float s = 0.9 * kEyeRadius * 2 / size.height();

    auto DrawEye = [&](Vec2 center, animation::SpringV2<Vec2>& googly) {
      Rect bounds = Rect::MakeCenterWH(center, kEyeRadius * 2, kEyeRadius * 2);

      if (animation_state.eyes_open > 0) {
        SkPaint white_eye_paint = SkPaint();
        white_eye_paint.setColor(SK_ColorWHITE);
        canvas.drawRect(bounds, white_eye_paint);

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
      }

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

  macro_recorder_front_color.draw(canvas);

  DrawChildren(canvas);
}

static Timeline* FindTimeline(MacroRecorder& macro_recorder) {
  Timeline* timeline = timeline_arg.FindObject<Timeline>(*macro_recorder.here.lock());
  return timeline;
}

static Timeline* FindOrCreateTimeline(MacroRecorder& macro_recorder) {
  Timeline* timeline = timeline_arg.FindObject<Timeline>(
      *macro_recorder.here.lock(), {.if_missing = Argument::IfMissing::CreateFromPrototype});
  assert(timeline);
  if (macro_recorder.keylogging && timeline->state != Timeline::State::kRecording) {
    timeline->BeginRecording();
  }
  return timeline;
}

SkPath MacroRecorder::Shape() const { return MacroRecorderShape(); }

void MacroRecorder::ConnectionAdded(Location& here, Connection& c) {
  if (&c.argument == &timeline_arg && IsOn()) {
    if (auto timeline = FindTimeline(*this)) {
      timeline->BeginRecording();
    }
  }
}
void MacroRecorder::ConnectionRemoved(Location& here, Connection& c) {
  if (&c.argument == &timeline_arg && IsOn()) {
    if (auto timeline = FindTimeline(*this)) {
      timeline->StopRecording();
    }
  }
}
LongRunning* MacroRecorder::OnRun(Location& here) {
  if (keylogging == nullptr) {
    auto timeline = FindOrCreateTimeline(*this);
    timeline->BeginRecording();
    audio::Play(embedded::assets_SFX_macro_start_wav);
    keylogging = &gui::keyboard->BeginKeylogging(*this);
  }
  return this;
}
void MacroRecorder::Cancel() {
  if (keylogging) {
    if (auto timeline = FindTimeline(*this)) {
      timeline->StopRecording();
    }
    audio::Play(embedded::assets_SFX_macro_stop_wav);
    keylogging->Release();
    keylogging = nullptr;
  }
}

static void RecordKeyEvent(MacroRecorder& macro_recorder, AnsiKey key, bool down) {
  auto machine = macro_recorder.here.lock()->ParentAs<Machine>();
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
    Rect key_presser_shape = key_presser_loc.object->Shape().getBounds();
    Argument& track_arg = *timeline->track_args.back();
    Vec2AndDir arg_start = track_arg.Start(*timeline, *timeline->parent);

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
    AnimateGrowFrom(*macro_recorder.here.lock(), key_presser_loc);
    timeline->here.lock()->ConnectTo(key_presser_loc, track_arg);
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

bool MacroRecorder::IsOn() const { return keylogging != nullptr; }
void MacroRecorder::On() { here.lock()->long_running = OnRun(*here.lock()); }
void MacroRecorder::Off() {
  Cancel();
  here.lock()->long_running = nullptr;
}
void MacroRecorder::PointerOver(gui::Pointer& p) {
  animation_state.pointers_over++;
  StartWatching(p);
  WakeAnimation();
}
void MacroRecorder::PointerLeave(gui::Pointer& p) {
  animation_state.pointers_over--;
  StopWatching(p);
  p.move_callbacks.Erase(this);
}

void MacroRecorder::PointerMove(gui::Pointer&, Vec2 position) { WakeAnimation(); }

void GlassRunButton::PointerOver(gui::Pointer& p) {
  ToggleButton::PointerOver(p);
  auto macro_recorder = dynamic_cast<MacroRecorder*>(target);
  if (auto h = macro_recorder->here.lock()) {
    if (auto connection_widget = ConnectionWidget::Find(*h, timeline_arg)) {
      connection_widget->animation_state.prototype_alpha_target = 1;
      connection_widget->WakeAnimation();
    }
  }
}

void GlassRunButton::PointerLeave(gui::Pointer& p) {
  ToggleButton::PointerLeave(p);
  auto macro_recorder = dynamic_cast<MacroRecorder*>(target);
  if (auto h = macro_recorder->here.lock()) {
    if (auto connection_widget = ConnectionWidget::Find(*h, timeline_arg)) {
      connection_widget->animation_state.prototype_alpha_target = 0;
      connection_widget->WakeAnimation();
    }
  }
}

Vec2AndDir MacroRecorder::ArgStart(const Argument& arg) {
  if (&arg == &timeline_arg) {
    return Vec2AndDir{
        .pos = {22_mm, 0},
        .dir = -90_deg,
    };
  }
  return Object::ArgStart(arg);
}

void MacroRecorder::SerializeState(Serializer& writer, const char* key) const {
  writer.Key(key);
  writer.StartObject();
  writer.Key("recording");
  writer.Bool(keylogging != nullptr);
  writer.EndObject();
}
void MacroRecorder::DeserializeState(Location& l, Deserializer& d) {
  Status status;
  for (auto& key : ObjectView(d, status)) {
    if (key == "recording") {
      bool value;
      d.Get(value, status);
      if (OK(status) && IsOn() != value) {
        if (value) {
          l.ScheduleRun();
        } else {
          (new CancelTask(l.SharedPtr<Location>()))
              ->Schedule();  // TODO: memory leak if NoScheduling is active
        }
      }
    }
  }
  if (!OK(status)) {
    l.ReportError("Failed to deserialize MacroRecorder. " + status.ToStr());
  }
}

}  // namespace automat::library