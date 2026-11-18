// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_process.hpp"

#include <include/core/SkBlendMode.h>
#include <include/core/SkBlurTypes.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPathTypes.h>
#include <include/core/SkRRect.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkShader.h>
#include <include/core/SkSize.h>
#include <include/effects/SkGradient.h>
#include <include/effects/SkImageFilters.h>

#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <memory>
#include <span>

#include "animation.hpp"
#include "color.hpp"
#include "drawing.hpp"
#include "font.hpp"
#include "format.hpp"
#include "log.hpp"
#include "math.hpp"
#include "random.hpp"
#include "sincos.hpp"
#include "status.hpp"
#include "textures.hpp"
#include "units.hpp"

#if defined(_WIN32)
#include "win32.hpp"
#else
#include <sys/syscall.h>
#include <unistd.h>

#include "fd.hpp"
#include "mux.hpp"
#include "path.hpp"
#include "virtual_fs.hpp"
#endif

namespace automat::library {

#if defined(_WIN32)

struct Process::Watch {
  HANDLE process = nullptr;
  HANDLE wait = nullptr;
};

static void CALLBACK OnProcessExit(void* context, BOOLEAN) {
  auto& process = *static_cast<Process*>(context);
  process.exited.store(true, std::memory_order_relaxed);
  process.WakeToys();
}

static void WatchExit(Process& process) {
  HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)process.pid);
  if (handle == nullptr) {
    DWORD error = GetLastError();
    process.exited.store(true, std::memory_order_relaxed);
    if (error != ERROR_INVALID_PARAMETER) {
      process.ReportError(f("OpenProcess: {}", win32::ErrorStr(error)));
    }
    return;
  }
  auto* watch = new Process::Watch{.process = handle};
  if (!RegisterWaitForSingleObject(&watch->wait, handle, OnProcessExit, &process, INFINITE,
                                   WT_EXECUTEONLYONCE)) {
    process.exited.store(true, std::memory_order_relaxed);
    process.ReportError(f("RegisterWaitForSingleObject: {}", win32::GetLastErrorStr()));
    CloseHandle(handle);
    delete watch;
    return;
  }
  process.watch = watch;
}

static void CloseWatch(Process::Watch* watch) {
  UnregisterWaitEx(watch->wait, INVALID_HANDLE_VALUE);
  CloseHandle(watch->process);
  delete watch;
}

static Str StartTime(I64 pid) {
  HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
  if (handle == nullptr) return {};
  FILETIME creation, exit, kernel, user;
  bool known = GetProcessTimes(handle, &creation, &exit, &kernel, &user);
  CloseHandle(handle);
  if (!known) return {};
  ULARGE_INTEGER creation_time;
  creation_time.LowPart = creation.dwLowDateTime;
  creation_time.HighPart = creation.dwHighDateTime;
  return f("{}", creation_time.QuadPart);
}

#else

struct Process::Watch : mux::Epoll::Listener {
  WeakPtr<Process> process;

  Watch(FD pidfd, WeakPtr<Process> process)
      : Listener(std::move(pidfd)), process(std::move(process)) {}

  StrView Name() const override { return "Process"sv; }

  void NotifyRead(Status&) override {
    Status status;
    mux::epoll.Del(this, status);
    fd.Close();
    if (auto locked = process.Lock()) {
      locked->exited.store(true, std::memory_order_relaxed);
      locked->WakeToys();
    }
  }
};

static void WatchExit(Process& process) {
  int pidfd = (int)syscall(SYS_pidfd_open, (pid_t)process.pid, 0);
  if (pidfd < 0) {
    int error = errno;
    process.exited.store(true, std::memory_order_relaxed);
    if (error != ESRCH) process.ReportError(f("pidfd_open: {}", strerror(error)));
    return;
  }
  auto* watch = new Process::Watch(FD(pidfd), process.AcquireWeakPtr());
  process.watch = watch;
  mux::epoll.Post([watch] {
    Status status;
    mux::epoll.Add(watch, status);
    if (!OK(status)) ERROR << "Process watch: " << status;
  });
}

static void CloseWatch(Process::Watch* watch) {
  mux::epoll.Post([watch] {
    if (watch->fd.Opened()) {
      Status status;
      mux::epoll.Del(watch, status);
    }
    delete watch;
  });
}

static Str StartTime(I64 pid) {
  static const Str boot_id = [] {
    Status status;
    Str id = fs::real.Read("/proc/sys/kernel/random/boot_id", status);
    if (id.ends_with('\n')) id.pop_back();
    return id;
  }();
  if (boot_id.empty()) return {};
  Status status;
  Str stat = fs::real.Read(Path(f("/proc/{}/stat", pid)), status);
  size_t field = stat.rfind(')');
  if (field == Str::npos) return {};
  for (int i = 2; i < 22; ++i) {
    field = stat.find(' ', field + 1);
    if (field == Str::npos) return {};
  }
  size_t end = stat.find(' ', field + 1);
  return f("{}@{}", StrView(stat).substr(field + 1, end - field - 1), boot_id);
}

#endif

