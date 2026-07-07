#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <include/core/SkImage.h>

#include <atomic>
#include <memory>
#include <mutex>

#include "base.hpp"
#include "image_provider.hpp"
#include "status.hpp"
#include "str.hpp"
#include "stream.hpp"
#include "vec.hpp"

namespace automat::library {

struct GstChain;

// One element factory compiled into the static GStreamer build, with the
// metadata the shelf presents.
struct GstFactoryInfo {
  Str name;
  Str klass;
};

// Every compiled-in element factory, sorted by name. Initializes GStreamer.
Vec<GstFactoryInfo> ListGstFactories();

// The GStreamer shelf: every compiled-in factory as a clone pile, grouped by
// the library's own klass taxonomy.
struct GStreamerShelf : Object {
  StrView Name() const override { return "GStreamer"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(GStreamerShelf); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
};

// One GStreamer element on the board. The object is named after its element
// factory ("videotestsrc", "videoflip"), holds property values as recipe
// data, and runs inside a hidden GstPipeline that Automat manages. Elements
// linked through stream connections share one pipeline (the chain); starting
// any element starts the whole linked run, and every unconsumed source pad
// terminates in a preview branch (queue ! videoconvert ! videoscale !
// appsink) shown on its element's face, or a fakesink when the pad cannot
// carry raw video.
//
// Stream ports mirror the factory's pad templates. The base "src"/"sink"
// ports are the first expansion of the factory's primary template in each
// direction; when that template is a request or sometimes template
// ("src_%u"), extra port slots expose the further expansions. Request ports
// mint their pad when the chain builds; sometimes ports wait for the
// element's pad-added signal.
struct GStreamerElement : Object {
  static constexpr int kMaxExtraPorts = 4;

  enum class PadMode : uint8_t { kNone, kAlways, kRequest, kSometimes };

  mutable std::mutex mutex;  // guards props and the runtime state below

  Str factory;  // GStreamer element factory name; also this object's Name()

  Vec<std::pair<Str, Str>> props;  // property values, applied at start and live

  // Port layout, fixed at construction from the factory's pad templates.
  // Each slot's name is its gst pad name.
  StreamOutSlot extra_out[kMaxExtraPorts];
  StreamInSlot extra_in[kMaxExtraPorts];
  int n_extra_out = 0;
  int n_extra_in = 0;
  PadMode out_mode = PadMode::kNone;  // the primary src template's presence
  PadMode in_mode = PadMode::kNone;
  Vec<Str> out_pad_names;  // gst pad names by port index; [0] is the base port
  Vec<Str> in_pad_names;

  // Runtime, guarded by `mutex`. While running, `chain` is the hidden
  // GstPipeline shared by every element of the linked run; `element` is a
  // strong reference to this object's GstElement*. The type is erased so
  // GStreamer headers stay out of this header.
  std::shared_ptr<GstChain> chain;
  void* element = nullptr;
  int chain_index = -1;       // this element's member index within the chain
  bool preview_tail = false;  // this element's face shows a preview branch
  Str state_word = "NULL";    // pipeline state, in GStreamer's own words

  // Frozen copies of this element's chain counters, kept after Stop so the
  // connection meters hold their totals instead of dropping to zero.
  uint64_t stream_bytes = 0;
  uint64_t stream_units = 0;

  // A preview frame as the appsink callback delivers it.
  struct Preview {
    int width = 0;
    int height = 0;
    Vec<uint8_t> rgba;
    uint64_t counter = 0;  // bumped per frame; the toy polls it
  };

  DEF_INTERFACE(GStreamerElement, Runnable, run, "Run")
  void OnRun(std::unique_ptr<RunTask>& t) { obj->Start(t); }
  DEF_END(run);

  DEF_INTERFACE(GStreamerElement, LongRunning, running, "Running")
  void OnCancel() { obj->Stop(); }
  DEF_END(running);

  DEF_INTERFACE(GStreamerElement, NextArg, next, "Next")
  DEF_END(next);

  DEF_INTERFACE(GStreamerElement, StreamArgument, out_stream, "src")
  Str OnFormat() { return obj->OutFormat(); }
  StreamStats OnStats() { return obj->OutStats(); }
  void OnCanConnect(Interface end, Status& status) {
    Table::DefaultCanConnect(*this, end, status);
    if (OK(status)) obj->CanFeedStream(end, status);
  }
  void OnConnect(Interface end) { obj->OnOutStreamConnect(*this, end); }
  DEF_END(out_stream);

  DEF_INTERFACE(GStreamerElement, StreamInput, in_stream, "sink")
  DEF_END(in_stream);

  void Interfaces(const std::function<LoopControl(Interface)>& cb) override;

  GStreamerElement(StrView factory);
  GStreamerElement(const GStreamerElement&);
  ~GStreamerElement() override;

