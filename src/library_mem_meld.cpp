// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_mem_meld.hpp"

#include <include/core/SkBlendMode.h>
#include <include/core/SkBlurTypes.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkM44.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPathTypes.h>
#include <include/core/SkShader.h>
#include <include/core/SkSize.h>
#include <include/core/SkTileMode.h>
#include <include/core/SkVertices.h>
#include <include/effects/SkGradient.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ranges>
#include <thread>

#include "animation.hpp"
#include "argument.hpp"
#include "casting.hpp"
#include "color.hpp"
#include "drawing.hpp"
#include "fd.hpp"
#include "format.hpp"
#include "library_process.hpp"
#include "location.hpp"
#include "math.hpp"
#include "mem_meld.hpp"
#include "memory_landmarks.hpp"
#include "mortal.hpp"
#include "object.hpp"
#include "random.hpp"
#include "raycast.hpp"
#include "sincos.hpp"
#include "status.hpp"
#include "thread_name.hpp"
#include "time.hpp"
#include "toy.hpp"
#include "ui_constants.hpp"
#include "units.hpp"
#include "widget.hpp"

namespace automat {
Str ToStr(const MemMeld::Region& reg) {
  return f("{}@{}[{}{}{}]+{} at {}", reg.path.empty() ? "Region"sv : reg.path, (void*)reg.remote,
           reg.prot & PROT_READ ? 'R' : '-', reg.prot & PROT_WRITE ? 'W' : '-',
           reg.prot & PROT_EXEC ? 'X' : '-', reg.Size(), (void*)reg.local.data());
}
}  // namespace automat

