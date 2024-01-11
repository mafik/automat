#include "library_timer.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/effects/SkGradientShader.h>
#include <include/pathops/SkPathOps.h>

#include <cmath>
#include <condition_variable>
#include <map>
#include <memory>
#include <thread>

#include "drag_action.hh"
#include "font.hh"
#include "library_macros.hh"
#include "pointer.hh"
#include "tasks.hh"
#include "thread_name.hh"

namespace automat::library {

DEFINE_PROTO(TimerDelay);
Argument TimerDelay::finished_arg = Argument("finished", Argument::kRequiresLocation);

TimerDelay::TimerDelay() {}

string_view TimerDelay::Name() const { return "Delay"; }

std::unique_ptr<Object> TimerDelay::Clone() const { return std::make_unique<TimerDelay>(*this); }

static constexpr float kOuterRadius = 0.02;
static constexpr SkRect kOuterOval =
    SkRect::MakeXYWH(-kOuterRadius, -kOuterRadius, 2 * kOuterRadius, 2 * kOuterRadius);
static constexpr float kSoftEdgeWidth = 0.0005;

float r0 = kOuterRadius;
float r1 = kOuterRadius - kSoftEdgeWidth;
float r2 = r1 - 3 * kSoftEdgeWidth;
float r3 = r2 - kSoftEdgeWidth;
float r4 = r3 - kSoftEdgeWidth;  // outer edge of white watch face
float r4_b = r4 * 0.9;
float r5 = kSoftEdgeWidth * 3;
float r6 = r5 - kSoftEdgeWidth;

static sk_sp<SkShader> MakeGradient(SkPoint a, SkPoint b, SkColor color_a, SkColor color_b) {
  SkPoint pts[2] = {a, b};
  SkColor colors[2] = {color_a, color_b};
  return SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
}

enum DrawRingMode {
  kRingAliased,
  kRingAntiAliased,
  kRingBlurred,
  kRingInset,
};

static void DrawRing(SkCanvas& canvas, float outer_r, float inner_r, SkColor top_left,
                     SkColor bottom_right, DrawRingMode mode = kRingAntiAliased) {
  SkPaint paint;
  SkPoint top_left_pos = SkPoint::Make(-outer_r * M_SQRT2 / 2, outer_r * M_SQRT2 / 2);
  SkPoint bottom_right_pos = SkPoint::Make(outer_r * M_SQRT2 / 2, -outer_r * M_SQRT2 / 2);
  paint.setShader(MakeGradient(top_left_pos, bottom_right_pos, top_left, bottom_right));
  if (mode == kRingAntiAliased) {
    paint.setAntiAlias(true);
  }
  // paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.0001f, true));
  float radius;
  if (mode == kRingBlurred) {
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, (outer_r - inner_r) / 4));
  }
  if (mode == kRingInset) {
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, (outer_r - inner_r) / 4));
    canvas.save();
    canvas.clipRRect(
        SkRRect::MakeOval(SkRect::MakeXYWH(-outer_r, -outer_r, outer_r * 2, outer_r * 2)), true);
    outer_r += (outer_r - inner_r);
  }
  if (inner_r > 0) {
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(outer_r - inner_r);
    radius = (outer_r + inner_r) / 2;
  } else {
    paint.setStyle(SkPaint::kFill_Style);
    radius = outer_r;
  }
  canvas.drawCircle(SkPoint::Make(0, 0), radius, paint);
  if (mode == kRingInset) {
    canvas.restore();
  }
}

static void DrawArrow(SkCanvas& canvas, float ratio) {
  float arrow_width = 0.0004;
  canvas.save();
  canvas.translate(0.001, -0.001);
  canvas.rotate(-360 * ratio);
  SkPaint shadow_paint;
  shadow_paint.setColor(0xff46464d);
  shadow_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.0005f, true));
  canvas.drawRect(SkRect::MakeXYWH(-arrow_width / 2, 0, arrow_width, r4 * 0.8),
                  shadow_paint);  // red arrow
  canvas.restore();

  SkPaint arrow_paint;
  arrow_paint.setColor(0xffd93f2a);
  arrow_paint.setAntiAlias(true);
  canvas.save();
  canvas.rotate(-360 * ratio);
  canvas.drawRect(SkRect::MakeXYWH(-arrow_width / 2, 0, arrow_width, r4 * 0.8),
                  arrow_paint);  // red arrow
  canvas.restore();
}

auto range_duration = 60s;

static SkPoint DurationHandlePos(const TimerDelay& timer) {
  float duration_radians = M_PI / 2 - timer.duration.count() / range_duration.count() * 2 * M_PI;
  float s = sin(duration_radians);
  float c = cos(duration_radians);
  return SkPoint::Make(c * r3, s * r3);
}

