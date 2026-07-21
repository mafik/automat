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
#include "prototypes.hpp"
#include "ui_beta.hpp"
#include "ui_shelf_button.hpp"
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

Vec<GstFactoryInfo> ListGstFactories() {
  Host();
  Vec<GstFactoryInfo> out;
  GList* features = gst_registry_get_feature_list(gst_registry_get(), GST_TYPE_ELEMENT_FACTORY);
  for (GList* l = features; l; l = l->next) {
    auto* factory = GST_ELEMENT_FACTORY(l->data);
    GstFactoryInfo info{.name = GST_OBJECT_NAME(factory)};
    if (const gchar* klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS))
      info.klass = klass;
    out.push_back(std::move(info));
  }
  gst_plugin_feature_list_free(features);
  std::sort(out.begin(), out.end(),
            [](const GstFactoryInfo& a, const GstFactoryInfo& b) { return a.name < b.name; });
  return out;
}

// ============================================================================
// GstChain: one hidden GstPipeline shared by a maximal component of linked
// elements. Built by Start on any member, torn down by Stop on any member.
// Streaming-thread callbacks (pad probes, preview appsinks, pad-added, the
// bus watch) reference the chain, never the member objects, so a member can
// die while the others keep the chain alive.
// ============================================================================

struct GstChain {
  static constexpr int kFlowsPerMember = 1 + GStreamerElement::kMaxExtraPorts;

  std::atomic<void*> pipeline{nullptr};  // GstElement*; the stopper takes it
  Vec<WeakPtr<GStreamerElement>> members;
  Vec<void*> gst_elems;  // GstElement* per member, owned by the pipeline

  // Per-port counters, bumped by src-pad probes on streaming threads and
  // read through GStreamerElement::PortStats.
  struct Flow {
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> units{0};
  };
  std::unique_ptr<Flow[]> flows;  // members × kFlowsPerMember
  Flow& FlowAt(int member, int port) { return flows[member * kFlowsPerMember + port]; }

  // The most recent preview frame per member, written by preview appsinks.
  std::mutex preview_mutex;
  std::unique_ptr<GStreamerElement::Preview[]> previews;

  // A consumer pad waiting for a producer's sometimes pad to appear.
  struct PendingLink {
    void* producer_elem = nullptr;  // GstElement*
    Str producer_pad;
    void* consumer_elem = nullptr;
    Str consumer_pad;
    std::atomic<bool> linked{false};
  };
  std::unique_ptr<PendingLink[]> pending;  // fixed after Start; pad-added scans it
  int n_pending = 0;