namespace automat::library {

// Idea: Decompose the memory visualization into more, reusable objects:
// 0. Object that represents another process
// 1. Object that maps memory of another process (that's MemMeld!)
// 2. Object that  selects a sub-section of another object
// 3. Object that scans memory for interesting regions
// Opens possibilities of extra composable objects:
// 1b. Core dump
// 1c. Local memory
// 1d. ELF file
// 1e. Regular ptrace-based attach
// 2b. Selects some file
// 2c. Follows some pointer + size
// 3b. Interprets memory as a specific data type

// How "1-type" objects will work:
// - will expose a "Refreshed" notification / command - (no-op for MemMeld) that notifies that
// memory contents have been refreshed
// - will show just the memory ranges as a simple (maybe color-coded) line
// - will allow dropping for "Crop" objects

// "Crop" object:
// - can be dropped on another toy to select a sub-range of it
//
// Problem: "Crop" object needs to ask the underlying "Memory" about the range described by some
// rect. Shape of objects is NOT part of the VM - this must happen in the UI. So Toys need to be
// able to talk with each other. So the "croppable" interface must be defined on the Toy.
//
// Design A: reuse the drag&drop infrastructure and create a "Crop" object that can be "dropped
//                 into" other objects and then moved around to select their sub-sections. Clones of
//                 the Crop object will create a toy for the original object parametrized by the
//                 cropped section. This way the "Crop" object doesn't need to care about the type
//                 of the object it's dropped onto. The "embedded" clone of Crop will be positioned
//                 by the parent toy so if position is changed, it'll be re-positioned during Tick
//
// Whet the "Crop" is dropped onto Memory, Memory will tell it how many control points to create and
// they'll be stored as child objects within memory.
//
// The original Crop will immediately create a "cropped" version of the toy it was dropped into.
//
// Effectively the "crop target" is going to be the first owner that implements "croppable".
//
// Process Object Idea:
// - Object that represents arbitrary process in the OS
// - uses pidfd to track some OS process
// - shows PID as text
// - shows the "mind meld" symbol (Star Trek reference)
// - it could be a snow globe!

// When the arrow is dragged out it'll create a MemMeld object.
// - allows dropping & interaction with the Crop object
//
// The plan:
// - Process object (snow globe) will be available in the bubble menu of Window
// - ...

using namespace automat::ui;
using namespace std;

MemMeld::MemMeld(const MemMeld& that) : Object(that), process(that.process) {}

MemMeld::~MemMeld() {
  mem_meld.Detach();
  if (auto target = process->FindObject()) {
    target->WakeToys();
  }
}

void MemMeld::process_Impl::OnConnect(Interface end) {
  ObjectArgument<Process>::Table::DefaultOnConnect(*this, end);
  if (auto target = state->target.Lock(); target && target->mem_meld.IsExpired()) {
    target->mem_meld = obj->AcquireWeakPtr();
    target->WakeToys();
  }
}

void MemMeld::Start() {
  if (!start_once.First()) return;
  auto target = process->FindObject();
  if (!target || target->exited.load(std::memory_order_relaxed)) {
    ReportError("No running Process");
    return;
  }
  FD pidfd = syscall(SYS_pidfd_open, (pid_t)target->pid, 0);
  if (!pidfd.Opened()) {
    Status status;
    AppendErrorMessage(status) += "Couldn't open pidfd";
    ReportError(status.ToStr());
    return;
  }
  auto WakeBothToys = [this] {
    WakeToys();
    process->WakeToys();
  };
  mem_meld.on_attached = WakeBothToys;
  mem_meld.on_changed = [WakeBothToys](const automat::MemMeld::Region& before,
                                       const automat::MemMeld::Region& after) { WakeBothToys(); };
  mem_meld.on_mapped = [WakeBothToys](const automat::MemMeld::Region& region) { WakeBothToys(); };
  attach_thread = std::jthread([this, WakeBothToys, pidfd = std::move(pidfd)] {
    SetThreadName("MemMeld");
    Status status;
    mem_meld.Attach(pidfd, status);
    WakeBothToys();
    if (!OK(status)) ReportError(status.ToStr());
  });
}

constexpr float kFreeLength = 15_cm;
constexpr float kSpearLengthOnGlobe = 19.6_mm;
constexpr float kSpearHiddenLength = 8_mm;
constexpr float kSpearLengthPeriod = 0.5f;
constexpr float kShaftTop = -5.75_mm;
constexpr float kShaftReach = 1.5_mm;
constexpr float kHiddenMargin = 0.5_mm;
constexpr float kTipReach = 8_mm;
constexpr float kBluntEndLengthPeriod = 0.125f;
constexpr float kBluntEndAnglePeriod = 0.15f;
constexpr uint32_t kApexAnchorId = 1;
constexpr uint32_t kBluntEndAnchorId = 2;

constexpr Vec2 kTipCorner = {2_mm, -5_mm};
constexpr float kTipBottom = -6_mm;
constexpr Rect kTipBounds = Rect(-kTipCorner.x, kTipBottom, kTipCorner.x, 0);
constexpr float kTipEdgeWidth = 0.11_mm;

constexpr Vec2 kRingCenter = {0.25_mm, -1.37_mm};
constexpr float kRingRadius = 3.2_mm;
constexpr Vec2 kHoleCenter = {0, -0.45_mm};
constexpr float kHoleRadius = 1.55_mm;
constexpr float kRingEdgeWidth = 0.22_mm;
constexpr Rect kRingBounds = Rect::MakeCenter(kRingCenter, 2 * kRingRadius, 2 * kRingRadius);

constexpr float kShaftHighlightShift = 0.16_mm;

static const SinCos kLight = 135_deg;

constexpr SkColor4f kSteelShade = "#070809"_color4f;
constexpr SkColor4f kSteelLit = "#3d4148"_color4f;
constexpr SkColor4f kShaftHighlight = "#1d2024"_color4f;
constexpr SkColor4f kSteelPale = "#aeb6c2"_color4f;
constexpr SkColor4f kSteelHighlight = "#c9ced6"_color4f;
constexpr SkColor4f kTipEdgeShade = "#30353b"_color4f;
constexpr SkColor4f kTipEdgeLit = "#7d838c"_color4f;
constexpr SkColor4f kGoldScratchDark = "#3a2606"_color4f;
constexpr SkColor4f kGoldScratchLight = "#fff3c4"_color4f;
constexpr SkColor4f kGoldEdgeShadow = "#2a1200"_color4f;
constexpr SkColor4f kGoldTipShadow = "#1c0f00"_color4f;

struct MemMeldToy;

struct SpearShaft : ArgumentToy {
  MortalPtr<MemMeldToy> tip;
  float length = kFreeLength;
  float degrees = 90;
  animation::Kinetic<float> length_deflection;
  animation::Kinetic<float> degrees_deflection;
  bool had_globe = false;
  Vec2 apex;
  float hidden = 0;
  Vec<AddressRange> address_ranges;  // merged address ranges in the remote process
  uint64_t n_bytes = 0;
  uint32_t ranges_monitor = 0;

  SpearShaft(MemMeldToy* tip, Object& owner);

  float SpearLength() const { return length + length_deflection; }

  SinCos Direction() const { return SinCos::FromDegrees(degrees + degrees_deflection); }