constexpr static float kStartPusherAxleWidth = 0.003;
constexpr static float kStartPusherAxleLength = 0.001;
constexpr static SkRect kStartPusherAxleBox =
    SkRect::MakeXYWH(-kStartPusherAxleWidth / 2, kOuterRadius - 0.0003, kStartPusherAxleWidth,
                     kStartPusherAxleLength + 0.0006);
static SkRRect kStartPusherAxle =
    SkRRect::MakeRectXY(kStartPusherAxleBox, kStartPusherAxleWidth / 2, 0.0002);

constexpr static float kStartPusherWidth = 0.005;
constexpr static float kStartPusherHeight = 0.004;
constexpr static SkRect kStartPusherBox =
    SkRect::MakeXYWH(-kStartPusherWidth / 2, kOuterRadius + kStartPusherAxleLength,
                     kStartPusherWidth, kStartPusherHeight);
static SkRRect kStartPusher =
    SkRRect::MakeRectXY(kStartPusherBox, kStartPusherWidth / 2, kStartPusherHeight / 12);

constexpr static float kSmallAxleWidth = 0.002;
constexpr static float kSmallAxleLength = 0.001;
constexpr static SkRect kSmallAxleBox = SkRect::MakeXYWH(
    -kSmallAxleWidth / 2, kOuterRadius - 0.0003, kSmallAxleWidth, kSmallAxleLength + 0.0006);
static SkRRect kSmallAxle = SkRRect::MakeRectXY(kSmallAxleBox, kSmallAxleWidth / 2, 0.0002);

constexpr static float kSmallPusherWidth = 0.004;
constexpr static float kSmallPusherHeight = 0.002;
constexpr static SkRect kSmallPusherBox = SkRect::MakeXYWH(
    -kSmallPusherWidth / 2, kOuterRadius + kSmallAxleLength, kSmallPusherWidth, kSmallPusherHeight);
static SkRRect kSmallPusher = SkRRect::MakeRectXY(kSmallPusherBox, 0.0002, 0.0002);

static void DrawPusher(SkCanvas& canvas, const SkRRect& axle, const SkRRect& pusher,
                       SkColor pusher_gradient[3]) {
  SkPath pusher_axle_path = SkPath::RRect(axle);

  SkPaint pusher_axle_paint;
  pusher_axle_paint.setAntiAlias(true);
  SkPoint axle_pts[2] = {{axle.rect().fLeft, 0}, {axle.rect().fRight, 0}};
  SkColor axle_colors[] = {0xffb0b0b0, 0xff383739, 0xffa8a9ab, 0xff000000, 0xff4d4b4f};
  float positions[] = {0, 0.1, 0.3, 0.6, 1};
  pusher_axle_paint.setShader(
      SkGradientShader::MakeLinear(axle_pts, axle_colors, positions, 5, SkTileMode::kClamp));

  canvas.drawPath(pusher_axle_path, pusher_axle_paint);

  SkPath pusher_path = SkPath::RRect(pusher);

  SkPaint pusher_paint;
  SkPoint btn_pts[2] = {{0, 0}, {0.0004, 0}};
  SkColor btn_colors[] = {0xff707070, 0xff303030, 0xff000000};
  float btn_positions[] = {0, 0.3, 1};
  pusher_paint.setShader(
      SkGradientShader::MakeLinear(btn_pts, btn_colors, btn_positions, 3, SkTileMode::kMirror));
  pusher_paint.setAntiAlias(true);
  canvas.drawPath(pusher_path, pusher_paint);

  SkPoint btn_pts2[2] = {{pusher.rect().fLeft, 0}, {pusher.rect().fRight, 0}};

  pusher_paint.setShader(
      SkGradientShader::MakeLinear(btn_pts2, pusher_gradient, nullptr, 3, SkTileMode::kClamp));
  canvas.drawPath(pusher_path, pusher_paint);
}

