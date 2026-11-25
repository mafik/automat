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
#include <array>
#include <cmath>
#include <cstdint>
#include <ranges>
#include <thread>

#include "action.hpp"
#include "animation.hpp"
#include "argument.hpp"
#include "casting.hpp"
#include "color.hpp"
#include "crop_tool.hpp"
#include "deserializer.hpp"
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
#include "pointer.hpp"
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
constexpr float kCroppedLength = 5_cm;
constexpr float kSpearLengthOnGlobe = 19.6_mm;
constexpr float kSpearHiddenLength = 8_mm;
constexpr float kSpearLengthPeriod = 0.5f;
constexpr float kShaftTop = -5.75_mm;
constexpr float kShaftReach = 1.5_mm;
constexpr float kShaftHalfWidth = kMargin / 2;
constexpr float kZigZagW = 0.5_mm;
constexpr float kShaftMargin = 1.5_mm;
constexpr float kCropBoxWidth = 5_mm;
constexpr float kDefaultCropLength = 12_mm;
constexpr float kMinCropLength = 2_mm;
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
  struct Section {
    AddressRange addresses;
    bool gap;
    float bottom = 0;
    float top = 0;
  };

  MortalPtr<MemMeldToy> tip;
  float length = kFreeLength;
  float degrees = 90;
  animation::Kinetic<float> length_deflection;
  animation::Kinetic<float> degrees_deflection;
  bool had_globe = false;
  Vec2 apex;
  float hidden = 0;
  Vec<Section> sections;
  uint64_t n_bytes = 0;
  uint32_t ranges_monitor = 0;

  SpearShaft(MemMeldToy* tip, Object& owner);

  float SpearLength() const { return length + length_deflection; }

  SinCos Direction() const { return SinCos::FromDegrees(degrees + degrees_deflection); }

  Vec2 BluntEnd() const { return apex - Vec2::Polar(Direction(), SpearLength()); }

  SkMatrix SpearToShaft() const {
    return SkMatrix::Translate(apex.x, apex.y).preConcat((Direction() - 90_deg).ToMatrix());
  }

  AddressRange ShownRange() const {
    return {sections.front().addresses.begin, sections.back().addresses.end};
  }

  float PositionOfAddress(uint64_t address) const {
    address = std::clamp(address, sections.front().addresses.begin, sections.back().addresses.end);
    for (auto& section : sections) {
      if (address <= section.addresses.end || &section == &sections.back()) {
        double fraction = double(address - section.addresses.begin) /
                          double(section.addresses.end - section.addresses.begin);
        return section.bottom + (section.top - section.bottom) * fraction;
      }
    }
    return 0;
  }

  uint64_t AddressAt(float position) const {
    for (auto& section : sections) {
      if (position < section.top || &section == &sections.back()) {
        if (position <= section.bottom) return section.addresses.begin;
        if (position >= section.top) return section.addresses.end;
        double fraction = (position - section.bottom) / (section.top - section.bottom);
        return section.addresses.begin +
               uint64_t(fraction * double(section.addresses.end - section.addresses.begin));
      }
    }
    return 0;
  }

  bool DrawnUnderEndpoints() const override { return true; }

  SkPath Shape() const override {
    if (sections.empty()) return SkPath();
    Rect band(-kShaftHalfWidth - kShaftMargin, sections.front().bottom,
              kShaftHalfWidth + kShaftMargin, sections.back().top);
    return SkPath::Rect(band).makeTransform(SpearToShaft());
  }

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
    SkAutoCanvasRestore restore(&canvas, true);
    canvas.concat(SpearToShaft());
    SkPaint silhouette_paint;
    silhouette_paint.setAntiAlias(true);
    silhouette_paint.setColor4f(kSteelShade);
    SkPaint highlight_paint;
    {  // Highlight along the shaft
      SkColor4f colors[] = {kShaftHighlight.withAlpha(0),     kShaftHighlight.withAlpha(0.25f),
                            kShaftHighlight.withAlpha(0.75f), kShaftHighlight,
                            kShaftHighlight.withAlpha(0.75f), kShaftHighlight.withAlpha(0.25f),
                            kShaftHighlight.withAlpha(0)};
      float peak = 0.5f + kShaftHighlightShift * (float)(kLight - turn).cos / (2 * kShaftHalfWidth);
      float positions[] = {0, peak / 3, peak * 2 / 3, peak, (2 * peak + 1) / 3, (peak + 2) / 3, 1};
      SkPoint pts[] = {Vec2(-kShaftHalfWidth, 0), Vec2(kShaftHalfWidth, 0)};
      highlight_paint.setShader(SkShaders::LinearGradient(
          pts, SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}}));
    }
    auto SetRow = [&](SkVertices::Builder& mesh, int row, float center, float y) {
      mesh.positions()[2 * row] = Vec2(center - kShaftHalfWidth, y);
      mesh.positions()[2 * row + 1] = Vec2(center + kShaftHalfWidth, y);
      mesh.texCoords()[2 * row] = Vec2(-kShaftHalfWidth, y);
      mesh.texCoords()[2 * row + 1] = Vec2(kShaftHalfWidth, y);
    };

    // Zig-zag bends at 60 degrees to the right (W to the right, W/2 up), then at 60 degrees
    // left (2W to the left, W up), then back 60deg to the right (W to the right, ending in the
    // middle, W/2 up). Total height is W*2.
    auto ZigZag = [](const Section& section) {
      float height = section.top - section.bottom;
      float y = section.bottom;
      return std::array<Vec2, 4>{Vec2(0, y), Vec2(kZigZagW, y + height / 4),
                                 Vec2(-kZigZagW, y + height * 3 / 4), Vec2(0, y + height)};
    };

    SkPathBuilder shaft_builder;
    shaft_builder.moveTo(kShaftHalfWidth, tail);
    for (auto& section : sections) {
      if (section.gap) {
        for (Vec2 point : ZigZag(section)) {
          shaft_builder.lineTo(point.x + kShaftHalfWidth, point.y);
        }
      }
      shaft_builder.lineTo(kShaftHalfWidth, section.top);
    }
    for (auto& section : std::ranges::reverse_view(sections)) {
      shaft_builder.lineTo(-kShaftHalfWidth, section.top);
      if (section.gap) {
        auto zig_zag = ZigZag(section);
        for (int i = zig_zag.size() - 1; i >= 0; --i) {
          shaft_builder.lineTo(zig_zag[i].x - kShaftHalfWidth, zig_zag[i].y);
        }
      }
    }
    shaft_builder.lineTo(-kShaftHalfWidth, tail);
    shaft_builder.close();

    canvas.drawPath(shaft_builder.detach(), silhouette_paint);

    int n_rows = 1;
    for (auto& section : sections) n_rows += section.gap ? 5 : 1;
    SkVertices::Builder mesh(SkVertices::kTriangleStrip_VertexMode, 2 * n_rows, 0,
                             SkVertices::kHasTexCoords_BuilderFlag);
    int row = 0;
    SetRow(mesh, row++, 0, tail);
    for (auto& section : sections) {
      if (section.gap) {
        for (Vec2 point : ZigZag(section)) SetRow(mesh, row++, point.x, point.y);
      }
      SetRow(mesh, row++, 0, section.top);
    }
    canvas.drawVertices(mesh.detach(), SkBlendMode::kSrcOver, highlight_paint);
  }
};

