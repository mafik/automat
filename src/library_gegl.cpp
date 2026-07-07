// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_gegl.hpp"

#include <gegl-paramspecs.h>
#include <gegl.h>
#include <include/core/SkCanvas.h>

#include <condition_variable>
#include <thread>

#include "format.hpp"
#include "mortal.hpp"
#include "prototypes.hpp"
#include "ui_beta.hpp"
#include "ui_shelf_button.hpp"
#include "units.hpp"

// Entry point of the statically linked operation bundle (the generated
// module_common.c, see src/gegl.py); a null GTypeModule makes GLib register
// the operation types statically.
extern "C" gboolean gegl_module_register(GTypeModule* module);

namespace automat::library {

using ui::beta::Hash2;

// ============================================================================
// Introspection: the operation list for the shelf and the instrument-shaped
// properties for the faces. Mirrors the GStreamer wrapper: a GEnum is a chip
// that cycles its nicks, a boolean is a checkbox, a bounded number is a
// slider over the spec's ui range; everything else stays recipe-only.
// ============================================================================

struct PropInfo {
  enum Kind { kEnum, kBool, kNumber };
  Kind kind;
  Str name;
  Vec<Str> nicks;           // kEnum: the GEnum's nicks, in order
  double min = 0, max = 0;  // kNumber
  bool integer = false;     // kNumber: whole steps
  Str def;                  // the operation's default, formatted for display
};

static Str FormatPropNumber(double v, bool integer) {
  if (integer) return f("{}", (int64_t)llround(v));
  return f("{:.2f}", v);
}

// The slider range: GEGL's ui range where the spec provides one (the hard
// range of e.g. std-dev runs to 1500, the ui range to 100), else the hard
// range.
static bool NumericRangeSpec(GParamSpec* pspec, double& lo, double& hi, double& def,
                             bool& integer) {
  if (G_IS_PARAM_SPEC_INT(pspec)) {
    auto* p = G_PARAM_SPEC_INT(pspec);
    lo = p->minimum, hi = p->maximum, def = p->default_value, integer = true;
    if (GEGL_IS_PARAM_SPEC_INT(pspec)) {
      auto* g = GEGL_PARAM_SPEC_INT(pspec);
      lo = g->ui_minimum, hi = g->ui_maximum;
    }
  } else if (G_IS_PARAM_SPEC_UINT(pspec)) {
    auto* p = G_PARAM_SPEC_UINT(pspec);
    lo = p->minimum, hi = p->maximum, def = p->default_value, integer = true;
  } else if (G_IS_PARAM_SPEC_DOUBLE(pspec)) {
    auto* p = G_PARAM_SPEC_DOUBLE(pspec);
    lo = p->minimum, hi = p->maximum, def = p->default_value, integer = false;
    if (GEGL_IS_PARAM_SPEC_DOUBLE(pspec)) {
      auto* g = GEGL_PARAM_SPEC_DOUBLE(pspec);
      lo = g->ui_minimum, hi = g->ui_maximum;
    }
  } else if (G_IS_PARAM_SPEC_FLOAT(pspec)) {
    auto* p = G_PARAM_SPEC_FLOAT(pspec);
    lo = p->minimum, hi = p->maximum, def = p->default_value, integer = false;
  } else {
    return false;
  }
  return true;
}

static Vec<PropInfo> IntrospectProps(const Str& op) {
  constexpr int kMaxProps = 4;
  Vec<PropInfo> out;
  guint n = 0;
  GParamSpec** specs = gegl_operation_list_properties(op.c_str(), &n);
  if (!specs) return out;
  for (guint i = 0; i < n && (int)out.size() < kMaxProps; ++i) {
    GParamSpec* pspec = specs[i];
    if (!(pspec->flags & G_PARAM_WRITABLE) || !(pspec->flags & G_PARAM_READABLE)) continue;
    if (pspec->flags & (G_PARAM_CONSTRUCT_ONLY | G_PARAM_DEPRECATED)) continue;
    PropInfo info;
    info.name = g_param_spec_get_name(pspec);
    const GValue* def = g_param_spec_get_default_value(pspec);
    if (G_IS_PARAM_SPEC_ENUM(pspec)) {
      info.kind = PropInfo::kEnum;
      GEnumClass* ec = G_PARAM_SPEC_ENUM(pspec)->enum_class;
      for (guint v = 0; v < ec->n_values; ++v) info.nicks.push_back(ec->values[v].value_nick);
      if (GEnumValue* ev = g_enum_get_value(ec, g_value_get_enum(def))) info.def = ev->value_nick;
    } else if (G_IS_PARAM_SPEC_BOOLEAN(pspec)) {
      info.kind = PropInfo::kBool;
      info.def = g_value_get_boolean(def) ? "true" : "false";
    } else if (double lo = 0, hi = 0, val = 0; NumericRangeSpec(pspec, lo, hi, val, info.integer)) {
      // An unbounded range makes a useless slider; such properties stay
      // recipe-only.
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
  return out;
}

// The operation's property named `name`, or null. The returned array must be
// g_free'd by the caller; the specs stay owned by the type system.
static GParamSpec* FindPspec(const Str& op, StrView name, GParamSpec**& specs) {
  guint n = 0;
  specs = gegl_operation_list_properties(op.c_str(), &n);
  if (!specs) return nullptr;
  for (guint i = 0; i < n; ++i) {
    if (name == g_param_spec_get_name(specs[i])) return specs[i];
  }
  return nullptr;
}

// The first segment of the operation's categories key ("enhance" of
// "enhance:noise-reduction"), or an empty string.
static Str PrimaryCategory(StrView categories) {
  auto colon = categories.find(':');
  return Str(colon == StrView::npos ? categories : categories.substr(0, colon));
}

// ============================================================================
// GeglHost: the one hidden GEGL graph and the worker that pumps processors.
// `mutex` serializes every GEGL call and guards the registry and each
// operation's node pointers. Lock order: host mutex before object mutex.
// ============================================================================

struct GeglHost {
  std::mutex mutex;
  std::condition_variable cv;
  GeglNode* root = nullptr;
  Vec<GeglOperation*> ops;  // registered blocks; entries removed in ~GeglOperation
  int next_op = 0;          // round-robin scan position
  std::thread worker;

  static GeglHost& Get() {
    // Deliberately leaked: the worker waits on `cv` forever, and a static
    // destructor would hang the process inside pthread_cond_destroy at exit.
    static GeglHost* host = new GeglHost;
    return *host;
  }

  GeglHost() {
    gegl_init(nullptr, nullptr);
    gegl_module_register(nullptr);
    root = gegl_node_new();
    worker = std::thread([this] { WorkerLoop(); });
    // The worker runs for the process's life; detaching keeps the static
    // host's destruction from terminating on a joinable thread.
    worker.detach();
  }

  void Notify() { cv.notify_one(); }

  // Signal handlers keep to atomics: they fire under the host mutex (from
  // gegl_node_set or gegl_processor_work) and must not take locks.
  static void OnInvalidated(GeglNode*, GeglRectangle*, void* data) {
    auto* op = (GeglOperation*)data;
    op->dirty.store(true, std::memory_order_relaxed);
    GeglHost::Get().Notify();
  }

  static void OnComputed(GeglNode*, GeglRectangle* rect, void* data) {
    auto* op = (GeglOperation*)data;
    op->pending_rects.push_back(GeglRect{rect->x, rect->y, rect->width, rect->height});
  }

  // Sets one property on the live node from its text value: enums by nick,
  // booleans by word, numbers parsed and transformed to the spec's type.
  void ApplyPropLocked(GeglOperation& op, StrView name, StrView value) {
    GParamSpec** specs = nullptr;
    GParamSpec* pspec = FindPspec(op.op, name, specs);
    if (pspec) {
      Str name_str(name), value_str(value);
      GValue gv = G_VALUE_INIT;
      g_value_init(&gv, pspec->value_type);
      bool ok = false;
      if (G_IS_PARAM_SPEC_ENUM(pspec)) {
        GEnumClass* ec = G_PARAM_SPEC_ENUM(pspec)->enum_class;
        if (GEnumValue* ev = g_enum_get_value_by_nick(ec, value_str.c_str())) {
          g_value_set_enum(&gv, ev->value);
          ok = true;
        }
      } else if (G_IS_PARAM_SPEC_BOOLEAN(pspec)) {
        g_value_set_boolean(&gv, value == "true");
        ok = true;
      } else if (G_IS_PARAM_SPEC_STRING(pspec)) {
        g_value_set_string(&gv, value_str.c_str());
        ok = true;
      } else {
        GValue dv = G_VALUE_INIT;
        g_value_init(&dv, G_TYPE_DOUBLE);
        g_value_set_double(&dv, g_ascii_strtod(value_str.c_str(), nullptr));
        ok = g_value_transform(&dv, &gv);
        g_value_unset(&dv);
      }
      if (ok) gegl_node_set_property((GeglNode*)op.node, name_str.c_str(), &gv);
      g_value_unset(&gv);
    }
    if (specs) g_free(specs);
  }

  // Creates the block's nodes on first use: the operation itself, its
  // self-demonstration (checkerboard cropped to a finite extent), and the
  // signal hooks.
  void EnsureNodeLocked(GeglOperation& op) {
    if (op.node) return;
    auto* node = gegl_node_new_child(root, "operation", op.op.c_str(), nullptr);
    op.node = node;
    Vec<std::pair<Str, Str>> recipe;
    {
      auto op_lock = std::lock_guard(op.mutex);
      recipe = op.props;
    }
    for (auto& [name, value] : recipe) ApplyPropLocked(op, name, value);
    // A source operation renders itself; only consumers get the
    // checkerboard demonstration.
    if (gegl_node_has_pad(node, "input")) {
      auto* checkerboard =
          gegl_node_new_child(root, "operation", "gegl:checkerboard", "x", 32, "y", 32, nullptr);
      auto* crop =
          gegl_node_new_child(root, "operation", "gegl:crop", "x", (double)0, "y", (double)0,
                              "width", (double)320, "height", (double)240, nullptr);
      gegl_node_link(checkerboard, crop);
      op.demo_node = crop;
    }
    g_signal_connect(node, "invalidated", G_CALLBACK(OnInvalidated), &op);
    g_signal_connect(node, "computed", G_CALLBACK(OnComputed), &op);
    bool registered = false;
    for (auto* o : ops) registered |= (o == &op);
    if (!registered) ops.push_back(&op);
    op.dirty.store(true, std::memory_order_relaxed);
  }

  void ReleaseLocked(GeglOperation& op) {
    std::erase(ops, &op);
    if (op.processor) {
      g_object_unref((GeglProcessor*)op.processor);
      op.processor = nullptr;
    }
    if (op.node) {
      gegl_node_disconnect((GeglNode*)op.node, "input");
      gegl_node_remove_child(root, (GeglNode*)op.node);
      op.node = nullptr;
    }
    if (op.demo_node) {
      GeglNode* checkerboard = gegl_node_get_producer((GeglNode*)op.demo_node, "input", nullptr);
      gegl_node_remove_child(root, (GeglNode*)op.demo_node);
      if (checkerboard) gegl_node_remove_child(root, checkerboard);
      op.demo_node = nullptr;
    }
    if (op.src_node) {
      gegl_node_remove_child(root, (GeglNode*)op.src_node);
      op.src_node = nullptr;
    }
    if (op.src_buffer) {
      g_object_unref((GeglBuffer*)op.src_buffer);
      op.src_buffer = nullptr;
    }
  }

  // The extent every preview of the chain presents: the head's source
  // rectangle (the bridged image's size, or the demonstration's crop).
  // Operations like blur grow their bounding box past it; the abyss beyond
  // the source is not shown.
  GeglRectangle PresentationRectLocked(GeglOperation& op) {
    GeglOperation* head = &op;
    for (int i = 0; i < 32; ++i) {
      auto producer = head->in_stream->Producer();
      auto* upstream = dynamic_cast<GeglOperation*>(producer.get());
      if (!upstream) break;
      head = upstream;
    }
    auto lock = std::lock_guard(head->mutex);
    if (head->bridged_input) {
      return GeglRectangle{0, 0, head->bridged_input->width(), head->bridged_input->height()};
    }
    return GeglRectangle{0, 0, 320, 240};
  }

  // The preview is bounded: past this edge length it is sampled at reduced
  // scale, so a huge source neither bloats the preview buffer nor makes the
  // renderer push megapixels through a face a few centimetres wide.
  static constexpr int kMaxPreviewPx = 1024;

  static double PreviewScale(const GeglRectangle& present) {
    int longest = std::max(present.width, present.height);
    return longest > kMaxPreviewPx ? (double)kMaxPreviewPx / longest : 1.0;
  }

  // Copies the computed regions from the node's cache into the preview and
  // wakes the block's toys. Runs on the worker under the host mutex. The
  // blit's roi is in scaled output space.
  void DrainComputedLocked(GeglOperation& op, const GeglRectangle& present) {
    if (op.pending_rects.empty()) return;
    Vec<GeglRect> rects = std::move(op.pending_rects);
    op.pending_rects.clear();
    const Babl* format = babl_format("R'G'B'A u8");
    double scale = PreviewScale(present);
    GeglRectangle bounds{0, 0, (int)ceil(present.width * scale), (int)ceil(present.height * scale)};
    auto lock = std::lock_guard(op.mutex);
    if (op.preview_w != bounds.width || op.preview_h != bounds.height) return;
    for (auto& r : rects) {
      GeglRectangle scaled{(int)floor((r.x - present.x) * scale),
                           (int)floor((r.y - present.y) * scale),
                           (int)ceil((r.x - present.x + r.width) * scale),
                           (int)ceil((r.y - present.y + r.height) * scale)};
      scaled.width -= scaled.x;
      scaled.height -= scaled.y;
      GeglRectangle clamped;
      if (!gegl_rectangle_intersect(&clamped, &scaled, &bounds)) continue;
      uint8_t* dest = op.preview.data() + ((size_t)clamped.y * bounds.width + clamped.x) * 4;
      gegl_node_blit((GeglNode*)op.node, scale, &clamped, format, dest, bounds.width * 4,
                     GEGL_BLIT_CACHE);
      ++op.preview_counter;
    }
    op.WakeToys();
  }

  void WorkerLoop() {
    std::unique_lock lock(mutex);
    while (true) {
      GeglOperation* op = nullptr;
      for (int i = 0; i < (int)ops.size(); ++i) {
        auto* candidate = ops[(next_op + i) % ops.size()];
        if (candidate->node && candidate->dirty.load(std::memory_order_relaxed)) {
          op = candidate;
          next_op = (next_op + i + 1) % (int)ops.size();
          break;
        }
      }
      if (!op) {
        cv.wait(lock);
        continue;
      }
      op->dirty.store(false, std::memory_order_relaxed);
      GeglRectangle present = PresentationRectLocked(*op);
      {
        double scale = PreviewScale(present);
        int pw = (int)ceil(present.width * scale);
        int ph = (int)ceil(present.height * scale);
        auto op_lock = std::lock_guard(op->mutex);
        if (op->preview_w != pw || op->preview_h != ph) {
          op->preview_w = pw;
          op->preview_h = ph;
          op->preview.assign((size_t)pw * ph * 4, 0);
          ++op->preview_counter;
        }
      }
      if (!op->processor) {
        op->processor = gegl_node_new_processor((GeglNode*)op->node, &present);
      } else if (!op->mid_render) {
        // A fresh invalidation recomputes the whole extent; a visit that
        // merely continues a capped render keeps the processor's remaining
        // work instead of starting it over.
        gegl_processor_set_rectangle((GeglProcessor*)op->processor, &present);
      }
      op->mid_render = false;
      if (op->progress.load(std::memory_order_relaxed) >= 1) {
        op->progress.store(0, std::memory_order_relaxed);
        op->WakeToys();
      }
      // A bounded visit: a slow render must not starve the other blocks, so
      // after a few chunks the block re-queues itself and the scan moves on.
      bool more = true;
      bool invalidated = false;
      for (int chunk = 0; chunk < 8 && more; ++chunk) {
        double p = 0;
        more = gegl_processor_work((GeglProcessor*)op->processor, &p);
        DrainComputedLocked(*op, present);
        op->progress.store(more ? (float)p : 1.f, std::memory_order_relaxed);
        if (op->dirty.load(std::memory_order_relaxed)) {  // fresh regions; start over
          invalidated = true;
          break;
        }
        // Yield the graph between chunks, so instrument turns land mid-render.
        lock.unlock();
        lock.lock();
        // The block may have died while the graph was unlocked.
        bool alive = false;
        for (auto* o : ops) alive |= (o == op);
        if (!alive) {
          op = nullptr;
          break;
        }
      }
      if (op) {
        if (more && !invalidated) {
          op->mid_render = true;
          op->dirty.store(true, std::memory_order_relaxed);
        }
        op->WakeToys();
      }
    }
  }
};

Vec<GeglOpInfo> ListGeglOperations() {
  GeglHost::Get();  // gegl_init + the static operation bundle
  Vec<GeglOpInfo> out;
  guint n = 0;
  gchar** names = gegl_list_operations(&n);
  for (guint i = 0; i < n; ++i) {
    StrView name = names[i];
    // "hidden" is the library's own word for operations not meant for users.
    const char* categories = gegl_operation_get_key(names[i], "categories");
    Str cats = categories ? categories : "";
    if (cats.contains("hidden")) continue;
    // These build internal children the static bundle cannot provide
    // (gegl:emboss is GPL3+, gegl:distance-transform sits in the colliding
    // C++ module set, gegl:text needs pango); they would place broken
    // passthroughs.
    if (name == "gegl:bevel" || name == "gegl:layer" || name == "gegl:styles") continue;
    out.push_back({Str(name), std::move(cats)});
  }
  g_free(names);
  return out;
}

// ============================================================================
// Object
// ============================================================================

GeglOperation::GeglOperation(StrView op_name) : op(op_name) {}

GeglOperation::GeglOperation(const GeglOperation& o)
    : Object(o),
      op(o.op),
      props(o.props),
      run(o.run),
      next(o.next),
      image(o.image),
      out_stream(o.out_stream) {}

GeglOperation::~GeglOperation() {
  auto& host = GeglHost::Get();
  auto lock = std::lock_guard(host.mutex);
  host.ReleaseLocked(*this);
}

void GeglOperation::SerializeState(ObjectSerializer& writer) const {
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

bool GeglOperation::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  if (key == "props") {
    auto lock = std::lock_guard(mutex);
    props.clear();
    for (auto& prop_name : ObjectView(d, status)) {
      Str value;
      d.Get(value, status);
      if (OK(status)) props.push_back({Str(prop_name), std::move(value)});
    }
    return true;
  }
  // Saves from before generic properties: blur's one "std-dev" drove both
  // axes; brightness-contrast wrote per-label doubles.
  if (op == "gegl:gaussian-blur" && key == "std-dev") {
    double v = 0;
    d.Get(v, status);
    if (OK(status)) {
      Str value = FormatPropNumber(v, false);
      SetProp("std-dev-x", value);
      SetProp("std-dev-y", value);
    }
    return true;
  }
  if (op == "gegl:brightness-contrast" && (key == "brightness" || key == "contrast")) {
    double v = 0;
    d.Get(v, status);
    if (OK(status)) SetProp(key, FormatPropNumber(v, false));
    return true;
  }
  return false;
}

void GeglOperation::SetProp(StrView name, StrView value) {
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
  }
  auto& host = GeglHost::Get();
  {
    auto lock = std::lock_guard(host.mutex);
    if (node) host.ApplyPropLocked(*this, name, value);
  }
  WakeToys();
}

Str GeglOperation::GetProp(StrView name) const {
  auto lock = std::lock_guard(mutex);
  for (auto& [prop_name, prop_value] : props) {
    if (prop_name == name) return prop_value;
  }
  return {};
}

sk_sp<SkImage> GeglOperation::Result() const {
  auto lock = std::lock_guard(mutex);
  if (preview.empty()) return nullptr;
  auto info =
      SkImageInfo::Make(preview_w, preview_h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  return SkImages::RasterFromData(info, SkData::MakeWithCopy(preview.data(), preview.size()),
                                  (size_t)preview_w * 4);
}

void GeglOperation::MarkDirty() {
  dirty.store(true, std::memory_order_relaxed);
  GeglHost::Get().Notify();
}

void GeglOperation::SyncSources() {
  auto& host = GeglHost::Get();
  // The bridged image is read outside the host lock (GetImage may lock the
  // providing object).
  sk_sp<SkImage> img;
  {
    auto ip_ptr = image->FindInterface();
    ImageProvider ip(ip_ptr.Owner<Object>(), ip_ptr.Get());
    if (ip) img = ip.GetImage();
  }
  GeglNode* stream_source = nullptr;
  {
    auto producer = in_stream->Producer();
    if (auto* upstream = dynamic_cast<GeglOperation*>(producer.get())) {
      auto lock = std::lock_guard(host.mutex);
      host.EnsureNodeLocked(*upstream);
      stream_source = (GeglNode*)upstream->node;
    }
  }
  auto lock = std::lock_guard(host.mutex);
  host.EnsureNodeLocked(*this);
  if (!gegl_node_has_pad((GeglNode*)node, "input")) return;  // a source renders itself
  bool image_changed = false;
  {
    auto obj_lock = std::lock_guard(mutex);
    image_changed = (bridged_input != img);
    bridged_input = img;
  }
  if (image_changed) {
    if (src_buffer) {
      g_object_unref((GeglBuffer*)src_buffer);
      src_buffer = nullptr;
    }
    if (img) {
      int w = img->width(), h = img->height();
      Vec<uint8_t> pixels((size_t)w * h * 4);
      auto info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
      if (img->readPixels(nullptr, info, pixels.data(), (size_t)w * 4, 0, 0)) {
        const Babl* format = babl_format("R'G'B'A u8");
        GeglRectangle rect{0, 0, w, h};
        auto* buffer = gegl_buffer_new(&rect, format);
        gegl_buffer_set(buffer, &rect, 0, format, pixels.data(), w * 4);
        src_buffer = buffer;
        if (!src_node) {
          src_node = gegl_node_new_child(host.root, "operation", "gegl:buffer-source", "buffer",
                                         buffer, nullptr);
        } else {
          gegl_node_set((GeglNode*)src_node, "buffer", buffer, nullptr);
        }
      }
    }
  }
  // Input priority: the stream edge, then the bridged image, then the
  // self-demonstration.
  GeglNode* desired = stream_source              ? stream_source
                      : (src_buffer && src_node) ? (GeglNode*)src_node
                                                 : (GeglNode*)demo_node;
  if (desired != (GeglNode*)linked_source) {
    gegl_node_disconnect((GeglNode*)node, "input");
    if (desired) gegl_node_connect_to(desired, "output", (GeglNode*)node, "input");
    linked_source = desired;
    dirty.store(true, std::memory_order_relaxed);
    host.Notify();
  } else if (image_changed) {
    dirty.store(true, std::memory_order_relaxed);
    host.Notify();
  }
}

void GeglOperation::CanFeedGegl(StreamArgument self, Interface end, Status& status) {
  StreamArgument::Table::DefaultCanConnect(self, end, status);
  if (!OK(status)) return;
  auto* peer = dynamic_cast<GeglOperation*>(end.object_ptr);
  if (!peer) {
    AppendErrorMessage(status) +=
        "GEGL edges stay between GEGL blocks; the Image and Result arguments bridge to the rest";
    return;
  }
  // A cycle would never converge: walk downstream from the peer.
  GeglOperation* cur = peer;
  for (int i = 0; i < 32 && cur; ++i) {
    if (cur == this) {
      AppendErrorMessage(status) += "that link would close a cycle";
      return;
    }
    auto target = cur->out_stream->FindInterface();
    cur = dynamic_cast<GeglOperation*>(target.Owner<Object>());
  }
}

void GeglOperation::OnOutConnect(StreamArgument self, Interface end) {
  GeglOperation* old_peer = nullptr;
  if (auto old = self.state->target.Lock()) {
    old_peer = dynamic_cast<GeglOperation*>(old.Owner<Object>());
  }
  StreamArgument::Table::StreamOnConnect(self, end);
  if (old_peer && old_peer != end.object_ptr) old_peer->SyncSources();
  if (auto* peer = dynamic_cast<GeglOperation*>(end.object_ptr)) peer->SyncSources();
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
constexpr float kBottomPad = 1.5_mm;

constexpr uint32_t kSeed = 0x6E6;

}  // namespace

struct GeglOperationToy;

// Dragging along a property's slider band; the value tracks the pointer.
struct GeglPropDrag : Action {
  MortalPtr<GeglOperationToy> widget;
  int index;
  GeglPropDrag(ui::Pointer& p, GeglOperationToy& w, int index);
  void Update() override;
};

struct GeglOperationToy : ui::beta::ObjectToy {
  Str op_;
  Str credit_;
  Vec<PropInfo> prop_infos_;
  Vec<Str> prop_values_;  // recipe override or the operation default
  Vec<Rect> prop_rects_;  // instrument hit areas, in face coordinates
  float plate_h_ = 0;

  // Tick-cached object state (UI thread only):
  float progress_ = 1;
  uint64_t preview_counter_ = 0;
  sk_sp<SkImage> preview_image_;

  // What the last repaint showed; a change requests a redraw.
  uint64_t drawn_counter_ = ~0ull;
  Vec<Str> drawn_values_;
  float drawn_progress_ = -1;

  GeglOperationToy(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) {
    if (auto* gegl = dynamic_cast<GeglOperation*>(&obj)) {
      op_ = gegl->op;
      prop_infos_ = IntrospectProps(op_);
      Str category = PrimaryCategory(gegl_operation_get_key(op_.c_str(), "categories")
                                         ? gegl_operation_get_key(op_.c_str(), "categories")
                                         : "");
      credit_ = category.empty() ? Str("GEGL") : f("GEGL · {}", category);
    }
    prop_values_.resize(prop_infos_.size());
    prop_rects_.resize(prop_infos_.size());
    drawn_values_.resize(prop_infos_.size());
    plate_h_ = kBand + kCreditRow + kPreviewH + 1_mm + prop_infos_.size() * kPropRow + kBottomPad;
    UpdateFromObject();
  }

  bool CenteredAtZero() const override { return true; }
  SkPath Shape() const override {
    return SkPath::RRect(RRect::MakeSimple(Rect::MakeCenterZero(kPlateW, plate_h_), 3_mm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Shape().getBounds().makeOutset(4_mm, 4_mm);
  }
  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&GeglOperation::out_stream_tbl)) {
      return Vec2AndDir{.pos = Vec2(-kPlateW / 2 + 10_mm, -plate_h_ / 2), .dir = -90_deg};
    }
    return ObjectToy::ArgStart(arg);
  }

  void UpdateFromObject() {
    if (auto gegl = LockObject<GeglOperation>()) {
      gegl->SyncSources();
      progress_ = gegl->progress.load(std::memory_order_relaxed);
      for (int i = 0; i < (int)prop_infos_.size(); ++i) {
        prop_values_[i] = gegl->GetProp(prop_infos_[i].name);
        if (prop_values_[i].empty()) prop_values_[i] = prop_infos_[i].def;
      }
      uint64_t counter;
      {
        auto lock = std::lock_guard(gegl->mutex);
        counter = gegl->preview_counter;
      }
      if (counter != preview_counter_) {
        preview_counter_ = counter;
        preview_image_ = gegl->Result();
      }
    }
  }

  Tock Tick(time::Timer&) override {
    UpdateFromObject();
    // The block watches its input for the lazy loop, so it keeps ticking; it
    // repaints only when what it shows changed.
    Tock tock = Tock::Ing;
    bool changed = preview_counter_ != drawn_counter_ || progress_ != drawn_progress_;
    for (int i = 0; i < (int)prop_infos_.size(); ++i)
      changed |= (prop_values_[i] != drawn_values_[i]);
    if (changed) {
      drawn_counter_ = preview_counter_;
      drawn_progress_ = progress_;
      drawn_values_ = prop_values_;
      tock |= Tock::Draw;
    }
    return tock;
  }

  void SetPropValue(int index, Str value) {
    prop_values_[index] = value;
    if (auto gegl = LockObject<GeglOperation>()) {
      gegl->SetProp(prop_infos_[index].name, value);
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
            return std::make_unique<GeglPropDrag>(p, *this, i);
        }
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void Draw(SkCanvas& canvas) const override {
    ui::beta::Panel(canvas, Rect::MakeCenterZero(kPlateW, plate_h_), op_, ui::beta::kCyan,
                    ui::beta::State::Default, Seed(kSeed), true);

    {  // credit and the stream ports
      float w = ui::beta::TextWidth(credit_, ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, credit_, {kPlateW / 2 - kSide - w, plate_h_ / 2 - kBand - 1.6_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
      float iw = ui::beta::TextWidth("input", ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, "input", {-iw / 2, plate_h_ / 2 - kBand - 1.6_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
      float ow = ui::beta::TextWidth("output", ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, "output", {-kPlateW / 2 + 10_mm - ow / 2, -plate_h_ / 2 + 0.8_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
    }

    // The preview, repainting region by region as the worker computes.
    Rect preview_rect =
        Rect::MakeCornerZero(kPreviewW, kPreviewH)
            .MoveBy({-kPreviewW / 2, plate_h_ / 2 - kBand - kCreditRow - kPreviewH});
    {
      SkPaint bg;
      bg.setColor(ui::beta::kInk);
      canvas.drawRect(preview_rect.sk, bg);
      if (preview_image_) {
        canvas.save();
        SkRect src = SkRect::Make(preview_image_->dimensions());
        SkMatrix m = SkMatrix::RectToRect(src, preview_rect.sk, SkMatrix::kCenter_ScaleToFit);
        m.preTranslate(0, preview_image_->height() / 2.f);
        m.preScale(1, -1);
        m.preTranslate(0, -preview_image_->height() / 2.f);
        canvas.concat(m);
        canvas.drawImage(preview_image_, 0, 0, SkSamplingOptions(SkFilterMode::kLinear), nullptr);
        canvas.restore();
      } else {
        StrView hint = "waiting for the first regions";
        float w = ui::beta::TextWidth(hint, ui::beta::kMicroSize);
        ui::beta::DrawText(canvas, hint, {-w / 2, preview_rect.CenterY()}, ui::beta::kMicroSize,
                           ui::beta::kGray, false, Seed(kSeed));
      }
      if (progress_ < 1) {  // the recompute's progress, a real number
        Rect bar{preview_rect.left + 1_mm, preview_rect.bottom + 1_mm, preview_rect.right - 1_mm,
                 preview_rect.bottom + 3.4_mm};
        ui::beta::Activity(canvas, bar, progress_, ui::beta::State::Default, Seed(kSeed));
      }
      SkPaint frame;
      frame.setStyle(SkPaint::kStroke_Style);
      frame.setStrokeWidth(ui::beta::kStroke);
      frame.setColor(ui::beta::kInk);
      canvas.drawRect(preview_rect.sk, frame);
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
    BakeChildren(canvas);
  }
};

GeglPropDrag::GeglPropDrag(ui::Pointer& p, GeglOperationToy& w, int index)
    : Action(p), widget(&w), index(index) {
  Update();
}

void GeglPropDrag::Update() {
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

std::unique_ptr<ObjectToy> GeglOperation::MakeToy(ui::Widget* parent) {
  return std::make_unique<GeglOperationToy>(parent, *this);
}

// ============================================================================
// Shelf
// ============================================================================

// Groups are ordered with sources first and plumbing last; between them the
// library's category names in alphabetical order.
static int CategoryRank(StrView category) {
  if (category == "render") return 0;
  if (category == "programming") return 2;
  if (category == "output") return 3;
  return 1;
}

static SkColor CategoryAccent(StrView category) {
  constexpr SkColor kPalette[] = {ui::beta::kGold, ui::beta::kCyan,   ui::beta::kPurple,
                                  ui::beta::kSky,  ui::beta::kBlue,   ui::beta::kGreen,
                                  ui::beta::kRose, ui::beta::kOrange, ui::beta::kLime};
  uint32_t h = 0x9E3779B9;
  for (char c : category) h = Hash2(h, (uint32_t)(unsigned char)c);
  return kPalette[h % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

struct GeglShelfToy : ui::beta::ObjectToy {
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

  GeglShelfToy(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) {
    struct Group {
      Str label;
      Vec<Str> names;
    };
    Vec<Group> grouped;
    for (auto& info : ListGeglOperations()) {
      Str category = PrimaryCategory(info.categories);
      if (category.empty()) category = "no categories";
      Group* group = nullptr;
      for (auto& g : grouped) {
        if (g.label == category) group = &g;
      }
      if (!group) group = &grouped.emplace_back(Group{category});
      group->names.push_back(info.name);
    }
    std::stable_sort(grouped.begin(), grouped.end(), [](const Group& a, const Group& b) {
      if (CategoryRank(a.label) != CategoryRank(b.label)) {
        return CategoryRank(a.label) < CategoryRank(b.label);
      }
      return a.label < b.label;
    });
    for (auto& g : grouped) std::sort(g.names.begin(), g.names.end());

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
      groups.push_back({g.label, CategoryAccent(g.label), frame});
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
  Optional<Rect> TextureBounds() const override {
    return Shape().getBounds().makeOutset(2_cm, 2_cm);
  }

  void Draw(SkCanvas& canvas) const override {
    Rect sheet = Rect::MakeCenterZero(sheet_w, sheet_h);
    SkPath body = Shape();
    SkPaint bg;
    bg.setAntiAlias(true);
    bg.setColor(ui::beta::kPaperCream);
    canvas.drawPath(body, bg);
    ui::beta::SketchyStroke(canvas, body, ui::beta::kInk, ui::beta::kStroke, Seed(0x2D), 1);

    StrView heading = "GEGL";
    float hw = ui::beta::TextWidth(heading, 8.2_mm);
    Vec2 hb = {-hw * 0.5f, sheet.top - 1.6_cm};
    ui::beta::DrawText(canvas, heading, hb, 8.2_mm, ui::beta::kInk, true, Seed(0x2E));
    canvas.drawPath(
        ui::beta::WobbleLine({hb.x - 0.9_mm, hb.y - 2.3_mm}, {hb.x + hw + 0.9_mm, hb.y - 2.3_mm},
                             ui::beta::kWonk, ui::beta::kSeg, Seed(0x2F)),
        ui::beta::InkPaint(ui::beta::kCyan, ui::beta::kStrokeBold));

    for (int i = 0; i < (int)groups.size(); ++i) {
      ui::beta::GroupFrame(canvas, groups[i].frame, groups[i].label, groups[i].accent,
                           Seed(Hash2(0x70, (uint32_t)i)));
    }

    BakeChildren(canvas);

    ui::beta::DrawBetaStamp(canvas, {sheet.right - 0.6_cm, sheet.top - 0.6_cm}, 2.2_cm, -15.f,
                            Seed(0xB3), "BETA");
  }
};

std::unique_ptr<ObjectToy> GeglShelf::MakeToy(ui::Widget* parent) {
  return std::make_unique<GeglShelfToy>(parent, *this);
}

}  // namespace automat::library
