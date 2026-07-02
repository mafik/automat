// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_gstreamer.hpp"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>

#include <thread>

#include "format.hpp"
#include "ui_beta.hpp"
#include "units.hpp"

namespace automat::library {

using ui::beta::Hash2;

// ============================================================================
// The GStreamer host: gst_init plus one GLib main loop thread that carries
// every pipeline's bus watch. Started lazily, never torn down.
// ============================================================================

struct GstHost {
  GMainContext* context;
  GMainLoop* loop;
  std::thread thread;

  GstHost() {
    gst_init(nullptr, nullptr);
    context = g_main_context_new();
    loop = g_main_loop_new(context, FALSE);
    thread = std::thread([this] {
      g_main_context_push_thread_default(context);
      g_main_loop_run(loop);
    });
    thread.detach();
  }
};

static GstHost& Host() {
  static GstHost host;
  return host;
}

// Runs `fn` on the host loop thread (where bus watches attach and detach).
static void OnHostThread(std::function<void()> fn) {
  auto* boxed = new std::function<void()>(std::move(fn));
  g_main_context_invoke(
      Host().context,
      [](gpointer data) -> gboolean {
        auto* fn = (std::function<void()>*)data;
        (*fn)();
        delete fn;
        return G_SOURCE_REMOVE;
      },
      boxed);
}

// ============================================================================
// GstChain: one hidden GstPipeline shared by a maximal run of linked
// elements. Built by Start on any member, torn down by Stop on any member.
// Streaming-thread callbacks (pad probes, the preview appsink, the bus
// watch) reference the chain, never the member objects, so a member can die
// while the others keep the chain alive.
// ============================================================================

struct GstChain {
  std::atomic<void*> pipeline{nullptr};    // GstElement*; the stopper takes it
  Vec<WeakPtr<GStreamerElement>> members;  // head..tail order
  WeakPtr<GStreamerElement> tail;          // whose face the preview feeds

  // Per-member counters, bumped by src-pad probes on streaming threads and
  // read through GStreamerElement::OutStats.
  struct Flow {
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> units{0};
  };
  std::unique_ptr<Flow[]> flows;

  // The most recent preview frame, written by the appsink callback.
  std::mutex preview_mutex;
  GStreamerElement::Preview preview;
};

// Stops the pipeline and detaches every member. Idempotent: only the caller
// that wins the pipeline exchange tears it down. Must not run on a GStreamer
// streaming thread (set_state to NULL joins them).
static void TeardownChain(GstChain& chain) {
  GstElement* pipe = (GstElement*)chain.pipeline.exchange(nullptr);
  if (pipe) {
    gst_element_set_state(pipe, GST_STATE_NULL);
    // The watch attach was posted to the host thread at start; posting the
    // removal there serializes the two.
    OnHostThread([pipe] {
      GstBus* bus = gst_element_get_bus(pipe);
      gst_bus_remove_watch(bus);
      gst_object_unref(bus);
      gst_object_unref(pipe);
    });
  }
  for (auto& weak : chain.members) {
    auto m = weak.Lock();
    if (!m) continue;
    {
      auto lock = std::lock_guard(m->mutex);
      if (m->chain.get() != &chain) continue;
      m->chain = nullptr;
      if (m->element) gst_object_unref(m->element);
      m->element = nullptr;
      m->chain_index = -1;
      m->preview_tail = false;
      m->state_word = "NULL";
      m->OnChainStopped();
    }
    if (m->running->IsRunning()) m->running->Done();
    m->WakeToys();
  }
}

// ============================================================================
// Object
// ============================================================================

GStreamerElement::GStreamerElement(StrView factory, StrView knob_prop)
    : factory(factory), knob_prop(knob_prop) {
  Host();
}

GStreamerElement::GStreamerElement(const GStreamerElement& o)
    : Object(o),
      factory(o.factory),
      knob_prop(o.knob_prop),
      props(o.props),
      run(o.run),
      next(o.next),
      out_stream(o.out_stream) {}

GStreamerElement::~GStreamerElement() { Stop(); }

void GStreamerElement::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  if (props.empty()) return;
  writer.Key("props");
  writer.StartObject();
  for (auto& [name, value] : props) {
    writer.Key(name.c_str());
    writer.String(value.data(), value.size());
  }
  writer.EndObject();
}

bool GStreamerElement::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key != "props") return false;
  Status status;
  auto lock = std::lock_guard(mutex);
  props.clear();
  for (auto& prop_name : ObjectView(d, status)) {
    Str value;
    d.Get(value, status);
    if (OK(status)) props.push_back({Str(prop_name), std::move(value)});
  }
  return true;
}

void GStreamerElement::SetProp(StrView name, StrView value) {
  {
    auto lock = std::lock_guard(mutex);
    bool found = false;
    for (auto& [prop_name, prop_value] : props) {
      if (prop_name == name) {
        prop_value = value;
        found = true;
        break;
      }
    }
    if (!found) props.push_back({Str(name), Str(value)});
    if (element) {
      gst_util_set_object_arg(G_OBJECT((GstElement*)element), Str(name).c_str(),
                              Str(value).c_str());
    }
  }
  WakeToys();
}

Str GStreamerElement::GetProp(StrView name) const {
  auto lock = std::lock_guard(mutex);
  for (auto& [prop_name, prop_value] : props) {
    if (prop_name == name) return prop_value;
  }
  return "";
}

Str GStreamerElement::OutFormat() {
  auto lock = std::lock_guard(mutex);
  if (!element) return "";
  GstPad* pad = gst_element_get_static_pad((GstElement*)element, "src");
  if (!pad) return "";
  Str result;
  if (GstCaps* caps = gst_pad_get_current_caps(pad)) {
    gchar* s = gst_caps_to_string(caps);
    result = s;
    g_free(s);
    gst_caps_unref(caps);
  }
  gst_object_unref(pad);
  return result;
}

// ============================================================================
// The link oracle: pad template caps decide whether two elements can ever
// negotiate, before any pipeline exists.
// ============================================================================

// Union of the factory's static pad template caps in one direction.
static GstCaps* FactoryCaps(GstElementFactory* factory, GstPadDirection dir) {
  GstCaps* caps = gst_caps_new_empty();
  for (const GList* l = gst_element_factory_get_static_pad_templates(factory); l; l = l->next) {
    auto* tmpl = (GstStaticPadTemplate*)l->data;
    if (tmpl->direction != dir) continue;
    caps = gst_caps_merge(caps, gst_static_pad_template_get_caps(tmpl));
  }
  return caps;
}

static GstCaps* FactoryCaps(const Str& factory_name, GstPadDirection dir) {
  GstElementFactory* factory = gst_element_factory_find(factory_name.c_str());
  if (!factory) return nullptr;
  GstCaps* caps = FactoryCaps(factory, dir);
  gst_object_unref(factory);
  return caps;
}

