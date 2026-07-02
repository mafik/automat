// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_pipewire.hpp"

#include <include/core/SkCanvas.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include "format.hpp"
#include "text_field.hpp"
#include "ui_beta.hpp"
#include "units.hpp"

namespace automat::library {

using ui::beta::Hash2;

// ============================================================================
// PwHost: one connection to the daemon - a thread loop, a core, and a
// registry mirror. Everything PipeWire runs on the loop thread; the mirror
// mutex bridges the mirrored facts to the UI, following the thread rule.
// ============================================================================

struct PwHost {
  pw_thread_loop* loop = nullptr;
  pw_context* context = nullptr;
  pw_core* core = nullptr;
  pw_registry* registry = nullptr;
  spa_hook registry_listener = {};
  bool connected = false;

  struct MirrorNode {
    uint32_t id = 0;
    Str name;
    Str media_class;
    Str state;
    pw_proxy* proxy = nullptr;  // the bound pw_node delivering info events
    spa_hook listener = {};
    PwHost* host = nullptr;
  };
  std::mutex mirror_mutex;  // guards `nodes` contents read from the UI thread
  Vec<std::unique_ptr<MirrorNode>> nodes;

  static PwHost& Get() {
    static PwHost host;
    return host;
  }

  PwHost() {
    pw_init(nullptr, nullptr);
    loop = pw_thread_loop_new("pw-mirror", nullptr);
    if (!loop) return;
    pw_thread_loop_lock(loop);
    pw_thread_loop_start(loop);
    context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
    if (context) core = pw_context_connect(context, nullptr, 0);
    if (core) {
      registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
      static const pw_registry_events registry_events = {
          .version = PW_VERSION_REGISTRY_EVENTS,
          .global = OnGlobal,
          .global_remove = OnGlobalRemove,
      };
      pw_registry_add_listener(registry, &registry_listener, &registry_events, this);
      connected = true;
    }
    pw_thread_loop_unlock(loop);
  }

  static void OnNodeInfo(void* data, const pw_node_info* info) {
    auto& node = *(MirrorNode*)data;
    auto& host = *node.host;
    auto lock = std::lock_guard(host.mirror_mutex);
    node.state = pw_node_state_as_string(info->state);
    if (info->props) {
      if (const char* v = spa_dict_lookup(info->props, PW_KEY_NODE_NAME)) node.name = v;
      if (const char* v = spa_dict_lookup(info->props, PW_KEY_MEDIA_CLASS)) node.media_class = v;
    }
  }

  static void OnGlobal(void* data, uint32_t id, uint32_t permissions, const char* type,
                       uint32_t version, const spa_dict* props) {
    auto& host = *(PwHost*)data;
    if (!spa_streq(type, PW_TYPE_INTERFACE_Node)) return;
    auto node = std::make_unique<MirrorNode>();
    node->id = id;
    node->host = &host;
    if (props) {
      if (const char* v = spa_dict_lookup(props, PW_KEY_NODE_NAME)) node->name = v;
      if (const char* v = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS)) node->media_class = v;
    }
    node->proxy = (pw_proxy*)pw_registry_bind(host.registry, id, type, PW_VERSION_NODE, 0);
    if (node->proxy) {
      static const pw_node_events node_events = {
          .version = PW_VERSION_NODE_EVENTS,
          .info = OnNodeInfo,
      };
      pw_node_add_listener((pw_node*)node->proxy, &node->listener, &node_events, node.get());
    }
    auto lock = std::lock_guard(host.mirror_mutex);
    host.nodes.push_back(std::move(node));
  }

  static void OnGlobalRemove(void* data, uint32_t id) {
    auto& host = *(PwHost*)data;
    auto lock = std::lock_guard(host.mirror_mutex);
    for (int i = 0; i < (int)host.nodes.size(); ++i) {
      if (host.nodes[i]->id != id) continue;
      // The proxy is destroyed on this (the loop) thread; the entry goes
      // with it.
      spa_hook_remove(&host.nodes[i]->listener);
      if (host.nodes[i]->proxy) pw_proxy_destroy(host.nodes[i]->proxy);
      host.nodes.erase(host.nodes.begin() + i);
      break;
    }
  }

  // UI-thread lookup by node.name.
  bool Find(StrView name, Str& media_class, Str& state) {
    auto lock = std::lock_guard(mirror_mutex);
    for (auto& node : nodes) {
      if (node->name == name) {
        media_class = node->media_class;
        state = node->state;
        return true;
      }
    }
    return false;
  }
};

// ============================================================================
// The VU capture stream: a tiny pw_stream targeting the node (capturing the
// monitor when the node is a sink). Peaks land in the object's atomic.
// ============================================================================

// The capture stream plus its listener hook, owned by one node object and
// managed on the loop thread.
struct VuStream {
  pw_stream* stream = nullptr;
  spa_hook listener = {};
  PipeWireNode* owner = nullptr;
};

