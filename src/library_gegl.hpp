#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <include/core/SkImage.h>

#include <mutex>

#include "base.hpp"
#include "image_provider.hpp"
#include "str.hpp"
#include "stream.hpp"

namespace automat::library {

// gegl:gaussian-blur as a board block. GEGL is lazy and Automat-driven: the
// block recomputes when its input image or its instrument changes (the toy
// schedules a Run), and Run performs one full recompute. Alone on the board
// it demonstrates itself by blurring a gegl:checkerboard, so the operation
// is visible the moment the block lands.
struct GeglBlur : Object {
  mutable std::mutex mutex;  // guards the fields below

  float std_dev = 4.0f;  // pixels; drives std-dev-x and std-dev-y together

  // Runtime, guarded by `mutex`:
  sk_sp<SkImage> result;
  sk_sp<SkImage> computed_input;  // the input identity `result` came from
  float computed_dev = -1;
  bool computing = false;

  DEF_INTERFACE(GeglBlur, Runnable, run, "Run")
  void OnRun(std::unique_ptr<RunTask>& t) { obj->Recompute(); }
  DEF_END(run);

  DEF_INTERFACE(GeglBlur, NextArg, next, "Next")
  DEF_END(next);

  DEF_INTERFACE(GeglBlur, InterfaceArgument<ImageProvider>, image, "Image")
  DEF_END(image);

  // Named "Result" because the "Image" name belongs to the input argument;
  // one object must not expose two interfaces with one name.
  DEF_INTERFACE(GeglBlur, ImageProvider, image_provider, "Result")
  sk_sp<SkImage> GetImage() { return obj->Result(); }
  DEF_END(image_provider);

  INTERFACES(run, next, image, image_provider);

  GeglBlur() = default;
  GeglBlur(const GeglBlur& o)
      : Object(o), std_dev(o.std_dev), run(o.run), next(o.next), image(o.image) {}

  StrView Name() const override { return "gegl:gaussian-blur"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(GeglBlur, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  void SetStdDev(float dev);
  sk_sp<SkImage> Result() const;
  // The connected input image, or null (the block then demonstrates itself).
  sk_sp<SkImage> InputImage();

  // Runs the GEGL graph over the current input and stores the result.
  void Recompute();
};

}  // namespace automat::library