// The unique top-level media types of `caps` - GStreamer's own words for
// what kind of data flows ("video/x-raw", "audio/x-raw").
static Str MediaTypes(GstCaps* caps) {
  Str out;
  guint n = gst_caps_get_size(caps);
  for (guint i = 0; i < n; ++i) {
    const char* name = gst_structure_get_name(gst_caps_get_structure(caps, i));
    if (out.find(name) != Str::npos) continue;
    if (!out.empty()) out += " · ";
    out += name;
  }
  if (out.empty()) out = "ANY";
  return out;
}

// Converter factories able to bridge `src` to `sink`, best rank first, the
// way GStreamer itself would pick them.
static Str ConverterProposals(GstCaps* src, GstCaps* sink) {
  struct Candidate {
    guint rank;
    Str name;
  };
  Vec<Candidate> candidates;
  GList* features = gst_registry_get_feature_list(gst_registry_get(), GST_TYPE_ELEMENT_FACTORY);
  for (GList* l = features; l; l = l->next) {
    auto* factory = GST_ELEMENT_FACTORY(l->data);
    const gchar* klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
    if (!klass || !strstr(klass, "Converter")) continue;
    GstCaps* in = FactoryCaps(factory, GST_PAD_SINK);
    GstCaps* out = FactoryCaps(factory, GST_PAD_SRC);
    if (gst_caps_can_intersect(src, in) && gst_caps_can_intersect(out, sink)) {
      candidates.push_back({gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(factory)),
                            Str(GST_OBJECT_NAME(factory))});
    }
    gst_caps_unref(in);
    gst_caps_unref(out);
  }
  gst_plugin_feature_list_free(features);
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate& a, const Candidate& b) { return a.rank > b.rank; });
  Str out;
  for (auto& c : candidates) {
    if (!out.empty()) out += ", ";
    out += c.name;
  }
  return out;
}

void GStreamerElement::CanFeedStream(Interface end, Status& status) {
  auto* peer = dynamic_cast<GStreamerElement*>(end.object_ptr);
  if (!peer) return;  // Non-GStreamer stream inputs negotiate elsewhere.
  Host();
  GstCaps* src = FactoryCaps(factory, GST_PAD_SRC);
  GstCaps* sink = FactoryCaps(peer->factory, GST_PAD_SINK);
  if (src && sink && !gst_caps_can_intersect(src, sink)) {
    auto& msg = AppendErrorMessage(status);
    msg = f("{} does not fit {}", MediaTypes(src), MediaTypes(sink));
    if (Str adapters = ConverterProposals(src, sink); !adapters.empty()) {
      msg += f("\ninsert {}", adapters);
    }
  }
  if (src) gst_caps_unref(src);
  if (sink) gst_caps_unref(sink);
}

StreamStats GStreamerElement::OutStats() {
  auto lock = std::lock_guard(mutex);
  if (chain && chain_index >= 0) {
    auto& flow = chain->flows[chain_index];
    stream_bytes = flow.bytes.load(std::memory_order_relaxed);
    stream_units = flow.units.load(std::memory_order_relaxed);
  }
  return {.bytes = stream_bytes, .units = stream_units};
}

void GStreamerElement::OnOutStreamConnect(StreamArgument self, Interface end) {
  Ptr<GStreamerElement> old_peer;
  if (auto old = self.state->target.Lock()) {
    if (auto* o = dynamic_cast<GStreamerElement*>(old.Owner<Object>())) old_peer = o->AcquirePtr();
  }
  StreamArgument::Table::StreamOnConnect(self, end);
  GStreamerElement* new_peer = nullptr;
  if (dyn_cast_if_present<StreamInput>(end)) {
    new_peer = dynamic_cast<GStreamerElement*>(end.object_ptr);
  }

  // A running chain that the change touches rebuilds around the new
  // topology: stop it and schedule a fresh start from this element.
  bool was_running = false;
  auto stop_chain_of = [&](GStreamerElement& e) {
    std::shared_ptr<GstChain> c;
    {
      auto lock = std::lock_guard(e.mutex);
      c = e.chain;
    }
    if (c) {
      was_running = true;
      TeardownChain(*c);
    }
  };
  stop_chain_of(*this);
  if (old_peer) stop_chain_of(*old_peer);
  if (new_peer && new_peer != this) stop_chain_of(*new_peer);
  if (was_running) run->ScheduleRun();
}

// appsink calls this on a streaming thread; the frame is copied under the
// chain's preview mutex and the tail's toys are woken through wake_counter.
static GstFlowReturn OnPreviewSample(GstAppSink* sink, gpointer user_data) {
  auto& chain = **(std::shared_ptr<GstChain>*)user_data;
  GstSample* sample = gst_app_sink_pull_sample(sink);
  if (!sample) return GST_FLOW_OK;
  GstCaps* caps = gst_sample_get_caps(sample);
  GstBuffer* buffer = gst_sample_get_buffer(sample);
  GstVideoInfo info;
  if (caps && buffer && gst_video_info_from_caps(&info, caps)) {
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      int width = GST_VIDEO_INFO_WIDTH(&info);
      int height = GST_VIDEO_INFO_HEIGHT(&info);
      int stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
      {
        auto lock = std::lock_guard(chain.preview_mutex);
        auto& preview = chain.preview;
        preview.width = width;
        preview.height = height;
        preview.rgba.resize((size_t)width * height * 4);
        for (int y = 0; y < height; ++y) {
          memcpy(preview.rgba.data() + (size_t)y * width * 4, map.data + (size_t)y * stride,
                 (size_t)width * 4);
        }
        preview.counter++;
      }
      gst_buffer_unmap(buffer, &map);
      if (auto tail = chain.tail.Lock()) tail->WakeToys();
    }
  }
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

// Src-pad probe on every member; counts buffers and bytes leaving the
// element. user_data is the member's Flow slot, owned by the chain.
static GstPadProbeReturn OnSrcProbe(GstPad*, GstPadProbeInfo* info, gpointer user_data) {
  auto& flow = *(GstChain::Flow*)user_data;
  if (GstBuffer* buffer = gst_pad_probe_info_get_buffer(info)) {
    flow.bytes.fetch_add(gst_buffer_get_size(buffer), std::memory_order_relaxed);
    flow.units.fetch_add(1, std::memory_order_relaxed);
  }
  return GST_PAD_PROBE_OK;
}