  Vec2 BluntEnd() const { return apex - Vec2::Polar(Direction(), SpearLength()); }

  bool DrawnUnderEndpoints() const override { return true; }

  SkPath Shape() const override { return SkPath(); }

  Optional<Rect> DrawBounds() const override {
    Vec2 blunt_end = BluntEnd();
    return Rect(std::min(apex.x, blunt_end.x), std::min(apex.y, blunt_end.y),
                std::max(apex.x, blunt_end.x), std::max(apex.y, blunt_end.y))
        .Outset(kShaftReach);
  }

  void OnReparent(ui::Widget& new_parent, SkM44& fix) override {
    local_to_parent.postConcat(fix);
    WakeAnimation();
    RedrawThisFrame();
  }

  void TransformUpdated(time::Timer& timer) override { WakeAnimationAt(timer.now); }

  Tock Tick(time::Timer&) override;

  void Draw(SkCanvas& canvas) const override {
    SinCos turn = Direction() - 90_deg;
    float tail = -SpearLength();
    float from = std::min(hidden + tail, kShaftTop);
    float to = kShaftTop;
    SkAutoCanvasRestore restore(&canvas, true);
    canvas.translate(apex.x, apex.y);
    canvas.concat(turn.ToMatrix());
    constexpr float kW = kMargin / 2;
    SkPaint silhouette_paint;
    silhouette_paint.setAntiAlias(true);
    silhouette_paint.setColor4f(kSteelShade);
    SkPaint highlight_paint;
    {  // Highlight along the shaft
      SkColor4f colors[] = {kShaftHighlight.withAlpha(0),     kShaftHighlight.withAlpha(0.25f),
                            kShaftHighlight.withAlpha(0.75f), kShaftHighlight,
                            kShaftHighlight.withAlpha(0.75f), kShaftHighlight.withAlpha(0.25f),
                            kShaftHighlight.withAlpha(0)};
      float peak = 0.5f + kShaftHighlightShift * (float)(kLight - turn).cos / (2 * kW);
      float positions[] = {0, peak / 3, peak * 2 / 3, peak, (2 * peak + 1) / 3, (peak + 2) / 3, 1};
      SkPoint pts[] = {Vec2(-kW, 0), Vec2(kW, 0)};
      highlight_paint.setShader(SkShaders::LinearGradient(
          pts, SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}}));
    }
    auto SetRow = [&](SkVertices::Builder& mesh, int row, float center, float y) {
      mesh.positions()[2 * row] = Vec2(center - kW, y);
      mesh.positions()[2 * row + 1] = Vec2(center + kW, y);
      mesh.texCoords()[2 * row] = Vec2(-kW, y);
      mesh.texCoords()[2 * row + 1] = Vec2(kW, y);
    };
    if (address_ranges.empty()) {
      canvas.drawRect(Rect(-kW, tail, kW, to), silhouette_paint);
      SkVertices::Builder mesh(SkVertices::kTriangleStrip_VertexMode, 4, 0,
                               SkVertices::kHasTexCoords_BuilderFlag);
      SetRow(mesh, 0, 0, tail);
      SetRow(mesh, 1, 0, to);
      canvas.drawVertices(mesh.detach(), SkBlendMode::kSrcOver, highlight_paint);
      return;
    }

    int n_gaps = address_ranges.size() - 1;
    if (address_ranges.front().begin > 0) n_gaps += 1;
    if (address_ranges.back().end < UINT64_MAX) n_gaps += 1;

    constexpr float kZigZagW = 0.5_mm;
    // Zig-zag bends at 60 degrees to the right (W to the right, W/2 up), then at 60 degrees
    // left (2W to the left, W up), then back 60deg to the right (W to the right, ending in the
    // middle, W/2 up). Total height is W*2.

    float shaft_height = to - from;
    float gap_height = min(kZigZagW * 2, shaft_height * 0.5f / n_gaps);
    float gaps_height = gap_height * n_gaps;
    float bytes_height = shaft_height - gaps_height;

