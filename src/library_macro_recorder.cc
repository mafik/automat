// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_macro_recorder.hh"

#include <include/core/SkColor.h>
#include <include/core/SkPaint.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradientShader.h>
#include <modules/svg/include/SkSVGDOM.h>

#include <tracy/Tracy.hpp>

#include "animation.hh"
#include "argument.hh"
#include "audio.hh"
#include "color.hh"
#include "embedded.hh"
#include "keyboard.hh"
#include "library_key_presser.hh"
#include "library_mouse.hh"
#include "library_timeline.hh"
#include "log.hh"
#include "math.hh"
#include "root_widget.hh"
#include "run_button.hh"
#include "sincos.hh"
#include "svg.hh"
#include "textures.hh"
#include "time.hh"
#include "ui_connection_widget.hh"
#include "widget.hh"

namespace automat::library {

using namespace automat::ui;
using namespace std;

const char kMacroRecorderShapeSVG[] =
    R"(m3.78-48.4c0-.58.49-.76.7-.76.6 0 2.62.04 2.62.04 0 0 3.06-.82 14.29-.82 11.22 0 15.12.75 15.12.75 0 0 2.17.03 2.69.03.46 0 .75.41.75.62l-.02 22.69.65.77-.51.05c.93 1 3.91 5.67 3.45 6.1-.28.26-.72-.3-.91-.06-.13.21 1.77 4.6.88 5.9-.29.42-.86 0-.88.48-.37 7.53-3.59 11.03-4.34 11.19-.09-.13-.17-.35-.17-.35 0 .97-.9 2.07-1.9 2.07-1.7 0-27.1 0-28.9 0-.9 0-2.2-1.2-2.3-2.3-.15.17-.5 2.05-1.24 2.03-2.94-4.1-2.8-12.41-2.64-13.19-2.07-.62-.06-5.09.28-5.51-.44-.04-1.31.06-1.34-.49-.03-.54 1.43-3.42 3.47-5.58-.03-.14-.64-.08-.65-.3-.02-.41.86-1.08.86-1.08z)";

constexpr float kEyeRadius = 9_mm / 2;
const Vec2 kLeftEyeCenter = {13_mm, 30.9_mm};
const Vec2 kRightEyeCenter = {30.2_mm, 30.9_mm};

constexpr float kHeight = 5_cm;

static SkPath& MacroRecorderShape() {
  static auto path = [] {
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

Ptr<Object> MacroRecorder::timeline_Impl::MakePrototype() {
  return prototypes->Find<Timeline>()->AcquirePtr<Object>();
}

void MacroRecorder::timeline_Impl::OnCanConnect(Interface end, Status& status) {
  if (end.table_ptr != nullptr || !dynamic_cast<Timeline*>(end.object_ptr)) {
    AppendErrorMessage(status) += "Must connect to a Timeline";
  }
}

void MacroRecorder::timeline_Impl::OnConnect(Interface end) {
  if (!end) {
    if (auto old_timeline_ptr = obj->timeline_connection.Lock()) {
      if (auto* old_timeline = dynamic_cast<Timeline*>(old_timeline_ptr.Get())) {
        if (old_timeline->state == Timeline::State::kRecording) {
          old_timeline->StopRecording();
        }
      }
    }
    obj->timeline_connection = {};
    return;
  }
  if (auto* timeline = dynamic_cast<Timeline*>(end.object_ptr)) {
    obj->timeline_connection = timeline->AcquireWeakPtr();
    if (obj->long_running->IsRunning()) {
      timeline->BeginRecording();
    }
  }
}

NestedPtr<Interface::Table> MacroRecorder::timeline_Impl::OnFind() {
  return NestedPtr<Interface::Table>(obj->timeline_connection.Lock(), nullptr);
}

static auto& timeline_arg = MacroRecorder::timeline_tbl;

static Timeline* FindTimeline(MacroRecorder& macro_recorder) {
  return dynamic_cast<Timeline*>(Argument(macro_recorder, timeline_arg).ObjectOrNull());
}

static Timeline* FindOrCreateTimeline(MacroRecorder& macro_recorder) {
  Timeline* timeline =
      dynamic_cast<Timeline*>(&Argument(macro_recorder, timeline_arg).ObjectOrMake());
  assert(timeline);
  if (macro_recorder.keylogging && timeline->state != Timeline::State::kRecording) {
    timeline->BeginRecording();
  }
  return timeline;
}

// MacroRecorder static interface definitions

void MacroRecorder::StartRecording(std::unique_ptr<RunTask>& run_task) {
  ZoneScopedN("MacroRecorder");
  if (keylogging == nullptr) {
    auto timeline = FindOrCreateTimeline(*this);
    timeline->BeginRecording();
    audio::Play(embedded::assets_SFX_macro_start_wav);
    root_widget->window->BeginLogging(this, &keylogging, this, &pointer_logging);
  }
  long_running->BeginLongRunning(std::move(run_task));
}

void MacroRecorder::StopRecording() {
  if (auto timeline = FindTimeline(*this)) {
    timeline->StopRecording();
  }
  audio::Play(embedded::assets_SFX_macro_stop_wav);
  if (keylogging) {
    keylogging->Release();
  }
  if (pointer_logging) {
    pointer_logging->Release();
    pointer_logging = nullptr;
  }
}

// MacroRecorder Object methods

MacroRecorder::MacroRecorder() {}
MacroRecorder::MacroRecorder(const MacroRecorder&) {}

MacroRecorder::~MacroRecorder() {
  if (keylogging) {
    keylogging->Release();
  }
  if (pointer_logging) {
    pointer_logging->Release();
    pointer_logging = nullptr;
  }
}

string_view MacroRecorder::Name() const { return "Macro Recorder"sv; }
Ptr<Object> MacroRecorder::Clone() const { return MAKE_PTR(MacroRecorder, *this); }

static void RecordOnOffEvent(MacroRecorder& macro_recorder, AnsiKey kb_key, PointerButton ptr_btn,
                             bool down) {
  auto board = macro_recorder.here->ParentAs<Board>();
  if (board == nullptr) {
    FATAL << "MacroRecorder must be a child of a Board";
    return;
  }
  // Find the nearby timeline (or create one)
  auto timeline = FindOrCreateTimeline(macro_recorder);

  // Find a track which is attached to the given key
  int track_index = -1;
  Str track_name;
  Fn<Location&()> make_fn;
  if (kb_key != AnsiKey::Unknown) {
    track_name = Str(ToStr(kb_key));
    make_fn = [&]() -> Location& {
      Location& l = board->Create<KeyPresser>();
      auto* kp = l.As<KeyPresser>();
      kp->SetKey(kb_key);
      return l;
    };
  } else if (ptr_btn != PointerButton::Unknown) {
    track_name = Str(ToStr(ptr_btn));
    make_fn = [&]() -> Location& {
      Location& l = board->Create<MouseButtonPresser>();
      auto* mb = l.As<MouseButtonPresser>();
      mb->button = ptr_btn;
      return l;
    };
  } else {
    ERROR_ONCE << "No key or pointer button specified";
    return;
  }
  for (int i = 0; i < timeline->tracks.size(); i++) {
    if (timeline->tracks[i]->name == track_name) {
      track_index = i;
      break;
    }
  }

  if (track_index == -1) {
    if (!down && timeline->tracks.empty()) {
      // Timeline is empty and the key is released. Do nothing.
      return;
    }
    auto& new_track = timeline->AddOnOffTrack(track_name);
    if (!down) {
      // If the key is released then we should assume that it was pressed before the recording and
      // the pressed section should start at 0.
      new_track.timestamps.push_back(0s);
    }
    track_index = timeline->tracks.size() - 1;
    Location& on_off_loc = make_fn();
    on_off_loc.Iconify();
    Argument::Table& track_arg = *timeline->tracks.back();

    PositionAhead(*timeline->here, track_arg, on_off_loc);
    AnimateGrowFrom(*macro_recorder.here, on_off_loc);
    Argument(*timeline, track_arg).Connect(Interface(*on_off_loc.object));
  }

  // Append the current timestamp to that track
  OnOffTrack* track = dynamic_cast<OnOffTrack*>(timeline->tracks[track_index]->track.Get());
  if (track == nullptr) {
    ERROR << "Track is not an OnOffTrack";
    return;
  }

  auto& ts = track->timestamps;
  time::Duration t = time::SteadyNow() - timeline->recording.started_at;

  size_t next_i = std::lower_bound(ts.begin(), ts.end(), t) - ts.begin();

  if (down) {
    if (next_i % 2) {
      ts[next_i - 1] = t;
      track->on_at = t;
    } else {
      if (next_i == ts.size()) {
        ts.push_back(t);
      }
      track->on_at = t;
    }
  } else {
    if (next_i % 2) {
      if (track->on_at == time::kDurationGuard) {
        if (next_i < ts.size()) {
          ts[next_i] = t;
        } else {
          ts.push_back(t);
        }
      } else {
        auto first_it = std::lower_bound(ts.begin(), ts.end(), track->on_at);
        size_t first_i = first_it - ts.begin();
        auto second_it = ts.begin() + next_i;

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
        track->on_at = time::kDurationGuard;
      }
    } else {
      if (track->on_at == time::kDurationGuard) {
        if (next_i > 0) {
          ts[next_i - 1] = t;
        }
      } else {
        auto first_it = std::lower_bound(ts.begin(), ts.end(), track->on_at);
        auto second_it = ts.begin() + next_i;
        if (second_it > first_it) {
          ts.erase(first_it, second_it);
        }
        ts.insert(first_it, {track->on_at, t});
        track->on_at = time::kDurationGuard;
      }
    }
  }
}

void MacroRecorder::KeyloggerKeyDown(ui::Key key) {
  RecordOnOffEvent(*this, key.physical, PointerButton::Unknown, true);
}
void MacroRecorder::KeyloggerKeyUp(ui::Key key) {
  RecordOnOffEvent(*this, key.physical, PointerButton::Unknown, false);
}
void MacroRecorder::KeyloggerOnRelease(const ui::Keylogging& keylogging) {
  this->keylogging = nullptr;
}

void MacroRecorder::PointerLoggerButtonDown(ui::Pointer::Logging&, ui::PointerButton btn) {
  RecordOnOffEvent(*this, AnsiKey::Unknown, btn, true);
}
void MacroRecorder::PointerLoggerButtonUp(ui::Pointer::Logging&, ui::PointerButton btn) {
  RecordOnOffEvent(*this, AnsiKey::Unknown, btn, false);
}

template <typename TrackT, typename ReceiverT>
static void RecordDelta(MacroRecorder& recorder, const char* track_name,
                        typename TrackT::ValueT delta) {
  auto timeline = FindOrCreateTimeline(recorder);

  int track_index = -1;
  for (int i = 0; i < timeline->tracks.size(); i++) {
    if (timeline->tracks[i]->name == track_name) {
      track_index = i;
      break;
    }
  }

  if (track_index == -1) {
    auto track_ptr = MAKE_PTR(TrackT);
    auto& new_track = *track_ptr;
    timeline->AddTrack(std::move(track_ptr), track_name);
    track_index = timeline->tracks.size() - 1;
    auto board = recorder.here->ParentAs<Board>();
    if (board == nullptr) {
      FATAL << "MacroRecorder must be a child of a Board";
      return;
    }
    Location& receiver_loc = board->Create<ReceiverT>();
    receiver_loc.Iconify();
    Argument::Table& track_arg = *timeline->tracks.back();

    PositionAhead(*timeline->here, track_arg, receiver_loc);
    AnimateGrowFrom(*recorder.here, receiver_loc);
    Argument(*timeline, track_arg).Connect(Interface(*receiver_loc.object));
  }

  auto* track = dynamic_cast<TrackT*>(timeline->tracks[track_index]->track.Get());
  if (track == nullptr) {
    ERROR << "Track is not a " << typeid(TrackT).name();
    return;
  }
  time::Duration t = time::SteadyNow() - timeline->recording.started_at;
  track->timestamps.push_back(t);
  track->values.push_back(delta);
}

void MacroRecorder::PointerLoggerScrollY(ui::Pointer::Logging&, float delta) {
  RecordDelta<Float64Track, MouseScrollY>(*this, "Scroll Y", delta);
}

void MacroRecorder::PointerLoggerScrollX(ui::Pointer::Logging&, float delta) {
  RecordDelta<Float64Track, MouseScrollX>(*this, "Scroll X", delta);
}

void MacroRecorder::PointerLoggerMove(ui::Pointer::Logging&, Vec2 relative_px) {
  RecordDelta<Vec2Track, MouseMove>(*this, "Mouse Position", relative_px);
}

void MacroRecorder::SerializeState(ObjectSerializer& writer) const {
  writer.Key("recording");
  writer.Bool(keylogging != nullptr);
}
bool MacroRecorder::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "recording") {
    Status status;
    bool value;
    d.Get(value, status);
    if (OK(status) && long_running->IsRunning() != value) {
      if (value) {
        runnable->ScheduleRun();
      } else {
        (new CancelTask(AcquireWeakPtr()))->Schedule();
      }
    }
    if (!OK(status)) {
      ReportError("Failed to deserialize MacroRecorder. " + status.ToStr());
    }
    return true;
  }
  return false;
}

// GlassRunButton

struct GlassRunButton : ui::PowerButton {
  GlassRunButton(ui::Widget* parent, NestedWeakPtr<OnOff::Table> on_off)
      : ui::PowerButton(parent, std::move(on_off), color::kParrotRed, "#eeeeee"_color) {}
  void PointerOver(ui::Pointer& p) override {
    ToggleButton::PointerOver(p);
    if (auto locked = target.Lock()) {
      auto& mr = static_cast<MacroRecorder&>(*locked.Owner<Object>());
      if (auto connection_widget = ConnectionWidget::FindOrNull(mr, timeline_arg)) {
        connection_widget->animation_state.prototype_alpha_target = 1;
        connection_widget->WakeAnimation();
      }
    }
  }
  void PointerLeave(ui::Pointer& p) override {
    ToggleButton::PointerLeave(p);
    if (auto locked = target.Lock()) {
      auto& mr = static_cast<MacroRecorder&>(*locked.Owner<Object>());
      if (auto connection_widget = ConnectionWidget::FindOrNull(mr, timeline_arg)) {
        connection_widget->animation_state.prototype_alpha_target = 0;
        connection_widget->WakeAnimation();
      }
    }
  }
  StrView Name() const override { return "GlassRunButton"; }
};

// MacroRecorderWidget

struct MacroRecorderWidget : ObjectToy, ui::PointerMoveCallback {
  struct AnimationState {
    animation::SpringV2<Vec2> googly_left;
    animation::SpringV2<Vec2> googly_right;
    float eye_rotation_speed = 0;
    float eye_rotation = 0;
    int pointers_over = 0;
    float eyes_open = 0;
  };