struct MemMeldToy : ObjectToy {
  struct CropEndHandle;
  struct DragCropEndAction;

  std::unique_ptr<SpearShaft> shaft;
  SinCos turn = 0_deg;
  animation::Kinetic<float> spear_length = kSpearHiddenLength;
  Optional<AddressRange> crop;

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
    if (crop) {
      constexpr float kHalfWidth = kShaftHalfWidth + kZigZagW;
      float edge =
          kTipCorner.y + (kTipBottom - kTipCorner.y) * (kTipCorner.x - kHalfWidth) / kTipCorner.x;
      builder.lineTo(kHalfWidth, edge);
      builder.lineTo(kHalfWidth, -kCroppedLength);
      builder.lineTo(-kHalfWidth, -kCroppedLength);
      builder.lineTo(-kHalfWidth, edge);
    } else {
      builder.lineTo(0, kTipBottom);
    }
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

  ui::Widget* FindWidget(Interface::Table*) override;

  Interface FindOption(ui::Pointer&, ui::ActionTrigger trigger) override {
    if (trigger != ui::PointerButton::Left) return {};
    auto* holder = dynamic_cast<ObjectToy*>(parent.Get());
    if (!holder) return {};
    auto process = dyn_cast_if_present<Process>(holder->LockOwner());
    return process ? Interface(*process, kMakeMemMeld) : Interface();
  }

