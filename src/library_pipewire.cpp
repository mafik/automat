// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_pipewire.hpp"

#include <include/core/SkCanvas.h>
#include <pipewire/pipewire.h>
#include <spa/debug/types.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/result.h>

#include "format.hpp"
#include "location.hpp"
#include "log.hpp"
#include "text_field.hpp"
#include "ui_beta.hpp"
#include "ui_shelf_button.hpp"
#include "units.hpp"

namespace automat::library {

using ui::beta::Hash2;

// The negotiated format in PipeWire's own notation ("F32LE 2ch 48000Hz").
static Str FormatLabelFromPod(const spa_pod* param) {
  uint32_t media_type, media_subtype;
  if (!param || spa_format_parse(param, &media_type, &media_subtype) < 0) return {};
  if (media_type == SPA_MEDIA_TYPE_audio && media_subtype == SPA_MEDIA_SUBTYPE_raw) {
    spa_audio_info_raw info{};
    if (spa_format_audio_raw_parse(param, &info) >= 0) {
      const char* fmt = spa_debug_type_find_short_name(spa_type_audio_format, info.format);
      return f("{} {}ch {}Hz", fmt ? fmt : "?", info.channels, info.rate);
    }
  } else if (media_type == SPA_MEDIA_TYPE_video && media_subtype == SPA_MEDIA_SUBTYPE_raw) {
    spa_video_info_raw info{};
    if (spa_format_video_raw_parse(param, &info) >= 0) {
      const char* fmt = spa_debug_type_find_short_name(spa_type_video_format, info.format);
      return f("{} {}x{}", fmt ? fmt : "?", info.size.width, info.size.height);
    }
  }
  const char* t = spa_debug_type_find_short_name(spa_type_media_type, media_type);
  const char* st = spa_debug_type_find_short_name(spa_type_media_subtype, media_subtype);
  return f("{}/{}", t ? t : "?", st ? st : "?");
}

// ============================================================================
// PwHost: one connection to the daemon - a thread loop, a core, and a
// registry mirror of the graph (nodes, ports, links). Everything PipeWire
// runs on the loop thread; the mirror mutex bridges the mirrored facts to the
// UI. Lock order: pw_thread_loop_lock before mirror_mutex.
// ============================================================================

struct PwHost {
  pw_thread_loop* loop = nullptr;
  pw_context* context = nullptr;
  pw_core* core = nullptr;
  pw_registry* registry = nullptr;
  spa_hook core_listener = {};
  spa_hook registry_listener = {};
  bool connected = false;
  int sync_seq = 0;   // the sequence number of the pending core sync
  int sync_seen = 0;  // the last sequence number the done event delivered

  struct MirrorNode {
    uint32_t id = 0;
    Str name;
    Str media_class;
    Str state;
    Str format;        // negotiated, from the Format param
    float volume = 0;  // mean of channelVolumes, linear
    int channels = 0;  // size of channelVolumes
    bool mute = false;
    bool has_volume = false;
    bool has_mute = false;
    pw_proxy* proxy = nullptr;  // the bound pw_node delivering info and param events
    spa_hook listener = {};
    PwHost* host = nullptr;
  };
  struct MirrorPort {
    uint32_t id = 0;
    uint32_t node_id = 0;
    uint32_t port_index = 0;  // port.id within the node
    bool input = false;       // port.direction
    Str channel;              // audio.channel ("FL"), empty when not audio
  };
  struct MirrorLink {
    uint32_t id = 0;
    uint32_t out_node = 0;
    uint32_t out_port = 0;
    uint32_t in_node = 0;
    uint32_t in_port = 0;
  };
  std::mutex mirror_mutex;  // guards the three vectors' contents, read from the UI thread
  Vec<std::unique_ptr<MirrorNode>> nodes;
  Vec<MirrorPort> ports;
  Vec<MirrorLink> links;

  // Bumped whenever the node set changes (arrival, removal, rename,
  // media.class change), so the shelf can notice without comparing lists.
  std::atomic<uint32_t> graph_generation{0};