static void OnVuProcess(void* data) {
  auto& vu_stream = *(VuStream*)data;
  auto& obj = *vu_stream.owner;
  auto* stream = vu_stream.stream;
  if (!stream) return;
  pw_buffer* b = pw_stream_dequeue_buffer(stream);
  if (!b) return;
  float peak = 0;
  if (b->buffer->n_datas > 0 && b->buffer->datas[0].data) {
    auto& d = b->buffer->datas[0];
    int n = (int)(d.chunk->size / sizeof(float));
    const float* samples = (const float*)((const uint8_t*)d.data + d.chunk->offset);
    for (int i = 0; i < n; ++i) peak = std::max(peak, fabsf(samples[i]));
  }
  float old = obj.vu.load(std::memory_order_relaxed);
  float next = std::max(peak, old * 0.9f);  // peak with decay
  obj.vu.store(next, std::memory_order_relaxed);
  if (fabsf(next - old) > 0.005f) obj.WakeToys();
  pw_stream_queue_buffer(stream, b);
}

static const pw_stream_events kVuStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = OnVuProcess,
};

// Both run with the thread loop locked.
static void DestroyVuStreamLocked(PipeWireNode& obj) {
  if (auto* vu_stream = (VuStream*)obj.stream) {
    spa_hook_remove(&vu_stream->listener);
    pw_stream_destroy(vu_stream->stream);
    delete vu_stream;
    obj.stream = nullptr;
  }
}

static void CreateVuStreamLocked(PipeWireNode& obj, const Str& target) {
  auto& host = PwHost::Get();
  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Music",
      PW_KEY_TARGET_OBJECT, target.c_str(), PW_KEY_STREAM_CAPTURE_SINK, "true", nullptr);
  auto* stream = pw_stream_new(host.core, "Automat peak meter", props);
  if (!stream) return;
  auto* vu_stream = new VuStream{.stream = stream, .owner = &obj};
  obj.stream = vu_stream;
  pw_stream_add_listener(stream, &vu_stream->listener, &kVuStreamEvents, vu_stream);
  uint8_t buffer[1024];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  auto info = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32);
  const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
  pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                    (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                    params, 1);
}

// ============================================================================
// Object
// ============================================================================

PipeWireNode::~PipeWireNode() {
  auto& host = PwHost::Get();
  if (host.loop && stream) {
    pw_thread_loop_lock(host.loop);
    DestroyVuStreamLocked(*this);
    pw_thread_loop_unlock(host.loop);
  }
}

void PipeWireNode::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  if (node_name.empty()) return;
  writer.Key("node");
  writer.String(node_name.data(), node_name.size());
}

bool PipeWireNode::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key != "node") return false;
  Status status;
  Str name;
  d.Get(name, status);
  if (OK(status)) SetNodeName(name);
  return true;
}

void PipeWireNode::SetNodeName(StrView name) {
  auto& host = PwHost::Get();
  {
    auto lock = std::lock_guard(mutex);
    node_name = name;
    vu.store(0, std::memory_order_relaxed);
  }
  if (host.connected) {
    pw_thread_loop_lock(host.loop);
    DestroyVuStreamLocked(*this);
    if (!name.empty()) CreateVuStreamLocked(*this, Str(name));
    pw_thread_loop_unlock(host.loop);
  }
  WakeToys();
}