static void WatchSavedProcess(Process& process) {
  if (process.pid == 0) return;
  if (!process.start_time.empty() && StartTime(process.pid) == process.start_time) {
    WatchExit(process);
  } else {
    process.exited.store(true, std::memory_order_relaxed);
  }
}

Process::Process(I64 pid) : pid(pid) {
  WatchExit(*this);
  if (watch) start_time = StartTime(pid);
}

Process::Process(const Process& that) : Object(that), pid(that.pid), start_time(that.start_time) {
  WatchSavedProcess(*this);
}

Process::~Process() {
  if (watch) CloseWatch(watch);
}

void Process::SerializeState(ObjectSerializer& writer) const {
  writer.Key("pid");
  writer.Int64(pid);
  if (!exited.load(std::memory_order_relaxed)) {
    writer.Key("start_time");
    writer.String(start_time.data(), start_time.size());
  }
}

bool Process::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  if (key == "pid") {
    int64_t value = 0;
    d.Get(value, status);
    pid = value;
  } else if (key == "start_time") {
    d.Get(start_time, status);
  } else {
    return false;
  }
  if (!OK(status)) ReportError(status.ToStr());
  return true;
}

void Process::OnUnsuspend() { WatchSavedProcess(*this); }

constexpr Vec2 kBallCenter = {0, 2_mm};
constexpr float kBallRadius = 13_mm;
constexpr float kGlassThickness = 0.1_mm;
constexpr float kWaterRadius = kBallRadius - kGlassThickness;
constexpr Rect kBallBounds = Rect::MakeCenter(kBallCenter, 2 * kBallRadius, 2 * kBallRadius);
constexpr Rect kWaterBounds = Rect::MakeCenter(kBallCenter, 2 * kWaterRadius, 2 * kWaterRadius);

constexpr float kLensZoom = 1.5f;
constexpr float kLensInset = 10_mm;
constexpr Rect kLensBounds = Rect::MakeCenter(kBallCenter, 4 * kLensInset, 4 * kLensInset);
constexpr float kLensRimWeight = 2 - kWaterRadius / kLensInset;
constexpr float kSceneRadius =
    kWaterRadius * (1 - kLensRimWeight * kLensRimWeight * (1 - 1 / kLensZoom));
constexpr Rect kSceneBounds = Rect::MakeCenter(kBallCenter, 2 * kSceneRadius, 2 * kSceneRadius);

constexpr float kRingTop = -6_mm;
constexpr float kRingBottom = -7.5_mm;
constexpr float kRingHalfWidth = 11_mm;
constexpr float kRingCornerRadius = 0.5_mm;
constexpr float kBeadBottom = -8.7_mm;
constexpr float kBeadHalfWidth = 10.7_mm;
constexpr float kBeadRadius = 0.6_mm;
constexpr float kNeckHalfWidth = 9.9_mm;
constexpr float kFilletTop = -12.2_mm;
constexpr float kFilletBottom = -12.7_mm;
constexpr float kFilletHalfWidth = 12.1_mm;
constexpr float kFootHalfWidth = 12.9_mm;
constexpr float kFootShoulderRadius = 0.8_mm;
constexpr float kFootHeelRadius = 0.5_mm;
constexpr float kBaseBottom = -15_mm;
constexpr Rect kRingBounds = {-kRingHalfWidth, kRingBottom, kRingHalfWidth, kRingTop};
constexpr Rect kBaseBounds = {-kFootHalfWidth, kBaseBottom, kFootHalfWidth, kRingBounds.CenterY()};

constexpr float kSnowCrest = 0.6_mm;
constexpr float kSnowDrop = 5_mm;
constexpr float kDigitsBaseline = -1.15_mm;
constexpr float kDigitsHeight = 5_mm;
constexpr float kDigitsMaxWidth = 12.2_mm;

constexpr int kFlakeCount = 40;
constexpr int kBackFlakeCount = 24;
constexpr float kFlakeTimeConstant = 0.35f;
constexpr float kSwayAmplitude = 2.5_mm;
constexpr float kSwayRate = 1.7f;
constexpr float kSwirl = 1.4f;
constexpr float kMaxImpulse = 45_mm;
constexpr float kLiquidLag = 0.3f;
constexpr float kMaxStep = 1 / 30.f;

constexpr SkColor4f kSkyTopLit = "#3f77a6"_color4f;
constexpr SkColor4f kSkyBottomLit = "#9cc5e0"_color4f;
constexpr SkColor4f kSkyTopDim = "#8b949b"_color4f;
constexpr SkColor4f kSkyBottomDim = "#a9b0b5"_color4f;
constexpr SkColor4f kSnowCrestLit = "#ffffff"_color4f;
constexpr SkColor4f kSnowFootLit = "#b7cde2"_color4f;
constexpr SkColor4f kSnowCrestDim = "#d3d7da"_color4f;
constexpr SkColor4f kSnowFootDim = "#b4b9bd"_color4f;
constexpr SkColor4f kDigitsTopLit = "#2c4a63"_color4f;
constexpr SkColor4f kDigitsBottomLit = "#162736"_color4f;
constexpr SkColor4f kDigitsTopDim = "#5d656c"_color4f;
constexpr SkColor4f kDigitsBottomDim = "#484f55"_color4f;
constexpr SkColor4f kFlakeLit = "#ffffff"_color4f;
constexpr SkColor4f kFlakeDim = "#e3e6e8"_color4f;
constexpr SkColor4f kGlassTint = "#0b3444"_color4f;
constexpr SkColor4f kGlassRim = "#06202c"_color4f;
constexpr SkColor4f kCoolLight = "#d7ecff"_color4f;
constexpr SkColor4f kWarmLight = "#fff3e0"_color4f;
constexpr SkColor4f kReflectedLight = "#e0c0ac"_color4f;
constexpr SkColor4f kBrassReflection = "#e3b055"_color4f;
constexpr SkColor4f kBrassShade = "#2a1200"_color4f;