  // Board proxies that tick, for mirroring daemon links into board
  // connections. Expired entries are pruned on iteration.
  std::mutex objects_mutex;
  Vec<WeakPtr<PipeWireNode>> objects;

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
      static const pw_core_events core_events = {
          .version = PW_VERSION_CORE_EVENTS,
          .done = OnCoreDone,
          .error = OnCoreError,
      };
      pw_core_add_listener(core, &core_listener, &core_events, this);
      registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
      static const pw_registry_events registry_events = {
          .version = PW_VERSION_REGISTRY_EVENTS,
          .global = OnGlobal,
          .global_remove = OnGlobalRemove,
      };
      pw_registry_add_listener(registry, &registry_listener, &registry_events, this);
      connected = true;
      SyncLocked();  // the initial registry dump completes before first use
    }
    pw_thread_loop_unlock(loop);
  }

  static void OnCoreDone(void* data, uint32_t id, int seq) {
    auto& host = *(PwHost*)data;
    if (id != PW_ID_CORE) return;
    host.sync_seen = seq;
    pw_thread_loop_signal(host.loop, false);
  }

  static void OnCoreError(void* data, uint32_t id, int seq, int res, const char* message) {
    ERROR << "PipeWire: object " << id << ": " << (message ? message : "") << " ("
          << spa_strerror(res) << ")";
  }

  // One round trip to the daemon; on return every method call issued so far
  // has been processed and its registry events have landed in the mirror.
  // Requires the thread loop lock.
  void SyncLocked() {
    sync_seq = pw_core_sync(core, PW_ID_CORE, sync_seq);
    while (sync_seen != sync_seq) pw_thread_loop_wait(loop);
  }

  static void OnNodeInfo(void* data, const pw_node_info* info) {
    auto& node = *(MirrorNode*)data;
    auto& host = *node.host;
    auto lock = std::lock_guard(host.mirror_mutex);
    node.state = pw_node_state_as_string(info->state);
    if (info->props) {
      Str name = node.name;
      Str media_class = node.media_class;
      if (const char* v = spa_dict_lookup(info->props, PW_KEY_NODE_NAME)) node.name = v;
      if (const char* v = spa_dict_lookup(info->props, PW_KEY_MEDIA_CLASS)) node.media_class = v;
      if (name != node.name || media_class != node.media_class) {
        host.graph_generation.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  static void OnNodeParam(void* data, int seq, uint32_t id, uint32_t index, uint32_t next,
                          const spa_pod* param) {
    auto& node = *(MirrorNode*)data;
    auto& host = *node.host;
    if (!param) return;
    if (id == SPA_PARAM_Props && spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Props)) {
      auto* obj = (const spa_pod_object*)param;
      auto lock = std::lock_guard(host.mirror_mutex);
      if (const spa_pod_prop* p = spa_pod_object_find_prop(obj, nullptr, SPA_PROP_mute)) {
        bool m;
        if (spa_pod_get_bool(&p->value, &m) == 0) {
          node.mute = m;
          node.has_mute = true;
        }
      }
      if (const spa_pod_prop* p = spa_pod_object_find_prop(obj, nullptr, SPA_PROP_channelVolumes)) {
        uint32_t n = 0;
        auto* v = (const float*)spa_pod_get_array(&p->value, &n);
        if (v && n > 0 && ((const spa_pod_array*)&p->value)->body.child.type == SPA_TYPE_Float) {
          float sum = 0;
          for (uint32_t i = 0; i < n; ++i) sum += v[i];
          node.volume = sum / n;
          node.channels = (int)n;
          node.has_volume = true;
        }
      }
    } else if (id == SPA_PARAM_Format) {
      Str label = FormatLabelFromPod(param);
      auto lock = std::lock_guard(host.mirror_mutex);
      node.format = label;
    }
  }

  static void OnGlobal(void* data, uint32_t id, uint32_t permissions, const char* type,
                       uint32_t version, const spa_dict* props) {
    auto& host = *(PwHost*)data;
    auto lookup_id = [&](const char* key) -> uint32_t {
      const char* v = props ? spa_dict_lookup(props, key) : nullptr;
      return v ? (uint32_t)atoi(v) : 0;
    };
    if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
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
            .param = OnNodeParam,
        };
        pw_node_add_listener((pw_node*)node->proxy, &node->listener, &node_events, node.get());
        uint32_t ids[] = {SPA_PARAM_Props, SPA_PARAM_Format};
        pw_node_subscribe_params((pw_node*)node->proxy, ids, 2);
      }
      auto lock = std::lock_guard(host.mirror_mutex);
      host.nodes.push_back(std::move(node));
      host.graph_generation.fetch_add(1, std::memory_order_relaxed);
    } else if (spa_streq(type, PW_TYPE_INTERFACE_Port)) {
      MirrorPort port;
      port.id = id;
      port.node_id = lookup_id(PW_KEY_NODE_ID);
      port.port_index = lookup_id(PW_KEY_PORT_ID);
      if (const char* v = props ? spa_dict_lookup(props, PW_KEY_PORT_DIRECTION) : nullptr) {
        port.input = spa_streq(v, "in");
      }
      if (const char* v = props ? spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL) : nullptr) {
        port.channel = v;
      }
      auto lock = std::lock_guard(host.mirror_mutex);
      host.ports.push_back(port);
    } else if (spa_streq(type, PW_TYPE_INTERFACE_Link)) {
      MirrorLink link;
      link.id = id;
      link.out_node = lookup_id(PW_KEY_LINK_OUTPUT_NODE);
      link.out_port = lookup_id(PW_KEY_LINK_OUTPUT_PORT);
      link.in_node = lookup_id(PW_KEY_LINK_INPUT_NODE);
      link.in_port = lookup_id(PW_KEY_LINK_INPUT_PORT);
      auto lock = std::lock_guard(host.mirror_mutex);
      host.links.push_back(link);
    }
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
      host.graph_generation.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    std::erase_if(host.ports, [&](const MirrorPort& p) { return p.id == id; });
    std::erase_if(host.links, [&](const MirrorLink& l) { return l.id == id; });
  }

  MirrorNode* FindNodeLocked(StrView name) {
    for (auto& node : nodes) {
      if (node->name == name) return node.get();
    }
    return nullptr;
  }

  bool NodeHasPortsLocked(uint32_t node_id, bool input) {
    for (auto& p : ports) {
      if (p.node_id == node_id && p.input == input) return true;
    }
    return false;
  }

  Vec<uint32_t> LinkIdsLocked(uint32_t out_node, uint32_t in_node) {
    Vec<uint32_t> ids;
    for (auto& l : links) {
      if (l.out_node == out_node && l.in_node == in_node) ids.push_back(l.id);
    }
    return ids;
  }

  // UI-thread snapshot of everything the face shows.
  struct NodeFacts {
    Str media_class;
    Str state;
    Str format;
    float volume = 0;  // linear
    bool mute = false;
    bool has_volume = false;
    bool has_mute = false;
    bool has_in_ports = false;
    bool has_out_ports = false;
  };
  bool Find(StrView name, NodeFacts& facts) {
    auto lock = std::lock_guard(mirror_mutex);
    MirrorNode* node = FindNodeLocked(name);
    if (!node) return false;
    facts.media_class = node->media_class;
    facts.state = node->state;
    facts.format = node->format;
    facts.volume = node->volume;
    facts.mute = node->mute;
    facts.has_volume = node->has_volume;
    facts.has_mute = node->has_mute;
    facts.has_in_ports = NodeHasPortsLocked(node->id, true);
    facts.has_out_ports = NodeHasPortsLocked(node->id, false);
    return true;
  }

  // Writes SPA Props on the named node. Volume is linear here; the cubic
  // scaling is the object's concern.
  void SetProps(StrView name, Optional<float> volume, Optional<bool> mute) {
    if (!connected) return;
    pw_thread_loop_lock(loop);
    pw_proxy* proxy = nullptr;
    int channels = 1;
    {
      auto lock = std::lock_guard(mirror_mutex);
      if (MirrorNode* node = FindNodeLocked(name)) {
        proxy = node->proxy;
        channels = std::max(node->channels, 1);
      }
    }
    if (proxy) {
      uint8_t buffer[1024];
      spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
      spa_pod_frame frame;
      spa_pod_builder_push_object(&b, &frame, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      if (volume) {
        float vols[SPA_AUDIO_MAX_CHANNELS];
        channels = std::min(channels, (int)SPA_AUDIO_MAX_CHANNELS);
        for (int i = 0; i < channels; ++i) vols[i] = *volume;
        spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
        spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float, channels, vols);
      }
      if (mute) {
        spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
        spa_pod_builder_bool(&b, *mute);
      }
      auto* pod = (spa_pod*)spa_pod_builder_pop(&b, &frame);
      if (pod) {
        pw_node_set_param((pw_node*)proxy, SPA_PARAM_Props, 0, pod);
        SyncLocked();  // the subscribed param event lands before the next tick reads
      }
    }
    pw_thread_loop_unlock(loop);
  }

  // Pairs the out-direction ports of `out_node` with the in-direction ports
  // of `in_node`: by audio.channel where both sides name channels, otherwise
  // one-to-many for a single port, otherwise index by index.
  struct PortPair {
    uint32_t out_port;
    uint32_t in_port;
  };
  Vec<PortPair> PortPairsLocked(uint32_t out_node, uint32_t in_node) {
    Vec<MirrorPort> outs, ins;
    for (auto& p : ports) {
      if (p.node_id == out_node && !p.input) outs.push_back(p);
      if (p.node_id == in_node && p.input) ins.push_back(p);
    }
    auto by_index = [](const MirrorPort& a, const MirrorPort& b) {
      return a.port_index < b.port_index;
    };
    std::sort(outs.begin(), outs.end(), by_index);
    std::sort(ins.begin(), ins.end(), by_index);
    Vec<PortPair> pairs;
    Vec<bool> in_used(ins.size(), false);
    for (auto& o : outs) {
      if (o.channel.empty()) continue;
      for (int i = 0; i < (int)ins.size(); ++i) {
        if (in_used[i] || ins[i].channel != o.channel) continue;
        pairs.push_back({o.id, ins[i].id});
        in_used[i] = true;
        break;
      }
    }
    if (pairs.empty()) {
      if (outs.size() == 1) {
        for (auto& i : ins) pairs.push_back({outs[0].id, i.id});
      } else if (ins.size() == 1) {
        for (auto& o : outs) pairs.push_back({o.id, ins[0].id});
      } else {
        for (int i = 0; i < (int)std::min(outs.size(), ins.size()); ++i) {
          pairs.push_back({outs[i].id, ins[i].id});
        }
      }
    }
    return pairs;
  }

  // Creates the daemon links that make a board connection real. Links carry
  // object.linger so they outlive Automat; pairs the daemon already links
  // are left alone. Returns whether links between the two nodes exist when
  // it is done - false means try again (a node or its ports are not in the
  // graph yet).
  bool EnsureLinks(StrView out_name, StrView in_name) {
    if (!connected) return false;
    pw_thread_loop_lock(loop);
    uint32_t out_node = 0, in_node = 0;
    Vec<PortPair> missing;
    {
      auto lock = std::lock_guard(mirror_mutex);
      MirrorNode* a = FindNodeLocked(out_name);
      MirrorNode* b = FindNodeLocked(in_name);
      if (a && b) {
        out_node = a->id;
        in_node = b->id;
        for (auto& pair : PortPairsLocked(a->id, b->id)) {
          bool exists = false;
          for (auto& l : links) {
            exists |= (l.out_port == pair.out_port && l.in_port == pair.in_port);
          }
          if (!exists) missing.push_back(pair);
        }
      }
    }
    Vec<pw_proxy*> created;
    for (auto& pair : missing) {
      pw_properties* props = pw_properties_new(
          PW_KEY_LINK_OUTPUT_NODE, f("{}", out_node).c_str(), PW_KEY_LINK_OUTPUT_PORT,
          f("{}", pair.out_port).c_str(), PW_KEY_LINK_INPUT_NODE, f("{}", in_node).c_str(),
          PW_KEY_LINK_INPUT_PORT, f("{}", pair.in_port).c_str(), PW_KEY_OBJECT_LINGER, "true",
          nullptr);
      auto* proxy = (pw_proxy*)pw_core_create_object(core, "link-factory", PW_TYPE_INTERFACE_Link,
                                                     PW_VERSION_LINK, &props->dict, 0);
      pw_properties_free(props);
      if (proxy) created.push_back(proxy);
    }
    if (!created.empty()) {
      SyncLocked();  // the new links land in the mirror before the sweep looks
      for (auto* proxy : created) pw_proxy_destroy(proxy);  // the links linger
    }
    bool realized;
    {
      auto lock = std::lock_guard(mirror_mutex);
      realized = out_node && in_node && !LinkIdsLocked(out_node, in_node).empty();
    }
    pw_thread_loop_unlock(loop);
    return realized;
  }

  // Destroys every daemon link between the two named nodes.
  void DestroyLinks(StrView out_name, StrView in_name) {
    if (!connected) return;
    pw_thread_loop_lock(loop);
    Vec<uint32_t> doomed;
    {
      auto lock = std::lock_guard(mirror_mutex);
      MirrorNode* a = FindNodeLocked(out_name);
      MirrorNode* b = FindNodeLocked(in_name);
      if (a && b) doomed = LinkIdsLocked(a->id, b->id);
    }
    for (uint32_t id : doomed) pw_registry_destroy(registry, id);
    if (!doomed.empty()) SyncLocked();
    pw_thread_loop_unlock(loop);
  }

  // True when both nodes are in the graph yet no daemon link connects them -
  // the state where a realized board connection is a lie and must disconnect.
  bool BothPresentNoLinks(StrView out_name, StrView in_name) {
    auto lock = std::lock_guard(mirror_mutex);
    MirrorNode* a = FindNodeLocked(out_name);
    MirrorNode* b = FindNodeLocked(in_name);
    if (!a || !b) return false;
    return LinkIdsLocked(a->id, b->id).empty();
  }

  // Names of the nodes the named node's daemon links feed into.
  Vec<Str> LinkedPeersOf(StrView out_name) {
    auto lock = std::lock_guard(mirror_mutex);
    Vec<Str> peers;
    MirrorNode* a = FindNodeLocked(out_name);
    if (!a) return peers;
    for (auto& l : links) {
      if (l.out_node != a->id) continue;
      for (auto& node : nodes) {
        if (node->id == l.in_node && !node->name.empty()) {
          bool seen = false;
          for (auto& p : peers) seen |= (p == node->name);
          if (!seen) peers.push_back(node->name);
        }
      }
    }
    return peers;
  }

  void Register(WeakPtr<PipeWireNode> obj) {
    auto lock = std::lock_guard(objects_mutex);
    for (auto& w : objects) {
      if (w.GetUnsafe() == obj.GetUnsafe()) return;
    }
    objects.push_back(std::move(obj));
  }

  Vec<Ptr<PipeWireNode>> LiveProxies() {
    auto lock = std::lock_guard(objects_mutex);
    Vec<Ptr<PipeWireNode>> live;
    std::erase_if(objects, [&](WeakPtr<PipeWireNode>& w) {
      if (auto p = w.Lock()) {
        live.push_back(std::move(p));
        return false;
      }
      return true;
    });
    return live;
  }

  // The live node set for the shelf, in registry order. Nodes without a
  // node.name are skipped (a proxy addresses its node by name).
  struct NodeListing {
    Str name;
    Str media_class;
  };
  Vec<NodeListing> ListNodes() {
    auto lock = std::lock_guard(mirror_mutex);
    Vec<NodeListing> listing;
    for (auto& node : nodes) {
      if (node->name.empty()) continue;
      listing.push_back({node->name, node->media_class});
    }
    return listing;
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
  if (host.loop && stream) {
    pw_thread_loop_lock(host.loop);
    DestroyVuStreamLocked(*this);
    pw_thread_loop_unlock(host.loop);
  }
  WakeToys();
}

