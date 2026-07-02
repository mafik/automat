// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_tensorflow.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkData.h>
#include <tensorflow/c/c_api.h>
#include <tensorflow/c/eager/c_api.h>

#include "format.hpp"
#include "ui_beta.hpp"
#include "units.hpp"

namespace automat::library {

using ui::beta::Hash2;

// TensorFlow is statically linked (src/tensorflow.py). One eager context is
// made at first use and lives for the process; null only if creation failed.
static TFE_Context* TfContext() {
  static TFE_Context* ctx = [] {
    TF_Status* status = TF_NewStatus();
    TFE_ContextOptions* opts = TFE_NewContextOptions();
    TFE_Context* c = TFE_NewContext(opts, status);
    TFE_DeleteContextOptions(opts);
    if (TF_GetCode(status) != TF_OK) c = nullptr;
    TF_DeleteStatus(status);
    return c;
  }();
  return ctx;
}

// All TensorFlow work is serialized under one mutex.
static std::mutex tf_mutex;

// Fills the printed facts from a handle: format string, device, and the
// min/mean/max computed over the resolved floats.
static bool FillFacts(TFE_TensorHandle* handle, TensorFacts& facts) {
  TF_Status* status = TF_NewStatus();
  TF_Tensor* tensor = TFE_TensorHandleResolve(handle, status);
  if (!tensor || TF_GetCode(status) != TF_OK) {
    TF_DeleteStatus(status);
    return false;
  }
  Str dims;
  for (int i = 0; i < TF_NumDims(tensor); ++i) {
    if (!dims.empty()) dims += ",";
    dims += f("{}", TF_Dim(tensor, i));
  }
  facts.format = f("f32[{}]", dims);
  if (const char* device = TFE_TensorHandleDeviceName(handle, status)) {
    StrView d = device;
    // The device in TensorFlow's own words, without the job/replica prefix.
    if (auto pos = d.rfind('/'); pos != StrView::npos) d = d.substr(pos + 1);
    facts.device = d;
  }
  const float* data = (const float*)TF_TensorData(tensor);
  size_t n = TF_TensorByteSize(tensor) / sizeof(float);
  float min = n ? data[0] : 0, max = n ? data[0] : 0;
  double sum = 0;
  for (size_t i = 0; i < n; ++i) {
    min = std::min(min, data[i]);
    max = std::max(max, data[i]);
    sum += data[i];
  }
  facts.min = min;
  facts.max = max;
  facts.mean = n ? (float)(sum / n) : 0;
  TF_DeleteTensor(tensor);
  TF_DeleteStatus(status);
  return true;
}

// ============================================================================
// TfTensor
// ============================================================================

TfTensor::~TfTensor() {
  auto lock = std::lock_guard(mutex);
  if (handle) TFE_DeleteTensorHandle((TFE_TensorHandle*)handle);
}

sk_sp<SkImage> TfTensor::InputImage() {
  auto ip_ptr = image->FindInterface();
  ImageProvider ip(ip_ptr.Owner<Object>(), ip_ptr.Get());
  return ip ? ip.GetImage() : nullptr;
}

Str TfTensor::Format() {
  auto lock = std::lock_guard(mutex);
  return facts.format;
}

void* TfTensor::Handle(uint64_t& version_out) {
  auto lock = std::lock_guard(mutex);
  version_out = version;
  return handle;
}

void TfTensor::Materialize() {
  sk_sp<SkImage> input = InputImage();
  {
    auto lock = std::lock_guard(mutex);
    if (computing) return;
    computing = true;
  }
  TFE_TensorHandle* new_handle = nullptr;
  TensorFacts new_facts;
  Str error;
  if (input) {
    int width = input->width();
    int height = input->height();
    Vec<uint8_t> rgba((size_t)width * height * 4);
    auto info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    if (input->readPixels(nullptr, info, rgba.data(), (size_t)width * 4, 0, 0)) {
      auto lock = std::lock_guard(tf_mutex);
      int64_t dims[4] = {1, height, width, 3};
      TF_Tensor* tensor =
          TF_AllocateTensor(TF_FLOAT, dims, 4, (size_t)height * width * 3 * sizeof(float));
      float* out = (float*)TF_TensorData(tensor);
      for (size_t i = 0; i < (size_t)width * height; ++i) {
        out[i * 3 + 0] = rgba[i * 4 + 0] / 255.f;
        out[i * 3 + 1] = rgba[i * 4 + 1] / 255.f;
        out[i * 3 + 2] = rgba[i * 4 + 2] / 255.f;
      }
      TF_Status* status = TF_NewStatus();
      new_handle = TFE_NewTensorHandle(tensor, status);
      if (TF_GetCode(status) != TF_OK) {
        error = f("tf: {}", TF_Message(status));
        new_handle = nullptr;
      }
      TF_DeleteTensor(tensor);
      TF_DeleteStatus(status);
      if (new_handle) FillFacts(new_handle, new_facts);
    } else {
      error = "tf:tensor: the image pixels are not readable on the CPU";
    }
  }
  {
    auto lock = std::lock_guard(mutex);
    computing = false;
    if (handle) TFE_DeleteTensorHandle((TFE_TensorHandle*)handle);
    handle = new_handle;
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

TfOp::~TfOp() {
  auto lock = std::lock_guard(mutex);
  if (handle) TFE_DeleteTensorHandle((TFE_TensorHandle*)handle);
}

Str TfOp::Format() {
  auto lock = std::lock_guard(mutex);
  return facts.format;
}

void* TfOp::Handle(uint64_t& version_out) {
  auto lock = std::lock_guard(mutex);
  version_out = version;
  return handle;
}

sk_sp<SkImage> TfOp::ResultImage() {
  auto lock = std::lock_guard(mutex);
  return result_image;
}

// The result tensor [1,H,W,3] rendered back into the image world, clamped
// to [0,1].
static sk_sp<SkImage> TensorToImage(TF_Tensor* tensor) {
  if (TF_NumDims(tensor) != 4) return nullptr;
  int height = (int)TF_Dim(tensor, 1);
  int width = (int)TF_Dim(tensor, 2);
  if ((int)TF_Dim(tensor, 3) != 3 || width <= 0 || height <= 0) return nullptr;
  const float* data = (const float*)TF_TensorData(tensor);
  sk_sp<SkData> out = SkData::MakeUninitialized((size_t)width * height * 4);
  uint8_t* px = (uint8_t*)out->writable_data();
  for (size_t i = 0; i < (size_t)width * height; ++i) {
    for (int c = 0; c < 3; ++c) {
      px[i * 4 + c] = (uint8_t)(std::clamp(data[i * 3 + c], 0.f, 1.f) * 255.f + 0.5f);
    }
    px[i * 4 + 3] = 255;
  }
  auto info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  return SkImages::RasterFromData(info, std::move(out), (size_t)width * 4);
}

void TfOp::Execute() {
  Ptr<Object> producer = in_stream->Producer();
  uint64_t in_version = 0;
  TFE_TensorHandle* in_handle = nullptr;
  if (auto* t = dynamic_cast<TfTensor*>(producer.get())) {
    in_handle = (TFE_TensorHandle*)t->Handle(in_version);
  } else if (auto* op = dynamic_cast<TfOp*>(producer.get())) {
    in_handle = (TFE_TensorHandle*)op->Handle(in_version);
  }
  Str op_type;
  {
    auto lock = std::lock_guard(mutex);
    if (computing) return;
    computing = true;
    op_type = op_name;
  }
  TFE_TensorHandle* new_handle = nullptr;
  TensorFacts new_facts;
  sk_sp<SkImage> new_image;
  Str error;
  if (!TfContext()) {
    error = "TensorFlow context creation failed";
  } else if (in_handle) {
    // The producer keeps the input handle alive: handles die only in their
    // owner's Materialize/Execute/dtor, and the board is quiescent between
    // Automat-driven runs of one linear chain.
    auto lock = std::lock_guard(tf_mutex);
    TF_Status* status = TF_NewStatus();
    TFE_Op* op = TFE_NewOp(TfContext(), op_type.c_str(), status);
    if (TF_GetCode(status) == TF_OK) TFE_OpAddInput(op, in_handle, status);
    int nret = 1;
    if (TF_GetCode(status) == TF_OK) TFE_Execute(op, &new_handle, &nret, status);
    if (TF_GetCode(status) != TF_OK) {
      error = f("{}: {}", op_type, TF_Message(status));
      new_handle = nullptr;
    }
    if (op) TFE_DeleteOp(op);
    if (new_handle) {
      FillFacts(new_handle, new_facts);
      TF_Tensor* resolved = TFE_TensorHandleResolve(new_handle, status);
      if (resolved) {
        new_image = TensorToImage(resolved);
        TF_DeleteTensor(resolved);
      }
    }
    TF_DeleteStatus(status);
  }
  {
    auto lock = std::lock_guard(mutex);
    computing = false;
    if (new_handle) {
      if (handle) TFE_DeleteTensorHandle((TFE_TensorHandle*)handle);
      handle = new_handle;
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
  Optional<Rect> TextureBounds() const override {
    return Shape().getBounds().makeOutset(4_mm, 4_mm);
  }
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
          if (auto* t = dynamic_cast<TfTensor*>(p.get())) t->Handle(producer_version);
          if (auto* o = dynamic_cast<TfOp*>(p.get())) o->Handle(producer_version);
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
      StrView credit = "TensorFlow · eager";
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