  int MemberOf(void* gst_elem) const {
    for (int i = 0; i < (int)gst_elems.size(); ++i) {
      if (gst_elems[i] == gst_elem) return i;
    }
    return -1;
  }
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

// Expands a pad template name for one port: "src_%u" becomes "src_0",
// "src_1", ...; a plain name only expands for port 0.
static Str ExpandPadName(StrView name_template, int port) {
  size_t at = name_template.find('%');
  if (at == StrView::npos) return Str(name_template);
  return f("{}{}", name_template.substr(0, at), port);
}

void GStreamerElement::InitPorts() {
  n_extra_out = n_extra_in = 0;
  out_mode = in_mode = PadMode::kNone;
  out_pad_names.clear();
  in_pad_names.clear();
  GstElementFactory* fac = gst_element_factory_find(factory.c_str());
  if (!fac) return;
  // The primary template per direction; always beats sometimes beats request.
  const GstStaticPadTemplate* primary[2] = {};  // [0] = src, [1] = sink
  for (const GList* l = gst_element_factory_get_static_pad_templates(fac); l; l = l->next) {
    auto* tmpl = (GstStaticPadTemplate*)l->data;
    auto& slot = primary[tmpl->direction == GST_PAD_SINK];
    if (!slot || tmpl->presence < slot->presence) slot = tmpl;
  }
  gst_object_unref(fac);
  auto mode_of = [](GstPadPresence presence) {
    switch (presence) {
      case GST_PAD_ALWAYS:
        return PadMode::kAlways;
      case GST_PAD_REQUEST:
        return PadMode::kRequest;
      case GST_PAD_SOMETIMES:
        return PadMode::kSometimes;
    }
    return PadMode::kNone;
  };
  auto ports_of = [](const GstStaticPadTemplate& tmpl) {
    bool numbered = StrView(tmpl.name_template).find('%') != StrView::npos;
    if (tmpl.presence == GST_PAD_ALWAYS || !numbered) return 1;
    return 1 + kMaxExtraPorts;
  };
  if (primary[0]) {
    out_mode = mode_of(primary[0]->presence);
    int n = ports_of(*primary[0]);
    for (int i = 0; i < n; ++i)
      out_pad_names.push_back(ExpandPadName(primary[0]->name_template, i));
    n_extra_out = n - 1;
  }
  if (primary[1]) {
    in_mode = mode_of(primary[1]->presence);
    int n = ports_of(*primary[1]);
    for (int i = 0; i < n; ++i) in_pad_names.push_back(ExpandPadName(primary[1]->name_template, i));
    n_extra_in = n - 1;
  }

  for (int i = 0; i < n_extra_out; ++i) {
    auto& p = extra_out[i];
    p.Init(out_pad_names[i + 1], int(offsetof(GStreamerElement, extra_out) +
                                     i * sizeof(StreamOutSlot) + offsetof(StreamOutSlot, state)));
    p.table.can_connect = [](Argument self, Interface end, Status& status) {
      StreamArgument::Table::DefaultCanConnect(self, end, status);
      if (OK(status)) static_cast<GStreamerElement*>(self.object_ptr)->CanFeedStream(end, status);
    };
    p.table.on_connect = [](Argument self, Interface end) {
      auto* elem = static_cast<GStreamerElement*>(self.object_ptr);
      elem->OnOutStreamConnect(cast<StreamArgument>(self), end);
    };
    p.table.format = [](StreamArgument self) {
      auto* elem = static_cast<GStreamerElement*>(self.object_ptr);
      return elem->PortFormat(1 + elem->OutSlotOf(self.table_ptr));
    };
    p.table.stats = [](StreamArgument self) {
      auto* elem = static_cast<GStreamerElement*>(self.object_ptr);
      return elem->PortStats(1 + elem->OutSlotOf(self.table_ptr));
    };
  }
  for (int i = 0; i < n_extra_in; ++i) {
    auto& p = extra_in[i];
    p.Init(in_pad_names[i + 1], int(offsetof(GStreamerElement, extra_in) +
                                    i * sizeof(StreamInSlot) + offsetof(StreamInSlot, state)));
  }
}

void GStreamerElement::Interfaces(const std::function<LoopControl(Interface)>& cb) {
  if (cb(run.Bind()) == LoopControl::Break) return;
  if (cb(running.Bind()) == LoopControl::Break) return;
  if (cb(next.Bind()) == LoopControl::Break) return;
  if (cb(out_stream.Bind()) == LoopControl::Break) return;
  if (cb(in_stream.Bind()) == LoopControl::Break) return;
  for (int i = 0; i < n_extra_out; ++i) {
    if (cb(Interface(*this, extra_out[i].table)) == LoopControl::Break) return;
  }
  for (int i = 0; i < n_extra_in; ++i) {
    if (cb(Interface(*this, extra_in[i].table)) == LoopControl::Break) return;
  }
}

int GStreamerElement::OutSlotOf(const Interface::Table* table) const {
  for (int i = 0; i < n_extra_out; ++i) {
    if (table == &extra_out[i].table) return i;
  }
  return -1;
}

int GStreamerElement::InSlotOf(const Interface::Table* table) const {
  for (int i = 0; i < n_extra_in; ++i) {
    if (table == &extra_in[i].table) return i;
  }
  return -1;
}

GStreamerElement::GStreamerElement(StrView factory) : factory(factory) {
  Host();
  InitPorts();
}

GStreamerElement::GStreamerElement(const GStreamerElement& o)
    : Object(o),
      factory(o.factory),
      props(o.props),
      run(o.run),
      next(o.next),
      out_stream(o.out_stream) {
  InitPorts();
}

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

Str GStreamerElement::OutFormat() { return PortFormat(0); }

Str GStreamerElement::PortFormat(int port) {
  auto lock = std::lock_guard(mutex);
  if (!element || port < 0 || port >= (int)out_pad_names.size()) return "";
  GstPad* pad = gst_element_get_static_pad((GstElement*)element, out_pad_names[port].c_str());
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

bool GStreamerElement::PortPadLive(int port) {
  auto lock = std::lock_guard(mutex);
  if (!element || port < 0 || port >= (int)out_pad_names.size()) return false;
  GstPad* pad = gst_element_get_static_pad((GstElement*)element, out_pad_names[port].c_str());
  if (!pad) return false;
  gst_object_unref(pad);
  return true;
}

// ============================================================================
// Link compatibility: pad template caps decide whether two elements can ever
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

// The unique top-level media types of `caps` ("video/x-raw", "audio/x-raw").
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

// Converter factories able to bridge `src` to `sink`, highest GStreamer rank
// first.
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

StreamStats GStreamerElement::OutStats() { return PortStats(0); }

StreamStats GStreamerElement::PortStats(int port) {
  auto lock = std::lock_guard(mutex);
  if (chain && chain_index >= 0) {
    auto& flow = chain->FlowAt(chain_index, port);
    uint64_t bytes = flow.bytes.load(std::memory_order_relaxed);
    uint64_t units = flow.units.load(std::memory_order_relaxed);
    // Port 0 keeps its totals across Stop so the connection meters hold.
    if (port == 0) {
      stream_bytes = bytes;
      stream_units = units;
    }
    return {.bytes = bytes, .units = units};
  }
  if (port == 0) return {.bytes = stream_bytes, .units = stream_units};
  return {};
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

// One preview branch's identity: the chain plus the member it feeds.
struct PreviewBox {
  std::shared_ptr<GstChain> chain;
  int member;
};

// A preview appsink calls this on a streaming thread; the frame is copied
// under the chain's preview mutex and the member's toys are woken through
// wake_counter.
static GstFlowReturn OnPreviewSample(GstAppSink* sink, gpointer user_data) {
  auto& box = *(PreviewBox*)user_data;
  auto& chain = *box.chain;
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
        auto& preview = chain.previews[box.member];
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
      if (auto m = chain.members[box.member].Lock()) m->WakeToys();
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
      // The error lands on the member whose element failed; errors from a
      // hidden preview branch land on the member whose face it feeds, or
      // failing that on any live member.
      Ptr<GStreamerElement> failed;
      Ptr<GStreamerElement> preview_member;
      Ptr<GStreamerElement> any_member;
      for (auto& weak : chain.members) {
        auto m = weak.Lock();
        if (!m) continue;
        bool match;
        {
          auto lock = std::lock_guard(m->mutex);
          match = m->element && GST_MESSAGE_SRC(msg) == (GstObject*)m->element;
          if (!preview_member && m->preview_tail) preview_member = m;
        }
        if (!any_member) any_member = m;
        if (match) {
          failed = std::move(m);
          break;
        }
      }
      if (!failed) failed = preview_member ? preview_member : any_member;
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

// Attaches a terminal branch to an unconsumed source pad: a preview branch
// (queue ! videoconvert ! videoscale ! appsink) when the pad can carry raw
// video, else queue ! fakesink. The leading queue gives the branch its own
// streaming thread, so several branches (a tee's fan-out) preroll without
// starving each other. `live` syncs the new elements to the already-running
// pipeline (the pad-added path).
static bool AttachSinkBranch(std::shared_ptr<GstChain>& chain_sp, GstElement* pipe, int member,
                             GstPad* src_pad, bool video, bool live) {
  Vec<GstElement*> parts;
  GstElement* queue = gst_element_factory_make("queue", nullptr);
  if (!queue) return false;
  if (video) {
    GstElement* convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement* scale = gst_element_factory_make("videoscale", nullptr);
    GstElement* appsink = gst_element_factory_make("appsink", nullptr);
    if (!convert || !scale || !appsink) {
      gst_object_unref(queue);
      if (convert) gst_object_unref(convert);
      if (scale) gst_object_unref(scale);
      if (appsink) gst_object_unref(appsink);
      return false;
    }
    GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", "width",
                                        G_TYPE_INT, 320, "height", G_TYPE_INT, 240, nullptr);
    g_object_set(appsink, "caps", caps, "max-buffers", 1, "drop", TRUE, "sync", TRUE, nullptr);
    gst_caps_unref(caps);
    GstAppSinkCallbacks callbacks = {};
    callbacks.new_sample = OnPreviewSample;
    auto* box = new PreviewBox{chain_sp, member};
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, box,
                               [](gpointer d) { delete (PreviewBox*)d; });
    gst_bin_add_many(GST_BIN(pipe), queue, convert, scale, appsink, nullptr);
    if (!gst_element_link(queue, convert) || !gst_element_link(convert, scale) ||
        !gst_element_link(scale, appsink)) {
      return false;
    }
    parts = {queue, convert, scale, appsink};
  } else {
    GstElement* sink = gst_element_factory_make("fakesink", nullptr);
    if (!sink) {
      gst_object_unref(queue);
      return false;
    }
    g_object_set(sink, "sync", TRUE, nullptr);
    gst_bin_add_many(GST_BIN(pipe), queue, sink, nullptr);
    if (!gst_element_link(queue, sink)) return false;
    parts = {queue, sink};
  }
  GstPad* branch_sink = gst_element_get_static_pad(queue, "sink");
  bool ok = gst_pad_link(src_pad, branch_sink) == GST_PAD_LINK_OK;
  gst_object_unref(branch_sink);
  if (live) {
    for (auto* e : parts) gst_element_sync_state_with_parent(e);
  }
  if (ok && video) {
    if (auto m = chain_sp->members[member].Lock()) {
      auto lock = std::lock_guard(m->mutex);
      m->preview_tail = true;
    }
  }
  return ok;
}

static void DeleteChainBox(gpointer data, GClosure*) { delete (std::shared_ptr<GstChain>*)data; }

// A sometimes pad appeared on a streaming thread: probe it, link the
// consumer that waits for it, or terminate it so the stream keeps flowing.
static void OnPadAdded(GstElement* elem, GstPad* pad, gpointer user_data) {
  auto& chain_sp = *(std::shared_ptr<GstChain>*)user_data;
  auto& chain = *chain_sp;
  GstElement* pipe = (GstElement*)chain.pipeline.load();
  if (!pipe) return;
  if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) return;
  int member = chain.MemberOf(elem);
  if (member < 0) return;
  Str pad_name = GST_PAD_NAME(pad);
  auto m = chain.members[member].Lock();
  if (m) {
    for (int port = 0; port < (int)m->out_pad_names.size(); ++port) {
      if (m->out_pad_names[port] == pad_name) {
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, OnSrcProbe, &chain.FlowAt(member, port),
                          nullptr);
        break;
      }
    }
  }
  for (int i = 0; i < chain.n_pending; ++i) {
    auto& p = chain.pending[i];
    if (p.linked.load(std::memory_order_relaxed)) continue;
    if (p.producer_elem != elem || p.producer_pad != pad_name) continue;
    GstPad* sink_pad =
        gst_element_get_static_pad((GstElement*)p.consumer_elem, p.consumer_pad.c_str());
    if (!sink_pad) continue;
    bool ok = gst_pad_link(pad, sink_pad) == GST_PAD_LINK_OK;
    gst_object_unref(sink_pad);
    if (ok) {
      p.linked.store(true, std::memory_order_relaxed);
      if (m) m->WakeToys();
      return;
    }
  }
  GstCaps* caps = gst_pad_query_caps(pad, nullptr);
  GstCaps* raw = gst_caps_new_empty_simple("video/x-raw");
  bool video = caps && gst_caps_can_intersect(caps, raw);
  if (caps) gst_caps_unref(caps);
  gst_caps_unref(raw);
  AttachSinkBranch(chain_sp, pipe, member, pad, video, true);
  if (m) m->WakeToys();
}