    SkPathBuilder shaft_builder;
    float x = 0;
    float y = from;
    uintptr_t addr = 0;
    shaft_builder.moveTo(x + kW, tail);
    shaft_builder.lineTo(x + kW, y);
    for (auto [a, b] : address_ranges) {
      if (a > addr) {
        y += gap_height / 4;
        shaft_builder.lineTo(x + kW + kZigZagW, y);
        y += gap_height / 2;
        shaft_builder.lineTo(x + kW - kZigZagW, y);
        y += gap_height / 4;
        shaft_builder.lineTo(x + kW, y);
      }
      // float bottom = y;
      y += bytes_height * (b - a) / n_bytes;
      shaft_builder.lineTo(x + kW, y);
      addr = b;
      // float top = y;
      // canvas.drawRect(Rect(x - kW, bottom, x + kW, top), {});
    }
    if (UINT64_MAX > addr) {
      y += gap_height / 4;
      shaft_builder.lineTo(x + kW + kZigZagW, y);
      y += gap_height / 2;
      shaft_builder.lineTo(x + kW - kZigZagW, y);
      y += gap_height / 4;
      shaft_builder.lineTo(x + kW, y);
    }
    addr = UINT64_MAX;
    shaft_builder.lineTo(x - kW, y);
    for (auto [a, b] : std::ranges::reverse_view(address_ranges)) {
      if (b < addr) {
        y -= gap_height / 4;
        shaft_builder.lineTo(x - kW - kZigZagW, y);
        y -= gap_height / 2;
        shaft_builder.lineTo(x - kW + kZigZagW, y);
        y -= gap_height / 4;
        shaft_builder.lineTo(x - kW, y);
      }
      y -= bytes_height * (b - a) / n_bytes;
      shaft_builder.lineTo(x - kW, y);
      addr = a;
    }
    if (addr > 0) {
      y -= gap_height / 4;
      shaft_builder.lineTo(x - kW - kZigZagW, y);
      y -= gap_height / 2;
      shaft_builder.lineTo(x - kW + kZigZagW, y);
      y -= gap_height / 4;
      shaft_builder.lineTo(x - kW, y);
    }
    shaft_builder.lineTo(x - kW, tail);
    shaft_builder.close();

    canvas.drawPath(shaft_builder.detach(), silhouette_paint);

    int n_rows = 2 + address_ranges.size() + 3 * n_gaps;
    SkVertices::Builder mesh(SkVertices::kTriangleStrip_VertexMode, 2 * n_rows, 0,
                             SkVertices::kHasTexCoords_BuilderFlag);
    int row = 0;
    y = from;
    addr = 0;
    auto AddZigZagRows = [&] {
      y += gap_height / 4;
      SetRow(mesh, row++, kZigZagW, y);
      y += gap_height / 2;
      SetRow(mesh, row++, -kZigZagW, y);
      y += gap_height / 4;
      SetRow(mesh, row++, 0, y);
    };
    SetRow(mesh, row++, 0, tail);
    SetRow(mesh, row++, 0, y);
    for (auto [a, b] : address_ranges) {
      if (a > addr) AddZigZagRows();
      y += bytes_height * (b - a) / n_bytes;
      SetRow(mesh, row++, 0, y);
      addr = b;
    }
    if (UINT64_MAX > addr) AddZigZagRows();
    canvas.drawVertices(mesh.detach(), SkBlendMode::kSrcOver, highlight_paint);
  }
};

struct MemMeldToy : ObjectToy {
  std::unique_ptr<SpearShaft> shaft;
  SinCos turn = 0_deg;
  animation::Kinetic<float> spear_length = kSpearHiddenLength;

  MemMeldToy(ui::Widget* parent, Object& owner)
      : ObjectToy(parent, owner), shaft(std::make_unique<SpearShaft>(this, owner)) {
    layers.OrderBelow(shaft.get());
  }

  bool CenteredAtZero() const override { return true; }

  SkPath Shape() const override {
    SkPathBuilder builder;
    builder.moveTo(-kTipCorner.x, kTipCorner.y);
    builder.lineTo(-1.59_mm, -3.99_mm);
    builder.arcTo({kRingRadius, kRingRadius}, 0, SkPathBuilder::kLarge_ArcSize,
                  SkPathDirection::kCCW, {1.69_mm, -4.23_mm});
    builder.lineTo(kTipCorner);
    builder.lineTo(0, kTipBottom);
    builder.close();
    SkMatrix turn_matrix = turn.ToMatrix();
    return builder.detach(&turn_matrix);
  }

  Optional<Rect> DrawBounds() const override {
    Rect bounds = Rect::MakeCircleR(kTipReach);
    if (shaft) bounds.ExpandToInclude(shaft->local_to_parent.asM33().mapRect(*shaft->DrawBounds()));
    return bounds;
  }

  float GetBaseScale() const override { return 1; }

  void OnPoll(time::Timer& timer) override {
    if (shaft) shaft->Poll(timer);
  }

