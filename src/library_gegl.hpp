#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <include/core/SkImage.h>

#include <atomic>
#include <mutex>

#include "base.hpp"
#include "image_provider.hpp"
#include "status.hpp"
#include "str.hpp"
#include "stream.hpp"
#include "vec.hpp"

namespace automat::library {

// Mirrors GeglRectangle, so GEGL headers stay out of this header.
struct GeglRect {
  int x = 0, y = 0, width = 0, height = 0;
};

// One registered GEGL operation, with the metadata the shelf presents.
struct GeglOpInfo {
  Str name;        // "gegl:gaussian-blur"
  Str categories;  // the operation's own colon-separated categories key
};

// Every operation compiled into the static bundle, minus the ones the
// library marks "hidden". Initializes GEGL.
Vec<GeglOpInfo> ListGeglOperations();

// A GEGL operation as a board block, living in Automat's one hidden GEGL
// graph (GEGL needs a parent node, and unlike a GstPipeline the parent
// carries no clock or bus, so a single root serves every block). GEGL is
// lazy: connections and property changes are legal at any time and only
// invalidate regions; a per-block GeglProcessor then recomputes the preview
// in chunks on a worker thread, each finished region repainting its part of
// the face as it lands.
// GEGL's caches make the shared upstream work cheap for every downstream
// preview.
//
// Any registered operation wraps generically: the face's instruments come
// from GParamSpec introspection (a GEnum is a chip that cycles its nicks, a
// boolean is a checkbox, a bounded number is a slider over the spec's ui
// range; everything else stays recipe-only). Stream ports carry GEGL-to-GEGL
// edges (buffer references inside the shared graph); the "Image" argument is
// the bridge in from the rest of Automat (a paper, a decoded frame), and
// "Result" is the bridge out. A block whose input ports are unconnected
// demonstrates itself on a checkerboard.
struct GeglOperation : Object {
  mutable std::mutex mutex;  // guards the recipe and preview fields below

  Str op;  // the GEGL operation name; also this object's Name()

  Vec<std::pair<Str, Str>> props;  // property values, applied at node creation and live

  // The preview, written region by region by the GEGL worker:
  Vec<uint8_t> preview;  // RGBA rows, preview_w x preview_h
  int preview_w = 0;
  int preview_h = 0;
  uint64_t preview_counter = 0;  // bumped per computed region; toys poll it
  sk_sp<SkImage> bridged_input;  // identity of the Image-argument input in use

  std::atomic<float> progress{1};  // 0..1; below 1 while recomputing
  std::atomic<bool> dirty{false};  // regions await the worker

  // GEGL objects in the hidden graph, managed under the host lock. Erased
  // types: GeglNode* except src_buffer (GeglBuffer*).
  void* node = nullptr;           // this operation
  void* src_node = nullptr;       // buffer-source carrying the bridged image
  void* src_buffer = nullptr;     // the bridged image's pixels
  void* demo_node = nullptr;      // checkerboard -> crop, the self-demonstration
  void* linked_source = nullptr;  // what currently feeds this node's input pad
  void* processor = nullptr;      // GeglProcessor* recomputing the preview
  bool mid_render = false;        // a capped visit left work in the processor
  Vec<GeglRect> pending_rects;    // computed regions awaiting the blit

  DEF_INTERFACE(GeglOperation, Runnable, run, "Run")
  void OnRun(std::unique_ptr<RunTask>& t) { obj->MarkDirty(); }
  DEF_END(run);

  DEF_INTERFACE(GeglOperation, NextArg, next, "Next")
  DEF_END(next);

  DEF_INTERFACE(GeglOperation, InterfaceArgument<ImageProvider>, image, "Image")
  DEF_END(image);

  DEF_INTERFACE(GeglOperation, ImageProvider, image_provider, "Result")
  sk_sp<SkImage> GetImage() { return obj->Result(); }
  DEF_END(image_provider);

  DEF_INTERFACE(GeglOperation, StreamInput, in_stream, "input")
  DEF_END(in_stream);

  DEF_INTERFACE(GeglOperation, StreamArgument, out_stream, "output")
  void OnCanConnect(Interface end, Status& status) { obj->CanFeedGegl(*this, end, status); }
  void OnConnect(Interface end) { obj->OnOutConnect(*this, end); }
  DEF_END(out_stream);

  INTERFACES(run, next, image, image_provider, in_stream, out_stream);

  GeglOperation(StrView op);
  GeglOperation(const GeglOperation&);
  ~GeglOperation() override;

  StrView Name() const override { return op; }
  Ptr<Object> Clone() const override { return MAKE_PTR(GeglOperation, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  // Stores the value (as text) and sets the GEGL property; GEGL invalidates
  // the affected regions and the worker follows.
  void SetProp(StrView name, StrView value);
  Str GetProp(StrView name) const;

  // The preview as an image for the rest of Automat.
  sk_sp<SkImage> Result() const;

  // Invalidates the whole preview (Run's meaning here: recompute now).
  void MarkDirty();

  // Chooses what feeds this node's input pad - the stream peer above, the
  // bridged Image, or the self-demonstration - and rewires the hidden graph
  // when the choice or the bridged image changed. Called from the toy's tick
  // and from connection changes.
  void SyncSources();

  // Link validity: GEGL edges stay between GEGL blocks (the Image/Result
  // bridges cross libraries), and a cycle would never converge.
  void CanFeedGegl(StreamArgument self, Interface end, Status& status);
  void OnOutConnect(StreamArgument self, Interface end);
};

// The GEGL shelf: every compiled-in operation as a clone pile, grouped by
// the library's categories key.
struct GeglShelf : Object {
  StrView Name() const override { return "GEGL"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(GeglShelf); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
};

}  // namespace automat::library