static float BankHeight(float x) {
  float t = x / kSceneRadius;
  return kSnowCrest - kSnowDrop * t * t;
}

static float FallSpeed(float flake_radius) {
  return 5_mm + 4_mm * (flake_radius - 0.22_mm) / 0.28_mm;
}

static ui::Font& DigitsFont() {
  static std::unique_ptr<ui::Font> font =
      ui::Font::MakeV2(ui::Font::MakeWeightVariation(ui::Font::GetNotoSans(), 800), kDigitsHeight);
  return *font;
}

static void DrawSky(SkCanvas& canvas, float light) {
  SkPoint pts[] = {kSceneBounds.TopCenter(), Vec2(0, kSnowCrest)};
  SkColor4f colors[] = {color::MixColors(kSkyTopDim, kSkyTopLit, light),
                        color::MixColors(kSkyBottomDim, kSkyBottomLit, light)};
  SkPaint paint;
  paint.setShader(SkShaders::LinearGradient(
      pts, SkGradient{SkGradient::Colors{colors, SkTileMode::kClamp}, {}}));
  canvas.drawPaint(paint);
}

struct Flake {
  Vec2 position;
  Vec2 velocity;
  float radius;
  SinCos sway;
  bool resting = true;
};

static void DrawFlakes(SkCanvas& canvas, std::span<const Flake> flakes, float scale, float alpha,
                       float light) {
  SkPaint paint;
  paint.setAntiAlias(true);
  SkColor4f color = color::MixColors(kFlakeDim, kFlakeLit, light);
  color.fA = alpha;
  paint.setColor4f(color);
  for (auto& flake : flakes) {
    if (flake.resting) continue;
    canvas.drawCircle(flake.position, flake.radius * scale, paint);
  }
}

static void DrawBank(SkCanvas& canvas, float light) {
  static const SkPath bank = [] {
    SkPathBuilder path;
    path.moveTo(-kSceneRadius, kSnowCrest - kSnowDrop);
    path.quadTo({0, kSnowCrest + kSnowDrop}, {kSceneRadius, kSnowCrest - kSnowDrop});
    path.lineTo(kSceneBounds.BottomRightCorner());
    path.lineTo(kSceneBounds.BottomLeftCorner());
    path.close();
    return path.detach();
  }();
  SkPoint pts[] = {Vec2(-2_mm, kSnowCrest), Vec2(2_mm, kSnowCrest - kSnowDrop)};
  SkColor4f colors[] = {color::MixColors(kSnowCrestDim, kSnowCrestLit, light),
                        color::MixColors(kSnowFootDim, kSnowFootLit, light)};
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setShader(SkShaders::LinearGradient(
      pts, SkGradient{SkGradient::Colors{colors, SkTileMode::kClamp}, {}}));
  canvas.drawPath(bank, paint);
}

static void DrawDigits(SkCanvas& canvas, StrView digits, float width, float light) {
  if (digits.empty()) return;
  auto& font = DigitsFont();
  float scale = std::min(1.f, kDigitsMaxWidth / width);
  SkColor4f bottom = color::MixColors(kDigitsBottomDim, kDigitsBottomLit, light);
  SkColor4f top = color::MixColors(kDigitsTopDim, kDigitsTopLit, light);

  Rect shadow_bounds =
      Rect::MakeCenter({0.25_mm, kDigitsBaseline - 0.05_mm}, width * scale + 1_mm, 0.9_mm);
  SkColor4f shadow_colors[] = {bottom.withAlpha(0.4f), bottom.withAlpha(0)};
  SkMatrix shadow_matrix = SkMatrix::RectToRect(Rect::MakeCircleR(1), shadow_bounds);
  SkPaint shadow;
  shadow.setAntiAlias(true);
  shadow.setShader(SkShaders::RadialGradient(
      {0, 0}, 1, SkGradient{SkGradient::Colors{shadow_colors, SkTileMode::kClamp}, {}},
      &shadow_matrix));
  canvas.drawOval(shadow_bounds, shadow);

  SkPoint pts[] = {{0, 0}, {0, -kDigitsHeight / font.font_scale}};
  SkColor4f colors[] = {bottom, top};
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setShader(SkShaders::LinearGradient(
      pts, SkGradient{SkGradient::Colors{colors, SkTileMode::kClamp}, {}}));
  canvas.save();
  canvas.translate(-width * scale / 2, kDigitsBaseline);
  canvas.scale(scale, scale);
  font.DrawText(canvas, digits, paint);
  canvas.restore();
}