Str PipeWireNode::NodeName() const {
  auto lock = std::lock_guard(mutex);
  return node_name;
}

void PipeWireNode::RefreshFromMirror() {
  auto& host = PwHost::Get();
  host.Register(AcquireWeakPtr());
  Str name = NodeName();
  // The capture stream follows board membership: only a proxy standing on a
  // board listens to its node.
  bool want_stream = host.connected && here != nullptr && !name.empty();
  if (want_stream != (stream != nullptr)) {
    pw_thread_loop_lock(host.loop);
    DestroyVuStreamLocked(*this);
    if (want_stream) CreateVuStreamLocked(*this, name);
    pw_thread_loop_unlock(host.loop);
  }
  PwHost::NodeFacts facts;
  bool found = host.connected && host.Find(name, facts);
  auto lock = std::lock_guard(mutex);
  daemon = host.connected;
  if (found) {
    media_class = facts.media_class;
    state_word = facts.state;
    format = facts.format;
    has_in_ports = facts.has_in_ports;
    has_out_ports = facts.has_out_ports;
    has_volume = facts.has_volume;
    has_mute = facts.has_mute;
    volume = cbrtf(facts.volume);
    mute = facts.mute;
  } else {
    media_class.clear();
    format.clear();
    state_word = "absent";
    has_in_ports = has_out_ports = has_volume = has_mute = false;
    volume = 0;
    mute = false;
  }
}

