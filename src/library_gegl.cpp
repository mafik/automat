// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_gegl.hpp"

#include <gegl.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkData.h>

#include "format.hpp"
#include "ui_beta.hpp"
#include "ui_leptonica.hpp"
#include "units.hpp"

// Entry point of the statically linked operation bundle (the generated
// module_common.c, see src/gegl.py); a null GTypeModule makes GLib register
// the operation types statically.
extern "C" gboolean gegl_module_register(GTypeModule* module);

namespace automat::library {

using ui::beta::Hash2;

static void EnsureGegl() {
  static std::once_flag once;
  std::call_once(once, [] {
    gegl_init(nullptr, nullptr);
    gegl_module_register(nullptr);
  });
}

// All GEGL work is serialized under one mutex; the graphs here are small and
// the recomputes already run on worker threads.
static std::mutex gegl_mutex;

// Runs buffer-source (or checkerboard when `input` is null) through
// gegl:gaussian-blur and blits the result out.
static sk_sp<SkImage> RunBlurGraph(sk_sp<SkImage> input, float dev) {
  EnsureGegl();
  auto lock = std::lock_guard(gegl_mutex);
  int width = input ? input->width() : 320;
  int height = input ? input->height() : 240;
  const Babl* format = babl_format("R'G'B'A u8");

  GeglNode* root = gegl_node_new();
  GeglNode* source = nullptr;
  GeglBuffer* buffer = nullptr;
  Vec<uint8_t> pixels;
  if (input) {
    pixels.resize((size_t)width * height * 4);
    auto info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    if (!input->readPixels(nullptr, info, pixels.data(), (size_t)width * 4, 0, 0)) {
      g_object_unref(root);
      return nullptr;
    }
    GeglRectangle rect{0, 0, width, height};
    buffer = gegl_buffer_new(&rect, format);
    gegl_buffer_set(buffer, &rect, 0, format, pixels.data(), width * 4);
    source =
        gegl_node_new_child(root, "operation", "gegl:buffer-source", "buffer", buffer, nullptr);
  } else {
    // The self-demonstration: a checkerboard, cropped because gaussian-blur
    // does not process the checkerboard's infinite plane.
    source = gegl_node_new_child(root, "operation", "gegl:checkerboard", "x", 32, "y", 32, nullptr);
    GeglNode* crop =
        gegl_node_new_child(root, "operation", "gegl:crop", "x", (double)0, "y", (double)0, "width",
                            (double)width, "height", (double)height, nullptr);
    gegl_node_link(source, crop);
    source = crop;
  }
  GeglNode* blur = gegl_node_new_child(root, "operation", "gegl:gaussian-blur", "std-dev-x",
                                       (double)dev, "std-dev-y", (double)dev, nullptr);
  gegl_node_link(source, blur);

  sk_sp<SkData> out = SkData::MakeUninitialized((size_t)width * height * 4);
  GeglRectangle out_rect{0, 0, width, height};
  gegl_node_blit(blur, 1.0, &out_rect, format, out->writable_data(), width * 4, GEGL_BLIT_DEFAULT);
  g_object_unref(root);
  if (buffer) g_object_unref(buffer);

  auto info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  return SkImages::RasterFromData(info, std::move(out), (size_t)width * 4);
}

void GeglBlur::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("std-dev");
  writer.Double(std_dev);
}

bool GeglBlur::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key != "std-dev") return false;
  Status status;
  double value = 0;
  d.Get(value, status);
  if (OK(status)) {
    auto lock = std::lock_guard(mutex);
    std_dev = (float)value;
  }
  return true;
}

void GeglBlur::SetStdDev(float dev) {
  {
    auto lock = std::lock_guard(mutex);
    std_dev = dev;
  }
  WakeToys();
}

sk_sp<SkImage> GeglBlur::Result() const {
  auto lock = std::lock_guard(mutex);
  return result;
}

sk_sp<SkImage> GeglBlur::InputImage() {
  auto ip_ptr = image->FindInterface();
  ImageProvider ip(ip_ptr.Owner<Object>(), ip_ptr.Get());
  return ip ? ip.GetImage() : nullptr;
}