static gboolean OnBusMessage(GstBus*, GstMessage* msg, gpointer user_data) {
  auto& chain = **(std::shared_ptr<GstChain>*)user_data;
  if (!chain.pipeline.load()) return G_SOURCE_CONTINUE;
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError* error = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(msg, &error, &debug);
      // The error lands on the member whose element failed; errors from the
      // hidden preview branch land on the tail, whose face the branch feeds.
      Ptr<GStreamerElement> failed;
      for (auto& weak : chain.members) {
        auto m = weak.Lock();
        if (!m) continue;
        bool match;
        {
          auto lock = std::lock_guard(m->mutex);
          match = m->element && GST_MESSAGE_SRC(msg) == (GstObject*)m->element;
        }
        if (match) {
          failed = std::move(m);
          break;
        }
      }
      if (!failed) failed = chain.tail.Lock();
      if (failed) {
        failed->ReportError(
            f("{}: {}", GST_OBJECT_NAME(msg->src), error ? error->message : "error"));
        failed->Stop();
      }
      if (error) g_error_free(error);
      g_free(debug);
      break;
    }
    case GST_MESSAGE_STATE_CHANGED: {
      void* pipe = chain.pipeline.load();
      if (pipe && GST_MESSAGE_SRC(msg) == GST_OBJECT(pipe)) {
        GstState new_state;
        gst_message_parse_state_changed(msg, nullptr, &new_state, nullptr);
        const char* word = gst_element_state_get_name(new_state);
        for (auto& weak : chain.members) {
          if (auto m = weak.Lock()) {
            {
              auto lock = std::lock_guard(m->mutex);
              m->state_word = word;
            }
            m->WakeToys();
          }
        }
      }
      break;
    }
    default:
      break;
  }
  return G_SOURCE_CONTINUE;
}

void GStreamerElement::Start(std::unique_ptr<RunTask>& task) {
  // One start at a time, process-wide: overlapping linked runs rebuild each
  // other, and the lock makes collect-then-build atomic.
  static std::mutex start_mutex;
  auto start_lock = std::lock_guard(start_mutex);
  {
    auto lock = std::lock_guard(mutex);
    if (chain) return;  // already running; STOP is the way to a restart
  }

  // Collect the linked run: up the producer back-pointers to the head, then
  // down the out_stream targets to the tail.
  Ptr<GStreamerElement> head = AcquirePtr();
  {
    Vec<GStreamerElement*> visited{head.get()};
    for (;;) {
      Ptr<GStreamerElement> up;
      if (auto* o = dynamic_cast<GStreamerElement*>(head->in_stream->Producer().get())) {
        up = o->AcquirePtr();
      }
      bool cycle = false;
      if (up) {
        for (auto* v : visited) cycle |= (v == up.get());
      }
      if (!up || cycle) break;
      visited.push_back(up.get());
      head = std::move(up);
    }
  }
  Vec<Ptr<GStreamerElement>> order;
  order.push_back(head);
  for (;;) {
    auto target = order.back()->out_stream->FindInterface();
    auto* next = dynamic_cast<GStreamerElement*>(target.Owner<Object>());
    if (!next) break;
    bool cycle = false;
    for (auto& m : order) cycle |= (m.get() == next);
    if (cycle) break;
    order.push_back(next->AcquirePtr());
  }

  // A member still running in an older chain restarts as part of this one.
  for (auto& m : order) {
    std::shared_ptr<GstChain> old_chain;
    {
      auto lock = std::lock_guard(m->mutex);
      old_chain = m->chain;
    }
    if (old_chain) TeardownChain(*old_chain);
  }

  int n = (int)order.size();
  GstElement* pipe = gst_pipeline_new(nullptr);
  Vec<GstElement*> elems;
  for (auto& m : order) {
    GstElement* elem = gst_element_factory_make(m->factory.c_str(), nullptr);
    if (!elem) {
      m->ReportError(f("{}: element factory not found", m->factory));
      gst_object_unref(pipe);
      return;
    }
    {
      auto lock = std::lock_guard(m->mutex);
      for (auto& [name, value] : m->props) {
        gst_util_set_object_arg(G_OBJECT(elem), name.c_str(), value.c_str());
      }
    }
    m->ConfigureGstElement(elem);
    gst_bin_add(GST_BIN(pipe), elem);
    elems.push_back(elem);
  }

  if (GstPad* sink_pad = gst_element_get_static_pad(elems[0], "sink")) {
    gst_object_unref(sink_pad);
    order[0]->ReportError(f("{}: sink is not connected", order[0]->factory));
    gst_object_unref(pipe);
    return;
  }
  for (int i = 0; i + 1 < n; ++i) {
    if (!gst_element_link(elems[i], elems[i + 1])) {
      order[i + 1]->ReportError(f("cannot link {} → {}", order[i]->factory, order[i + 1]->factory));
      gst_object_unref(pipe);
      return;
    }
  }

  auto chain_sp = std::make_shared<GstChain>();
  chain_sp->flows.reset(new GstChain::Flow[n]);
  for (auto& m : order) chain_sp->members.push_back(m->AcquireWeakPtr());
  chain_sp->tail = order.back()->AcquireWeakPtr();

  // The preview branch feeds the tail's face; a tail without a src pad (a
  // real sink) needs none. A tail whose src can never carry raw video gets a
  // fakesink instead, so the chain still reaches PLAYING and the face shows
  // the run state.
  bool video_preview = false;
  bool tail_has_src = false;
  if (GstPad* src = gst_element_get_static_pad(elems.back(), "src")) {
    tail_has_src = true;
    gst_object_unref(src);
    GstCaps* tail_caps = FactoryCaps(order.back()->factory, GST_PAD_SRC);
    GstCaps* raw_video = gst_caps_new_empty_simple("video/x-raw");
    video_preview = tail_caps && gst_caps_can_intersect(tail_caps, raw_video);
    gst_caps_unref(raw_video);
    if (tail_caps) gst_caps_unref(tail_caps);
  }
  if (video_preview) {
    GstElement* convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement* scale = gst_element_factory_make("videoscale", nullptr);
    GstElement* appsink = gst_element_factory_make("appsink", nullptr);
    if (!convert || !scale || !appsink) {
      order.back()->ReportError(
          "GStreamer base plugins are missing (videoconvert, videoscale, appsink)");
      if (convert) gst_object_unref(convert);
      if (scale) gst_object_unref(scale);
      if (appsink) gst_object_unref(appsink);
      gst_object_unref(pipe);
      return;
    }
    GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", "width",
                                        G_TYPE_INT, 320, "height", G_TYPE_INT, 240, nullptr);
    g_object_set(appsink, "caps", caps, "max-buffers", 1, "drop", TRUE, "sync", TRUE, nullptr);
    gst_caps_unref(caps);
    GstAppSinkCallbacks callbacks = {};
    callbacks.new_sample = OnPreviewSample;
    auto* box = new std::shared_ptr<GstChain>(chain_sp);
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, box,
                               [](gpointer d) { delete (std::shared_ptr<GstChain>*)d; });
    gst_bin_add_many(GST_BIN(pipe), convert, scale, appsink, nullptr);
    if (!gst_element_link_many(elems.back(), convert, scale, appsink, nullptr)) {
      order.back()->ReportError(f("{}: could not link the preview branch", order.back()->factory));
      gst_object_unref(pipe);
      return;
    }
  } else if (tail_has_src) {
    GstElement* sink = gst_element_factory_make("fakesink", nullptr);
    g_object_set(sink, "sync", TRUE, nullptr);
    gst_bin_add(GST_BIN(pipe), sink);
    if (!gst_element_link(elems.back(), sink)) {
      order.back()->ReportError(f("{}: could not attach a sink", order.back()->factory));
      gst_object_unref(pipe);
      return;
    }
  }

  for (int i = 0; i < n; ++i) {
    if (GstPad* src = gst_element_get_static_pad(elems[i], "src")) {
      gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, OnSrcProbe, &chain_sp->flows[i], nullptr);
      gst_object_unref(src);
    }
  }

  chain_sp->pipeline.store(pipe);
  for (int i = 0; i < n; ++i) {
    auto& m = order[i];
    {
      auto lock = std::lock_guard(m->mutex);
      m->chain = chain_sp;
      m->element = gst_object_ref(elems[i]);
      m->chain_index = i;
      m->preview_tail = (i == n - 1) && video_preview;
      // The element whose Run started the chain consumes the caller's task;
      // the other members synthesize their own.
      std::unique_ptr<RunTask> own;
      auto& consumed = m.get() == this ? task : own;
      if (!consumed) consumed = std::make_unique<RunTask>(m->AcquireWeakPtr(), &run_tbl);
      m->running->BeginLongRunning(std::move(consumed));
    }
    m->ClearOwnError();
  }

  GstBus* bus = gst_element_get_bus(pipe);
  auto* box = new std::shared_ptr<GstChain>(chain_sp);
  OnHostThread([bus, box] {
    gst_bus_add_watch_full(bus, G_PRIORITY_DEFAULT, OnBusMessage, box,
                           [](gpointer d) { delete (std::shared_ptr<GstChain>*)d; });
    gst_object_unref(bus);
  });

  gst_element_set_state(pipe, GST_STATE_PLAYING);
  for (auto& m : order) m->WakeToys();
}