void PipeWireNode::SetVolume(float v) {
  v = std::clamp(v, 0.f, 1.f);
  {
    auto lock = std::lock_guard(mutex);
    volume = v;
  }
  PwHost::Get().SetProps(NodeName(), v * v * v, std::nullopt);
  WakeToys();
}

void PipeWireNode::SetMute(bool m) {
  {
    auto lock = std::lock_guard(mutex);
    mute = m;
  }
  PwHost::Get().SetProps(NodeName(), std::nullopt, m);
  WakeToys();
}

Str PipeWireNode::FormatLabel() const {
  auto lock = std::lock_guard(mutex);
  return format;
}

void PipeWireNode::CanFeed(StreamArgument self, Interface end, Status& status) {
  StreamArgument::Table::DefaultCanConnect(self, end, status);
  if (!OK(status)) return;
  auto* peer = dynamic_cast<PipeWireNode*>(end.object_ptr);
  if (!peer) {
    AppendErrorMessage(status) +=
        "only PipeWire nodes can be linked; a bridge to other libraries is not built";
    return;
  }
  auto& host = PwHost::Get();
  PwHost::NodeFacts mine, theirs;
  if (!host.connected || !host.Find(NodeName(), mine)) {
    AppendErrorMessage(status) += f("node \"{}\" is not in the daemon's graph", NodeName());
    return;
  }
  if (!host.Find(peer->NodeName(), theirs)) {
    AppendErrorMessage(status) += f("node \"{}\" is not in the daemon's graph", peer->NodeName());
    return;
  }
  if (!mine.has_out_ports) {
    AppendErrorMessage(status) += f("node \"{}\" has no output ports", NodeName());
    return;
  }
  if (!theirs.has_in_ports) {
    AppendErrorMessage(status) += f("node \"{}\" has no input ports", peer->NodeName());
  }
}