void GeglBlur::Recompute() {
  sk_sp<SkImage> input = InputImage();
  float dev;
  {
    auto lock = std::lock_guard(mutex);
    if (computing) return;
    computing = true;
    dev = std_dev;
  }
  sk_sp<SkImage> out = RunBlurGraph(input, dev);
  bool failed = !out;
  {
    auto lock = std::lock_guard(mutex);
    computing = false;
    if (out) result = std::move(out);
    // Recorded even on failure, so the invalidation loop does not spin; a
    // knob or input change retries.
    computed_input = std::move(input);
    computed_dev = dev;
  }
  if (failed) {
    ReportError("gegl: the render failed");
  }
  WakeToys();
}

// ============================================================================
// Toy
// ============================================================================

namespace {

constexpr float kPlateW = 7_cm;
constexpr float kBand = ui::beta::kTitleSize + 2 * ui::beta::kPadS + 0.45_mm;
constexpr float kCreditRow = 2.0_mm;
constexpr float kSide = 2.0_mm;
constexpr float kPreviewW = kPlateW - 2 * kSide;
constexpr float kPreviewH = kPreviewW * 3 / 4;
constexpr float kLevelRow = 8.0_mm;
constexpr float kBottomPad = 1.5_mm;
constexpr float kPlateH = kBand + kCreditRow + kPreviewH + 1.5_mm + kLevelRow + kBottomPad;

constexpr float kDevMax = 40;  // pixels; gegl clamps its own range anyway

constexpr uint32_t kSeed = 0x6E6;

}  // namespace

struct GeglBlurToy;

struct DevDrag : Action {
  GeglBlurToy* widget;
  DevDrag(ui::Pointer& p, GeglBlurToy& w);
  ~DevDrag() override;
  void Update() override;
};

struct GeglBlurToy : ui::beta::ObjectToy {
  // Tick-cached object state (UI thread only):
  float std_dev_ = 0;
  bool computing_ = false;
  sk_sp<SkImage> result_;
  bool dragging = false;

  // What the last repaint showed; a change requests a redraw.
  const SkImage* drawn_result_ = nullptr;
  float drawn_dev_ = -1;
  bool drawn_computing_ = false;

  GeglBlurToy(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) {
    UpdateFromObject();
  }