void TimerDelay::Draw(gui::DrawContext& ctx) const {
  auto& canvas = ctx.canvas;

  int range_max = 60;
  int tick_count = 60;
  int major_tick_count = tick_count / 5;

  DrawRing(canvas, r4, r5, 0xffcfd0cf, 0xffc9c9cb);  // white watch face

  // Draw duration
  float duration_angle = -duration.count() / range_duration.count() * 360;
  SkPaint duration_paint;
  duration_paint.setColor(0xff23a9f2);
  duration_paint.setAntiAlias(true);
  canvas.drawArc(SkRect::MakeXYWH(-r4, -r4, r4 * 2, r4 * 2), 90, duration_angle, true,
                 duration_paint);

  DrawRing(canvas, r4, r4_b, 0x46000000, 0xe1ffffff, kRingInset);  // shadow over white watch face

  canvas.save();
  canvas.translate(0.001, -0.001);
  DrawRing(canvas, r5, 0, 0xff46464d, 0xff46464d, kRingBlurred);  // black pin shadow
  canvas.restore();

  // Draw ticks
  SkPaint tick_paint;
  tick_paint.setColor(0xff121215);
  tick_paint.setAntiAlias(true);
  float major_tick_w = 0.0005;
  SkRect major_tick = SkRect::MakeXYWH(-major_tick_w / 2, r4 * 0.9, major_tick_w, r4 * 0.05);
  float minor_tick_w = 0.0002;
  SkRect minor_tick = SkRect::MakeXYWH(-minor_tick_w / 2, r4 * 0.925, minor_tick_w, r4 * 0.025);
  static auto font = gui::Font::Make(2);
  for (int i = 1; i <= major_tick_count; ++i) {
    float a = (float)i / major_tick_count;
    canvas.save();
    canvas.rotate(360.f * a);
    canvas.drawRect(major_tick, tick_paint);
    canvas.restore();
    auto text = f("%d", i * range_max / major_tick_count);
    canvas.save();
    float text_r = r4 * 0.8;
    float s = sin(a * M_PI * 2);
    float w = font->MeasureText(text);
    canvas.translate(s * (text_r - w / 4) - w / 2, cos(a * M_PI * 2) * text_r - 0.002 / 2);
    font->DrawText(canvas, text, SkPaint());
    canvas.restore();
  }
  for (int i = 0; i < tick_count; ++i) {
    if (i * major_tick_count % tick_count == 0) {
      continue;
    }
    canvas.save();
    canvas.rotate(360.f * i / tick_count);
    canvas.drawRect(minor_tick, tick_paint);
    canvas.restore();
  }

  DrawRing(canvas, r0, r2, 0xfff6f6f0, 0xff6a6a71);              // white case
  DrawRing(canvas, r0, r1, 0xfff7f4f2, 0xff5e5f65, kRingInset);  // white case soft edge

  {  // Draw pusher
    SkColor colors1[] = {0x20ffffff, 0x15000000, 0xA0000000};
    DrawPusher(canvas, kStartPusherAxle, kStartPusher, colors1);
    canvas.save();
    canvas.rotate(45);
    SkColor colors2[] = {0xD0000000, 0x40000000, 0xD0000000};
    DrawPusher(canvas, kSmallAxle, kSmallPusher, colors2);
    canvas.rotate(-90);
    SkColor colors3[] = {0x40FFFFFF, 0x40000000, 0xFF000000};
    DrawPusher(canvas, kSmallAxle, kSmallPusher, colors3);
    canvas.restore();
  }

  DrawRing(canvas, r2, r3, 0xff878682, 0xff020302);  // black metal band outer edge
  DrawRing(canvas, r3, r4, 0xff080604, 0xffe2e2e1);  // black metal band inner edge

  Duration elapsed = state == State::RUNNING ? Clock::now() - start_time : 0s;

  DrawArrow(canvas, elapsed.count() / range_duration.count());

  DrawRing(canvas, r5, 0, 0xff25272e, 0xff0d0b0f);               // black pin fill
  DrawRing(canvas, r5, r6, 0xff7e7d7a, 0xff05070b, kRingInset);  // black pin soft outer edge

  canvas.save();
  SkPoint duration_pos = DurationHandlePos(*this);
  canvas.translate(duration_pos.x(), duration_pos.y());
  canvas.save();
  DrawRing(canvas, 0.003, 0, 0xff25272e, 0xff0d0b0f);
  SkPath duration_arrow;
  float h = 0.0035;
  float a = h * 2 / sqrt(3);
  duration_arrow.moveTo(-h * 2 / 3, 0);
  duration_arrow.lineTo(h / 3, a / 2);
  duration_arrow.lineTo(0, 0);
  duration_arrow.lineTo(h / 3, -a / 2);
  duration_arrow.close();

  canvas.rotate(90 + duration_angle);
  canvas.drawPath(duration_arrow, duration_paint);
  canvas.restore();

  DrawRing(canvas, 0.003, 0.0026, 0x7dffffff, 0x10000000, kRingInset);
  DrawRing(canvas, 0.0025, 0.0015, 0x50000000, 0x50ffffff, kRingBlurred);
  canvas.restore();
}

SkPath TimerDelay::Shape() const {
  static SkPath shape = []() {
    SkPathBuilder path_builder;
    path_builder.addOval(kOuterOval);
    path_builder.addRect(kStartPusherAxleBox);
    path_builder.addRRect(kStartPusher);
    SkPath small_pusher = SkPath::RRect(kSmallPusher);
    small_pusher.addRRect(kSmallAxle);
    small_pusher.transform(SkMatrix::RotateDeg(45));
    path_builder.addPath(small_pusher);
    small_pusher.transform(SkMatrix::RotateDeg(-90));
    path_builder.addPath(small_pusher);
    SkPath path = path_builder.detach();
    SkPath simple_path;
    if (Simplify(path, &simple_path)) {
      return simple_path;
    } else {
      return path;
    }
  }();
  return shape;
}