void GStreamerElement::Stop() {
  std::shared_ptr<GstChain> ch;
  {
    auto lock = std::lock_guard(mutex);
    ch = chain;
  }
  if (ch) TeardownChain(*ch);
  {
    // TeardownChain reaches members through weak pointers, which cannot
    // resolve this object while it is being destroyed - clear directly.
    auto lock = std::lock_guard(mutex);
    if (chain) {
      chain = nullptr;
      if (element) gst_object_unref(element);
      element = nullptr;
      chain_index = -1;
      preview_tail = false;
      state_word = "NULL";
      OnChainStopped();
    }
  }
  if (running->IsRunning()) running->Done();
  WakeToys();
}

// ============================================================================
// AppSinkBoundary
// ============================================================================

// The appsink's new-sample callback only counts; pulling happens on Run. The
// box holds a weak reference so a dying object simply stops counting.
struct SinkSampleBox {
  WeakPtr<AppSinkBoundary> weak;
};

static GstFlowReturn OnBoundarySample(GstAppSink*, gpointer user_data) {
  auto& box = *(SinkSampleBox*)user_data;
  if (auto obj = box.weak.Lock()) {
    obj->queued.fetch_add(1, std::memory_order_relaxed);
    obj->WakeToys();
  }
  return GST_FLOW_OK;
}

void AppSinkBoundary::ConfigureGstElement(void* gst_element) {
  auto* sink = (GstElement*)gst_element;
  GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", nullptr);
  // drop=FALSE makes a full queue backpressure the chain - that is the
  // boundary's honest behavior; sync=FALSE because the pull side has no
  // clock.
  g_object_set(sink, "caps", caps, "max-buffers", kMaxBuffers, "drop", FALSE, "sync", FALSE,
               nullptr);
  gst_caps_unref(caps);
  GstAppSinkCallbacks callbacks = {};
  callbacks.new_sample = OnBoundarySample;
  auto* box = new SinkSampleBox{AcquireWeakPtr()};
  gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, box,
                             [](gpointer d) { delete (SinkSampleBox*)d; });
  queued.store(0, std::memory_order_relaxed);
}

void AppSinkBoundary::StepOne() {
  bool live;
  {
    auto lock = std::lock_guard(mutex);
    live = chain != nullptr;
  }
  if (!live) {
    // The chain synthesizes its own LongRunning tasks; the step task stays
    // with the caller so Next fires after this step.
    std::unique_ptr<RunTask> own;
    Start(own);
  }
  PullOne();
}

void AppSinkBoundary::OnChainStopped() { queued.store(0, std::memory_order_relaxed); }

sk_sp<SkImage> AppSinkBoundary::Held() const {
  auto lock = std::lock_guard(mutex);
  return held;
}

void AppSinkBoundary::PullOne() {
  auto lock = std::lock_guard(mutex);
  if (!element) return;
  GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK((GstElement*)element), 0);
  if (!sample) return;
  queued.fetch_sub(1, std::memory_order_relaxed);
  GstCaps* caps = gst_sample_get_caps(sample);
  GstBuffer* buffer = gst_sample_get_buffer(sample);
  GstVideoInfo info;
  if (caps && buffer && gst_video_info_from_caps(&info, caps)) {
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      int width = GST_VIDEO_INFO_WIDTH(&info);
      int height = GST_VIDEO_INFO_HEIGHT(&info);
      int stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
      auto image_info =
          SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
      held = SkImages::RasterFromData(image_info, SkData::MakeWithCopy(map.data, map.size),
                                      (size_t)stride);
      pulled_bytes += (uint64_t)width * height * 4;
      pulled_units += 1;
      gst_buffer_unmap(buffer, &map);
    }
  }
  gst_sample_unref(sample);
  WakeToys();
}

// ============================================================================
// AppSrcBoundary
// ============================================================================

void AppSrcBoundary::ConfigureGstElement(void* gst_element) {
  auto* src = (GstElement*)gst_element;
  // is-live skips preroll, so the chain reaches PLAYING before the first
  // push; do-timestamp stamps each buffer at push time.
  g_object_set(src, "is-live", TRUE, "do-timestamp", TRUE, "format", GST_FORMAT_TIME, "max-bytes",
               (guint64)(4 << 20), nullptr);
  auto lock = std::lock_guard(mutex);
  last_width = 0;
  last_height = 0;
}

void AppSrcBoundary::StepOne() {
  bool live;
  {
    auto lock = std::lock_guard(mutex);
    live = chain != nullptr;
  }
  if (!live) {
    // The chain synthesizes its own LongRunning tasks; the step task stays
    // with the caller so Next fires after this step.
    std::unique_ptr<RunTask> own;
    Start(own);
  }
  PushOne();
}

StreamStats AppSrcBoundary::OutStats() {
  auto lock = std::lock_guard(mutex);
  StreamStats stats{.bytes = pushed_bytes, .units = pushed_units};
  if (element) {
    auto* src = (GstElement*)element;
    stats.fill = gst_app_src_get_current_level_bytes(GST_APP_SRC(src));
    guint64 max_bytes = 0;
    g_object_get(src, "max-bytes", &max_bytes, nullptr);
    stats.capacity = max_bytes;
  }
  return stats;
}