  Interface FindOption(ui::Pointer&, ui::ActionTrigger trigger) override {
    if (trigger != ui::PointerButton::Left) return {};
    auto* holder = dynamic_cast<ObjectToy*>(parent.Get());
    if (!holder) return {};
    auto process = dyn_cast_if_present<Process>(holder->LockOwner());
    return process ? Interface(*process, kMakeMemMeld) : Interface();
  }

  Tock Tick(time::Timer& timer) override {
    Tock tock = Tock::Shape;
    auto* holder = dynamic_cast<ObjectToy*>(parent.Get());
    if (holder && dyn_cast_if_present<Process>(holder->LockOwner())) {
      tock.ing |=
          !spear_length.SineTowards(kSpearLengthOnGlobe, timer.d, kSpearLengthPeriod).settled;
      Vec<Vec2AndDir> positions;
      holder->ConnectionPositions(positions);
      Vec2 apex = positions.front().pos + Vec2(0, spear_length);
      local_to_parent = SkM44::Translate(apex.x, apex.y);
    }
    return tock;
  }

  void Draw(SkCanvas& canvas) const override {
    SkAutoCanvasRestore restore(&canvas, true);
    canvas.concat(turn.ToMatrix());
    static const SkPath tip_path = SkPathBuilder()
                                       .moveTo(0, 0)
                                       .lineTo(-kTipCorner.x, kTipCorner.y)
                                       .lineTo(0, kTipBottom)
                                       .lineTo(kTipCorner)
                                       .close()
                                       .detach();
    static const SkPath ring_path = [] {
      SkPathBuilder builder(SkPathFillType::kEvenOdd);
      builder.addCircle(kRingCenter, kRingRadius - kRingEdgeWidth / 2);
      builder.addCircle(kHoleCenter, kHoleRadius + kRingEdgeWidth / 2);
      return builder.detach();
    }();
    SinCos light = kLight - turn;

    {  // Ring
      Vec2 to_light = Vec2::Polar(light, kRingRadius);
      SkPoint pts[] = {kRingCenter + to_light, kRingCenter - to_light};
      SkColor4f colors[] = {"#f3dc96"_color4f, "#dbb65f"_color4f, "#c79a3c"_color4f,
                            "#ae8431"_color4f, "#8a6622"_color4f};
      float positions[] = {0, 0.3f, 0.55f, 0.8f, 1};
      SkPaint gold;
      gold.setAntiAlias(true);
      gold.setShader(SkShaders::LinearGradient(
          pts, SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}}));
      canvas.drawPath(ring_path, gold);
    }

    {  // Scratches and shadows on the ring
      static RasterPatch ring_patch;
      ring_patch.DrawCached(canvas, kRingBounds, SkISize(208, 208), [](SkCanvas& canvas) {
        canvas.clipPath(ring_path, true);
        {  // Scratches
          struct {
            SinCos direction;
            float pitch;
            SkColor4f color;
            float alpha;
          } passes[] = {
              {62_deg, 0.13_mm, kGoldScratchDark, 0.2f},
              {62_deg, 0.13_mm, kGoldScratchLight, 0.18f},
              {-28_deg, 0.2_mm, kGoldScratchDark, 0.13f},
              {-28_deg, 0.2_mm, kGoldScratchLight, 0.12f},
          };
          XorShift32 rng;
          SkPaint scratch;
          scratch.setAntiAlias(true);
          scratch.setStrokeCap(SkPaint::kRound_Cap);
          for (auto& pass : passes) {
            Vec2 along = Vec2::Polar(pass.direction, 1);
            Vec2 across = Rotate90DegreesCounterClockwise(along);
            for (float row = -kRingRadius; row < kRingRadius;
                 row += pass.pitch * rng.RollFloat(0.7f, 1.3f)) {
              for (float start = -kRingRadius; start < kRingRadius;) {
                float length = rng.RollFloat(0.5_mm, 1.6_mm);
                Vec2 from = kRingCenter + across * row + along * start;
                scratch.setColor4f(pass.color.withAlpha(pass.alpha * rng.RollFloat(0.4f, 1)));
                scratch.setStrokeWidth(rng.RollFloat(0.022_mm, 0.042_mm));
                canvas.drawLine(from, from + along * length, scratch);
                start += length + rng.RollFloat(0.02_mm, 0.25_mm);
              }
            }
          }
        }
        {  // Shadow of the spear tip
          SkPaint shadow;
          shadow.setColor4f(kGoldTipShadow.withAlpha(0.75f));
          shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.22_mm));
          canvas.drawPath(tip_path, shadow);
          shadow.setColor4f(kGoldTipShadow.withAlpha(0.5f));
          shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.5_mm));
          canvas.drawPath(tip_path, shadow);
        }
        {  // Shadows along the edges
          SkPaint shadow;
          shadow.setStyle(SkPaint::kStroke_Style);
          shadow.setStrokeWidth(0.16_mm);
          shadow.setColor4f(kGoldEdgeShadow.withAlpha(0.45f));
          shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.07_mm));
          canvas.drawCircle(kRingCenter, kRingRadius - kRingEdgeWidth, shadow);
          canvas.drawCircle(kHoleCenter, kHoleRadius + kRingEdgeWidth, shadow);
        }
        return SkPath();
      });
    }

    {  // Edges of the ring and of the hole
      static const SkPaint edge = [] {
        SkColor4f colors[] = {"#6b4c14"_color4f, "#b48a34"_color4f, "#e4bd62"_color4f,
                              "#fff6cf"_color4f, "#e4bd62"_color4f, "#8a6622"_color4f,
                              "#2e1f06"_color4f, "#a67f2e"_color4f, "#6b4c14"_color4f};
        float positions[] = {0,           50 / 360.f,  108 / 360.f,
                             135 / 360.f, 162 / 360.f, 225 / 360.f,
                             292 / 360.f, 332 / 360.f, 1};
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(kRingEdgeWidth);
        paint.setShader(SkShaders::SweepGradient(
            {0, 0}, SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}}));
        return paint;
      }();
      struct {
        SinCos rotation;
        Vec2 center;
        float radius;
      } circles[] = {
          {turn, kRingCenter, kRingRadius - kRingEdgeWidth / 2},
          {turn.Opposite(), kHoleCenter, kHoleRadius + kRingEdgeWidth / 2},
      };
      for (auto& circle : circles) {
        canvas.save();
        canvas.translate(circle.center.x, circle.center.y);
        canvas.concat((-circle.rotation).ToMatrix());
        canvas.drawCircle({0, 0}, circle.radius, edge);
        canvas.restore();
      }
    }

    {  // Halves of the spear tip
      auto SteelColor = [](float brightness) {
        return color::MixColors(kSteelShade, kSteelLit, std::clamp(brightness, 0.f, 1.f));
      };
      for (float side : {-1.f, 1.f}) {
        Vec2 corner = {side * kTipCorner.x, kTipCorner.y};
        SinCos normal = SinCos::FromVec2(Vec2(-side * kTipCorner.y, kTipCorner.x));
        Vec2 inward = Vec2::Polar(normal.Opposite(), 1);
        float depth = Dot(Vec2(0, kTipBottom), inward);
        float facing = (float)(normal - light).cos;
        SkColor4f edge =
            color::MixColors(kTipEdgeShade, kTipEdgeLit, powf(0.5f + 0.5f * facing, 1.5f));

        SkPoint pts[] = {Vec2(0, 0), inward * depth};
        SkColor4f colors[] = {edge, edge, SteelColor(0.38f + 0.67f * facing),
                              SteelColor(0.46f + 0.29f * facing)};
        float positions[] = {0, kTipEdgeWidth / depth, 1.6f * kTipEdgeWidth / depth, 1};
        SkPaint steel;
        steel.setAntiAlias(true);
        steel.setShader(SkShaders::LinearGradient(
            pts, SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}}));
        SkPoint corners[] = {Vec2(0, 0), corner, Vec2(0, kTipBottom)};
        canvas.drawPath(SkPath::Polygon(corners, true), steel);
      }
    }

    {  // Dents, specks and shaft shadow on the spear tip
      static RasterPatch tip_patch;
      tip_patch.DrawCached(canvas, kTipBounds, SkISize(112, 168), [](SkCanvas& canvas) {
        SkMatrix inset = SkMatrix::Scale(0.88f, 0.93f).postTranslate(0, -0.22_mm);
        canvas.clipPath(tip_path.makeTransform(inset), true);
        XorShift32 rng;
        for (int i = 0; i < 70; ++i) {
          Vec2 center = {rng.RollFloat(-kTipCorner.x, kTipCorner.x), rng.RollFloat(kTipBottom, 0)};
          float radius = rng.RollFloat(0.1_mm, 0.42_mm);
          bool dark = rng.RollFloat(0, 1) < 0.45f;
          SkPaint dent;
          dent.setColor4f(dark ? SkColors::kBlack.withAlpha(rng.RollFloat(0.12f, 0.34f))
                               : kSteelPale.withAlpha(rng.RollFloat(0.04f, 0.13f)));
          dent.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, radius * 0.45f));
          canvas.drawOval(Rect::MakeCenter(center, 2 * radius, 1.2f * radius), dent);
        }
        for (int i = 0; i < 110; ++i) {
          Vec2 center = {rng.RollFloat(-kTipCorner.x, kTipCorner.x), rng.RollFloat(kTipBottom, 0)};
          bool dark = rng.RollFloat(0, 1) < 0.7f;
          SkPaint speck;
          speck.setAntiAlias(true);
          speck.setColor4f(dark ? SkColors::kBlack.withAlpha(rng.RollFloat(0.12f, 0.32f))
                                : kSteelHighlight.withAlpha(rng.RollFloat(0.04f, 0.11f)));
          canvas.drawCircle(center, rng.RollFloat(0.02_mm, 0.045_mm), speck);
        }
        SkPaint shaft_shadow;
        shaft_shadow.setColor4f(SkColors::kBlack.withAlpha(0.35f));
        shaft_shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.5_mm));
        canvas.drawRect(Rect(-kTipCorner.x, kTipBottom, kTipCorner.x, -4.6_mm), shaft_shadow);
        return SkPath();
      });
    }

    {  // Ridge of the spear tip
      SkPaint ridge;
      ridge.setAntiAlias(true);
      ridge.setStrokeWidth(0.06_mm);
      ridge.setStrokeCap(SkPaint::kRound_Cap);
      ridge.setColor4f(kSteelHighlight.withAlpha(0.12f + 0.3f * fabsf((float)light.cos)));
      canvas.drawLine(0, -0.35_mm, 0, kTipBottom + 0.1_mm, ridge);
    }
  }
};