void PipeWireNode::RefreshFromMirror() {
  auto& host = PwHost::Get();
  Str media, state;
  bool found;
  Str name;
  {
    auto lock = std::lock_guard(mutex);
    name = node_name;
  }
  found = host.connected && host.Find(name, media, state);
  auto lock = std::lock_guard(mutex);
  daemon = host.connected;
  if (found) {
    media_class = media;
    state_word = state;
  } else {
    media_class.clear();
    state_word = "absent";
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
constexpr float kNameRow = 6.0_mm;
constexpr float kVuRow = 6.0_mm;
constexpr float kStatusRow = 5.0_mm;
constexpr float kBottomPad = 1.5_mm;
constexpr float kPlateH = kBand + kCreditRow + kNameRow + 1.5_mm + kVuRow + kStatusRow + kBottomPad;

constexpr uint32_t kSeed = 0x9F1;

}  // namespace

struct PwNameField : ui::TextField {
  PwNameField(ui::Widget* parent, std::string* text, float width)
      : ui::TextField(parent, text, width) {}
  StrView Name() const override { return "PwNameField"; }
};

struct PipeWireNodeToy : ui::beta::ObjectToy {
  std::unique_ptr<PwNameField> field;
  std::string name_edit_;

  // Tick-cached object state (UI thread only):
  Str name_applied_;
  Str media_class_;
  Str state_word_;
  bool daemon_ = false;
  float vu_ = 0;

  PipeWireNodeToy(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) {
    if (auto node = LockObject<PipeWireNode>()) {
      auto lock = std::lock_guard(node->mutex);
      name_edit_ = node->node_name;
      name_applied_ = node->node_name;
    }
    field = std::make_unique<PwNameField>(this, &name_edit_, kPlateW - 2 * kSide);
    float field_bottom = kPlateH / 2 - (kBand + kCreditRow + kNameRow);
    field->local_to_parent =
        SkM44::Translate(-kPlateW / 2 + kSide, field_bottom) * SkM44::Scale(0.55f, 0.55f, 1);
    UpdateFromObject();
  }

  bool CenteredAtZero() const override { return true; }
  SkPath Shape() const override {
    return SkPath::RRect(RRect::MakeSimple(Rect::MakeCenterZero(kPlateW, kPlateH), 3_mm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Shape().getBounds().makeOutset(4_mm, 4_mm);
  }

  void UpdateFromObject() {
    if (auto node = LockObject<PipeWireNode>()) {
      if (Str(name_edit_) != name_applied_) {
        name_applied_ = name_edit_;
        node->SetNodeName(name_applied_);
      }
      node->RefreshFromMirror();
      auto lock = std::lock_guard(node->mutex);
      media_class_ = node->media_class;
      state_word_ = node->state_word;
      daemon_ = node->daemon;
      vu_ = node->vu.load(std::memory_order_relaxed);
    }
  }

  Tock Tick(time::Timer&) override {
    UpdateFromObject();
    // The proxy mirrors a live external graph, so it keeps observing; it
    // repaints per tick only while the meter moves.
    if (vu_ > 0.001f) return Tock::Drawing;
    return Tock::Draw | Tock::Ing;
  }

  void Draw(SkCanvas& canvas) const override {
    Str title = name_applied_.empty() ? Str("pipewire node") : name_applied_;
    ui::beta::Panel(canvas, Rect::MakeCenterZero(kPlateW, kPlateH), title, ui::beta::kRose,
                    ui::beta::State::Default, Seed(kSeed), true);

    {  // credit: the daemon's own classification of the node
      Str credit = media_class_.empty() ? Str("PipeWire") : f("PipeWire · {}", media_class_);
      float w = ui::beta::TextWidth(credit, ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, credit, {kPlateW / 2 - kSide - w, kPlateH / 2 - kBand - 1.6_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
    }

    {  // caption over the name field
      float cap_y = kPlateH / 2 - kBand - kCreditRow - 1.4_mm;
      ui::beta::DrawText(canvas, "node.name", {-kPlateW / 2 + kSide + 0.6_mm, cap_y},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
    }

    {  // The peak meter, fed by the capture stream.
      float bar_y = kPlateH / 2 - (kBand + kCreditRow + kNameRow + 1.5_mm) - 4.0_mm;
      Rect bar{-kPlateW / 2 + kSide, bar_y, kPlateW / 2 - kSide, bar_y + 3.2_mm};
      SkPaint bar_bg;
      bar_bg.setColor(ui::beta::kInk);
      canvas.drawRect(bar.sk, bar_bg);
      float level = std::min(1.f, vu_);
      SkPaint bar_fill;
      bar_fill.setColor(level > 0.9f ? ui::beta::kRed : ui::beta::kLime);
      canvas.drawRect(
          SkRect::MakeLTRB(bar.left, bar.bottom, bar.left + bar.Width() * level, bar.top),
          bar_fill);
      SkPaint bar_stroke;
      bar_stroke.setStyle(SkPaint::kStroke_Style);
      bar_stroke.setStrokeWidth(ui::beta::kStroke * 0.8f);
      bar_stroke.setColor(ui::beta::kInk);
      canvas.drawRect(bar.sk, bar_stroke);
    }

    {  // Status row: the node state in PipeWire's own words.
      float row_mid = -kPlateH / 2 + kBottomPad + kStatusRow * 0.5f;
      Str label = daemon_ ? state_word_ : Str("no daemon");
      SkColor color = !daemon_                   ? ui::beta::kRed
                      : state_word_ == "running" ? ui::beta::kLime
                      : state_word_ == "idle"    ? ui::beta::kGold
                                                 : ui::beta::kGray;
      float w = ui::beta::TextWidth(label, ui::beta::kMicroSize + 0.3_mm) + 2.6_mm;
      float chip_left = -kPlateW / 2 + kSide;
      float chip_bottom = row_mid - 1.6_mm;
      Rect chip{chip_left, chip_bottom, chip_left + w, chip_bottom + 3.2_mm};
      uint32_t cs = Seed(Hash2(kSeed, 0xC2));
      SkPath path = ui::beta::WonkyRoundRect(chip, 1.2_mm, ui::beta::kWonk * 0.8f, cs);
      ui::beta::HandShadow(canvas, path, {0.3_mm, -0.3_mm}, ui::beta::kShadow, cs);
      ui::beta::MisregFill(canvas, path, color, cs);
      ui::beta::SketchyStroke(canvas, path, ui::beta::kInk, ui::beta::kStroke * 0.8f, cs, 1);
      ui::beta::DrawTextIn(canvas, label, chip, ui::beta::kMicroSize + 0.3_mm,
                           ui::beta::TextOn(color), ui::beta::TextAlign::Center, false, cs);
    }
    BakeChildren(canvas);
  }
};

std::unique_ptr<ObjectToy> PipeWireNode::MakeToy(ui::Widget* parent) {
  return std::make_unique<PipeWireNodeToy>(parent, *this);
}

}  // namespace automat::library