static void DrawContents(SkCanvas& canvas, std::span<const Flake> flakes, StrView digits,
                         float digits_width, float light) {
  static const SkMatrix ball_to_layer =
      SkMatrix().setScale(kLensZoom, kLensZoom, kBallCenter.x, kBallCenter.y);
  static const SkMatrix layer_to_ball =
      SkMatrix().setScale(1 / kLensZoom, 1 / kLensZoom, kBallCenter.x, kBallCenter.y);
  static const SkPaint lens = [] {
    SkSamplingOptions sampling(SkFilterMode::kLinear);
    SkPaint paint;
    paint.setImageFilter(SkImageFilters::MatrixTransform(
        layer_to_ball, sampling,
        SkImageFilters::Magnifier(kLensBounds.sk, kLensZoom, kLensInset, sampling, nullptr)));
    return paint;
  }();
  canvas.save();
  canvas.clipRRect(SkRRect::MakeOval(kWaterBounds.sk), true);
  canvas.concat(ball_to_layer);
  canvas.saveLayer(&kSceneBounds.sk, &lens);
  DrawSky(canvas, light);
  DrawFlakes(canvas, flakes.first(kBackFlakeCount), 0.55f, 0.7f, light);
  DrawBank(canvas, light);
  DrawDigits(canvas, digits, digits_width, light);
  DrawFlakes(canvas, flakes.subspan(kBackFlakeCount), 0.7f, 0.95f, light);
  canvas.restore();
  canvas.restore();
}

struct ProcessToy : ObjectToy {
  Str digits;
  float digits_width = 0;
  float light = 1;
  XorShift32 rng;
  std::array<Flake, kFlakeCount> flakes;
  Vec2 last_position;
  Vec2 last_velocity;

  ProcessToy(ui::Widget* parent, Process& process) : ObjectToy(parent, process) {
    if (process.pid) {
      digits = f("{}", process.pid);
      digits_width = DigitsFont().MeasureText(digits);
    }
    light = process.exited.load(std::memory_order_relaxed) ? 0 : 1;
    rng.state = (U32)process.pid * 2654435761u | 1;
    for (auto& flake : flakes) {
      flake.radius = rng.RollFloat(0.22_mm, 0.5_mm);
      flake.position.x = rng.RollFloat(-0.8f, 0.8f) * kSceneRadius;
      flake.position.y = BankHeight(flake.position.x);
      flake.sway = SinCos::FromDegrees(rng.RollFloat(0, 360));
    }
  }

  bool CenteredAtZero() const override { return true; }

  SkPath Shape() const override {
    static const SkPath shape = [] {
      float junction_height = kRingTop - kBallCenter.y;
      float junction_x = sqrtf(kBallRadius * kBallRadius - junction_height * junction_height);
      SkPathBuilder outline;
      outline.moveTo(junction_x, kRingTop);
      outline.arcTo({kBallRadius, kBallRadius}, 0, SkPathBuilder::kLarge_ArcSize,
                    SkPathDirection::kCW, {-junction_x, kRingTop});
      outline.arcTo({-kRingHalfWidth, kRingTop}, {-kRingHalfWidth, kRingBottom}, kRingCornerRadius);
      outline.arcTo({-kRingHalfWidth, kRingBottom}, {0, kRingBottom}, kRingCornerRadius);
      outline.arcTo({-kBeadHalfWidth, kRingBottom}, {-kBeadHalfWidth, kBeadBottom}, kBeadRadius);
      outline.arcTo({-kBeadHalfWidth, kBeadBottom}, {0, kBeadBottom}, kBeadRadius);
      outline.lineTo(-kNeckHalfWidth, kBeadBottom);
      outline.quadTo({-kNeckHalfWidth, kFilletTop}, {-kFilletHalfWidth, kFilletTop});
      outline.lineTo(-kFilletHalfWidth, kFilletBottom);
      outline.arcTo({-kFootHalfWidth, kFilletBottom}, {-kFootHalfWidth, kBaseBottom},
                    kFootShoulderRadius);
      outline.arcTo({-kFootHalfWidth, kBaseBottom}, {0, kBaseBottom}, kFootHeelRadius);
      outline.arcTo({kFootHalfWidth, kBaseBottom}, {kFootHalfWidth, kFilletBottom},
                    kFootHeelRadius);
      outline.arcTo({kFootHalfWidth, kFilletBottom}, {0, kFilletBottom}, kFootShoulderRadius);
      outline.lineTo(kFilletHalfWidth, kFilletBottom);
      outline.lineTo(kFilletHalfWidth, kFilletTop);
      outline.quadTo({kNeckHalfWidth, kFilletTop}, {kNeckHalfWidth, kBeadBottom});
      outline.arcTo({kBeadHalfWidth, kBeadBottom}, {kBeadHalfWidth, kRingBottom}, kBeadRadius);
      outline.arcTo({kBeadHalfWidth, kRingBottom}, {0, kRingBottom}, kBeadRadius);
      outline.arcTo({kRingHalfWidth, kRingBottom}, {kRingHalfWidth, kRingTop}, kRingCornerRadius);
      outline.arcTo({kRingHalfWidth, kRingTop}, {0, kRingTop}, kRingCornerRadius);
      outline.close();
      return outline.detach();
    }();
    return shape;
  }