void GStreamerElement::Start(std::unique_ptr<RunTask>& task) {
  // One start at a time, process-wide: overlapping linked runs rebuild each
  // other, and the lock makes collect-then-build atomic.
  static std::mutex start_mutex;
  auto start_lock = std::lock_guard(start_mutex);
  {
    auto lock = std::lock_guard(mutex);
    if (chain) return;  // already running; Stop before restarting
  }

  // Collect the linked component over every stream port, in both directions.
  Vec<Ptr<GStreamerElement>> order{AcquirePtr()};
  auto index_of = [&](Object* o) -> int {
    auto* e = dynamic_cast<GStreamerElement*>(o);
    if (!e) return -1;
    for (int i = 0; i < (int)order.size(); ++i) {
      if (order[i].get() == e) return i;
    }
    order.push_back(e->AcquirePtr());
    return (int)order.size() - 1;
  };
  for (int scan = 0; scan < (int)order.size(); ++scan) {
    auto m = order[scan];  // a copy: index_of may grow `order`
    index_of(m->in_stream->Producer().get());
    for (int i = 0; i < m->n_extra_in; ++i) {
      index_of(StreamInput(*m, m->extra_in[i].table).Producer().get());
    }
    index_of(m->out_stream->FindInterface().Owner<Object>());
    for (int i = 0; i < m->n_extra_out; ++i) {
      index_of(StreamArgument(*m, m->extra_out[i].table).FindInterface().Owner<Object>());
    }
  }
  int n = (int)order.size();

  // Stream edges, enumerated from the producer side.
  struct Edge {
    int producer, producer_port;
    int consumer, consumer_port;
  };
  Vec<Edge> edges;
  for (int i = 0; i < n; ++i) {
    auto& m = order[i];
    auto add_edge = [&](int port, NestedPtr<StreamInput::Table> target) {
      auto* c = dynamic_cast<GStreamerElement*>(target.Owner<Object>());
      if (!c) return;
      int ci = index_of(c);
      int slot = c->InSlotOf(target.Get());
      edges.push_back({i, port, ci, slot >= 0 ? 1 + slot : 0});
    };
    add_edge(0, m->out_stream->FindInterface());
    for (int s = 0; s < m->n_extra_out; ++s) {
      add_edge(1 + s, StreamArgument(*m, m->extra_out[s].table).FindInterface());
    }
  }

  // Every member with sink templates needs at least one connected input.
  for (int i = 0; i < n; ++i) {
    if (order[i]->in_mode == PadMode::kNone) continue;
    bool fed = false;
    for (auto& e : edges) fed |= (e.consumer == i);
    if (!fed) {
      order[i]->ReportError(f("{}: sink is not connected", order[i]->factory));
      return;
    }
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

  auto chain_sp = std::make_shared<GstChain>();
  chain_sp->flows.reset(new GstChain::Flow[n * GstChain::kFlowsPerMember]);
  chain_sp->previews.reset(new Preview[n]);
  chain_sp->pending.reset(new GstChain::PendingLink[edges.size()]);
  for (int i = 0; i < n; ++i) {
    chain_sp->members.push_back(order[i]->AcquireWeakPtr());
    chain_sp->gst_elems.push_back(elems[i]);
  }

  // Link the edges. Request pads are minted by their port's name; an edge
  // from a sometimes pad waits in `pending` until pad-added delivers it.
  for (auto& e : edges) {
    auto& P = order[e.producer];
    auto& C = order[e.consumer];
    Str out_name =
        e.producer_port < (int)P->out_pad_names.size() ? P->out_pad_names[e.producer_port] : Str();
    Str in_name =
        e.consumer_port < (int)C->in_pad_names.size() ? C->in_pad_names[e.consumer_port] : Str();
    if (out_name.empty() || in_name.empty()) {
      C->ReportError(f("cannot link {} → {}", P->factory, C->factory));
      gst_object_unref(pipe);
      return;
    }
    GstPad* sink_pad = C->in_mode == PadMode::kRequest
                           ? gst_element_request_pad_simple(elems[e.consumer], in_name.c_str())
                           : gst_element_get_static_pad(elems[e.consumer], in_name.c_str());
    if (!sink_pad) {
      C->ReportError(f("{}: no pad {}", C->factory, in_name));
      gst_object_unref(pipe);
      return;
    }
    if (P->out_mode == PadMode::kSometimes) {
      auto& p = chain_sp->pending[chain_sp->n_pending++];
      p.producer_elem = elems[e.producer];
      p.producer_pad = out_name;
      p.consumer_elem = elems[e.consumer];
      p.consumer_pad = in_name;
      gst_object_unref(sink_pad);
      continue;
    }
    GstPad* src_pad = P->out_mode == PadMode::kRequest
                          ? gst_element_request_pad_simple(elems[e.producer], out_name.c_str())
                          : gst_element_get_static_pad(elems[e.producer], out_name.c_str());
    bool ok = src_pad && gst_pad_link(src_pad, sink_pad) == GST_PAD_LINK_OK;
    if (src_pad) gst_object_unref(src_pad);
    gst_object_unref(sink_pad);
    if (!ok) {
      C->ReportError(f("cannot link {} → {}", P->factory, C->factory));
      gst_object_unref(pipe);
      return;
    }
  }

  // Every unconsumed always-src pad terminates in a preview branch (raw
  // video) or a fakesink, so the chain reaches PLAYING and the face shows
  // what flows. A request-src member with no linked output mints one pad for
  // the same treatment; unconsumed sometimes pads terminate from pad-added.
  for (int i = 0; i < n; ++i) {
    auto& m = order[i];
    if (m->out_mode == PadMode::kNone || m->out_mode == PadMode::kSometimes) continue;
    bool any_out = false;
    for (auto& e : edges) any_out |= (e.producer == i);
    if (any_out) continue;
    GstPad* src = m->out_mode == PadMode::kRequest
                      ? gst_element_request_pad_simple(elems[i], m->out_pad_names[0].c_str())
                      : gst_element_get_static_pad(elems[i], m->out_pad_names[0].c_str());
    if (!src) continue;
    GstCaps* src_caps = FactoryCaps(m->factory, GST_PAD_SRC);
    GstCaps* raw = gst_caps_new_empty_simple("video/x-raw");
    bool video = src_caps && gst_caps_can_intersect(src_caps, raw);
    gst_caps_unref(raw);
    if (src_caps) gst_caps_unref(src_caps);
    bool ok = AttachSinkBranch(chain_sp, pipe, i, src, video, false);
    gst_object_unref(src);
    if (!ok) {
      m->ReportError(f("{}: could not attach a sink", m->factory));
      gst_object_unref(pipe);
      return;
    }
  }

  // Byte and buffer probes on every existing out pad; sometimes pads get
  // theirs from pad-added.
  for (int i = 0; i < n; ++i) {
    auto& m = order[i];
    for (int port = 0; port < (int)m->out_pad_names.size(); ++port) {
      if (GstPad* src = gst_element_get_static_pad(elems[i], m->out_pad_names[port].c_str())) {
        gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, OnSrcProbe, &chain_sp->FlowAt(i, port),
                          nullptr);
        gst_object_unref(src);
      }
    }
  }

  chain_sp->pipeline.store(pipe);

  for (int i = 0; i < n; ++i) {
    if (order[i]->out_mode != PadMode::kSometimes) continue;
    auto* box = new std::shared_ptr<GstChain>(chain_sp);
    g_signal_connect_data(elems[i], "pad-added", G_CALLBACK(OnPadAdded), box, &DeleteChainBox,
                          (GConnectFlags)0);
  }

  for (int i = 0; i < n; ++i) {
    auto& m = order[i];
    {
      auto lock = std::lock_guard(m->mutex);
      m->chain = chain_sp;
      m->element = gst_object_ref(elems[i]);
      m->chain_index = i;
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
// box holds a weak reference so a dying object stops counting.
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
  // drop=FALSE makes a full queue backpressure the chain; sync=FALSE because
  // the pull side has no clock.
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
  // has not decoded anything yet).
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
constexpr float kPropRow = 5.0_mm;
constexpr float kStatusRow = 5.0_mm;
constexpr float kBottomPad = 1.5_mm;

constexpr uint32_t kSeed = 0x657;

// A writable element property the face edits with an instrument: a GEnum as
// a chip that cycles its nicks, a boolean as a checkbox, a bounded number as
// a slider. Everything else stays recipe-only.
struct PropInfo {
  enum Kind { kEnum, kBool, kNumber };
  Kind kind;
  Str name;
  Vec<Str> nicks;           // kEnum: the GEnum's nicks, in order
  double min = 0, max = 0;  // kNumber
  bool integer = false;     // kNumber: whole steps
  Str def;                  // the element's default, formatted for display
};

Str FormatPropNumber(double v, bool integer) {
  if (integer) return f("{}", (int64_t)llround(v));
  return f("{:.2f}", v);
}

bool NumericRange(GstElement* elem, GParamSpec* pspec, double& lo, double& hi, double& val,
                  bool& integer) {
  const char* name = g_param_spec_get_name(pspec);
  if (G_IS_PARAM_SPEC_INT(pspec)) {
    auto* p = G_PARAM_SPEC_INT(pspec);
    gint v;
    g_object_get(elem, name, &v, nullptr);
    lo = p->minimum, hi = p->maximum, val = v, integer = true;
  } else if (G_IS_PARAM_SPEC_UINT(pspec)) {
    auto* p = G_PARAM_SPEC_UINT(pspec);
    guint v;
    g_object_get(elem, name, &v, nullptr);
    lo = p->minimum, hi = p->maximum, val = v, integer = true;
  } else if (G_IS_PARAM_SPEC_INT64(pspec)) {
    auto* p = G_PARAM_SPEC_INT64(pspec);
    gint64 v;
    g_object_get(elem, name, &v, nullptr);
    lo = (double)p->minimum, hi = (double)p->maximum, val = (double)v, integer = true;
  } else if (G_IS_PARAM_SPEC_UINT64(pspec)) {
    auto* p = G_PARAM_SPEC_UINT64(pspec);
    guint64 v;
    g_object_get(elem, name, &v, nullptr);
    lo = (double)p->minimum, hi = (double)p->maximum, val = (double)v, integer = true;
  } else if (G_IS_PARAM_SPEC_FLOAT(pspec)) {
    auto* p = G_PARAM_SPEC_FLOAT(pspec);
    gfloat v;
    g_object_get(elem, name, &v, nullptr);
    lo = p->minimum, hi = p->maximum, val = v, integer = false;
  } else if (G_IS_PARAM_SPEC_DOUBLE(pspec)) {
    auto* p = G_PARAM_SPEC_DOUBLE(pspec);
    gdouble v;
    g_object_get(elem, name, &v, nullptr);
    lo = p->minimum, hi = p->maximum, val = v, integer = false;
  } else {
    return false;
  }
  return true;
}

// The first few instrument-shaped properties, resolved through a throwaway
// element. Own-class properties come first (the element author's declaration
// order), then base-class ones; GstObject/GstElement plumbing is skipped.
Vec<PropInfo> IntrospectProps(const Str& factory) {
  constexpr int kMaxProps = 4;
  Vec<PropInfo> out;
  GstElement* elem = gst_element_factory_make(factory.c_str(), nullptr);
  if (!elem) return out;
  GType own = G_OBJECT_TYPE(elem);
  guint n = 0;
  GParamSpec** specs = g_object_class_list_properties(G_OBJECT_GET_CLASS(elem), &n);
  Vec<GParamSpec*> ordered;
  for (guint i = 0; i < n; ++i)
    if (specs[i]->owner_type == own) ordered.push_back(specs[i]);
  for (guint i = 0; i < n; ++i)
    if (specs[i]->owner_type != own) ordered.push_back(specs[i]);
  for (GParamSpec* pspec : ordered) {
    if ((int)out.size() >= kMaxProps) break;
    if (!(pspec->flags & G_PARAM_WRITABLE) || !(pspec->flags & G_PARAM_READABLE)) continue;
    if (pspec->flags & (G_PARAM_CONSTRUCT_ONLY | G_PARAM_DEPRECATED)) continue;
    if (pspec->owner_type == GST_TYPE_OBJECT || pspec->owner_type == GST_TYPE_ELEMENT) continue;
    PropInfo info;
    info.name = g_param_spec_get_name(pspec);
    if (G_IS_PARAM_SPEC_ENUM(pspec)) {
      info.kind = PropInfo::kEnum;
      GEnumClass* ec = G_PARAM_SPEC_ENUM(pspec)->enum_class;
      for (guint v = 0; v < ec->n_values; ++v) info.nicks.push_back(ec->values[v].value_nick);
      gint value = 0;
      g_object_get(elem, info.name.c_str(), &value, nullptr);
      if (GEnumValue* ev = g_enum_get_value(ec, value)) info.def = ev->value_nick;
    } else if (G_IS_PARAM_SPEC_BOOLEAN(pspec)) {
      info.kind = PropInfo::kBool;
      gboolean value = FALSE;
      g_object_get(elem, info.name.c_str(), &value, nullptr);
      info.def = value ? "true" : "false";
    } else if (double lo = 0, hi = 0, val = 0;
               NumericRange(elem, pspec, lo, hi, val, info.integer)) {
      // An unbounded range (a G_MAXINT limit, a nanosecond count) makes a
      // useless slider; such properties stay recipe-only.
      if (hi - lo > 1e6) continue;
      info.kind = PropInfo::kNumber;
      info.min = lo;
      info.max = hi;
      info.def = FormatPropNumber(val, info.integer);
    } else {
      continue;
    }
    out.push_back(std::move(info));
  }
  g_free(specs);
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

struct GStreamerToy;

// Dragging along a property's slider band; the value tracks the pointer.
struct PropSliderDrag : Action {
  MortalPtr<GStreamerToy> widget;
  int index;
  PropSliderDrag(ui::Pointer& p, GStreamerToy& w, int index);
  void Update() override;
};

struct GStreamerToy : ui::beta::ObjectToy {
  std::unique_ptr<ui::beta::RunButton> button;

  // Tick-cached object state (UI thread only):
  Str factory_;
  Str credit_;
  Str description_;
  Vec<PropInfo> prop_infos_;
  Vec<Str> prop_values_;  // recipe override or the element default
  Vec<Rect> prop_rects_;  // instrument hit areas, in face coordinates
  float plate_h_;
  bool running_ = false;
  bool preview_tail_ = false;
  bool has_sink_ = false;
  bool has_src_ = false;
  GStreamerElement::PadMode out_mode_ = GStreamerElement::PadMode::kNone;
  int n_extra_out_ = 0;
  int n_extra_in_ = 0;
  Vec<const Interface::Table*> out_slot_tables_;  // anchor identities; never dereferenced
  Vec<Str> out_port_names_;                       // gst pad names, base port first
  Vec<Str> in_port_names_;
  Vec<bool> out_slot_connected_;
  Vec<bool> out_port_live_;  // per port, base first
  Str state_word_;
  Str out_format_;
  uint64_t preview_counter_ = 0;
  uint64_t units_total_ = 0;
  sk_sp<SkImage> preview_image_;
  RateEstimator fps_;
  float fps_now_ = 0;

  GStreamerToy(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) {
    button = std::make_unique<ui::beta::RunButton>(this, [this] { OnButton(); }, Seed(0x31));
    if (auto elem = LockObject<GStreamerElement>()) {
      factory_ = elem->factory;
      credit_ = FactoryKlass(factory_);
      description_ = FactoryDescription(factory_);
      prop_infos_ = IntrospectProps(factory_);
      prop_values_.resize(prop_infos_.size());
      prop_rects_.resize(prop_infos_.size());
      has_src_ = !elem->out_pad_names.empty();
      has_sink_ = !elem->in_pad_names.empty();
      out_mode_ = elem->out_mode;
      n_extra_out_ = elem->n_extra_out;
      n_extra_in_ = elem->n_extra_in;
      out_port_names_ = elem->out_pad_names;
      in_port_names_ = elem->in_pad_names;
      for (int i = 0; i < n_extra_out_; ++i) out_slot_tables_.push_back(&elem->extra_out[i].table);
      out_slot_connected_.resize(n_extra_out_);
      out_port_live_.resize(out_port_names_.size());
    }
    plate_h_ = kBand + kCreditRow + kPreviewH + 1_mm + prop_infos_.size() * kPropRow + kStatusRow +
               kBottomPad;
    UpdateFromObject();
  }

  float OutSlotX(int slot) const { return kPlateW / 2 - 8_mm - slot * 9_mm; }
  float InPortX(int port) const { return -kPlateW / 2 + 10_mm + port * 9_mm; }

  bool CenteredAtZero() const override { return true; }
  SkPath Shape() const override {
    return SkPath::RRect(RRect::MakeSimple(Rect::MakeCenterZero(kPlateW, plate_h_), 3_mm).sk);
  }
  Optional<Rect> DrawBounds() const override { return Shape().getBounds().makeOutset(4_mm, 4_mm); }
  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg ==
        static_cast<const Interface::Table*>(&decltype(GStreamerElement::out_stream)::tbl)) {
      return Vec2AndDir{.pos = Vec2(-kPlateW / 2 + 10_mm, -plate_h_ / 2), .dir = -90_deg};
    }
    for (int i = 0; i < n_extra_out_; ++i) {
      if (&arg == out_slot_tables_[i]) {
        return Vec2AndDir{.pos = Vec2(OutSlotX(i), -plate_h_ / 2), .dir = -90_deg};
      }
    }
    return ui::beta::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  // With extra input ports, incoming cables aim at the port row instead of
  // the default bounding-box anchors.
  void ConnectionPositions(Vec<Vec2AndDir>& out_positions) const override {
    if (n_extra_in_ == 0) return ObjectToy::ConnectionPositions(out_positions);
    for (int i = 0; i <= n_extra_in_; ++i) {
      out_positions.push_back(Vec2AndDir{.pos = Vec2(InPortX(i), plate_h_ / 2), .dir = -90_deg});
    }
  }

  void UpdateFromObject() {
    if (auto elem = LockObject<GStreamerElement>()) {
      std::shared_ptr<GstChain> ch;
      int member = -1;
      {
        auto lock = std::lock_guard(elem->mutex);
        running_ = elem->chain != nullptr;
        state_word_ = elem->state_word;
        preview_tail_ = elem->preview_tail;
        ch = elem->chain;
        member = elem->chain_index;
        if (ch && member >= 0) {
          units_total_ = ch->FlowAt(member, 0).units.load(std::memory_order_relaxed);
        }
      }
      out_format_ = elem->OutFormat();
      for (int i = 0; i < (int)prop_infos_.size(); ++i) {
        prop_values_[i] = elem->GetProp(prop_infos_[i].name);
        if (prop_values_[i].empty()) prop_values_[i] = prop_infos_[i].def;
      }
      for (int i = 0; i < n_extra_out_; ++i) {
        out_slot_connected_[i] = StreamArgument(*elem, elem->extra_out[i].table).IsConnected();
      }
      for (int port = 0; port < (int)out_port_live_.size(); ++port) {
        out_port_live_[port] = elem->PortPadLive(port);
      }
      if (ch && preview_tail_ && member >= 0) {
        auto lock = std::lock_guard(ch->preview_mutex);
        auto& p = ch->previews[member];
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

  void SetPropValue(int index, Str value) {
    prop_values_[index] = value;
    if (auto elem = LockObject<GStreamerElement>()) {
      elem->SetProp(prop_infos_[index].name, value);
    }
    WakeAnimation();
  }

  void CycleProp(int index) {
    auto& nicks = prop_infos_[index].nicks;
    int at = 0;
    for (int i = 0; i < (int)nicks.size(); ++i) {
      if (nicks[i] == prop_values_[index]) at = i;
    }
    SetPropValue(index, nicks[(at + 1) % nicks.size()]);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      for (int i = 0; i < (int)prop_infos_.size(); ++i) {
        if (!prop_rects_[i].Contains(pos)) continue;
        switch (prop_infos_[i].kind) {
          case PropInfo::kEnum:
            CycleProp(i);
            return nullptr;
          case PropInfo::kBool:
            SetPropValue(i, prop_values_[i] == "true" ? "false" : "true");
            return nullptr;
          case PropInfo::kNumber:
            return std::make_unique<PropSliderDrag>(p, *this, i);
        }
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void Draw(SkCanvas& canvas) const override {
    ui::beta::Panel(canvas, Rect::MakeCenterZero(kPlateW, plate_h_), factory_, ui::beta::kPurple,
                    ui::beta::State::Default, Seed(kSeed), true);

    {  // credit: the library and the factory's klass
      float w = ui::beta::TextWidth(credit_, ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, credit_, {kPlateW / 2 - kSide - w, plate_h_ / 2 - kBand - 1.6_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
    }

    // Preview: a member with a preview branch shows the flowing frames; a
    // running member without one prints its negotiated output caps; an idle
    // element keeps its last frame, or shows the factory description.
    Rect preview_rect =
        Rect::MakeCornerZero(kPreviewW, kPreviewH)
            .MoveBy({-kPreviewW / 2, plate_h_ / 2 - kBand - kCreditRow - kPreviewH});
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

    {  // stream ports, named with the factory's own pad names
      if (has_src_) {
        float w = ui::beta::TextWidth(out_port_names_[0], ui::beta::kMicroSize);
        ui::beta::DrawText(canvas, out_port_names_[0],
                           {-kPlateW / 2 + 10_mm - w / 2, -plate_h_ / 2 + 0.8_mm},
                           ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
      }
      if (has_sink_) {
        if (n_extra_in_ == 0) {
          StrView sink_label = in_port_names_[0];
          float sink_w = ui::beta::TextWidth(sink_label, ui::beta::kMicroSize);
          ui::beta::DrawText(canvas, sink_label, {-sink_w / 2, plate_h_ / 2 - kBand - 1.6_mm},
                             ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
        } else {
          // The input port row: cables land at these positions from above.
          for (int i = 0; i < (int)in_port_names_.size(); ++i) {
            float w = ui::beta::TextWidth(in_port_names_[i], ui::beta::kMicroSize);
            ui::beta::DrawText(canvas, in_port_names_[i],
                               {InPortX(i) - w / 2, plate_h_ / 2 - kBand - 1.6_mm},
                               ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
          }
        }
      }
      // Extra output sockets: connected and live ones in full; one more
      // available socket as a plus (request) or a faint one (sometimes).
      int shown = 0;
      for (int i = 0; i < n_extra_out_; ++i) {
        if (out_slot_connected_[i] || out_port_live_[1 + i]) shown = i + 1;
      }
      if (shown < n_extra_out_) ++shown;
      bool request = out_mode_ == GStreamerElement::PadMode::kRequest;
      for (int i = 0; i < shown; ++i) {
        bool active = out_slot_connected_[i] || out_port_live_[1 + i];
        Vec2 c{OutSlotX(i), -plate_h_ / 2 + 1.4_mm};
        uint32_t ps = Seed(Hash2(kSeed, 0x90 + (uint32_t)i));
        ui::beta::Port(canvas, c, 1.4_mm, true, ui::beta::kBlue, active,
                       active ? ui::beta::State::Default : ui::beta::State::Disabled, ps);
        if (!active && request) {
          ui::beta::DrawText(canvas, "+", {c.x + 1.9_mm, c.y - 0.9_mm}, ui::beta::kMicroSize,
                             ui::beta::kInkSoft, false, ps);
        }
        if (active) {
          float w = ui::beta::TextWidth(out_port_names_[1 + i], ui::beta::kMicroSize);
          ui::beta::DrawText(canvas, out_port_names_[1 + i], {c.x - w / 2, c.y + 2.2_mm},
                             ui::beta::kMicroSize, ui::beta::kInkSoft, false, ps);
        }
      }
    }

    // Property instruments, one row each: an enum chip, a checkbox, or a
    // labeled slider band.
    float row_top = preview_rect.bottom - 1_mm;
    for (int i = 0; i < (int)prop_infos_.size(); ++i) {
      const PropInfo& info = prop_infos_[i];
      const Str& value = prop_values_[i];
      uint32_t cs = Seed(Hash2(kSeed, 0x71 + (uint32_t)i));
      auto* rects = const_cast<Vec<Rect>*>(&prop_rects_);
      switch (info.kind) {
        case PropInfo::kEnum: {
          Str label = f("{}: {}", info.name, value);
          float w = ui::beta::TextWidth(label, ui::beta::kMicroSize + 0.3_mm) + 3.2_mm;
          Rect chip{-kPlateW / 2 + kSide, row_top - 4.3_mm, -kPlateW / 2 + kSide + w,
                    row_top - 0.7_mm};
          (*rects)[i] = chip;
          SkPath path = ui::beta::WonkyRoundRect(chip, 1.2_mm, ui::beta::kWonk * 0.8f, cs);
          ui::beta::HandShadow(canvas, path, {0.3_mm, -0.3_mm}, ui::beta::kShadow, cs);
          ui::beta::MisregFill(canvas, path, ui::beta::kPaper, cs);
          ui::beta::SketchyStroke(canvas, path, ui::beta::kInk, ui::beta::kStroke * 0.8f, cs, 1);
          ui::beta::DrawTextIn(canvas, label, chip, ui::beta::kMicroSize + 0.3_mm, ui::beta::kInk,
                               ui::beta::TextAlign::Center, false, cs);
          break;
        }
        case PropInfo::kBool: {
          Rect box{-kPlateW / 2 + kSide, row_top - 4.1_mm, -kPlateW / 2 + kSide + 3.4_mm,
                   row_top - 0.7_mm};
          ui::beta::Checkbox(canvas, box, value == "true", ui::beta::State::Default, cs);
          ui::beta::DrawText(canvas, info.name, {box.right + 1.5_mm, box.bottom + 0.9_mm},
                             ui::beta::kMicroSize, ui::beta::kInk, false, cs);
          float w = ui::beta::TextWidth(info.name, ui::beta::kMicroSize);
          (*rects)[i] = Rect{box.left, box.bottom, box.right + 1.5_mm + w, box.top};
          break;
        }
        case PropInfo::kNumber: {
          ui::beta::DrawText(canvas, info.name, {-kPlateW / 2 + kSide, row_top - 3.7_mm},
                             ui::beta::kMicroSize, ui::beta::kInk, false, cs);
          Str value_label = value;
          float vw = ui::beta::TextWidth(value_label, ui::beta::kMicroSize);
          ui::beta::DrawText(canvas, value_label, {kPlateW / 2 - kSide - vw, row_top - 3.7_mm},
                             ui::beta::kMicroSize, ui::beta::kInk, false, cs);
          Rect band{-kPlateW / 2 + kSide + 21_mm, row_top - 4.0_mm, kPlateW / 2 - kSide - 11_mm,
                    row_top - 0.9_mm};
          double v = atof(value.c_str());
          float t = info.max > info.min
                        ? std::clamp((float)((v - info.min) / (info.max - info.min)), 0.f, 1.f)
                        : 0.f;
          ui::beta::Slider(canvas, band, t, ui::beta::State::Default, cs);
          (*rects)[i] = band;
          break;
        }
      }
      row_top -= kPropRow;
    }

    {  // Status row: the pipeline state, plus fps.
      float row_mid = -plate_h_ / 2 + kBottomPad + kStatusRow * 0.5f;
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

PropSliderDrag::PropSliderDrag(ui::Pointer& p, GStreamerToy& w, int index)
    : Action(p), widget(&w), index(index) {
  Update();
}

void PropSliderDrag::Update() {
  if (!widget) return;
  const PropInfo& info = widget->prop_infos_[index];
  const Rect& band = widget->prop_rects_[index];
  if (band.Width() <= 0) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  float t = std::clamp((pos.x - band.left) / band.Width(), 0.f, 1.f);
  double v = info.min + t * (info.max - info.min);
  if (info.integer) v = llround(v);
  widget->SetPropValue(index, FormatPropNumber(v, info.integer));
}

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
  Optional<Rect> DrawBounds() const override { return Shape().getBounds().makeOutset(4_mm, 4_mm); }
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

// ============================================================================
// The GStreamer shelf: every compiled-in factory as a clone pile, grouped by
// the first segment of its klass string. Groups read in flow order: sources
// feed filters feed sinks.
// ============================================================================

namespace {

SkColor KlassAccent(StrView segment) {
  if (segment == "Source") return ui::beta::kGold;
  if (segment == "Filter") return ui::beta::kCyan;
  if (segment == "Sink") return ui::beta::kBlue;
  if (segment == "Generic") return ui::beta::kPurple;
  return ui::beta::kGrayDark;
}

int KlassRank(StrView segment) {
  if (segment == "Source") return 0;
  if (segment == "Filter") return 1;
  if (segment == "Sink") return 2;
  return 3;
}

}  // namespace

struct GStreamerShelfToy : ui::beta::ObjectToy {
  static constexpr float kCell = 3.6_cm;
  static constexpr float kPad = 0.3_cm;
  static constexpr float kGroupGap = 0.95_cm;  // room for the next frame's tab
  static constexpr float kHeader = 2.4_cm;
  static constexpr float kMargin = 0.85_cm;
  static constexpr int kMaxCols = 6;

  struct PlacedGroup {
    Str label;
    SkColor accent;
    Rect frame;
  };

  Vec<std::unique_ptr<ui::ShelfButton>> buttons;
  Vec<PlacedGroup> groups;
  float sheet_w = 0;
  float sheet_h = 0;

  GStreamerShelfToy(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) {
    struct Group {
      Str label;
      Vec<Str> names;
    };
    Vec<Group> grouped;
    for (auto& info : ListGstFactories()) {
      Str segment{StrView(info.klass).substr(0, info.klass.find('/'))};
      Group* group = nullptr;
      for (auto& g : grouped) {
        if (g.label == segment) group = &g;
      }
      if (!group) group = &grouped.emplace_back(Group{segment});
      group->names.push_back(info.name);
    }
    std::stable_sort(grouped.begin(), grouped.end(), [](const Group& a, const Group& b) {
      if (KlassRank(a.label) != KlassRank(b.label)) return KlassRank(a.label) < KlassRank(b.label);
      return a.label < b.label;
    });

    float widest = 0;
    float total_h = 0;
    for (auto& g : grouped) {
      int cols = std::min<int>(kMaxCols, (int)g.names.size());
      int rows = ((int)g.names.size() + kMaxCols - 1) / kMaxCols;
      widest = std::max(widest, cols * kCell + 2 * kPad);
      total_h += rows * kCell + 2 * kPad;
    }
    total_h += kGroupGap * ((float)grouped.size() - 1);
    sheet_w = widest + 2 * kMargin;
    sheet_h = kHeader + total_h + kMargin;

    Rect sheet = Rect::MakeCenterZero(sheet_w, sheet_h);
    float y = sheet.top - kHeader;
    for (auto& g : grouped) {
      int cols = std::min<int>(kMaxCols, (int)g.names.size());
      int rows = ((int)g.names.size() + kMaxCols - 1) / kMaxCols;
      float fw = cols * kCell + 2 * kPad;
      float fh = rows * kCell + 2 * kPad;
      Rect frame{-fw / 2, y - fh, fw / 2, y};
      groups.push_back({g.label, KlassAccent(g.label), frame});
      for (int i = 0; i < (int)g.names.size(); ++i) {
        auto* proto = prototypes ? prototypes->Find(g.names[i]) : nullptr;
        if (!proto) continue;
        buttons.emplace_back(std::make_unique<ui::ShelfButton>(this, proto->Clone()));
        buttons.back()->Init();
        float cx = frame.left + kPad + kCell * (i % kMaxCols + 0.5f);
        float cy = frame.top - kPad - kCell * (i / kMaxCols + 0.5f);
        Rect src = buttons.back()->CoarseBounds().rect;
        Rect dst = Rect::MakeCenter({cx, cy}, kCell * 0.88f, kCell * 0.88f);
        buttons.back()->local_to_parent =
            SkM44(SkMatrix::RectToRect(src.sk, dst.sk, SkMatrix::kCenter_ScaleToFit));
      }
      y -= fh + kGroupGap;
    }
  }

  bool CenteredAtZero() const override { return true; }

  SkPath Shape() const override {
    return SkPath::RRect(RRect::MakeSimple(Rect::MakeCenterZero(sheet_w, sheet_h), 4_mm).sk);
  }

  // The BETA stamp overhangs the top-right corner; without this it gets clipped at the sheet.
  Optional<Rect> DrawBounds() const override { return Shape().getBounds().makeOutset(2_cm, 2_cm); }

  void Draw(SkCanvas& canvas) const override {
    Rect sheet = Rect::MakeCenterZero(sheet_w, sheet_h);
    SkPath body = Shape();
    SkPaint bg;
    bg.setAntiAlias(true);
    bg.setColor(ui::beta::kPaperCream);
    canvas.drawPath(body, bg);
    ui::beta::SketchyStroke(canvas, body, ui::beta::kInk, ui::beta::kStroke, Seed(0x2D), 1);

    StrView heading = "GStreamer";
    float hw = ui::beta::TextWidth(heading, 8.2_mm);
    Vec2 hb = {-hw * 0.5f, sheet.top - 1.6_cm};
    ui::beta::DrawText(canvas, heading, hb, 8.2_mm, ui::beta::kInk, true, Seed(0x2E));
    canvas.drawPath(
        ui::beta::WobbleLine({hb.x - 0.9_mm, hb.y - 2.3_mm}, {hb.x + hw + 0.9_mm, hb.y - 2.3_mm},
                             ui::beta::kWonk, ui::beta::kSeg, Seed(0x2F)),
        ui::beta::InkPaint(ui::beta::kPurple, ui::beta::kStrokeBold));

    for (int i = 0; i < (int)groups.size(); ++i) {
      ui::beta::GroupFrame(canvas, groups[i].frame, groups[i].label, groups[i].accent,
                           Seed(Hash2(0x70, (uint32_t)i)));
    }

    BakeChildren(canvas);

    ui::beta::DrawBetaStamp(canvas, {sheet.right - 0.6_cm, sheet.top - 0.6_cm}, 2.2_cm, -15.f,
                            Seed(0xB3), "BETA");
  }
};

std::unique_ptr<ObjectToy> GStreamerShelf::MakeToy(ui::Widget* parent) {
  return std::make_unique<GStreamerShelfToy>(parent, *this);
}

}  // namespace automat::library
