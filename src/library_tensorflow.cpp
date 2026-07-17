// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_tensorflow.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkData.h>

#include "format.hpp"
#include "tensorflow_runtime.hpp"
#include "ui_beta.hpp"
#include "units.hpp"

namespace automat::library {

using ui::beta::Hash2;

// Packs the runtime's plain facts into the printed TensorFacts.
static TensorFacts ToFacts(const tf::Facts& raw) {
  return {
      .format = raw.format, .device = raw.device, .min = raw.min, .mean = raw.mean, .max = raw.max};
}

// Renders a value back into the image world, or null if it is not a [1,h,w,3]
// float value.
static sk_sp<SkImage> ToImage(const tf::Value& v) {
  std::vector<uint8_t> rgba;
  int width = 0, height = 0;
  if (!tf::ValueToImage(v, rgba, width, height)) return nullptr;
  sk_sp<SkData> data = SkData::MakeWithCopy(rgba.data(), rgba.size());
  auto info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  return SkImages::RasterFromData(info, std::move(data), (size_t)width * 4);
}

// ============================================================================
// TfTensor
// ============================================================================

TfTensor::~TfTensor() = default;

sk_sp<SkImage> TfTensor::InputImage() {
  auto ip_ptr = image->FindInterface();
  ImageProvider ip(ip_ptr.Owner<Object>(), ip_ptr.Get());
  return ip ? ip.GetImage() : nullptr;
}

Str TfTensor::Format() {
  auto lock = std::lock_guard(mutex);
  return facts.format;
}

std::shared_ptr<tf::Value> TfTensor::Value(uint64_t& version_out) {
  auto lock = std::lock_guard(mutex);
  version_out = version;
  return tensor;
}

void TfTensor::Materialize() {
  sk_sp<SkImage> input = InputImage();
  {
    auto lock = std::lock_guard(mutex);
    if (computing) return;
    computing = true;
  }
  std::shared_ptr<tf::Value> new_tensor;
  TensorFacts new_facts;
  Str error;
  if (input) {
    int width = input->width();
    int height = input->height();
    Vec<uint8_t> rgba((size_t)width * height * 4);
    auto info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    if (input->readPixels(nullptr, info, rgba.data(), (size_t)width * 4, 0, 0)) {
      new_tensor = tf::ImageToValue(rgba.data(), width, height);
      new_facts = ToFacts(tf::Describe(*new_tensor));
    } else {
      error = "tf:tensor: the image pixels are not readable on the CPU";
    }
  }
  {
    auto lock = std::lock_guard(mutex);
    computing = false;
    tensor = std::move(new_tensor);
    facts = new_facts;
    computed_input = std::move(input);
    version += 1;
  }
  if (!error.empty()) ReportError(error);
  WakeToys();
}

// ============================================================================
// TfOp
// ============================================================================

TfOp::~TfOp() = default;

Str TfOp::Format() {
  auto lock = std::lock_guard(mutex);
  return facts.format;
}

std::shared_ptr<tf::Value> TfOp::Value(uint64_t& version_out) {
  auto lock = std::lock_guard(mutex);
  version_out = version;
  return tensor;
}

sk_sp<SkImage> TfOp::ResultImage() {
  auto lock = std::lock_guard(mutex);
  return result_image;
}

void TfOp::Execute() {
  Ptr<Object> producer = in_stream->Producer();
  uint64_t in_version = 0;
  // The shared_ptr copy keeps the input alive for the whole run, even if the
  // producer re-materializes concurrently.
  std::shared_ptr<tf::Value> in_tensor;
  if (auto* t = dynamic_cast<TfTensor*>(producer.get())) {
    in_tensor = t->Value(in_version);
  } else if (auto* op = dynamic_cast<TfOp*>(producer.get())) {
    in_tensor = op->Value(in_version);
  }
  Str op_type;
  {
    auto lock = std::lock_guard(mutex);
    if (computing) return;
    computing = true;
    op_type = op_name;
  }
  std::shared_ptr<tf::Value> new_tensor;
  TensorFacts new_facts;
  sk_sp<SkImage> new_image;
  Str error;
  if (in_tensor) {
    new_tensor = tf::RunUnaryOp(op_type, *in_tensor, error);
    if (new_tensor) {
      new_facts = ToFacts(tf::Describe(*new_tensor));
      new_image = ToImage(*new_tensor);
    }
  }
  {
    auto lock = std::lock_guard(mutex);
    computing = false;
    if (new_tensor) {
      tensor = std::move(new_tensor);
      facts = new_facts;
      result_image = std::move(new_image);
      version += 1;
    }
    computed_version = in_version;
    computed_producer = producer.get();
  }
  if (!error.empty()) ReportError(error);
  WakeToys();
}

// ============================================================================
// Toys: one compact plate for both blocks - the printed tensor facts, plus
// the result image on ops.
// ============================================================================

namespace {

constexpr float kPlateW = 7_cm;
constexpr float kBand = ui::beta::kTitleSize + 2 * ui::beta::kPadS + 0.45_mm;
constexpr float kCreditRow = 2.0_mm;
constexpr float kSide = 2.0_mm;
constexpr float kImageH = 2.6_cm;
constexpr float kFactRow = 3.2_mm;
constexpr float kBottomPad = 1.5_mm;
constexpr float kPlateH = kBand + kCreditRow + kImageH + 1_mm + kFactRow * 3 + kBottomPad;

constexpr uint32_t kSeed = 0x7F0;

}  // namespace

struct TfToy : ui::beta::ObjectToy {
  bool is_op;