  Tock Tick(time::Timer& timer) override {
    Tock tock = Tock::Shape;
    if (crop && shaft) shaft->WakeAnimation(&timer);
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
  float old_hidden = hidden;
  bool sections_changed = false;

  ObjectToy* globe = nullptr;
  Vec2 blunt_end;
  const TextureAnchor* apex_grab = nullptr;
  const TextureAnchor* blunt_end_grab = nullptr;
  {  // Globe and end points
    if (!tip->crop) {
      auto* holder = dynamic_cast<ObjectToy*>(tip->parent.Get());
      if (holder && dyn_cast_if_present<Process>(holder->LockOwner())) {
        globe = holder;
      } else if (auto end = EndLocation()) {
        globe = tip->ToyScope().FindOrNull(*end->object);
      }
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
      blunt_end = apex - Vec2(0, tip->crop ? kCroppedLength : kFreeLength);
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
    hidden = 0;
    if (globe) {
      SkMatrix local_to_globe = TransformBetween(*this, *globe);
      Vec2 outside = local_to_globe.mapPoint(BluntEnd() + Vec2::Polar(Direction(), 1_m));
      if (auto exit = Raycast(globe->shape, {outside, Direction().Opposite()})) {
        Vec2 exit_local = TransformBetween(*globe, *this).mapPoint(*exit);
        hidden = Length(exit_local - BluntEnd()) + kHiddenMargin;
      }
    }
  }

  {  // Turn of the spear tip
    SinCos turn = Direction() - 90_deg;
    if (turn != tip->turn) {
      tip->turn = turn;
      tip->RedrawThisFrame();
      tip->WakeAnimation(&timer);
    }
  }

  {  // Sections
    AddressRange space = tip->crop.value_or(AddressRange{0, UINT64_MAX});
    bool same_space = !sections.empty() && sections.front().addresses.begin == space.begin &&
                      sections.back().addresses.end == space.end;
    if (!same_space || ranges_monitor != observed_monitor) {
      ranges_monitor = observed_monitor;
      int n = 0;
      uint64_t cursor = space.begin;
      auto Append = [&](uint64_t end, bool gap) {
        Section section = {.addresses = {cursor, end}, .gap = gap};
        if (n == (int)sections.size()) {
          sections.push_back(section);
          sections_changed = true;
        } else if (sections[n].addresses.begin != cursor || sections[n].addresses.end != end ||
                   sections[n].gap != gap) {
          sections[n] = section;
          sections_changed = true;
        }
        ++n;
        cursor = end;
      };
      n_bytes = 0;
      bool has_regions = false;
      if (auto mem_meld = LockOwner<MemMeld>()) {
        automat::MemMeld::ReadGuard guard(mem_meld->mem_meld);
        uint64_t mapped_end = cursor;
        // TODO: rather than re-querying the tree, introduce an API to walk the tree
        for (auto* it = guard.FindLowerBound(space.begin); it != nullptr && it->remote < space.end;
             it = guard.FindLowerBound(it->remote + it->Size())) {
          has_regions = true;
          uint64_t begin = std::max<uint64_t>(it->remote, space.begin);
          uint64_t end = std::min<uint64_t>(it->remote + it->Size(), space.end);
          if (begin != mapped_end) {
            if (mapped_end > cursor) Append(mapped_end, false);
            Append(begin, true);
          }
          mapped_end = end;
          n_bytes += end - begin;
        }
        if (mapped_end > cursor) Append(mapped_end, false);
      }
      if (cursor < space.end) Append(space.end, has_regions);
      if (n != (int)sections.size()) {
        sections.resize(n);
        sections_changed = true;
      }
    }
  }

  {  // Positions of the sections
    float bottom = std::min(hidden - SpearLength(), kShaftTop);
    float height = kShaftTop - bottom;
    int n_gaps = 0;
    for (auto& section : sections) n_gaps += section.gap;
    float gap_height = 0;
    if (n_gaps > 0) gap_height = std::min(kZigZagW * 2, height * 0.5f / n_gaps);
    float bytes_height = height - gap_height * n_gaps;
    for (auto& section : sections) {
      section.bottom = bottom;
      if (section.gap) {
        bottom += gap_height;
      } else if (n_bytes == 0) {
        bottom += bytes_height;
      } else {
        bottom += bytes_height * (section.addresses.end - section.addresses.begin) / n_bytes;
      }
      section.top = bottom;
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
  if (apex != old_apex || BluntEnd() != old_blunt_end || hidden != old_hidden || sections_changed) {
    tock |= Tock::Shape;
    if (auto* location_widget = ui::Closest<LocationWidget>(*tip)) {
      if (auto location = location_widget->LockLocation()) {
        location->InvalidateConnectionWidgets(true, false);
      }
    }
  }
  if (!tip->crop) TickSplits();
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

static AddressRange CropInside(AddressRange parent, AddressRange offsets) {
  uint64_t size = parent.end - parent.begin;
  uint64_t begin = std::min(offsets.begin, size - 1);
  uint64_t end = std::clamp(offsets.end, begin + 1, size);
  return {parent.begin + begin, parent.begin + end};
}

struct MemMeld::CropParameters : CropViewParameters {
  AddressRange range = {0, UINT64_MAX};
};

struct MemMeldToy::CropEndHandle : CropHandle {
  float side = 1;
  MortalPtr<SpearShaft> shaft;

  using CropHandle::CropHandle;
};

static Rect ShaftFractionsOf(SpearShaft& shaft, AddressRange range) {
  float bottom = shaft.sections.front().bottom;
  float length = shaft.sections.back().top - bottom;
  return Rect(-0.5f, (shaft.PositionOfAddress(range.begin) - bottom) / length, 0.5f,
              (shaft.PositionOfAddress(range.end) - bottom) / length);
}

struct MemMeldToy::DragCropEndAction : Action {
  MortalPtr<SpearShaft> shaft;
  MortalPtr<CropScissors> scissors;
  Ptr<CropView> view;
  Linked<Croppable> croppable;
  float side;
  float grab_offset;

  DragCropEndAction(ui::Pointer& pointer, CropEndHandle& handle, Ptr<CropView> crop_view,
                    Linked<Croppable> mem_meld_croppable)
      : Action(pointer),
        shaft(handle.shaft),
        scissors(&static_cast<CropScissors&>(*handle.parent)),
        view(std::move(crop_view)),
        croppable(std::move(mem_meld_croppable)),
        side(handle.side) {
    AddressRange shown = shaft->ShownRange();
    AddressRange range = shown;
    {
      auto lock = std::lock_guard(view->mutex);
      if (view->chain_end == croppable) {
        range = CropInside(shown, static_cast<MemMeld::CropParameters&>(*view->parameters).range);
      }
    }
    grab_offset = shaft->PositionOfAddress(side > 0 ? range.end : range.begin) - PointerPosition();
  }

  float PointerPosition() const {
    SkMatrix shaft_to_spear;
    (void)shaft->SpearToShaft().invert(&shaft_to_spear);
    return Vec2(shaft_to_spear.mapPoint(pointer.PositionWithin(*shaft))).y;
  }

  void Update() override {
    if (!shaft || !scissors || shaft->sections.empty() ||
        shaft->sections.back().top <= shaft->sections.front().bottom) {
      pointer.ReplaceAction(*this, nullptr);
      return;
    }
    AddressRange shown = shaft->ShownRange();
    uint64_t size = shown.end - shown.begin;
    float position = PointerPosition() + grab_offset;
    AddressRange dragged;
    {
      auto lock = std::lock_guard(view->mutex);
      if (view->chain_end != croppable) return;
      AddressRange& range = static_cast<MemMeld::CropParameters&>(*view->parameters).range;
      range = CropInside({0, size}, range);
      if (side > 0) {
        float low = shaft->PositionOfAddress(shown.begin + range.begin);
        uint64_t offset = shaft->AddressAt(std::max(position, low + kMinCropLength)) - shown.begin;
        range.end = std::max(offset, range.begin + 1);
      } else {
        float high = shaft->PositionOfAddress(shown.begin + range.end);
        uint64_t offset = shaft->AddressAt(std::min(position, high - kMinCropLength)) - shown.begin;
        range.begin = std::min(offset, range.end - 1);
      }
      dragged = range;
    }
    if (auto* group = scissors->FindGroup(croppable)) {
      Rect fractions = ShaftFractionsOf(*shaft, CropInside(shown, dragged));
      group->center = fractions.Center();
      group->size = fractions.Size();
    }
    view->WakeToys();
  }
};

static constinit Interface::Table kDragCropEnd = [] {
  Interface::Table t(Interface::kMenu, "Crop End");
  t.cursor = ui::Cursor::Crosshair;
  t.activate = [](Span<Interface> chain, ui::Pointer& pointer, Toy*) -> std::unique_ptr<Action> {
    auto* handle = dynamic_cast<MemMeldToy::CropEndHandle*>(pointer.hover.Get());
    if (!handle || !handle->shaft) return nullptr;
    auto& view = static_cast<CropView&>(*chain.back().object_ptr);
    Linked<Croppable> croppable;
    {
      auto lock = std::lock_guard(view.mutex);
      croppable = view.chain_end;
    }
    if (croppable.Unsafe().table_ptr != &MemMeld::croppable_tbl) return nullptr;
    return std::make_unique<MemMeldToy::DragCropEndAction>(pointer, *handle, view.AcquirePtr(),
                                                           std::move(croppable));
  };
  return t;
}();

static SpearShaft* FindShaft(MemMeldToy& tip, MemMeld& mem_meld) {
  if (tip.shaft) return tip.shaft.get();
  return static_cast<SpearShaft*>(tip.ToyScope().FindOrNull(mem_meld.process.Bind()));
}

ui::Widget* MemMeldToy::FindWidget(Interface::Table* table) {
  if (table != &MemMeld::croppable_tbl) return this;
  auto mem_meld = LockOwner<MemMeld>();
  SpearShaft* shaft = mem_meld ? FindShaft(*this, *mem_meld) : nullptr;
  return shaft ? static_cast<ui::Widget*>(shaft) : this;
}

static SpearShaft* FindVisibleShaft(MemMeldToy& tip, MemMeld& mem_meld) {
  SpearShaft* shaft = FindShaft(tip, mem_meld);
  if (!shaft || shaft->sections.empty()) return nullptr;
  if (shaft->sections.back().top <= shaft->sections.front().bottom) return nullptr;
  return shaft;
}

static AddressRange CropRangeAt(MemMeldToy& tip, SpearShaft& shaft, Vec2 point, float length) {
  SkMatrix spear_to_tip = TransformBetween(shaft, tip);
  spear_to_tip.preConcat(shaft.SpearToShaft());
  SkMatrix tip_to_spear;
  (void)spear_to_tip.invert(&tip_to_spear);
  float center = Vec2(tip_to_spear.mapPoint(point)).y;
  float bottom = shaft.sections.front().bottom;
  float top = shaft.sections.back().top;
  length = std::min(length, top - bottom);
  float low = std::max(bottom, std::min(center - length / 2, top - length));
  AddressRange shown = shaft.ShownRange();
  return CropInside({0, shown.end - shown.begin}, {shaft.AddressAt(low) - shown.begin,
                                                   shaft.AddressAt(low + length) - shown.begin});
}

static void PlaceCropGizmos(CropScissors::Group& group, Interface::Table* option) {
  Vec2 size = group.DrawnSize() * group.extent;
  static_cast<CropBox&>(*group.gizmos[0]).size = size;
  for (int i = 0; i < 2; ++i) {
    auto& handle = static_cast<MemMeldToy::CropEndHandle&>(*group.gizmos[i + 1]);
    handle.origin = Vec2(0, handle.side * size.y / 2);
    handle.scale = group.unfolding.value;
    handle.option = option;
  }
}

void MemMeld::croppable_Impl::CropViewSerialize(const CropView& view, ObjectSerializer& writer) {
  AddressRange range = {0, UINT64_MAX};
  {
    auto lock = std::lock_guard(view.mutex);
    if (view.chain_end.object_ptr == &*obj) {
      range = static_cast<const MemMeld::CropParameters&>(*view.parameters).range;
    }
  }
  writer.StartArray();
  writer.Uint64(range.begin);
  writer.Uint64(range.end);
  writer.EndArray();
}

void MemMeld::croppable_Impl::CropViewDeserialize(CropView& view, ObjectDeserializer& d) {
  AddressRange range = {0, UINT64_MAX};
  uint64_t* ends[] = {&range.begin, &range.end};
  Status status;
  for (int i : ArrayView(d, status)) {
    if (i < 2) {
      d.Get(*ends[i], status);
    } else {
      d.Skip();
    }
  }
  if (!OK(status)) {
    view.ReportError(status.ToStr());
    return;
  }
  auto lock = std::lock_guard(view.mutex);
  if (view.chain_end.object_ptr != &*obj) return;
  static_cast<MemMeld::CropParameters&>(*view.parameters).range = range;
}

std::unique_ptr<CropViewParameters> MemMeld::croppable_Impl::CropViewMakeParameters() {
  return std::make_unique<MemMeld::CropParameters>();
}

void MemMeld::croppable_Impl::CropScissorsOpen(CropView& view, automat::Toy& croppable_toy,
                                               CropScissors&, Vec2 point) {
  auto& tip = static_cast<MemMeldToy&>(croppable_toy);
  SpearShaft* shaft = FindVisibleShaft(tip, *obj);
  if (!shaft) return;
  AddressRange opened = CropRangeAt(tip, *shaft, point, kDefaultCropLength);
  {
    auto lock = std::lock_guard(view.mutex);
    if (view.chain_end.object_ptr != &*obj) return;
    static_cast<MemMeld::CropParameters&>(*view.parameters).range = opened;
  }
  view.WakeToys();
}

void MemMeld::croppable_Impl::CropScissorsMove(CropView& view, automat::Toy& croppable_toy,
                                               CropScissors& scissors, Vec2 point) {
  auto& tip = static_cast<MemMeldToy&>(croppable_toy);
  SpearShaft* shaft = FindVisibleShaft(tip, *obj);
  if (!shaft) return;
  AddressRange shown = shaft->ShownRange();
  AddressRange moved;
  {
    auto lock = std::lock_guard(view.mutex);
    if (view.chain_end.object_ptr != &*obj) return;
    AddressRange& range = static_cast<MemMeld::CropParameters&>(*view.parameters).range;
    AddressRange current = CropInside(shown, range);
    float length = shaft->PositionOfAddress(current.end) - shaft->PositionOfAddress(current.begin);
    moved = CropRangeAt(tip, *shaft, point, length);
    if (moved.begin == range.begin && moved.end == range.end) return;
    range = moved;
  }
  if (auto* group = scissors.FindGroup(*this)) {
    Rect fractions = ShaftFractionsOf(*shaft, CropInside(shown, moved));
    group->center = fractions.Center();
    group->size = fractions.Size();
  }
  view.WakeToys();
}

ui::Tock MemMeld::croppable_Impl::CropScissorsTick(CropView& view, automat::Toy& croppable_toy,
                                                   CropScissors& scissors, time::Timer& timer) {
  auto& tip = static_cast<MemMeldToy&>(croppable_toy);
  SpearShaft* shaft = FindVisibleShaft(tip, *obj);
  if (!shaft) return {};
  AddressRange range = shaft->ShownRange();
  {
    auto lock = std::lock_guard(view.mutex);
    if (view.chain_end.object_ptr != &*obj) return {};
    range = CropInside(range, static_cast<MemMeld::CropParameters&>(*view.parameters).range);
  }
  Rect fractions = ShaftFractionsOf(*shaft, range);
  auto& group = scissors.FindOrMakeGroup(*this);
  if (group.gizmos.empty()) {
    group.gizmos.push_back(std::make_unique<CropBox>(&scissors));
    for (float side : {-1.f, 1.f}) {
      auto handle = std::make_unique<MemMeldToy::CropEndHandle>(&scissors);
      handle->side = side;
      handle->outline = CropHandle::End(Vec2(0, side));
      group.gizmos.push_back(std::move(handle));
    }
    group.center = fractions.Center();
    group.size = fractions.Size();
  }
  ui::Tock tock = group.Unfold(fractions.Center(), fractions.Size(), timer);
  float bottom = shaft->sections.front().bottom;
  group.extent = Vec2(kCropBoxWidth, shaft->sections.back().top - bottom);
  SkMatrix spear_to_scissors = TransformBetween(*shaft, scissors);
  spear_to_scissors.preConcat(shaft->SpearToShaft());
  Vec2 x_axis = spear_to_scissors.mapVector(1, 0);
  scissors.box = CropScissors::Box{
      .center = spear_to_scissors.mapPoint(Vec2(0, bottom) + group.DrawnCenter() * group.extent),
      .turn = SinCos::FromVec2(x_axis),
      .scale = Length(x_axis),
      .size = group.DrawnSize() * group.extent,
  };
  PlaceCropGizmos(group, &kDragCropEnd);
  for (int i = 0; i < 2; ++i) {
    static_cast<MemMeldToy::CropEndHandle&>(*group.gizmos[i + 1]).shaft = shaft;
  }
  return tock;
}

ui::Tock MemMeld::croppable_Impl::CropScissorsClose(CropScissors& scissors, time::Timer& timer) {
  auto* group = scissors.FindGroup(*this);
  if (!group) return {};
  ui::Tock tock = group->Fold(timer);
  if (!group->gizmos.empty()) PlaceCropGizmos(*group, nullptr);
  return tock;
}

std::unique_ptr<automat::Toy> MemMeld::croppable_Impl::CropMakeToy(CropView&, ui::Widget* parent) {
  return obj->MakeToy(parent);
}

ui::Tock MemMeld::croppable_Impl::CropTickToy(CropView& view, automat::Toy& cropped_toy,
                                              time::Timer&) {
  auto CropOf = [this](this auto& self, CropView& link) -> AddressRange {
    AddressRange offsets = {0, UINT64_MAX};
    {
      auto lock = std::lock_guard(link.mutex);
      if (link.chain_end.object_ptr != &*obj) return offsets;
      offsets = static_cast<MemMeld::CropParameters&>(*link.parameters).range;
    }
    AddressRange outer = {0, UINT64_MAX};
    auto next = link.scissors->FindInterface();
    if (next.table_ptr == &CropView::croppable_tbl) {
      outer = self(static_cast<CropView&>(*next.object_ptr));
    }
    return CropInside(outer, offsets);
  };
  AddressRange crop = CropOf(view);
  auto& tip = static_cast<MemMeldToy&>(cropped_toy);
  if (tip.crop && tip.crop->begin == crop.begin && tip.crop->end == crop.end) return {};
  tip.crop = crop;
  return ui::Tock::Shape;
}

std::unique_ptr<MemMeld::Toy> MemMeld::MakeToy(ui::Widget* parent) {
  return std::make_unique<MemMeldToy>(parent, *this);
}

std::unique_ptr<MemMeld::Toy> MemMeld::MakeDeadToy(ui::Widget* parent) {
  return std::make_unique<MemMeldToy>(parent, dead_object);
}

}  // namespace automat::library