SpearShaft::SpearShaft(MemMeldToy* tip, Object& owner)
    : ArgumentToy(tip, owner, MemMeld::process_tbl), tip(tip) {}

ui::Tock SpearShaft::Tick(time::Timer& timer) {
  if (!tip) {
    MarkDead(timer.now);
    return {};
  }
  alpha = parent == tip.Get() ? 1 : tip->parent->alpha;
  if (parent != tip.Get() && owner.IsExpired()) return {};
  Tock tock;
  Vec2 old_apex = apex;
  Vec2 old_blunt_end = BluntEnd();

  ObjectToy* globe = nullptr;
  Vec2 blunt_end;
  const TextureAnchor* apex_grab = nullptr;
  const TextureAnchor* blunt_end_grab = nullptr;
  {  // Globe and end points
    auto* holder = dynamic_cast<ObjectToy*>(tip->parent.Get());
    if (holder && dyn_cast_if_present<Process>(holder->LockOwner())) {
      globe = holder;
    } else if (auto end = EndLocation()) {
      globe = tip->ToyScope().FindOrNull(*end->object);
    }
    auto GrabAnchor = [&](ui::Widget* toy) -> const TextureAnchor* {
      if (!toy || parent == tip.Get()) return nullptr;
      for (auto& anchor : toy->texture_anchors) {
        if (anchor.pointer) return &anchor;
      }
      return nullptr;
    };
    auto PositionOf = [&](ui::Widget& toy, const TextureAnchor* grab, Vec2 point) -> Vec2 {
      SkMatrix toy_to_shaft = TransformBetween(toy, *this);
      if (!grab) return toy_to_shaft.mapPoint(point);
      return grab->pointer->PositionWithin(*this) +
             Vec2(toy_to_shaft.mapVector(point + grab->warp_by - grab->pos));
    };
    apex_grab = GrabAnchor(tip.Get());
    blunt_end_grab = GrabAnchor(globe);
    apex = PositionOf(*tip, apex_grab, {0, 0});
    if (globe) {
      Vec<Vec2AndDir> positions;
      globe->ConnectionPositions(positions);
      blunt_end = PositionOf(*globe, blunt_end_grab, positions.front().pos);
    } else {
      blunt_end = apex - Vec2(0, kFreeLength);
    }
  }

  {  // Length and direction
    float old_length = length;
    float old_degrees = degrees;
    length = Length(apex - blunt_end);
    degrees = SinCos::FromVec2(apex - blunt_end).ToDegrees();
    bool has_globe = globe != nullptr;
    if (has_globe != had_globe && last_tick != time::SteadyPoint::min()) {
      length_deflection.value += old_length - length;
      degrees_deflection.value += old_degrees - degrees;
      animation::WrapModulo(degrees_deflection.value, 0, 360);
    }
    had_globe = has_globe;
    tock.shaping |= length_deflection.SineTowards(0, timer.d, kBluntEndLengthPeriod);
    tock.shaping |= degrees_deflection.SineTowards(0, timer.d, kBluntEndAnglePeriod);
  }

  {  // Part of the shaft behind the globe
    float old_hidden = hidden;
    hidden = 0;
    if (globe) {
      SkMatrix local_to_globe = TransformBetween(*this, *globe);
      Vec2 outside = local_to_globe.mapPoint(BluntEnd() + Vec2::Polar(Direction(), 1_m));
      if (auto exit = Raycast(globe->shape, {outside, Direction().Opposite()})) {
        Vec2 exit_local = TransformBetween(*globe, *this).mapPoint(*exit);
        hidden = Length(exit_local - BluntEnd()) + kHiddenMargin;
      }
    }
    if (hidden != old_hidden) tock |= Tock::Draw;
  }

  {  // Turn of the spear tip
    SinCos turn = Direction() - 90_deg;
    if (turn != tip->turn) {
      tip->turn = turn;
      tip->RedrawThisFrame();
      tip->WakeAnimation(&timer);
    }
  }

  {  // Address ranges
    if (ranges_monitor != observed_monitor) {
      ranges_monitor = observed_monitor;
      if (auto mem_meld = LockOwner<MemMeld>()) {
        address_ranges.clear();
        n_bytes = 0;
        automat::MemMeld::ReadGuard guard(mem_meld->mem_meld);
        // TODO: rather than re-querying the tree, introduce an API to walk the tree
        for (auto* it = guard.FindLowerBound(0); it != nullptr;
             it = guard.FindLowerBound(it->remote + it->Size())) {
          if (!address_ranges.empty() && address_ranges.back().end == it->remote) {  // merge
            address_ranges.back().end = it->remote + it->Size();
          } else {
            address_ranges.emplace_back(it->remote, it->remote + it->Size());
          }
          n_bytes += it->Size();
        }
        tock |= Tock::Draw;
      }
    }
  }

  {  // Texture anchors
    auto Anchor = [&](uint32_t id, const TextureAnchor* grab, Vec2 end) -> TextureAnchor {
      if (!grab) return {.pos = end, .id = id};
      Vec2 pointer = grab->pointer->PositionWithin(*this);
      return {end, id, grab->pointer, end - pointer, grab->decay, grab->radius};
    };
    if (parent == tip.Get()) {
      texture_anchors.clear();
    } else if (globe) {
      texture_anchors = {Anchor(kApexAnchorId, apex_grab, apex),
                         Anchor(kBluntEndAnchorId, blunt_end_grab, BluntEnd())};
    } else {
      texture_anchors = {Anchor(kApexAnchorId, apex_grab, apex)};
    }
  }

  if (shadow_elevation != tip->shadow_elevation) {
    shadow_elevation = tip->shadow_elevation;
    tock |= Tock::Draw;
  }
  if (apex != old_apex || BluntEnd() != old_blunt_end) tock |= Tock::Draw;
  TickSplits();
  return tock;
}

std::unique_ptr<ArgumentToy> MemMeld::process_Impl::OnMakeToy(ui::Widget* parent) {
  auto& tip = static_cast<MemMeldToy&>(*parent->ToyScope().FindOrNull(*obj));
  tip.shaft->SetOwner(*obj, state->monitor);
  tip.shaft->Reparent(*parent);
  return std::move(tip.shaft);
}

void MemMeld::process_Impl::OnDestroyToy(std::unique_ptr<automat::Toy> toy) {
  auto& shaft = static_cast<SpearShaft&>(*toy);
  if (!shaft.tip) return;
  shaft.tip->shaft.reset(static_cast<SpearShaft*>(toy.release()));
  shaft.Reparent(*shaft.tip);
  shaft.tip->layers.OrderBelow(&shaft);
}

std::unique_ptr<MemMeld::Toy> MemMeld::MakeToy(ui::Widget* parent) {
  return std::make_unique<MemMeldToy>(parent, *this);
}

std::unique_ptr<MemMeld::Toy> MemMeld::MakeDeadToy(ui::Widget* parent) {
  return std::make_unique<MemMeldToy>(parent, dead_object);
}

}  // namespace automat::library