void AppSrcBoundary::PushOne() {
  sk_sp<SkImage> img;
  {
    auto ip_ptr = image->FindInterface();
    ImageProvider ip(ip_ptr.Owner<Object>(), ip_ptr.Get());
    if (ip) img = ip.GetImage();
  }
  // An empty upstream is a normal transient in a step cycle (the producer
  // has not decoded anything yet); there is just nothing to push.
  if (!img) return;
  int width = img->width();
  int height = img->height();
  auto image_info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  Str error;
  {
    auto lock = std::lock_guard(mutex);
    if (!element) return;
    auto* src = (GstElement*)element;
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, (gsize)width * height * 4, nullptr);
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
      gst_buffer_unref(buffer);
      return;
    }
    bool ok = img->readPixels(nullptr, image_info, map.data, (size_t)width * 4, 0, 0);
    gst_buffer_unmap(buffer, &map);
    if (!ok) {
      gst_buffer_unref(buffer);
      error = "appsrc: the image pixels are not readable on the CPU";
    } else {
      if (width != last_width || height != last_height) {
        GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", "width",
                                            G_TYPE_INT, width, "height", G_TYPE_INT, height,
                                            "framerate", GST_TYPE_FRACTION, 0, 1, nullptr);
        gst_app_src_set_caps(GST_APP_SRC(src), caps);
        gst_caps_unref(caps);
        last_width = width;
        last_height = height;
      }
      if (gst_app_src_push_buffer(GST_APP_SRC(src), buffer) == GST_FLOW_OK) {
        pushed_bytes += (uint64_t)width * height * 4;
        pushed_units += 1;
      }
    }
  }
  if (!error.empty()) {
    ReportError(error);
  } else {
    WakeToys();
  }
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
constexpr float kKnobRow = 5.0_mm;
constexpr float kStatusRow = 5.0_mm;
constexpr float kBottomPad = 1.5_mm;
constexpr float kPlateH =
    kBand + kCreditRow + kPreviewH + 1_mm + kKnobRow + kStatusRow + kBottomPad;

constexpr uint32_t kSeed = 0x657;

// The values of a GEnum property, as GStreamer nicks, resolved through a
// throwaway element instance.
struct EnumValues {
  Vec<Str> nicks;
  Str current;
};

EnumValues IntrospectEnumProp(const Str& factory, const Str& prop) {
  EnumValues out;
  if (prop.empty()) return out;
  GstElement* elem = gst_element_factory_make(factory.c_str(), nullptr);
  if (!elem) return out;
  if (GParamSpec* pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(elem), prop.c_str())) {
    if (G_IS_PARAM_SPEC_ENUM(pspec)) {
      GEnumClass* enum_class = G_PARAM_SPEC_ENUM(pspec)->enum_class;
      for (guint i = 0; i < enum_class->n_values; ++i) {
        out.nicks.push_back(enum_class->values[i].value_nick);
      }
      gint value = 0;
      g_object_get(elem, prop.c_str(), &value, nullptr);
      if (GEnumValue* v = g_enum_get_value(enum_class, value)) {
        out.current = v->value_nick;
      }
    }
  }
  gst_object_unref(elem);
  return out;
}

Str FactoryKlass(const Str& factory_name) {
  Str out = "GStreamer";
  if (GstElementFactory* fac = gst_element_factory_find(factory_name.c_str())) {
    const gchar* klass = gst_element_factory_get_metadata(fac, GST_ELEMENT_METADATA_KLASS);
    if (klass) out = f("GStreamer · {}", klass);
    gst_object_unref(fac);
  }
  return out;
}

Str FactoryDescription(const Str& factory_name) {
  Str out;
  if (GstElementFactory* fac = gst_element_factory_find(factory_name.c_str())) {
    if (const gchar* d = gst_element_factory_get_metadata(fac, GST_ELEMENT_METADATA_DESCRIPTION)) {
      out = d;
    }
    gst_object_unref(fac);
  }
  return out;
}

// Greedy word wrap, drawn downward from the rect's top edge; lines that
// would leave the rect are dropped.
void DrawWrapped(SkCanvas& canvas, StrView text, Rect rect, float size, SkColor color,
                 uint32_t seed) {
  float line_step = size * 1.5f;
  float y = rect.top - line_step;
  size_t i = 0;
  while (i < text.size() && y >= rect.bottom) {
    size_t j = i;
    size_t last_fit = i;
    while (j <= text.size()) {
      if (j == text.size() || text[j] == ' ') {
        if (ui::beta::TextWidth(text.substr(i, j - i), size) > rect.Width()) break;
        last_fit = j;
        if (j == text.size()) break;
      }
      ++j;
    }
    if (last_fit == i) {  // a single word wider than the rect: hard cut
      last_fit = std::min(text.size(), i + 20);
    }
    ui::beta::DrawText(canvas, text.substr(i, last_fit - i), {rect.left, y}, size, color, false,
                       seed);
    i = last_fit;
    while (i < text.size() && text[i] == ' ') ++i;
    y -= line_step;
  }
}

}  // namespace

struct GStreamerToy : ui::beta::ObjectToy {
  std::unique_ptr<ui::beta::RunButton> button;

  // Tick-cached object state (UI thread only):
  Str factory_;
  Str credit_;
  Str description_;
  Str knob_prop_;
  EnumValues knob_values_;
  Str knob_value_;
  bool running_ = false;
  bool preview_tail_ = false;
  bool has_sink_ = false;
  Str state_word_;
  Str out_format_;
  uint64_t preview_counter_ = 0;
  uint64_t units_total_ = 0;
  sk_sp<SkImage> preview_image_;
  RateEstimator fps_;
  float fps_now_ = 0;
  Rect knob_rect_ = {};