  mutable AnimationState animation_state;
  std::unique_ptr<ui::Widget> record_button;
  bool is_recording = false;

  Ptr<MacroRecorder> LockMacroRecorder() const { return LockObject<MacroRecorder>(); }

  MacroRecorderWidget(ui::Widget* parent, Object& mr_obj) : ObjectToy(parent, mr_obj) {
    if (auto mr = LockMacroRecorder()) {
      record_button.reset(new GlassRunButton(
          this,
          NestedWeakPtr<OnOff::Table>(mr->AcquireWeakPtr(), &MacroRecorder::long_running_tbl)));
      record_button->local_to_parent = SkM44::Translate(17.5_mm, 3.2_mm);
      is_recording = mr->keylogging != nullptr;
    }
  }

  animation::Phase Tick(time::Timer& timer) override {
    if (auto mr = LockMacroRecorder()) {
      is_recording = mr->keylogging != nullptr;
    }

    record_button->WakeAnimationAt(timer.last);

    auto phase = is_recording ? animation::Animating : animation::Finished;

    phase |= animation::ExponentialApproach(is_recording ? 1 : 0, timer.d, 0.2,
                                            animation_state.eye_rotation_speed);

    animation_state.eye_rotation -= timer.d * 360 * animation_state.eye_rotation_speed;
    if (animation_state.eye_rotation < 0) {
      animation_state.eye_rotation += 360;
    }

    float eyes_open_target;
    if (is_recording) {
      eyes_open_target = 1;
    } else if (animation_state.pointers_over > 0) {
      eyes_open_target = 0.8;
    } else {
      eyes_open_target = 0;
    }

    phase |=
        animation::ExponentialApproach(eyes_open_target, timer.d, 0.2, animation_state.eyes_open);

    auto local_to_window = TransformUp(*this);
    auto& rw = FindRootWidget();
    auto main_pointer_screen = *rw.window->MousePositionScreenPx();

    auto UpdateEye = [&](Vec2 center, animation::SpringV2<Vec2>& googly) -> animation::Phase {
      auto eye_window = local_to_window.mapPoint(center.sk);
      auto eye_screen = rw.window->WindowPxToScreen(eye_window);
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

  void Draw(SkCanvas& canvas) const override {
    {  // Draw the eyes
      auto sharingan = SharinganColor();

      auto size = sharingan->containerSize();
      float s = 0.9 * kEyeRadius * 2 / size.height();

      auto DrawEye = [&](Vec2 center, animation::SpringV2<Vec2>& googly) {
        Rect bounds = Rect::MakeCenter(center, kEyeRadius * 2, kEyeRadius * 2);

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

  SkPath Shape() const override { return MacroRecorderShape(); }

  void FillChildren(Vec<Widget*>& children) override { children.push_back(record_button.get()); }

  void PointerOver(ui::Pointer& p) override {
    animation_state.pointers_over++;
    StartWatching(p);
    WakeAnimation();
  }
  void PointerLeave(ui::Pointer& p) override {
    animation_state.pointers_over--;
    StopWatching(p);
    p.move_callbacks.Erase(this);
  }

  void PointerMove(ui::Pointer&, Vec2 position) override { WakeAnimation(); }

  Vec2AndDir ArgStart(const Interface::Table& arg, ui::Widget* coordinate_space) override {
    if (&arg == &timeline_arg) {
      Vec2AndDir pos_dir{
          .pos = {22_mm, 0},
          .dir = -90_deg,
      };
      if (coordinate_space) {
        auto m = TransformBetween(*this, *coordinate_space);
        pos_dir.pos = m.mapPoint(pos_dir.pos);
      }
      return pos_dir;
    }
    return ObjectToy::ArgStart(arg, coordinate_space);
  }
};

std::unique_ptr<ObjectToy> MacroRecorder::MakeToy(ui::Widget* parent) {
  return std::make_unique<MacroRecorderWidget>(parent, *this);
}

}  // namespace automat::library