  Vec2 Position() const { return Vec2(local_to_parent.rc(0, 3), local_to_parent.rc(1, 3)); }

  void TransformUpdated(time::Timer& timer) override {
    if (Position() != last_position) WakeAnimationAt(timer.now);
  }

  void StirSnow() {
    float swirl = rng.Roll() & 1 ? kSwirl : -kSwirl;
    for (auto& flake : flakes) {
      if (flake.resting) {
        flake.resting = false;
        flake.velocity = {rng.RollFloat(-12_mm, 12_mm), rng.RollFloat(14_mm, 34_mm)};
      }
      flake.velocity += Rotate90DegreesCounterClockwise(flake.position - kBallCenter) * swirl;
    }
  }

  void ShakeSnow(Vec2 impulse) {
    float strength = Length(impulse) / kMaxImpulse;
    for (auto& flake : flakes) {
      if (!flake.resting) {
        flake.velocity += impulse * kLiquidLag;
      } else if (rng.RollFloat(0, 1) < strength) {
        flake.resting = false;
        flake.velocity =
            impulse * kLiquidLag + Vec2(0, Length(impulse) * rng.RollFloat(0.6f, 1.2f));
      }
    }
  }

  bool MoveFlakes(float d) {
    bool moved = false;
    float approach = std::min(1.f, d / kFlakeTimeConstant);
    for (auto& flake : flakes) {
      if (flake.resting) continue;
      moved = true;
      flake.sway = flake.sway + SinCos::FromRadians(d * kSwayRate);
      Vec2 terminal = {kSwayAmplitude * (float)flake.sway.sin, -FallSpeed(flake.radius)};
      flake.velocity += (terminal - flake.velocity) * approach;
      flake.position += flake.velocity * d;
      Vec2 offset = flake.position - kBallCenter;
      float limit = kSceneRadius - flake.radius;
      if (LengthSquared(offset) > limit * limit) {
        Vec2 normal = Normalize(offset);
        flake.position = kBallCenter + normal * limit;
        flake.velocity -= normal * std::max(0.f, Dot(flake.velocity, normal));
      }
      float bank = BankHeight(flake.position.x);
      if (flake.position.y <= bank && flake.velocity.y <= 0) {
        flake.position.y = bank;
        flake.resting = true;
      }
    }
    return moved;
  }

  Tock Tick(time::Timer& timer) override {
    float d = std::min<float>(timer.d, kMaxStep);
    bool exited = true;
    if (auto process = LockObject<Process>()) {
      exited = process->exited.load(std::memory_order_relaxed);
    }
    Vec2 position = Position();
    if (last_tick == time::SteadyPoint::min()) {
      last_position = position;
      if (!digits.empty() && !exited) StirSnow();
    }
    bool moved = position != last_position;
    Vec2 velocity = timer.d > 0 ? (position - last_position) / timer.d : last_velocity;
    ShakeSnow(LimitLength(last_velocity - velocity, kMaxImpulse));
    last_position = position;
    last_velocity = velocity;

    Tock tock;
    tock.ing |= moved;
    tock.drawing |= animation::LinearApproach(exited ? 0 : 1, d, 2.5f, light);
    tock.drawing |= MoveFlakes(d);
    return tock;
  }

  static inline RasterPatch glass_patch;
  static inline RasterPatch shadow_patch;
  static inline RasterPatch gloss_patch;
  static inline RasterPatch ring_patch;