void PipeWireNode::OnOutConnect(StreamArgument self, Interface end) {
  Str old_peer;
  if (auto old = self.state->target.Lock()) {
    if (auto* o = dynamic_cast<PipeWireNode*>(old.Owner<Object>())) old_peer = o->NodeName();
  }
  StreamArgument::Table::StreamOnConnect(self, end);
  auto* peer = dynamic_cast<PipeWireNode*>(end.object_ptr);
  Str my = NodeName();
  auto& host = PwHost::Get();
  if (!old_peer.empty() && (!peer || peer->NodeName() != old_peer)) {
    host.DestroyLinks(my, old_peer);
  }
  bool realized = peer && host.EnsureLinks(my, peer->NodeName());
  auto lock = std::lock_guard(mutex);
  out_links_realized = realized;
}

static bool SameBoard(const Object& a, const Object& b) {
  if (!a.here || !b.here) return false;
  auto board_a = a.here->parent_location.Lock();
  auto board_b = b.here->parent_location.Lock();
  return board_a && board_a == board_b;
}

void PipeWireNode::SyncBoardLinks() {
  auto& host = PwHost::Get();
  if (!host.connected) return;
  Str my = NodeName();
  if (my.empty()) return;
  if (auto target = out_stream.target.Lock()) {
    if (auto* peer = dynamic_cast<PipeWireNode*>(target.Owner<Object>())) {
      bool realized;
      {
        auto lock = std::lock_guard(mutex);
        realized = out_links_realized;
      }
      if (!realized) {
        // The connection is board intent the daemon does not carry yet (a
        // node may have been missing when it was made); keep trying.
        realized = host.EnsureLinks(my, peer->NodeName());
        auto lock = std::lock_guard(mutex);
        out_links_realized = realized;
      } else if (host.BothPresentNoLinks(my, peer->NodeName())) {
        // Realized links gone: another tool destroyed them, and the board
        // connection follows.
        out_stream->Disconnect();
      }
    }
  } else {
    // Board says disconnected: a daemon link whose peer has a proxy on the
    // same board appears as a connection (WirePlumber links simply appear).
    Vec<Str> peers = host.LinkedPeersOf(my);
    if (peers.empty()) return;
    for (auto& obj : host.LiveProxies()) {
      if (obj.get() == this || !SameBoard(*this, *obj)) continue;
      Str obj_name = obj->NodeName();
      for (Str& peer_name : peers) {
        if (obj_name != peer_name) continue;
        out_stream->Connect(obj->in_stream.Bind());
        return;
      }
    }
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
constexpr float kFormatRow = 3.2_mm;
constexpr float kVolumeRow = 5.0_mm;
constexpr float kMuteRow = 4.2_mm;
constexpr float kVuRow = 6.0_mm;
constexpr float kStatusRow = 5.0_mm;
constexpr float kBottomPad = 1.5_mm;
constexpr float kPlateH = kBand + kCreditRow + kNameRow + kFormatRow + kVolumeRow + kMuteRow +
                          kVuRow + kStatusRow + kBottomPad;

constexpr uint32_t kSeed = 0x9F1;

// Row tops, measured downward from the plate's top edge.
constexpr float kNameTop = kPlateH / 2 - kBand - kCreditRow;
constexpr float kFormatTop = kNameTop - kNameRow;
constexpr float kVolumeTop = kFormatTop - kFormatRow;
constexpr float kMuteTop = kVolumeTop - kVolumeRow;
constexpr float kVuTop = kMuteTop - kMuteRow;

}  // namespace

struct PwNameField : ui::TextField {
  PwNameField(ui::Widget* parent, std::string* text, float width)
      : ui::TextField(parent, text, width) {}
  StrView Name() const override { return "PwNameField"; }
};

struct PipeWireNodeToy;

// Dragging along the volume band; SPA Props follow the pointer.
struct VolumeDrag : Action {
  MortalPtr<PipeWireNodeToy> widget;
  VolumeDrag(ui::Pointer& p, PipeWireNodeToy& w);
  void Update() override;
};

struct PipeWireNodeToy : ui::beta::ObjectToy {
  std::unique_ptr<PwNameField> field;
  std::string name_edit_;

  // Tick-cached object state (UI thread only):
  Str name_applied_;
  Str media_class_;
  Str state_word_;
  Str format_;
  bool daemon_ = false;
  bool has_in_ = false;
  bool has_out_ = false;
  bool has_volume_ = false;
  bool has_mute_ = false;
  float volume_ = 0;
  bool mute_ = false;
  float vu_ = 0;

  PipeWireNodeToy(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) {
    if (auto node = LockObject<PipeWireNode>()) {
      auto lock = std::lock_guard(node->mutex);
      name_edit_ = node->node_name;
      name_applied_ = node->node_name;
    }
    field = std::make_unique<PwNameField>(this, &name_edit_, kPlateW - 2 * kSide);
    field->local_to_parent =
        SkM44::Translate(-kPlateW / 2 + kSide, kNameTop - kNameRow) * SkM44::Scale(0.55f, 0.55f, 1);
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
    if (&arg == static_cast<const Interface::Table*>(&PipeWireNode::out_stream_tbl)) {
      return Vec2AndDir{.pos = Vec2(-kPlateW / 2 + 10_mm, -kPlateH / 2), .dir = -90_deg};
    }
    return ObjectToy::ArgStart(arg);
  }

  Rect VolumeBand() const {
    return Rect{-kPlateW / 2 + kSide + 13_mm, kVolumeTop - 4.0_mm, kPlateW / 2 - kSide - 9_mm,
                kVolumeTop - 0.9_mm};
  }
  Rect MuteBox() const {
    return Rect{-kPlateW / 2 + kSide, kMuteTop - 3.9_mm, -kPlateW / 2 + kSide + 3.4_mm,
                kMuteTop - 0.5_mm};
  }

  // Returns whether any drawn fact changed.
  bool UpdateFromObject() {
    bool changed = false;
    if (auto node = LockObject<PipeWireNode>()) {
      if (Str(name_edit_) != name_applied_) {
        name_applied_ = name_edit_;
        node->SetNodeName(name_applied_);
      }
      node->RefreshFromMirror();
      node->SyncBoardLinks();
      auto lock = std::lock_guard(node->mutex);
      auto pull = [&changed](auto& cached, const auto& fresh) {
        changed |= (cached != fresh);
        cached = fresh;
      };
      pull(media_class_, node->media_class);
      pull(state_word_, node->state_word);
      pull(format_, node->format);
      pull(daemon_, node->daemon);
      pull(has_in_, node->has_in_ports);
      pull(has_out_, node->has_out_ports);
      pull(has_volume_, node->has_volume);
      pull(has_mute_, node->has_mute);
      pull(mute_, node->mute);
      changed |= fabsf(volume_ - node->volume) > 0.002f;
      volume_ = node->volume;
      float vu = node->vu.load(std::memory_order_relaxed);
      changed |= fabsf(vu_ - vu) > 0.002f;
      vu_ = vu;
    }
    return changed;
  }

  Tock Tick(time::Timer&) override {
    // The proxy mirrors a live external graph, so it keeps observing; it
    // repaints only when a mirrored fact moved.
    Tock tock = Tock::Ing;
    if (UpdateFromObject()) tock |= Tock::Draw;
    return tock;
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      if (has_mute_ && MuteBox().Contains(pos)) {
        if (auto node = LockObject<PipeWireNode>()) node->SetMute(!mute_);
        WakeAnimation();
        return nullptr;
      }
      if (has_volume_ && VolumeBand().Contains(pos)) {
        return std::make_unique<VolumeDrag>(p, *this);
      }
    }
    return ObjectToy::FindAction(p, btn);
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

    {  // stream ports, on the edges the streams flow through
      if (has_in_) {
        float w = ui::beta::TextWidth("input", ui::beta::kMicroSize);
        ui::beta::DrawText(canvas, "input", {-w / 2, kPlateH / 2 - kBand - 1.6_mm},
                           ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
      }
      if (has_out_) {
        float w = ui::beta::TextWidth("output", ui::beta::kMicroSize);
        ui::beta::DrawText(canvas, "output", {-kPlateW / 2 + 10_mm - w / 2, -kPlateH / 2 + 0.8_mm},
                           ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
      }
    }

    {  // caption over the name field
      ui::beta::DrawText(canvas, "node.name", {-kPlateW / 2 + kSide + 0.6_mm, kNameTop - 1.4_mm},
                         ui::beta::kMicroSize, ui::beta::kInkSoft, false, Seed(kSeed));
    }

    if (!format_.empty()) {  // the negotiated format, in PipeWire's own notation
      float w = ui::beta::TextWidth(format_, ui::beta::kMicroSize);
      ui::beta::DrawText(canvas, format_, {-w / 2, kFormatTop - 2.6_mm}, ui::beta::kMicroSize,
                         ui::beta::kInkSoft, false, Seed(kSeed));
    }

    {  // volume: an SPA Props instrument, printed as wpctl prints it
      auto state = has_volume_ ? ui::beta::State::Default : ui::beta::State::Disabled;
      uint32_t cs = Seed(Hash2(kSeed, 0x71));
      ui::beta::DrawText(canvas, "volume", {-kPlateW / 2 + kSide, kVolumeTop - 3.7_mm},
                         ui::beta::kMicroSize, has_volume_ ? ui::beta::kInk : ui::beta::kGray,
                         false, cs);
      ui::beta::Slider(canvas, VolumeBand(), has_volume_ ? std::min(volume_, 1.f) : 0, state, cs);
      if (has_volume_) {
        Str value = f("{:.2f}", volume_);
        float vw = ui::beta::TextWidth(value, ui::beta::kMicroSize);
        ui::beta::DrawText(canvas, value, {kPlateW / 2 - kSide - vw, kVolumeTop - 3.7_mm},
                           ui::beta::kMicroSize, ui::beta::kInk, false, cs);
      }
    }

    {  // mute: the other SPA Props instrument
      auto state = has_mute_ ? ui::beta::State::Default : ui::beta::State::Disabled;
      uint32_t cs = Seed(Hash2(kSeed, 0x72));
      Rect box = MuteBox();
      ui::beta::Checkbox(canvas, box, mute_, state, cs);
      ui::beta::DrawText(canvas, "mute", {box.right + 1.5_mm, box.bottom + 0.9_mm},
                         ui::beta::kMicroSize, has_mute_ ? ui::beta::kInk : ui::beta::kGray, false,
                         cs);
    }

    {  // The peak meter, fed by the capture stream.
      float bar_y = kVuTop - 4.4_mm;
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

VolumeDrag::VolumeDrag(ui::Pointer& p, PipeWireNodeToy& w) : Action(p), widget(&w) { Update(); }

void VolumeDrag::Update() {
  if (!widget) return;
  Rect band = widget->VolumeBand();
  Vec2 pos = pointer.PositionWithin(*widget);
  float t = std::clamp((pos.x - band.left) / band.Width(), 0.f, 1.f);
  widget->volume_ = t;
  if (auto node = widget->LockObject<PipeWireNode>()) node->SetVolume(t);
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> PipeWireNode::MakeToy(ui::Widget* parent) {
  return std::make_unique<PipeWireNodeToy>(parent, *this);
}

// ============================================================================
// Shelf
// ============================================================================

// Groups are ordered the way data flows on the board: producers above
// consumers, matching the GStreamer shelf's Source-to-Sink order.
static int MediaClassRank(StrView media_class) {
  if (media_class.contains("Source")) return 0;
  if (media_class.contains("Stream/Output")) return 1;
  if (media_class.contains("Duplex") || media_class.contains("Filter")) return 2;
  if (media_class.contains("Stream/Input")) return 3;
  if (media_class.contains("Sink")) return 4;
  if (media_class.empty()) return 6;
  return 5;
}

static SkColor MediaClassAccent(StrView media_class) {
  switch (MediaClassRank(media_class)) {
    case 0:
      return ui::beta::kGold;
    case 1:
      return ui::beta::kCyan;
    case 2:
      return ui::beta::kPurple;
    case 3:
      return ui::beta::kSky;
    case 4:
      return ui::beta::kBlue;
    default:
      return ui::beta::kGrayDark;
  }
}

struct PipeWireShelfToy : ui::beta::ObjectToy {
  static constexpr float kCell = 3.6_cm;
  static constexpr float kPad = 0.3_cm;
  static constexpr float kGroupGap = 0.95_cm;  // room for the next frame's tab
  static constexpr float kHeader = 2.4_cm;
  static constexpr float kMargin = 0.85_cm;
  static constexpr int kMaxCols = 4;

  struct PlacedGroup {
    Str label;
    SkColor accent;
    Rect frame;
  };

  Vec<std::unique_ptr<ui::ShelfButton>> buttons;
  Vec<PlacedGroup> groups;
  uint32_t generation_seen = 0;  // the graph_generation the current layout shows
  bool daemon = false;
  float sheet_w = 0;
  float sheet_h = 0;

  PipeWireShelfToy(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) {
    auto& host = PwHost::Get();
    generation_seen = host.graph_generation.load(std::memory_order_relaxed);
    daemon = host.connected;
    Rebuild(host.ListNodes());
  }

  // Lays the shelf out for `listing`: one clone-pile entry per distinct
  // node.name, framed by media.class group.
  void Rebuild(Vec<PwHost::NodeListing> listing) {
    buttons.clear();
    groups.clear();

    struct Group {
      Str label;
      Vec<Str> names;
    };
    Vec<Group> grouped;
    for (auto& node : listing) {
      Group* group = nullptr;
      for (auto& g : grouped) {
        if (g.label == node.media_class) group = &g;
      }
      if (!group) group = &grouped.emplace_back(Group{node.media_class});
      bool seen = false;  // several nodes may share a name; one entry serves them
      for (auto& n : group->names) seen |= (n == node.name);
      if (!seen) group->names.push_back(node.name);
    }
    std::stable_sort(grouped.begin(), grouped.end(), [](const Group& a, const Group& b) {
      if (MediaClassRank(a.label) != MediaClassRank(b.label)) {
        return MediaClassRank(a.label) < MediaClassRank(b.label);
      }
      return a.label < b.label;
    });

    float widest = ui::beta::TextWidth("PipeWire", 8.2_mm) + 2 * kMargin;
    float total_h = 0;
    for (auto& g : grouped) {
      int cols = std::min<int>(kMaxCols, (int)g.names.size());
      int rows = ((int)g.names.size() + kMaxCols - 1) / kMaxCols;
      widest = std::max(widest, cols * kCell + 2 * kPad);
      total_h += rows * kCell + 2 * kPad;
    }
    if (!grouped.empty()) total_h += kGroupGap * ((float)grouped.size() - 1);
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
      Str label = g.label.empty() ? Str("no media.class") : g.label;
      groups.push_back({label, MediaClassAccent(g.label), frame});
      for (int i = 0; i < (int)g.names.size(); ++i) {
        auto proxy = MAKE_PTR(PipeWireNode);
        proxy->SetNodeName(g.names[i]);
        buttons.emplace_back(std::make_unique<ui::ShelfButton>(this, std::move(proxy)));
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

  // The shelf follows the daemon: it keeps observing and relays out when the
  // node set changes.
  Tock Tick(time::Timer&) override {
    auto& host = PwHost::Get();
    uint32_t generation = host.graph_generation.load(std::memory_order_relaxed);
    if (generation != generation_seen || daemon != host.connected) {
      generation_seen = generation;
      daemon = host.connected;
      Rebuild(host.ListNodes());
      return Tock::Shape | Tock::Ing;
    }
    return Tock::Ing;
  }

  void Draw(SkCanvas& canvas) const override {
    Rect sheet = Rect::MakeCenterZero(sheet_w, sheet_h);
    SkPath body = Shape();
    SkPaint bg;
    bg.setAntiAlias(true);
    bg.setColor(ui::beta::kPaperCream);
    canvas.drawPath(body, bg);
    ui::beta::SketchyStroke(canvas, body, ui::beta::kInk, ui::beta::kStroke, Seed(0x2D), 1);

    StrView heading = "PipeWire";
    float hw = ui::beta::TextWidth(heading, 8.2_mm);
    Vec2 hb = {-hw * 0.5f, sheet.top - 1.6_cm};
    ui::beta::DrawText(canvas, heading, hb, 8.2_mm, ui::beta::kInk, true, Seed(0x2E));
    canvas.drawPath(
        ui::beta::WobbleLine({hb.x - 0.9_mm, hb.y - 2.3_mm}, {hb.x + hw + 0.9_mm, hb.y - 2.3_mm},
                             ui::beta::kWonk, ui::beta::kSeg, Seed(0x2F)),
        ui::beta::InkPaint(ui::beta::kRose, ui::beta::kStrokeBold));

    if (!daemon) {
      StrView note = "no daemon";
      float w = ui::beta::TextWidth(note, ui::beta::kBodySize);
      ui::beta::DrawText(canvas, note, {-w / 2, sheet.top - kHeader - 1_cm}, ui::beta::kBodySize,
                         ui::beta::kGrayDark, false, Seed(0x30));
    }

    for (int i = 0; i < (int)groups.size(); ++i) {
      ui::beta::GroupFrame(canvas, groups[i].frame, groups[i].label, groups[i].accent,
                           Seed(Hash2(0x70, (uint32_t)i)));
    }

    BakeChildren(canvas);

    ui::beta::DrawBetaStamp(canvas, {sheet.right - 0.6_cm, sheet.top - 0.6_cm}, 2.2_cm, -15.f,
                            Seed(0xB3), "BETA");
  }
};

std::unique_ptr<ObjectToy> PipeWireShelf::MakeToy(ui::Widget* parent) {
  return std::make_unique<PipeWireShelfToy>(parent, *this);
}

}  // namespace automat::library