  GStreamerToy(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) {
    button = std::make_unique<ui::beta::RunButton>(this, [this] { OnButton(); }, Seed(0x31));
    if (auto elem = LockObject<GStreamerElement>()) {
      factory_ = elem->factory;
      knob_prop_ = elem->knob_prop;
      credit_ = FactoryKlass(factory_);
      description_ = FactoryDescription(factory_);
      knob_values_ = IntrospectEnumProp(factory_, knob_prop_);
      knob_value_ = elem->GetProp(knob_prop_);
      if (knob_value_.empty()) knob_value_ = knob_values_.current;
      if (GstElementFactory* fac = gst_element_factory_find(factory_.c_str())) {
        for (const GList* t = gst_element_factory_get_static_pad_templates(fac); t; t = t->next) {
          auto* tmpl = (GstStaticPadTemplate*)t->data;
          has_sink_ |= tmpl->direction == GST_PAD_SINK;
        }
        gst_object_unref(fac);
      }
    }
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
    if (&arg ==
        static_cast<const Interface::Table*>(&decltype(GStreamerElement::out_stream)::tbl)) {
      return Vec2AndDir{.pos = Vec2(-kPlateW / 2 + 10_mm, -kPlateH / 2), .dir = -90_deg};
    }
    return ui::beta::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  void UpdateFromObject() {
    if (auto elem = LockObject<GStreamerElement>()) {
      std::shared_ptr<GstChain> ch;
      {
        auto lock = std::lock_guard(elem->mutex);
        running_ = elem->chain != nullptr;
        state_word_ = elem->state_word;
        preview_tail_ = elem->preview_tail;
        ch = elem->chain;
        if (ch && elem->chain_index >= 0) {
          units_total_ = ch->flows[elem->chain_index].units.load(std::memory_order_relaxed);
        }
      }
      out_format_ = elem->OutFormat();
      if (ch && preview_tail_) {
        auto lock = std::lock_guard(ch->preview_mutex);
        auto& p = ch->preview;
        if (p.counter != preview_counter_ && p.width > 0) {
          preview_counter_ = p.counter;
          auto info =
              SkImageInfo::Make(p.width, p.height, kRGBA_8888_SkColorType, kOpaque_SkAlphaType);
          preview_image_ = SkImages::RasterFromData(
              info, SkData::MakeWithCopy(p.rgba.data(), p.rgba.size()), (size_t)p.width * 4);
        }
      }
    }
    if (running_ != button->running || !button->enabled) {
      button->running = running_;
      button->enabled = true;
      button->WakeAnimation();
    }
  }

  Tock Tick(time::Timer& t) override {
    UpdateFromObject();
    fps_now_ = (float)fps_.Update(t.NowSeconds(), units_total_);
    if (running_) return Tock::Drawing;
    return Tock::Draw;
  }

  void OnButton() {
    if (auto elem = LockObject<GStreamerElement>()) {
      if (elem->running->IsRunning()) {
        elem->running->Cancel();
      } else {
        elem->run->ScheduleRun();
      }
    }
    WakeAnimation();
  }

  void CycleKnob() {
    if (knob_values_.nicks.empty()) return;
    int index = 0;
    for (int i = 0; i < (int)knob_values_.nicks.size(); ++i) {
      if (knob_values_.nicks[i] == knob_value_) {
        index = i;
        break;
      }
    }
    knob_value_ = knob_values_.nicks[(index + 1) % knob_values_.nicks.size()];
    if (auto elem = LockObject<GStreamerElement>()) {
      elem->SetProp(knob_prop_, knob_value_);
    }
    WakeAnimation();
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left && !knob_values_.nicks.empty()) {
      Vec2 pos = p.PositionWithin(*this);
      if (knob_rect_.Contains(pos)) {
        CycleKnob();
        return nullptr;
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void Draw(SkCanvas& canvas) const override {
    ui::beta::Panel(canvas, Rect::MakeCenterZero(kPlateW, kPlateH), factory_, ui::beta::kPurple,
                    ui::beta::State::Default, Seed(kSeed), true);

    {  // credit: the library and the factory's klass, in its own words
      float w = ui::beta::TextWidth(credit_, ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, credit_, {kPlateW / 2 - kSide - w, kPlateH / 2 - kBand - 1.6_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
    }

    // Preview: the tail of a running chain shows the flowing frames; a
    // running mid-chain element prints its negotiated output caps; an idle
    // element keeps its last frame, or teaches with the factory description.
    Rect preview_rect = Rect::MakeCornerZero(kPreviewW, kPreviewH)
                            .MoveBy({-kPreviewW / 2, kPlateH / 2 - kBand - kCreditRow - kPreviewH});
    {
      SkPaint bg;
      bg.setColor(ui::beta::kInk);
      canvas.drawRect(preview_rect.sk, bg);
      Rect text_rect = preview_rect.Outset(-1.2_mm);
      if (running_ && !preview_tail_) {
        if (!out_format_.empty()) {
          DrawWrapped(canvas, out_format_, text_rect, ui::beta::kMicroSize, ui::beta::kPaper,
                      Seed(kSeed));
        }
      } else if (preview_image_) {
        canvas.save();
        SkRect src = SkRect::Make(preview_image_->dimensions());
        SkMatrix m = SkMatrix::RectToRect(src, preview_rect.sk, SkMatrix::kCenter_ScaleToFit);
        m.preTranslate(0, preview_image_->height() / 2.f);
        m.preScale(1, -1);
        m.preTranslate(0, -preview_image_->height() / 2.f);
        canvas.concat(m);
        canvas.drawImage(preview_image_, 0, 0, SkSamplingOptions(SkFilterMode::kLinear), nullptr);
        canvas.restore();
      } else if (running_) {
        StrView hint = "waiting for the first frame";
        float w = ui::beta::TextWidth(hint, ui::beta::kMicroSize);
        ui::beta::DrawText(canvas, hint, {-w / 2, preview_rect.CenterY()}, ui::beta::kMicroSize,
                           ui::beta::kGray, false, Seed(kSeed));
      } else if (!description_.empty()) {
        DrawWrapped(canvas, description_, text_rect, ui::beta::kMicroSize, ui::beta::kGray,
                    Seed(kSeed));
      }
      SkPaint frame;
      frame.setStyle(SkPaint::kStroke_Style);
      frame.setStrokeWidth(ui::beta::kStroke);
      frame.setColor(ui::beta::kInk);
      canvas.drawRect(preview_rect.sk, frame);
    }

    {  // stream port labels, in GStreamer's own words
      ui::beta::DrawText(canvas, "src", {-kPlateW / 2 + 6.9_mm, -kPlateH / 2 + 0.8_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
      if (has_sink_) {
        StrView sink_label = "sink";
        float sink_w = ui::beta::TextWidth(sink_label, ui::beta::kMicroSize);
        ui::beta::DrawText(canvas, sink_label, {-sink_w / 2, kPlateH / 2 - kBand - 1.6_mm},
                           ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
      }
    }

    float knob_y = preview_rect.bottom - 1_mm - kKnobRow;
    if (!knob_prop_.empty() && !knob_values_.nicks.empty()) {
      // The enum property chip: click cycles through the GEnum's nicks.
      Str label = f("{}: {}", knob_prop_, knob_value_);
      float w = ui::beta::TextWidth(label, ui::beta::kMicroSize + 0.3_mm) + 3.2_mm;
      Rect chip{-kPlateW / 2 + kSide, knob_y, -kPlateW / 2 + kSide + w, knob_y + 3.6_mm};
      const_cast<GStreamerToy*>(this)->knob_rect_ = chip;
      uint32_t cs = Seed(Hash2(kSeed, 0x71));
      SkPath path = ui::beta::WonkyRoundRect(chip, 1.2_mm, ui::beta::kWonk * 0.8f, cs);
      ui::beta::HandShadow(canvas, path, {0.3_mm, -0.3_mm}, ui::beta::kShadow, cs);
      ui::beta::MisregFill(canvas, path, ui::beta::kPaper, cs);
      ui::beta::SketchyStroke(canvas, path, ui::beta::kInk, ui::beta::kStroke * 0.8f, cs, 1);
      ui::beta::DrawTextIn(canvas, label, chip, ui::beta::kMicroSize + 0.3_mm, ui::beta::kInk,
                           ui::beta::TextAlign::Center, false, cs);
    }

    {  // Status row: the pipeline state in GStreamer's own words, plus fps.
      float row_mid = -kPlateH / 2 + kBottomPad + kStatusRow * 0.5f;
      Str state_label = state_word_;
      SkColor color = running_ ? ui::beta::kLime : ui::beta::kGray;
      float w = ui::beta::TextWidth(state_label, ui::beta::kMicroSize + 0.3_mm) + 2.6_mm;
      float chip_left = -kPlateW / 2 + kSide;
      float chip_bottom = row_mid - 1.6_mm;
      Rect chip{chip_left, chip_bottom, chip_left + w, chip_bottom + 3.2_mm};
      uint32_t cs = Seed(Hash2(kSeed, 0xC2));
      SkPath path = ui::beta::WonkyRoundRect(chip, 1.2_mm, ui::beta::kWonk * 0.8f, cs);
      ui::beta::HandShadow(canvas, path, {0.3_mm, -0.3_mm}, ui::beta::kShadow, cs);
      ui::beta::MisregFill(canvas, path, color, cs);
      ui::beta::SketchyStroke(canvas, path, ui::beta::kInk, ui::beta::kStroke * 0.8f, cs, 1);
      ui::beta::DrawTextIn(canvas, state_label, chip, ui::beta::kMicroSize + 0.3_mm,
                           ui::beta::TextOn(color), ui::beta::TextAlign::Center, false, cs);
      if (running_ && fps_now_ > 0.1f) {
        ui::beta::DrawText(canvas, f("{:.1f} f/s", fps_now_), {chip.right + 2_mm, row_mid - 0.7_mm},
                           ui::beta::kMicroSize + 0.3_mm, ui::beta::kInk, false, Seed(kSeed));
      }
    }
    BakeChildren(canvas);
  }
};

std::unique_ptr<ObjectToy> GStreamerElement::MakeToy(ui::Widget* parent) {
  return std::make_unique<GStreamerToy>(parent, *this);
}

// ============================================================================
// Boundary toy: a compact queue block with an image area, a fill meter, the
// state chip, and a step chip that moves one buffer across the boundary.
// ============================================================================

namespace {

constexpr float kBImageH = 3.0_cm;
constexpr float kBMeterRow = 4.5_mm;
constexpr float kBPlateH =
    kBand + kCreditRow + kBImageH + 1_mm + kBMeterRow + kStatusRow + kBottomPad;

constexpr uint32_t kBSeed = 0x9A2;

}  // namespace

struct BoundaryToy : ui::beta::ObjectToy {
  bool is_sink;  // appsink pulls; appsrc pushes
  std::unique_ptr<ui::beta::RunButton> button;

  // Tick-cached object state (UI thread only):
  Str factory_;
  Str credit_;
  bool running_ = false;
  Str state_word_;
  uint64_t fill_ = 0;
  uint64_t capacity_ = 0;
  Str meter_label_;
  uint64_t steps_ = 0;
  sk_sp<SkImage> image_;
  Rect step_rect_ = {};

  BoundaryToy(ui::Widget* parent, Object& obj, bool is_sink)
      : ui::beta::ObjectToy(parent, obj), is_sink(is_sink) {
    button = std::make_unique<ui::beta::RunButton>(this, [this] { OnButton(); }, Seed(0x77));
    if (auto elem = LockObject<GStreamerElement>()) {
      factory_ = elem->factory;
      credit_ = FactoryKlass(factory_);
    }
    UpdateFromObject();
  }

  bool CenteredAtZero() const override { return true; }
  SkPath Shape() const override {
    return SkPath::RRect(RRect::MakeSimple(Rect::MakeCenterZero(kPlateW, kBPlateH), 3_mm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Shape().getBounds().makeOutset(4_mm, 4_mm);
  }
  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg ==
        static_cast<const Interface::Table*>(&decltype(GStreamerElement::out_stream)::tbl)) {
      return Vec2AndDir{.pos = Vec2(-kPlateW / 2 + 10_mm, -kBPlateH / 2), .dir = -90_deg};
    }
    return ui::beta::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  void UpdateFromObject() {
    if (is_sink) {
      if (auto sink = LockObject<AppSinkBoundary>()) {
        {
          auto lock = std::lock_guard(sink->mutex);
          running_ = sink->chain != nullptr;
          state_word_ = sink->state_word;
          steps_ = sink->pulled_units;
          image_ = sink->held;
        }
        fill_ = (uint64_t)std::max(0, sink->queued.load(std::memory_order_relaxed));
        capacity_ = AppSinkBoundary::kMaxBuffers;
        meter_label_ = f("{} / {} buffers", fill_, capacity_);
      }
    } else {
      if (auto src = LockObject<AppSrcBoundary>()) {
        {
          auto lock = std::lock_guard(src->mutex);
          running_ = src->chain != nullptr;
          state_word_ = src->state_word;
          steps_ = src->pushed_units;
        }
        auto stats = src->OutStats();
        fill_ = stats.fill;
        capacity_ = stats.capacity;
        meter_label_ = f("{} / {}", FormatBytes(fill_), FormatBytes(capacity_));
        auto ip_ptr = src->image->FindInterface();
        ImageProvider ip(ip_ptr.Owner<Object>(), ip_ptr.Get());
        image_ = ip ? ip.GetImage() : nullptr;
      }
    }
    if (running_ != button->running || !button->enabled) {
      button->running = running_;
      button->enabled = true;
      button->WakeAnimation();
    }
  }

  Tock Tick(time::Timer&) override {
    UpdateFromObject();
    if (running_) return Tock::Drawing;
    return Tock::Draw;
  }

  void OnButton() {
    if (auto elem = LockObject<GStreamerElement>()) {
      if (elem->running->IsRunning()) {
        elem->running->Cancel();
      } else {
        elem->run->ScheduleRun();
      }
    }
    WakeAnimation();
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      if (step_rect_.Contains(pos)) {
        if (is_sink) {
          if (auto sink = LockObject<AppSinkBoundary>()) sink->step->ScheduleRun();
        } else {
          if (auto src = LockObject<AppSrcBoundary>()) src->step->ScheduleRun();
        }
        WakeAnimation();
        return nullptr;
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void Draw(SkCanvas& canvas) const override {
    ui::beta::Panel(canvas, Rect::MakeCenterZero(kPlateW, kBPlateH), factory_, ui::beta::kPurple,
                    ui::beta::State::Default, Seed(kBSeed), true);

    {  // credit
      float w = ui::beta::TextWidth(credit_, ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, credit_, {kPlateW / 2 - kSide - w, kBPlateH / 2 - kBand - 1.6_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kBSeed));
    }

    // Image area: the last pulled frame (appsink) or the connected image
    // about to be pushed (appsrc).
    Rect image_rect = Rect::MakeCornerZero(kPreviewW, kBImageH)
                          .MoveBy({-kPreviewW / 2, kBPlateH / 2 - kBand - kCreditRow - kBImageH});
    {
      SkPaint bg;
      bg.setColor(ui::beta::kInk);
      canvas.drawRect(image_rect.sk, bg);
      if (image_) {
        canvas.save();
        SkRect src_rect = SkRect::Make(image_->dimensions());
        SkMatrix m = SkMatrix::RectToRect(src_rect, image_rect.sk, SkMatrix::kCenter_ScaleToFit);
        m.preTranslate(0, image_->height() / 2.f);
        m.preScale(1, -1);
        m.preTranslate(0, -image_->height() / 2.f);
        canvas.concat(m);
        canvas.drawImage(image_, 0, 0, SkSamplingOptions(SkFilterMode::kLinear), nullptr);
        canvas.restore();
      } else {
        StrView hint = is_sink ? StrView("Run pulls one frame out of the stream")
                               : StrView("connect an Image; Run pushes it");
        float w = ui::beta::TextWidth(hint, ui::beta::kMicroSize);
        ui::beta::DrawText(canvas, hint, {-w / 2, image_rect.CenterY()}, ui::beta::kMicroSize,
                           ui::beta::kGray, false, Seed(kBSeed));
      }
      SkPaint frame;
      frame.setStyle(SkPaint::kStroke_Style);
      frame.setStrokeWidth(ui::beta::kStroke);
      frame.setColor(ui::beta::kInk);
      canvas.drawRect(image_rect.sk, frame);
    }

    {  // Fill meter: the queue between the two drive models.
      float bar_y = image_rect.bottom - 1_mm - kBMeterRow + 0.6_mm;
      Rect bar{-kPlateW / 2 + kSide, bar_y, -kPlateW / 2 + kSide + 26_mm, bar_y + 3.0_mm};
      float fraction = capacity_ ? std::min(1.f, (float)fill_ / (float)capacity_) : 0.f;
      SkPaint bar_fill;
      bar_fill.setColor(SkColorSetRGB(0x99, 0xd9, 0xea));
      bar_fill.setAntiAlias(true);
      canvas.drawRect(
          SkRect::MakeLTRB(bar.left, bar.bottom, bar.left + bar.Width() * fraction, bar.top),
          bar_fill);
      SkPaint bar_stroke;
      bar_stroke.setStyle(SkPaint::kStroke_Style);
      bar_stroke.setStrokeWidth(ui::beta::kStroke * 0.8f);
      bar_stroke.setColor(ui::beta::kInk);
      canvas.drawRect(bar.sk, bar_stroke);
      ui::beta::DrawText(canvas, meter_label_, {bar.right + 2_mm, bar.bottom + 0.6_mm},
                         ui::beta::kMicroSize, ui::beta::kInk, false, Seed(kBSeed));
    }

    {  // Status row: state chip, step chip, step counter.
      float row_mid = -kBPlateH / 2 + kBottomPad + kStatusRow * 0.5f;
      SkColor color = running_ ? ui::beta::kLime : ui::beta::kGray;
      float w = ui::beta::TextWidth(state_word_, ui::beta::kMicroSize + 0.3_mm) + 2.6_mm;
      float chip_left = -kPlateW / 2 + kSide;
      float chip_bottom = row_mid - 1.6_mm;
      Rect chip{chip_left, chip_bottom, chip_left + w, chip_bottom + 3.2_mm};
      uint32_t cs = Seed(Hash2(kBSeed, 0xC2));
      SkPath path = ui::beta::WonkyRoundRect(chip, 1.2_mm, ui::beta::kWonk * 0.8f, cs);
      ui::beta::HandShadow(canvas, path, {0.3_mm, -0.3_mm}, ui::beta::kShadow, cs);
      ui::beta::MisregFill(canvas, path, color, cs);
      ui::beta::SketchyStroke(canvas, path, ui::beta::kInk, ui::beta::kStroke * 0.8f, cs, 1);
      ui::beta::DrawTextIn(canvas, state_word_, chip, ui::beta::kMicroSize + 0.3_mm,
                           ui::beta::TextOn(color), ui::beta::TextAlign::Center, false, cs);

      StrView step_label = is_sink ? StrView("pull one") : StrView("push one");
      float sw = ui::beta::TextWidth(step_label, ui::beta::kMicroSize + 0.3_mm) + 3.2_mm;
      Rect step_chip{chip.right + 2_mm, chip_bottom, chip.right + 2_mm + sw, chip_bottom + 3.6_mm};
      const_cast<BoundaryToy*>(this)->step_rect_ = step_chip;
      uint32_t ss = Seed(Hash2(kBSeed, 0x71));
      SkPath step_path = ui::beta::WonkyRoundRect(step_chip, 1.2_mm, ui::beta::kWonk * 0.8f, ss);
      ui::beta::HandShadow(canvas, step_path, {0.3_mm, -0.3_mm}, ui::beta::kShadow, ss);
      ui::beta::MisregFill(canvas, step_path, ui::beta::kPaper, ss);
      ui::beta::SketchyStroke(canvas, step_path, ui::beta::kInk, ui::beta::kStroke * 0.8f, ss, 1);
      ui::beta::DrawTextIn(canvas, step_label, step_chip, ui::beta::kMicroSize + 0.3_mm,
                           ui::beta::kInk, ui::beta::TextAlign::Center, false, ss);

      if (steps_ > 0) {
        Str count = f("{} {}", steps_, is_sink ? "pulled" : "pushed");
        ui::beta::DrawText(canvas, count, {step_chip.right + 2_mm, row_mid - 0.7_mm},
                           ui::beta::kMicroSize + 0.3_mm, ui::beta::kInk, false, Seed(kBSeed));
      }
    }

    {  // stream port labels
      if (is_sink) {
        StrView sink_label = "sink";
        float sink_w = ui::beta::TextWidth(sink_label, ui::beta::kMicroSize);
        ui::beta::DrawText(canvas, sink_label, {-sink_w / 2, kBPlateH / 2 - kBand - 1.6_mm},
                           ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kBSeed));
      } else {
        ui::beta::DrawText(canvas, "src", {-kPlateW / 2 + 6.9_mm, -kBPlateH / 2 + 0.8_mm},
                           ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kBSeed));
      }
    }
    BakeChildren(canvas);
  }
};

std::unique_ptr<ObjectToy> AppSinkBoundary::MakeToy(ui::Widget* parent) {
  return std::make_unique<BoundaryToy>(parent, *this, true);
}

std::unique_ptr<ObjectToy> AppSrcBoundary::MakeToy(ui::Widget* parent) {
  return std::make_unique<BoundaryToy>(parent, *this, false);
}

}  // namespace automat::library