  static void DrawGlass(SkCanvas& canvas) {
    glass_patch.DrawCached(canvas, kBallBounds, SkISize(192, 192), [](SkCanvas& canvas) {
      SkColor4f tint_colors[] = {kGlassTint.withAlpha(0), kGlassTint.withAlpha(0),
                                 kGlassTint.withAlpha(0.22f), kGlassRim.withAlpha(0.75f)};
      float tint_positions[] = {0, 0.7f, 0.9f, 1};
      SkPaint tint;
      tint.setShader(SkShaders::RadialGradient(
          kBallCenter + Vec2(-0.8_mm, 0.8_mm), kBallRadius + 1.2_mm,
          SkGradient{SkGradient::Colors{tint_colors, tint_positions, SkTileMode::kClamp}, {}}));
      canvas.drawPaint(tint);

      SkPaint dome_reflection;
      dome_reflection.setStyle(SkPaint::kStroke_Style);
      dome_reflection.setStrokeWidth(3.6_mm);
      dome_reflection.setStrokeCap(SkPaint::kRound_Cap);
      dome_reflection.setColor4f(SkColors::kWhite.withAlpha(0.19f));
      dome_reflection.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 1.2_mm));
      canvas.drawArc(kBallBounds.Inset(3.2_mm), 38, 62, false, dome_reflection);

      SkPaint highlight_glow;
      highlight_glow.setStyle(SkPaint::kStroke_Style);
      highlight_glow.setStrokeWidth(2.8_mm);
      highlight_glow.setStrokeCap(SkPaint::kRound_Cap);
      highlight_glow.setColor4f(SkColors::kWhite.withAlpha(0.28f));
      highlight_glow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.7_mm));
      canvas.drawArc(kBallBounds.Inset(2.8_mm), 108, 54, false, highlight_glow);

      SkColor4f highlight_colors[] = {SkColors::kWhite.withAlpha(0),
                                      SkColors::kWhite.withAlpha(0.5f), SkColors::kWhite};
      float highlight_positions[] = {0.76f, 0.9f, 0.98f};
      SkPaint highlight;
      highlight.setStyle(SkPaint::kStroke_Style);
      highlight.setStrokeWidth(2.6_mm);
      highlight.setStrokeCap(SkPaint::kRound_Cap);
      highlight.setShader(SkShaders::RadialGradient(
          kBallCenter, kBallRadius - 1.4_mm,
          SkGradient{SkGradient::Colors{highlight_colors, highlight_positions, SkTileMode::kClamp},
                     {}}));
      highlight.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.26_mm));
      canvas.drawArc(kBallBounds.Inset(2.7_mm), 120, 32, false, highlight);

      SkPaint counter_highlight;
      counter_highlight.setStyle(SkPaint::kStroke_Style);
      counter_highlight.setStrokeWidth(0.9_mm);
      counter_highlight.setStrokeCap(SkPaint::kRound_Cap);
      counter_highlight.setColor4f(kCoolLight.withAlpha(0.45f));
      counter_highlight.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.4_mm));
      canvas.drawArc(kBallBounds.Inset(1.5_mm), -42, 47, false, counter_highlight);

      SkPaint ring_reflection;
      ring_reflection.setColor4f(kBrassReflection.withAlpha(0.4f));
      ring_reflection.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.45_mm));
      canvas.drawRect(Rect{-8_mm, kRingTop - 0.3_mm, 8_mm, kRingTop + 0.7_mm}, ring_reflection);

      Rect contact_bounds = {-kBallRadius, kRingTop, kBallRadius, kRingTop + 1.1_mm};
      SkPoint contact_pts[] = {contact_bounds.BottomCenter(), contact_bounds.TopCenter()};
      SkColor4f contact_colors[] = {kGlassRim.withAlpha(0.4f), kGlassRim.withAlpha(0)};
      SkPaint contact_shadow;
      contact_shadow.setShader(SkShaders::LinearGradient(
          contact_pts, SkGradient{SkGradient::Colors{contact_colors, SkTileMode::kClamp}, {}}));
      canvas.drawRect(contact_bounds, contact_shadow);

      SkPaint caustic;
      caustic.setColor4f(SkColors::kWhite.withAlpha(0.6f));
      caustic.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.7_mm));
      canvas.drawOval(Rect::MakeCenter({4.8_mm, -4_mm}, 7.5_mm, 1.5_mm), caustic);

      return SkPath::Circle(kBallCenter.x, kBallCenter.y, kBallRadius);
    });

    static const SkPaint rim = [] {
      SkColor4f colors[] = {kGlassRim.withAlpha(0.6f),  kGlassRim.withAlpha(0.4f),
                            kGlassRim.withAlpha(0.25f), kGlassRim.withAlpha(0.4f),
                            kGlassRim.withAlpha(0.8f),  kGlassRim.withAlpha(0.9f),
                            kGlassRim.withAlpha(0.6f)};
      float positions[] = {0, 0.25f, 0.375f, 0.5f, 0.75f, 0.875f, 1};
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setStyle(SkPaint::kStroke_Style);
      paint.setStrokeWidth(2 * kGlassThickness);
      paint.setShader(SkShaders::SweepGradient(
          kBallCenter, SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}}));
      return paint;
    }();
    canvas.drawCircle(kBallCenter, kWaterRadius, rim);

    static const SkPaint rim_light = [] {
      SkColor4f colors[] = {SkColors::kWhite.withAlpha(0.4f), SkColors::kWhite.withAlpha(0),
                            SkColors::kWhite.withAlpha(0), SkColors::kWhite.withAlpha(0.55f),
                            SkColors::kWhite.withAlpha(0.4f)};
      float positions[] = {0, 0.06f, 0.84f, 0.94f, 1};
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setStyle(SkPaint::kStroke_Style);
      paint.setStrokeWidth(0.14_mm);
      paint.setShader(SkShaders::SweepGradient(
          kBallCenter, SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}}));
      return paint;
    }();
    canvas.drawCircle(kBallCenter, kBallRadius - 0.34_mm, rim_light);
  }

  static void DrawBase(SkCanvas& canvas, const SkPath& outline) {
    static const SkPaint doubled_wood = [] {
      sk_sp<SkShader> wood = textures::RosewoodColor().paint.getShader()->makeWithLocalMatrix(
          SkMatrix::Scale(0.8f, 0.3f).preRotate(90));
      SkPaint paint;
      paint.setShader(SkShaders::Blend(SkBlendMode::kPlus, wood, wood));
      return paint;
    }();
    canvas.save();
    canvas.clipPath(outline, true);
    canvas.clipRect(kBaseBounds, true);
    canvas.drawPaint(doubled_wood);

    shadow_patch.DrawCached(canvas, kBaseBounds, SkISize(208, 66), [&](SkCanvas& canvas) {
      struct {
        float height;
        float shadow;
      } stations[] = {
          {kRingBottom, 0.63f},
          {kRingBottom - 0.12_mm, 0.39f},
          {kRingBottom - 0.3_mm, 0.14f},
          {kRingBottom - 0.55_mm, 0.29f},
          {kBeadBottom + 0.25_mm, 0.49f},
          {kBeadBottom, 0.68f},
          {kBeadBottom - 0.15_mm, 0.58f},
          {kBeadBottom - 0.9_mm, 0.41f},
          {kBeadBottom - 1.7_mm, 0.33f},
          {kFilletTop + 0.6_mm, 0.26f},
          {kFilletTop + 0.05_mm, 0.18f},
          {kFilletTop - 0.05_mm, 0.33f},
          {kFilletBottom + 0.05_mm, 0.47f},
          {kFilletBottom - 0.05_mm, 0.16f},
          {kFilletBottom - 0.5_mm, 0.27f},
          {kFilletBottom - 1.1_mm, 0.37f},
          {kBaseBottom + 0.5_mm, 0.54f},
          {kBaseBottom, 0.71f},
      };
      SkColor4f profile_colors[std::size(stations)];
      float profile_positions[std::size(stations)];
      for (int i = 0; i < std::size(stations); ++i) {
        profile_colors[i] = SkColors::kBlack.withAlpha(stations[i].shadow);
        profile_positions[i] = (kRingBottom - stations[i].height) / (kRingBottom - kBaseBottom);
      }
      SkPoint profile_pts[] = {Vec2(0, kRingBottom), Vec2(0, kBaseBottom)};
      SkPaint profile;
      profile.setShader(SkShaders::LinearGradient(
          profile_pts,
          SkGradient{SkGradient::Colors{profile_colors, profile_positions, SkTileMode::kClamp},
                     {}}));
      canvas.drawPaint(profile);

      constexpr float kShadowSideBlur = 8_mm;
      Rect shadow_side = kBaseBounds.Outset(4 * kShadowSideBlur);
      shadow_side.left = -2.5_mm;
      SkPaint shadow_side_profile = profile;
      shadow_side_profile.setMaskFilter(
          SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, kShadowSideBlur));
      canvas.drawRect(shadow_side, shadow_side_profile);

      SkPoint roll_off_pts[] = {kBaseBounds.LeftCenter(), kBaseBounds.RightCenter()};
      SkColor4f roll_off_colors[] = {
          SkColors::kBlack.withAlpha(0.53f), SkColors::kBlack.withAlpha(0.18f),
          SkColors::kBlack.withAlpha(0),     SkColors::kBlack.withAlpha(0),
          SkColors::kBlack.withAlpha(0.19f), SkColors::kBlack.withAlpha(0.36f),
          SkColors::kBlack.withAlpha(0.29f)};
      float roll_off_positions[] = {0, 0.1f, 0.2f, 0.56f, 0.7f, 0.88f, 1};
      SkPaint roll_off;
      roll_off.setShader(SkShaders::LinearGradient(
          roll_off_pts,
          SkGradient{SkGradient::Colors{roll_off_colors, roll_off_positions, SkTileMode::kClamp},
                     {}}));
      canvas.drawPaint(roll_off);

      SkPaint edge_shadow;
      edge_shadow.setColor4f(SkColors::kBlack.withAlpha(0.3f));
      edge_shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.7_mm));
      canvas.translate(-0.6_mm, 0);
      canvas.drawPath(outline.makeToggleInverseFillType(), edge_shadow);
      return SkPath();
    });

    gloss_patch.DrawCached(canvas, kBaseBounds, SkISize(208, 66), [&](SkCanvas& canvas) {
      struct {
        Rect bounds;
        float alpha;
      } lobes[] = {
          {Rect::MakeCenter({-5.4_mm, kRingBottom - 0.42_mm}, 6.8_mm, 0.84_mm), 0.6f},
          {Rect::MakeCenter({-5.2_mm, kBeadBottom - 2.2_mm}, 4.4_mm, 3_mm), 0.16f},
          {Rect::MakeCenter({-5.6_mm, (kFilletTop + kFilletBottom) / 2}, 6_mm, 0.5_mm), 0.3f},
          {Rect::MakeCenter({-5.8_mm, kFilletBottom - 0.6_mm}, 8_mm, 1.3_mm), 0.55f},
      };
      SkColor4f lobe_colors[] = {kWarmLight, kWarmLight.withAlpha(0)};
      for (auto& lobe : lobes) {
        SkMatrix lobe_matrix = SkMatrix::RectToRect(Rect::MakeCircleR(1), lobe.bounds);
        SkPaint paint;
        paint.setShader(SkShaders::RadialGradient(
            {0, 0}, 1, SkGradient{SkGradient::Colors{lobe_colors, SkTileMode::kClamp}, {}},
            &lobe_matrix));
        paint.setAlphaf(lobe.alpha);
        canvas.drawOval(lobe.bounds, paint);
      }

      SkPoint reflected_pts[] = {Vec2(0, kRingBottom), Vec2(0, kBaseBottom)};
      SkColor4f reflected_colors[] = {
          kReflectedLight.withAlpha(0.14f), kReflectedLight.withAlpha(0.28f),
          kReflectedLight.withAlpha(0.28f), kReflectedLight.withAlpha(0)};
      float reflected_positions[] = {0, 0.2f, 0.8f, 0.95f};
      SkPaint reflected_light;
      reflected_light.setShader(SkShaders::LinearGradient(
          reflected_pts,
          SkGradient{SkGradient::Colors{reflected_colors, reflected_positions, SkTileMode::kClamp},
                     {}}));
      reflected_light.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.35_mm));
      canvas.clipRect(Rect{kNeckHalfWidth - 1.5_mm, kBaseBottom, kFootHalfWidth, kRingBottom});
      canvas.translate(-0.25_mm, 0);
      canvas.drawPath(outline.makeToggleInverseFillType(), reflected_light);
      return SkPath();
    });

    constexpr float kGlintLeft = -10_mm;
    constexpr float kGlintRight = 2_mm;
    static const SkPaint glint = [] {
      SkPoint pts[] = {Vec2(kGlintLeft, 0), Vec2(kGlintRight, 0)};
      SkColor4f colors[] = {kWarmLight.withAlpha(0), kWarmLight.withAlpha(0.7f),
                            kWarmLight.withAlpha(0)};
      float positions[] = {0, 0.38f, 1};
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setStyle(SkPaint::kStroke_Style);
      paint.setStrokeWidth(0.12_mm);
      paint.setShader(SkShaders::LinearGradient(
          pts, SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}}));
      return paint;
    }();
    for (float height : {kRingBottom - 0.22_mm, kFilletTop, kFilletBottom - 0.12_mm}) {
      canvas.drawLine(kGlintLeft, height, kGlintRight, height, glint);
    }
    canvas.restore();
  }

  static void DrawRing(SkCanvas& canvas, const SkPath& outline) {
    canvas.save();
    canvas.clipPath(outline, true);
    canvas.clipRect(kRingBounds, true);
    ring_patch.DrawCached(canvas, kRingBounds, SkISize(176, 16), [](SkCanvas& canvas) {
      SkPoint band_pts[] = {kRingBounds.LeftCenter(), kRingBounds.RightCenter()};
      SkColor4f band_colors[] = {
          "#2e1f06"_color4f, "#8a6622"_color4f, "#c79a3c"_color4f, "#fff6cf"_color4f,
          "#e4bd62"_color4f, "#b48a34"_color4f, "#6b4c14"_color4f, "#8a6622"_color4f,
          "#d9b25a"_color4f, "#5c400f"_color4f, "#2a1c05"_color4f,
      };
      float band_positions[] = {0, 0.05f, 0.2f, 0.29f, 0.36f, 0.5f, 0.66f, 0.76f, 0.9f, 0.96f, 1};
      SkPaint bands;
      bands.setShader(SkShaders::LinearGradient(
          band_pts,
          SkGradient{SkGradient::Colors{band_colors, band_positions, SkTileMode::kClamp}, {}}));
      canvas.drawPaint(bands);

      SkPoint profile_pts[] = {kRingBounds.TopCenter(), kRingBounds.BottomCenter()};
      SkColor4f profile_colors[] = {kBrassShade.withAlpha(0.6f),  kCoolLight.withAlpha(0.7f),
                                    kCoolLight.withAlpha(0.3f),   kCoolLight.withAlpha(0),
                                    kBrassShade.withAlpha(0.45f), kBrassShade.withAlpha(0.25f),
                                    kBrassShade.withAlpha(0.65f), kBrassShade.withAlpha(0.85f)};
      float profile_positions[] = {0, 0.1f, 0.3f, 0.44f, 0.52f, 0.62f, 0.85f, 1};
      SkPaint profile;
      profile.setShader(SkShaders::LinearGradient(
          profile_pts,
          SkGradient{SkGradient::Colors{profile_colors, profile_positions, SkTileMode::kClamp},
                     {}}));
      canvas.drawPaint(profile);
      return SkPath();
    });

    static const SkPaint seam = [] {
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setStyle(SkPaint::kStroke_Style);
      paint.setStrokeWidth(0.12_mm);
      paint.setColor4f(kBrassShade.withAlpha(0.6f));
      return paint;
    }();
    canvas.drawLine(kRingBounds.TopLeftCorner(), kRingBounds.TopRightCorner(), seam);
    canvas.drawLine(kRingBounds.BottomLeftCorner(), kRingBounds.BottomRightCorner(), seam);
    canvas.restore();
  }

  void Draw(SkCanvas& canvas) const override {
    DrawContents(canvas, flakes, digits, digits_width, light);
    DrawGlass(canvas);
    DrawBase(canvas, shape);
    DrawRing(canvas, shape);
  }
};

std::unique_ptr<ObjectToy> Process::MakeToy(ui::Widget* parent) {
  return std::make_unique<ProcessToy>(parent, *this);
}

}  // namespace automat::library