  StrView Name() const override { return factory; }
  Ptr<Object> Clone() const override { return MAKE_PTR(GStreamerElement, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  // Builds the hidden pipeline for the whole linked run and sets it PLAYING.
  // Reports errors on the member that caused them; consumes `task`
  // (LongRunning) only on success.
  void Start(std::unique_ptr<RunTask>& task);
  // Stops the whole chain this element runs in. Safe from any thread except
  // a GStreamer streaming thread.
  void Stop();

  // Stores a property value and applies it to the live element if running.
  void SetProp(StrView name, StrView value);
  Str GetProp(StrView name) const;

  // The negotiated caps of the port's pad while running, in GStreamer's own
  // notation; empty until known. Port 0 is the base "src" port.
  virtual Str OutFormat();
  Str PortFormat(int port);
  // Byte and buffer totals through the port's pad, from its pad probe.
  virtual StreamStats OutStats();
  StreamStats PortStats(int port);

  // The extra-port slot owning `table`, or -1 for the base port tables.
  int OutSlotOf(const Interface::Table* table) const;
  int InSlotOf(const Interface::Table* table) const;

  // Whether the port's gst pad currently exists on the live element.
  bool PortPadLive(int port);

  // Configures this object's GstElement at chain build, after the recipe
  // props are applied. `gst_element` is a GstElement*.
  virtual void ConfigureGstElement(void* gst_element) {}
  // Called with `mutex` held when this element detaches from its chain.
  virtual void OnChainStopped() {}

  // An out port's OnConnect: stores the target, maintains the downstream
  // peer's producer back-pointer, and rebuilds any running chain the change
  // touches.
  void OnOutStreamConnect(StreamArgument self, Interface end);

  // The link oracle: refuses ends whose pad template caps can never
  // intersect this element's source caps, naming both formats and any
  // converter factories that would bridge them.
  void CanFeedStream(Interface end, Status& status);

 private:
  // Reads the factory's pad templates and fills the port layout: pad names,
  // extra slot counts, and the per-instance slot tables.
  void InitPorts();
};

// ============================================================================
// Boundary blocks: the seam between a self-running GStreamer chain and
// Automat-driven control flow. Both are queue blocks - their meters show
// fill against capacity - and their Pull/Push signal steps one buffer across
// the boundary, starting the chain first when it is stopped.
// ============================================================================

// appsink ends a chain. Frames queue inside the element, backpressuring the
// chain when full; each Run pulls one out and holds it as an image the rest
// of Automat can consume (ImageProvider) - the tap mechanism for GStreamer.
struct AppSinkBoundary : GStreamerElement {
  static constexpr int kMaxBuffers = 8;

  // Guarded by GStreamerElement::mutex:
  sk_sp<SkImage> held;  // the last pulled frame
  uint64_t pulled_bytes = 0;
  uint64_t pulled_units = 0;

  std::atomic<int> queued{0};  // frames waiting inside the appsink

  DEF_INTERFACE(AppSinkBoundary, ImageProvider, image_provider, "Image")
  sk_sp<SkImage> GetImage() { return obj->Held(); }
  DEF_END(image_provider);

  DEF_INTERFACE(AppSinkBoundary, Signal, step, "Pull")
  void OnRun(std::unique_ptr<RunTask>& t) { obj->StepOne(); }
  DEF_END(step);

  INTERFACES(run, running, step, next, in_stream, image_provider);

  AppSinkBoundary() : GStreamerElement("appsink") {}
  AppSinkBoundary(const AppSinkBoundary& o) : GStreamerElement(o) {}

  Ptr<Object> Clone() const override { return MAKE_PTR(AppSinkBoundary, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  void ConfigureGstElement(void* gst_element) override;
  void OnChainStopped() override;

  sk_sp<SkImage> Held() const;
  // Starts the chain when stopped, then pulls one queued sample.
  void StepOne();
  // Pulls one queued sample into `held`; no-op when the queue is empty.
  void PullOne();
};

// appsrc heads a chain. Each Run reads the connected image and pushes it
// into the chain as one video buffer; the element's internal byte queue is
// the boundary's meter.
struct AppSrcBoundary : GStreamerElement {
  // Guarded by GStreamerElement::mutex:
  uint64_t pushed_bytes = 0;
  uint64_t pushed_units = 0;
  int last_width = 0;  // the caps follow the pushed image's size
  int last_height = 0;

  DEF_INTERFACE(AppSrcBoundary, InterfaceArgument<ImageProvider>, image, "Image")
  DEF_END(image);

  DEF_INTERFACE(AppSrcBoundary, Signal, step, "Push")
  void OnRun(std::unique_ptr<RunTask>& t) { obj->StepOne(); }
  DEF_END(step);

  INTERFACES(run, running, step, next, out_stream, image);

  AppSrcBoundary() : GStreamerElement("appsrc") {}
  AppSrcBoundary(const AppSrcBoundary& o) : GStreamerElement(o), image(o.image) {}

  Ptr<Object> Clone() const override { return MAKE_PTR(AppSrcBoundary, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  void ConfigureGstElement(void* gst_element) override;
  StreamStats OutStats() override;

  // Starts the chain when stopped, then pushes the connected image.
  void StepOne();
  // Reads the connected image and pushes it as one buffer.
  void PushOne();
};

}  // namespace automat::library
