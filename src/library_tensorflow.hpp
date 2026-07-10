#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <include/core/SkImage.h>

#include <memory>
#include <mutex>

#include "base.hpp"
#include "image_provider.hpp"
#include "str.hpp"
#include "stream.hpp"

namespace automat::tf {
struct Value;
}

namespace automat::library {

// TensorFlow blocks run on the CPU through the C++ graph API
// (src/tensorflow.py), as designed in docs/parrots/Pipeline Language.md:
// tensors are data objects with dtype, shape and device printed on the face;
// the tensor port's format label uses the "f32[1,240,320,3]" notation.

// The printed facts of one held tensor.
struct TensorFacts {
  Str format;  // "f32[1,240,320,3]"; empty = no tensor
  Str device;
  float min = 0;
  float mean = 0;
  float max = 0;
};

// A tensor materialized from the connected image: float32, NHWC [1,H,W,3].
// Recomputes when the image changes; the "tensor" port hands the handle to
// a connected op.
struct TfTensor : Object {
  mutable std::mutex mutex;  // guards the runtime state below

  std::shared_ptr<tf::Value> tensor;  // the materialized value; null = none
  TensorFacts facts;
  uint64_t version = 0;  // bumped per materialize; consumers compare
  sk_sp<SkImage> computed_input;
  bool computing = false;

  DEF_INTERFACE(TfTensor, Runnable, run, "Run")
  void OnRun(std::unique_ptr<RunTask>& t) { obj->Materialize(); }
  DEF_END(run);

  DEF_INTERFACE(TfTensor, NextArg, next, "Next")
  DEF_END(next);

  DEF_INTERFACE(TfTensor, InterfaceArgument<ImageProvider>, image, "Image")
  DEF_END(image);

  DEF_INTERFACE(TfTensor, StreamArgument, out_stream, "tensor")
  Str OnFormat() { return obj->Format(); }
  DEF_END(out_stream);

  INTERFACES(run, next, image, out_stream);

  TfTensor() = default;
  TfTensor(const TfTensor& o)
      : Object(o), run(o.run), next(o.next), image(o.image), out_stream(o.out_stream) {}
  ~TfTensor() override;

  StrView Name() const override { return "tf:tensor"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(TfTensor, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  // Reads the connected image and builds the tensor from it.
  void Materialize();
  sk_sp<SkImage> InputImage();
  Str Format();
  // Returns the current tensor and its version; the shared_ptr keeps the value
  // alive for the caller.
  std::shared_ptr<tf::Value> Value(uint64_t& version_out);
};

// One eager TensorFlow op (a unary one, "Square" in the slice) applied to
// the connected tensor. The result is a tensor port for further ops and an
// image ("Result") for the rest of Automat.
struct TfOp : Object {
  mutable std::mutex mutex;  // guards the runtime state below

  Str op_name;  // the TensorFlow op type, also this object's Name

  std::shared_ptr<tf::Value> tensor;  // the result value; null = none
  TensorFacts facts;
  uint64_t version = 0;
  uint64_t computed_version = 0;  // the input version `handle` came from
  Object* computed_producer = nullptr;
  bool computing = false;
  sk_sp<SkImage> result_image;

  DEF_INTERFACE(TfOp, Runnable, run, "Run")
  void OnRun(std::unique_ptr<RunTask>& t) { obj->Execute(); }
  DEF_END(run);

  DEF_INTERFACE(TfOp, NextArg, next, "Next")
  DEF_END(next);

  DEF_INTERFACE(TfOp, StreamInput, in_stream, "tensor")
  DEF_END(in_stream);

  DEF_INTERFACE(TfOp, StreamArgument, out_stream, "tensor")
  Str OnFormat() { return obj->Format(); }
  DEF_END(out_stream);

  DEF_INTERFACE(TfOp, ImageProvider, image_provider, "Result")
  sk_sp<SkImage> GetImage() { return obj->ResultImage(); }
  DEF_END(image_provider);

  INTERFACES(run, next, in_stream, out_stream, image_provider);

  TfOp(StrView op_name) : op_name(op_name) {}
  TfOp(const TfOp& o)
      : Object(o), op_name(o.op_name), run(o.run), next(o.next), out_stream(o.out_stream) {}
  ~TfOp() override;

  StrView Name() const override { return op_name; }
  Ptr<Object> Clone() const override { return MAKE_PTR(TfOp, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  // Runs the op on the producer's current tensor.
  void Execute();
  Str Format();
  std::shared_ptr<tf::Value> Value(uint64_t& version_out);
  sk_sp<SkImage> ResultImage();
};

}  // namespace automat::library