  // Tick-cached object state (UI thread only):
  Str title_;
  TensorFacts facts_;
  sk_sp<SkImage> image_;

  // Change detection for redraws and the lazy loop:
  const SkImage* drawn_image_ = nullptr;
  Str drawn_format_;

  TfToy(ui::Widget* parent, Object& obj, bool is_op)
      : ui::beta::ObjectToy(parent, obj), is_op(is_op) {
    title_ = Str(obj.Name());
    UpdateFromObject();
  }

  bool CenteredAtZero() const override { return true; }
  SkPath Shape() const override {
    return SkPath::RRect(RRect::MakeSimple(Rect::MakeCenterZero(kPlateW, kPlateH), 3_mm).sk);
  }
  Optional<Rect> DrawBounds() const override { return Shape().getBounds().makeOutset(4_mm, 4_mm); }
  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&decltype(TfTensor::out_stream)::tbl) ||
        &arg == static_cast<const Interface::Table*>(&decltype(TfOp::out_stream)::tbl)) {
      return Vec2AndDir{.pos = Vec2(-kPlateW / 2 + 10_mm, -kPlateH / 2), .dir = -90_deg};
    }
    return ObjectToy::ArgStart(arg);
  }

  void UpdateFromObject() {
    if (is_op) {
      if (auto op = LockObject<TfOp>()) {
        uint64_t producer_version = 0;
        Object* producer = nullptr;
        {
          Ptr<Object> p = op->in_stream->Producer();
          producer = p.get();
          if (auto* t = dynamic_cast<TfTensor*>(p.get())) t->Value(producer_version);
          if (auto* o = dynamic_cast<TfOp*>(p.get())) o->Value(producer_version);
        }
        bool stale;
        {
          auto lock = std::lock_guard(op->mutex);
          facts_ = op->facts;
          image_ = op->result_image;
          stale = !op->computing &&
                  (op->computed_version != producer_version || op->computed_producer != producer);
        }
        if (stale && producer) op->run->ScheduleRun();
      }
    } else {
      if (auto tensor = LockObject<TfTensor>()) {
        sk_sp<SkImage> input_now = tensor->InputImage();
        bool stale;
        {
          auto lock = std::lock_guard(tensor->mutex);
          facts_ = tensor->facts;
          image_ = nullptr;
          stale = !tensor->computing && tensor->computed_input != input_now;
        }
        if (stale) tensor->run->ScheduleRun();
      }
    }
  }

  Tock Tick(time::Timer&) override {
    UpdateFromObject();
    Tock tock = Tock::Ing;
    if (image_.get() != drawn_image_ || facts_.format != drawn_format_) {
      drawn_image_ = image_.get();
      drawn_format_ = facts_.format;
      tock |= Tock::Draw;
    }
    return tock;
  }

  void Draw(SkCanvas& canvas) const override {
    ui::beta::Panel(canvas, Rect::MakeCenterZero(kPlateW, kPlateH), title_, ui::beta::kGold,
                    ui::beta::State::Default, Seed(kSeed), true);

    {  // credit
      StrView credit = "TensorFlow";
      float w = ui::beta::TextWidth(credit, ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, credit, {kPlateW / 2 - kSide - w, kPlateH / 2 - kBand - 1.6_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
    }

    // Image area: an op shows its result back in the image world; the
    // tensor block is a noun, so its area stays a labelled slab.
    Rect image_rect =
        Rect::MakeCornerZero(kPlateW - 2 * kSide, kImageH)
            .MoveBy({-(kPlateW - 2 * kSide) / 2, kPlateH / 2 - kBand - kCreditRow - kImageH});
    {
      SkPaint bg;
      bg.setColor(ui::beta::kInk);
      canvas.drawRect(image_rect.sk, bg);
      if (image_) {
        canvas.save();
        SkRect src = SkRect::Make(image_->dimensions());
        SkMatrix m = SkMatrix::RectToRect(src, image_rect.sk, SkMatrix::kCenter_ScaleToFit);
        m.preTranslate(0, image_->height() / 2.f);
        m.preScale(1, -1);
        m.preTranslate(0, -image_->height() / 2.f);
        canvas.concat(m);
        canvas.drawImage(image_, 0, 0, SkSamplingOptions(SkFilterMode::kLinear), nullptr);
        canvas.restore();
      } else {
        StrView hint = facts_.format.empty()
                           ? (is_op ? StrView("connect a tensor") : StrView("connect an Image"))
                           : StrView(facts_.format);
        float w = ui::beta::TextWidth(hint, ui::beta::kMicroSize + 0.6_mm);
        ui::beta::DrawText(canvas, hint, {-w / 2, image_rect.CenterY()},
                           ui::beta::kMicroSize + 0.6_mm, ui::beta::kSky, false, Seed(kSeed));
      }
      SkPaint frame;
      frame.setStyle(SkPaint::kStroke_Style);
      frame.setStrokeWidth(ui::beta::kStroke);
      frame.setColor(ui::beta::kInk);
      canvas.drawRect(image_rect.sk, frame);
    }

    {  // The printed facts: format, device, and the value range.
      float y = image_rect.bottom - 1_mm - kFactRow;
      if (!facts_.format.empty()) {
        ui::beta::DrawText(canvas, facts_.format, {-kPlateW / 2 + kSide, y},
                           ui::beta::kMicroSize + 0.3_mm, ui::beta::kInk, false, Seed(kSeed));
        y -= kFactRow;
        if (!facts_.device.empty()) {
          ui::beta::DrawText(canvas, facts_.device, {-kPlateW / 2 + kSide, y}, ui::beta::kMicroSize,
                             ui::beta::kInkSoft, false, Seed(kSeed));
        }
        y -= kFactRow;
        Str range = f("min {:.3f}  mean {:.3f}  max {:.3f}", facts_.min, facts_.mean, facts_.max);
        ui::beta::DrawText(canvas, range, {-kPlateW / 2 + kSide, y}, ui::beta::kMicroSize,
                           ui::beta::kInk, false, Seed(kSeed));
      }
    }

    {  // stream port labels
      ui::beta::DrawText(canvas, "tensor", {-kPlateW / 2 + 5.8_mm, -kPlateH / 2 + 0.8_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
      if (is_op) {
        StrView in_label = "tensor";
        float in_w = ui::beta::TextWidth(in_label, ui::beta::kMicroSize);
        ui::beta::DrawText(canvas, in_label, {-in_w / 2, kPlateH / 2 - kBand - 1.6_mm},
                           ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
      }
    }
    BakeChildren(canvas);
  }
};

std::unique_ptr<ObjectToy> TfTensor::MakeToy(ui::Widget* parent) {
  return std::make_unique<TfToy>(parent, *this, false);
}

std::unique_ptr<ObjectToy> TfOp::MakeToy(ui::Widget* parent) {
  return std::make_unique<TfToy>(parent, *this, true);
}

}  // namespace automat::library