static std::thread timer_thread;
std::mutex mtx;
std::condition_variable cv;

std::multimap<TimePoint, std::unique_ptr<Task>> tasks;

static void TimerFinished(Location* here) {
  TimerDelay* timer = here->As<TimerDelay>();
  if (timer == nullptr) {
    return;
  }
  timer->state = TimerDelay::State::IDLE;
  if (timer->here) {
    timer->finished_arg.LoopLocations<bool>(*here, [](Location& then) {
      then.ScheduleRun();
      return false;
    });
  }
}

struct TimerFinishedTask : Task {
  TimerFinishedTask(Location* target) : Task(target) {}
  std::string Format() override { return "TimerFinishedTask"; }
  void Execute() override {
    PreExecute();
    TimerFinished(target);
    PostExecute();
  }
};

struct DragDurationHandleAction : Action {
  TimerDelay& timer;
  DragDurationHandleAction(TimerDelay& timer) : timer(timer) {}
  virtual void Begin(gui::Pointer& pointer) {}
  virtual void Update(gui::Pointer& pointer) {
    auto pos = pointer.PositionWithin(timer);
    float angle = atan2(pos.sk.y(), pos.sk.x());

    float new_duration = (1.25 - angle / (2 * M_PI));
    if (new_duration < 0) {
      new_duration += 1;
    } else if (new_duration > 1) {
      new_duration -= 1;
    }
    new_duration *= range_duration.count();
    new_duration = ceil(new_duration);

    if (timer.state == TimerDelay::State::RUNNING) {
      std::unique_lock<std::mutex> lck(mtx);
      if (timer.state == TimerDelay::State::RUNNING) {
        auto [a, b] = tasks.equal_range(timer.start_time + timer.duration);
        for (auto it = a; it != b; ++it) {
          if (it->second->target == timer.here) {
            tasks.erase(it);
            break;
          }
        }
        auto new_end = timer.start_time + Duration(new_duration);
        if (new_end <= Clock::now()) {
          TimerFinished(timer.here);
        } else {
          tasks.emplace(new_end, new TimerFinishedTask(timer.here));
        }
        cv.notify_all();
      }
    }

    timer.duration = Duration(new_duration);
  }
  virtual void End() {}
  virtual void DrawAction(gui::DrawContext&) {}
};

std::unique_ptr<Action> TimerDelay::ButtonDownAction(gui::Pointer& pointer,
                                                     gui::PointerButton btn) {
  if (btn == gui::PointerButton::kMouseLeft) {
    auto pos = pointer.PositionWithin(*this);
    auto duration_handle_pos = DurationHandlePos(*this);
    if ((duration_handle_pos - pos.sk).length() < 0.003) {
      return std::make_unique<DragDurationHandleAction>(*this);
    }
    if (kStartPusherBox.contains(pos.x, pos.y)) {
      if (here) {
        here->ScheduleRun();
        return nullptr;
      }
    }
    auto left_rot = SkMatrix::RotateDeg(-45).mapPoint(pos.sk);
    auto right_rot = SkMatrix::RotateDeg(45).mapPoint(pos.sk);
    if (kSmallPusherBox.contains(left_rot.x(), left_rot.y())) {
      LOG << "Clicked left pusher";
      return nullptr;
    }
    if (kSmallPusherBox.contains(right_rot.x(), right_rot.y())) {
      LOG << "Clicked right pusher";
      return nullptr;
    }
    auto action = std::make_unique<DragLocationAction>(here);
    action->contact_point = pos;
    return action;
  }
  return nullptr;
}

void TimerDelay::Args(std::function<void(Argument&)> cb) { cb(finished_arg); }

static void TimerThread() {
  SetThreadName("Timer");
  while (true) {
    std::unique_lock<std::mutex> lck(mtx);
    if (tasks.empty()) {
      cv.wait(lck);
    } else {
      cv.wait_until(lck, tasks.begin()->first);
    }
    auto now = Clock::now();
    while (!tasks.empty() && tasks.begin()->first <= now) {
      events.send(std::move(tasks.begin()->second));
      tasks.erase(tasks.begin());
    }
  }
}

void __attribute__((constructor)) InitTimer() {
  timer_thread = std::thread(TimerThread);
  timer_thread.detach();
}

void TimerDelay::Run(Location& here) {
  if (state == State::IDLE) {
    state = State::RUNNING;
    start_time = Clock::now();
    {
      std::unique_lock<std::mutex> lck(mtx);
      tasks.emplace(start_time + duration, new TimerFinishedTask(&here));
      cv.notify_all();
    }
  }
}

}  // namespace automat::library