  bool CenteredAtZero() const override { return true; }
  SkPath Shape() const override {
    return SkPath::RRect(RRect::MakeSimple(Rect::MakeCenterZero(kPlateW, kPlateH), 3_mm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Shape().getBounds().makeOutset(4_mm, 4_mm);
  }

  Rect LevelBand() const {
    float top = -kPlateH / 2 + kBottomPad + kLevelRow - 2.4_mm;
    return Rect{-kPlateW / 2 + kSide + 11_mm, top - 2.6_mm, kPlateW / 2 - kSide - 8_mm, top};
  }

  void UpdateFromObject() {
    if (auto blur = LockObject<GeglBlur>()) {
      sk_sp<SkImage> computed_input;
      float computed_dev;
      bool computing;
      {
        auto lock = std::lock_guard(blur->mutex);
        if (!dragging) std_dev_ = blur->std_dev;
        computing = blur->computing;
        computing_ = computing;
        result_ = blur->result;
        computed_input = blur->computed_input;
        computed_dev = blur->computed_dev;
      }
      // The lazy loop: anything that invalidates the result schedules one
      // recompute; the repaint follows through wake_counter.
      sk_sp<SkImage> input_now = blur->InputImage();
      if (!computing && (input_now != computed_input || blur->std_dev != computed_dev)) {
        blur->run->ScheduleRun();
      }
    }
  }

  Tock Tick(time::Timer&) override {
    UpdateFromObject();
    // The block observes its input for the lazy-recompute loop, so it keeps
    // ticking; it repaints only when what it shows changed.
    Tock tock = Tock::Ing;
    if (result_.get() != drawn_result_ || std_dev_ != drawn_dev_ ||
        computing_ != drawn_computing_) {
      drawn_result_ = result_.get();
      drawn_dev_ = std_dev_;
      drawn_computing_ = computing_;
      tock |= Tock::Draw;
    }
    return tock;
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      Rect band = LevelBand();
      if (ui::leptonica::LevelGrabsMarker(band, pos, std_dev_, 0, kDevMax, 2_mm) ||
          band.Outset(1_mm).Contains(pos)) {
        return std::make_unique<DevDrag>(p, *this);
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void Draw(SkCanvas& canvas) const override {
    ui::beta::Panel(canvas, Rect::MakeCenterZero(kPlateW, kPlateH), "gegl:gaussian-blur",
                    ui::beta::kCyan, ui::beta::State::Default, Seed(kSeed), true);

    {  // credit
      StrView credit = "GEGL · blur";
      float w = ui::beta::TextWidth(credit, ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, credit, {kPlateW / 2 - kSide - w, kPlateH / 2 - kBand - 1.6_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
    }

    // The result preview; alone, the block blurs a checkerboard so the
    // operation demonstrates itself.
    Rect preview_rect = Rect::MakeCornerZero(kPreviewW, kPreviewH)
                            .MoveBy({-kPreviewW / 2, kPlateH / 2 - kBand - kCreditRow - kPreviewH});
    {
      SkPaint bg;
      bg.setColor(ui::beta::kInk);
      canvas.drawRect(preview_rect.sk, bg);
      if (result_) {
        canvas.save();
        SkRect src = SkRect::Make(result_->dimensions());
        SkMatrix m = SkMatrix::RectToRect(src, preview_rect.sk, SkMatrix::kCenter_ScaleToFit);
        m.preTranslate(0, result_->height() / 2.f);
        m.preScale(1, -1);
        m.preTranslate(0, -result_->height() / 2.f);
        canvas.concat(m);
        canvas.drawImage(result_, 0, 0, SkSamplingOptions(SkFilterMode::kLinear), nullptr);
        canvas.restore();
      } else {
        StrView hint = computing_ ? StrView("computing") : StrView("waiting for the first render");
        float w = ui::beta::TextWidth(hint, ui::beta::kMicroSize);
        ui::beta::DrawText(canvas, hint, {-w / 2, preview_rect.CenterY()}, ui::beta::kMicroSize,
                           ui::beta::kGray, false, Seed(kSeed));
      }
      SkPaint frame;
      frame.setStyle(SkPaint::kStroke_Style);
      frame.setStrokeWidth(ui::beta::kStroke);
      frame.setColor(ui::beta::kInk);
      canvas.drawRect(preview_rect.sk, frame);
    }

    {  // The std-dev instrument: one Level band driving both axes.
      Rect band = LevelBand();
      ui::beta::DrawText(canvas, "std-dev", {-kPlateW / 2 + kSide, band.top - 2.0_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
      ui::leptonica::DrawLevel(canvas, band, std_dev_, 0, kDevMax, nullptr, 0, false, false,
                               dragging ? ui::beta::State::Active : ui::beta::State::Default,
                               Seed(kSeed));
      Str value = f("{:.1f} px", std_dev_);
      ui::beta::DrawText(canvas, value, {band.right + 1.5_mm, band.top - 2.0_mm},
                         ui::beta::kMicroSize, ui::beta::kInk, false, Seed(kSeed));
    }
    BakeChildren(canvas);
  }
};

DevDrag::DevDrag(ui::Pointer& p, GeglBlurToy& w) : Action(p), widget(&w) {
  widget->dragging = true;
}
DevDrag::~DevDrag() {
  if (widget) {
    widget->dragging = false;
    widget->WakeAnimation();
  }
}
void DevDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  float v = std::clamp(ui::leptonica::LevelXToValue(widget->LevelBand(), pos.x, 0, kDevMax), 0.f,
                       kDevMax);
  widget->std_dev_ = v;
  if (auto blur = widget->LockObject<GeglBlur>()) blur->SetStdDev(v);
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> GeglBlur::MakeToy(ui::Widget* parent) {
  return std::make_unique<GeglBlurToy>(parent, *this);
}

}  // namespace automat::library
