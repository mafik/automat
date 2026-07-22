// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "renderer.hpp"

#include <include/core/SkColor.h>
#include <include/core/SkColorType.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPathTypes.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkShader.h>
#include <include/core/SkStream.h>
#include <include/effects/SkGradient.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/encode/SkWebpEncoder.h>
#include <include/gpu/GpuTypes.h>
#include <include/gpu/graphite/BackendSemaphore.h>
#include <include/gpu/graphite/BackendTexture.h>
#include <include/gpu/graphite/GraphiteTypes.h>
#include <include/gpu/graphite/Surface.h>
#include <include/gpu/graphite/TextureInfo.h>
#include <include/gpu/graphite/vk/VulkanGraphiteTypes.h>
#include <src/gpu/graphite/Surface_Graphite.h>
#include <vulkan/vulkan_core.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <ranges>
#include <tracy/Tracy.hpp>

#include "../build/generated/embedded.hpp"
#include "blockingconcurrentqueue.hpp"
#include "color.hpp"
#include "drawable_rtti.hpp"
#include "font.hpp"
#include "global_resources.hpp"
#include "log.hpp"
#include "render_cost_model.hpp"
#include "render_shadows.hpp"
#include "root_widget.hpp"
#include "textures.hpp"
#include "thread_name.hpp"
#include "time.hpp"
#include "vk.hpp"
#include "vm.hpp"
#include "widget.hpp"

// TODO: move render_cost_model.txt into a more appropriate location
// TODO: replace `root_canvas` with surface properties
// TODO: move the "rendering" logic of Widget into a separate class (intended to run in the Client)
// TODO: use correct bounds in SkPictureRecorder::beginRecording
// TODO: render using a job system (tree of Semaphores)

constexpr bool kDebugRendering = false;
constexpr bool kDebugRenderEvents = false;
constexpr bool kDebugPackFrame = false;
constexpr bool kDebugVisual = false;
constexpr bool kDebugShapes = false;

using namespace automat::ui;
using namespace std;

template <typename T>
struct Concat {
  vector<T>& a;
  vector<T>& b;

  struct end_iterator {};

  struct iterator {
    vector<T>& a;
    vector<T>& b;
    size_t i = 0;

    bool operator==(end_iterator) const { return i == a.size() + b.size(); }
    bool operator!=(end_iterator) const { return i != a.size() + b.size(); }
    T& operator*() const { return i < a.size() ? a[i] : b[i - a.size()]; }
    iterator& operator++() {
      ++i;
      return *this;
    }
  };

  iterator begin() { return iterator{a, b, 0}; }
  end_iterator end() { return end_iterator{}; }
};

namespace automat {

string debug_render_events;

PackFrameRequest next_frame_request = {};

time::SteadyPoint frame_start = time::kZeroSteady;
static CostModel cost_model;
static std::vector<CostModel::Obs> cost_model_obs;
static const std::string cost_model_path = "render_cost_model.txt";

struct WidgetDrawable;

struct WidgetDrawableHolder {
  sk_sp<SkDrawable> sk_drawable;
  WidgetDrawable* widget_drawable;

  // May create a new WidgetDrawable - if none exists for the given id. Otherwise returns a
  // reference to the existing WidgetDrawable.
  static WidgetDrawableHolder& FindOrMake(uint32_t id);
};

// Map used by the client to keep track of resources needed to render widgets.
// TODO: replace with a set
std::mutex cached_widget_drawables_mutex;
map<uint32_t, WidgetDrawableHolder> cached_widget_drawables;

// Holds all the data necessary to render a Widget (without refering to the Widget itself).
struct WidgetDrawable : SkDrawableRTTI {
  const uint32_t id = 0;

  // Debugging
  float average_draw_millis = NAN;
  Str name;

  // Rendering
  SkIRect update_surface_bounds_root;
  sk_sp<SkDrawable> recording = nullptr;
  SkMatrix fresh_matrix;   // the most recent transform
  SkMatrix update_matrix;  // transform at the time of the last UpdateState

  Optional<SkRect> pack_frame_draw_bounds;
  Vec<Vec2> pack_frame_texture_anchors;

  Vec<Vec2> fresh_texture_anchors;
  time::SteadyPoint last_tick;

  Widget::Compositor compositor;
  float shadow_elevation = 0;
  vector<ShadowCaster> shadow_casters;  // shadow-casting children composited by this widget

  // RenderToSurface results
  struct Rendered {
    skgpu::graphite::BackendTexture texture;
    sk_sp<SkSurface> surface = nullptr;
    SkMatrix matrix;  // transform at the time of the last RenderToSurface

    // TODO: size part of this is already stored in `image_info`. Maybe only store the top/left
    // position?
    SkIRect surface_bounds_root;
    Vec<Vec2> texture_anchors;
    // Bounds of the widget's texture (without any clipping) in its local coordinate space.
    // Note that surface have different dimensions. It may be larger (to account for rounding to
    // full pixels) or smaller (due too clipping).
    Rect surface_bounds_local;
    SkImageInfo image_info;
    float snap_millis = 0;

    ~Rendered() {
      if (texture.isValid()) {
        surface->recorder()->deleteBackendTexture(texture);
      }
    }
  } frame_a;

  // Time spent in insertRecording().
  float insert_recording_millis = 0;

  // Note: these were used for multi-buffering but are no longer necessary. Kept around since it may
  // actually be a good idea to reintroduce it in the future.
  Rendered& rendered() { return frame_a; }
  Rendered& in_progress() { return frame_a; }

  std::unique_ptr<skgpu::graphite::Recording> graphite_recording;
  void InsertRecording();

  // Synchronization
  vk::Semaphore vk_semaphore;
  skgpu::graphite::BackendSemaphore sk_semaphore;
  bool signal_semaphore = false;  // only signal semaphore if there is a parent that waits for it
  vector<WidgetDrawable*>
      wait_list;  // child widgets that must be rendered first, cleared after every frame
  vector<WidgetDrawable*>
      then_list;       // parent widgets that must render after this one, cleared after every frame
  int wait_count = 0;  // number of children that must be rendered first, cleared after every frame

  WidgetDrawable(uint32_t id);
  ~WidgetDrawable() = default;

  struct Update {
    uint32_t id;
    uint32_t parent_id;  // used to delay rendering of parents (which must render after children)

    // Debugging
    float average_draw_millis;
    Str name;
    time::SteadyPoint last_tick;

    // Rendering
    SkIRect surface_bounds_root;

    // When rendering locally, we prefer passing drawables without serialization.
    // Remote rendering (not implemented yet) requires us to serialize them.
    sk_sp<SkDrawable> recording_drawable;
    sk_sp<SkData> recording_data;

    Optional<SkRect> pack_frame_draw_bounds;
    Vec<Vec2> pack_frame_texture_anchors;

    Widget::Compositor compositor;
  };

  void UpdateState(const Update&);

  SkRect onGetBounds() override;
  void onDraw(SkCanvas* canvas) override;
  const char* getTypeName() const override { return "WidgetDrawable"; }
  void flatten(SkWriteBuffer& buffer) const override;
  static sk_sp<SkFlattenable> CreateProc(SkReadBuffer& buffer);

  static WidgetDrawable* FindOrNull(uint32_t id) {
    auto lock = std::lock_guard(cached_widget_drawables_mutex);
    if (auto it = cached_widget_drawables.find(id); it != cached_widget_drawables.end()) {
      return it->second.widget_drawable;
    }
    // This may happen when the widget is off screen. Its parent usually isn't aware of that and
    // attempts to compose it regardless. On the client side the result is that we don't find any
    // WidgetDrawable to compose.
    return nullptr;
  }
};

sk_sp<SkDrawable> MakeWidgetDrawable(Widget& widget) {
  auto& drawable_holder = WidgetDrawableHolder::FindOrMake(widget.ID());
  return drawable_holder.sk_drawable;
}
// In order for remote rendering to work, the bandwidth of rendering commands must fit the
// capabilities of the network. Automat aspires to render itself over a typical home Wi-Fi -
// targetting 10 Mbps LAN connections. While it may seem conservative, even the sophisticated Wi-Fi
// setups are unreliable and often degrade temporarily when under noisy conditions.
//
// Rendering at 60fps means that a single frame has a budget of 20kB.
//
// This procedure is meant to warn about widgets that require more than 10kB of rendering commands.
// (10kB chosen arbitrarily - to leave some space for other widgets).
static void WarnLargeRecording(StrView name, Size size) {
  if constexpr (build_variant::NotRelease) {
    static Size warning_threshold = 10 * 1024;
    if (size > warning_threshold) {
      LOG << "Warning: Widget " << name << " drew a frame of size " << size / 1024 << "kB";
      warning_threshold = size;  // prevent spamming the log
    }
  }
}

SkRect WidgetDrawable::onGetBounds() { return *pack_frame_draw_bounds; }
void WidgetDrawable::flatten(SkWriteBuffer& buffer) const {
  // Normally this shouldn't be called. There is no point serializing the render state.
  buffer.writeInt(id);
}
WidgetDrawableHolder& WidgetDrawableHolder::FindOrMake(uint32_t id) {
  auto lock = std::lock_guard(cached_widget_drawables_mutex);
  auto it = cached_widget_drawables.find(id);
  if (it == cached_widget_drawables.end()) {
    WidgetDrawable* typed_ptr = nullptr;
    auto sk = SkDrawableRTTI::Make<WidgetDrawable>(&typed_ptr, id);
    it = cached_widget_drawables.emplace(id, WidgetDrawableHolder{std::move(sk), typed_ptr}).first;
  }
  return it->second;
}
sk_sp<SkFlattenable> WidgetDrawable::CreateProc(SkReadBuffer& buffer) {
  int id = buffer.readInt();
  auto& ref = WidgetDrawableHolder::FindOrMake(id);
  return ref.sk_drawable;
}

void WidgetDrawable::UpdateState(const Update& update) {
  average_draw_millis = update.average_draw_millis;
  update_surface_bounds_root = update.surface_bounds_root;

  name = update.name;
  last_tick = update.last_tick;

  if (update.recording_drawable) {
    recording = std::move(update.recording_drawable);
  } else {
    SkDeserialProcs deserial_procs{};
    auto data = update.recording_data;

    WarnLargeRecording(name, data->size());
    sk_sp<SkFlattenable> deserialized_recording = SkFlattenable::Deserialize(
        SkFlattenable::kSkDrawable_Type, data->data(), data->size(), &deserial_procs);
    recording = sk_sp<SkDrawable>(static_cast<SkDrawable*>(deserialized_recording.release()));
  }

  update_matrix = fresh_matrix;
  pack_frame_draw_bounds = update.pack_frame_draw_bounds;
  pack_frame_texture_anchors = update.pack_frame_texture_anchors;
  compositor = update.compositor;
}

const skgpu::graphite::TextureInfo kTextureInfo = [] {
  skgpu::graphite::VulkanTextureInfo vulkan_texture_info{};
  vulkan_texture_info.fFormat = VK_FORMAT_B8G8R8A8_UNORM;
  vulkan_texture_info.fImageUsageFlags |=
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  return skgpu::graphite::TextureInfos::MakeVulkan(vulkan_texture_info);
}();

moodycamel::BlockingConcurrentQueue<WidgetDrawable*> recording_queue;
moodycamel::BlockingConcurrentQueue<WidgetDrawable*> recorded_queue;

void VkRecorderThread(int thread_id, std::unique_ptr<skgpu::graphite::Recorder> recorder) {
  SetThreadName("VkRecorder" + std::to_string(thread_id), 1);

  while (true) {
    WidgetDrawable* w = nullptr;
    recording_queue.wait_dequeue(w);
    if (w == nullptr) break;

    ZoneScopedN("Recording");
    ZoneName(w->name.c_str(), w->name.size());

    auto cpu_started = time::SteadyNow();
    auto& frame = w->in_progress();
    if (!w->shadow_casters.empty()) {
      RenderShadowHeightMap(*recorder, w->shadow_casters, frame.image_info.dimensions(),
                            {frame.surface_bounds_root.left(), frame.surface_bounds_root.top()});
    }
    auto graphite_canvas = recorder->makeDeferredCanvas(frame.image_info, kTextureInfo);
    graphite_canvas->clear(SK_ColorTRANSPARENT);
    graphite_canvas->translate(-frame.surface_bounds_root.left(), -frame.surface_bounds_root.top());
    graphite_canvas->clipIRect(frame.surface_bounds_root);

    float scale = frame.matrix.getMaxScale();
    // Bug workaround: when RRect with a blur is renderid in skia at a very high scale, it may
    // allocate a gigantic texture on the GPU. This check tries to prevent it - but it leaves the
    // widget's texture as transparent black.
    // This is hidden by a "quantum realm" compositor effect on RootWidget.
    // One example of this bug is the Timeline widget. If it works OK without this check then it's
    // probably fine to remove it.
    if (scale < 1000000) {
      // Remove all Drawables by converting the commands into SkPicture
      // This line calls the onDraw methods of all the CHILD widgets.
      w->recording->makePictureSnapshot()->playback(graphite_canvas);
    }

    w->graphite_recording = recorder->snap();
    frame.snap_millis = time::ToSeconds(time::SteadyNow() - cpu_started) * 1000;

    recorded_queue.enqueue(w);
  }
}

constexpr int kNumVkRecorderThreads = 4;

std::jthread vk_recorder_threads[kNumVkRecorderThreads];

std::unique_ptr<skgpu::graphite::Recorder> global_foreground_recorder;

bool renderer_initialized = false;

void RendererInit() {
  if (renderer_initialized) return;
  SkFlattenable::Register("WidgetDrawable", WidgetDrawable::CreateProc);
  skgpu::graphite::RecorderOptions options;
  if (image_provider == nullptr) {
    image_provider.reset(new AutomatImageProvider());
  }
  options.fImageProvider = image_provider;
  // Recordings which are part of the current frame might be recorded with
  // "fRequireOrderedRecordings" set to true.
  // This would require us to set up separate recorders for `frame` & `overflow` widgets.
  // The performance gain doesn't justify this split just yet.
  options.fRequireOrderedRecordings = false;
  for (int i = 0; i < kNumVkRecorderThreads; ++i) {
    auto fg_recorder = vk::graphite_context->makeRecorder(options);
    vk_recorder_threads[i] = std::jthread(VkRecorderThread, i, std::move(fg_recorder));
  }
  global_foreground_recorder = vk::graphite_context->makeRecorder(options);
  renderer_initialized = true;
}

int foreground_rendering_jobs = 0;
void RendererShutdown() {
  if (!renderer_initialized) return;

  for (int i = 0; i < kNumVkRecorderThreads; ++i) {
    recording_queue.enqueue(nullptr);
  }
  for (int i = 0; i < kNumVkRecorderThreads; ++i) {
    vk_recorder_threads[i].join();
  }

  {
    auto lock = std::lock_guard(cached_widget_drawables_mutex);
    cached_widget_drawables.clear();
  }
  next_frame_request.render_results.clear();
  if (image_provider) {
    image_provider->cache.clear();
  }

  ShutdownShadows(*global_foreground_recorder);
  global_foreground_recorder.reset();
  vk::graphite_context->submit({skgpu::graphite::SyncToCpu::kYes});
  vk::background_context->submit({skgpu::graphite::SyncToCpu::kYes});

  renderer_initialized = false;
}

void WidgetDrawable::InsertRecording() {
  auto& frame = in_progress();
  skgpu::graphite::InsertRecordingInfo insert_recording_info;
  insert_recording_info.fRecording = graphite_recording.get();
  insert_recording_info.fTargetSurface = frame.surface.get();

  if (signal_semaphore) {
    insert_recording_info.fSignalSemaphores = &sk_semaphore;
    insert_recording_info.fNumSignalSemaphores = 1;
  }

  vector<skgpu::graphite::BackendSemaphore> wait_list_vec;
  if (!wait_list.empty()) {
    wait_list_vec.reserve(wait_list.size());
    for (auto* dep : wait_list) {
      wait_list_vec.emplace_back(dep->sk_semaphore);
    }
    insert_recording_info.fWaitSemaphores = wait_list_vec.data();
    insert_recording_info.fNumWaitSemaphores = wait_list_vec.size();
  }

  // Note: Do not use "FinishedWithStats"! On some drivers (lavapipe) it makes submissions 30x more
  // expensive by requireing extra synchronizations.
  insert_recording_info.fFinishedContext = this;
  insert_recording_info.fFinishedProc = [](skgpu::graphite::GpuFinishedContext context,
                                           skgpu::CallbackResult result) {
    auto* w = static_cast<WidgetDrawable*>(context);
    if (result == skgpu::CallbackResult::kFailed) {
      ERROR << "Failed to insert recording for " << w->name;
    }
    auto& frame = w->in_progress();
    float approx_render_millis = frame.snap_millis + w->insert_recording_millis;
    next_frame_request.render_results.emplace_back(RenderResult{w->id, approx_render_millis});
    --foreground_rendering_jobs;
    if constexpr (kDebugRendering && kDebugRenderEvents) {
      debug_render_events += "Finished(";
      debug_render_events += w->name;
      debug_render_events += ") ";
    }
  };
  ++foreground_rendering_jobs;
  skgpu::graphite::Context* context = vk::graphite_context.get();
  if constexpr (kDebugRendering && kDebugRenderEvents) {
    debug_render_events += "InsertRecording(";
    debug_render_events += name;
    debug_render_events += ") ";
  }
  auto _t_ins = time::SteadyNow();
  context->insertRecording(insert_recording_info);
  insert_recording_millis = time::ToSeconds(time::SteadyNow() - _t_ins) * 1000;
  // submit() is deferred and batched to preserve semaphore ordering
}

void WidgetDrawable::onDraw(SkCanvas* canvas) {
  const auto& frame = rendered();
  if (frame.surface == nullptr) {
    // This widget wasn't included by frame packing - there is no need to draw anything.
    return;
  }
  if (shadow_elevation > 0) {
    DrawShadow(*canvas, {SkSurfaces::AsImage(frame.surface), frame.surface_bounds_root,
                         frame.matrix, frame.surface_bounds_local, shadow_elevation});
  }
  if constexpr (kDebugVisual) {
    SkPaint texture_bounds_paint;  // translucent black
    texture_bounds_paint.setStyle(SkPaint::kStroke_Style);
    texture_bounds_paint.setColor(SkColorSetARGB(128, 0, 0, 0));
    canvas->drawRect(frame.surface_bounds_local.sk, texture_bounds_paint);
  }

  SkRect surface_size = SkRect::MakeWH(frame.surface->width(), frame.surface->height());
  auto anchor_count = min<int>(frame.texture_anchors.size(), fresh_texture_anchors.size());

  if (compositor == Widget::Compositor::COPY_RAW) {
    canvas->resetMatrix();
    // It feels like SkSurface::draw should work, but for unknown reason it doesn't:
    // frame.surface->draw(canvas, 0, 0);
    canvas->drawImage(SkSurfaces::AsImage(frame.surface), 0, 0);
  } else if (compositor == Widget::Compositor::ANCHOR_WARP) {
    Status status;
    static auto effect = resources::CompileShader(embedded::assets_anchor_warp_rt_sksl, status);
    assert(effect);
    SkRuntimeEffectBuilder builder(effect);

    SkMatrix root_to_local;
    (void)frame.matrix.invert(&root_to_local);
    Rect local_surface_bounds = SkRect::Make(frame.surface_bounds_root);
    root_to_local.mapRect(&local_surface_bounds.sk);
    builder.uniform("surfaceOrigin") = local_surface_bounds.BottomLeftCorner();
    builder.uniform("surfaceSize") = local_surface_bounds.Size();
    builder.uniform("surfaceResolution") = Vec2(frame.surface->width(), frame.surface->height());
    Vec2 anchors_last[2] = {
        frame.texture_anchors[0],
        frame.texture_anchors[anchor_count > 1 ? 1 : 0],
    };
    Vec2 anchors_curr[2] = {
        fresh_texture_anchors[0],
        fresh_texture_anchors[anchor_count > 1 ? 1 : 0],
    };
    builder.uniform("anchorsLast").set(anchors_last, 2);
    builder.uniform("anchorsCurr").set(anchors_curr, 2);
    builder.child("surface") = SkSurfaces::AsImage(frame.surface)->makeShader({});

    auto shader = builder.makeShader();
    SkPaint paint;
    paint.setShader(shader);

    // Heuristic for finding same texture bounds (guaranteed to contain the whole widget):
    // - for every anchor move the old texture bounds by its displacement
    // - compute a union of all the moved bounds
    Rect new_anchor_bounds = Rect::MakeEmptyAt(fresh_texture_anchors[0]);
    for (int i = 0; i < anchor_count; ++i) {
      Vec2 delta = fresh_texture_anchors[i] - frame.texture_anchors[i];
      Rect offset_bounds = frame.surface_bounds_local.MoveBy(delta);
      new_anchor_bounds.ExpandToInclude(offset_bounds);
    }
    canvas->drawRect(new_anchor_bounds.sk, paint);
  } else if (compositor == Widget::Compositor::GLITCH) {
    canvas->save();

    // TODO: use `fresh_matrix` to draw the object at its most recent position.
    // Here is the "classic" approach to do it:
    // SkMatrix inverse;
    // (void)rendered_matrix.invert(&inverse);
    // canvas->concat(inverse);
    // canvas->concat(fresh_matrix);
    // Unfortunately this doesn't work because `rendered_matrix` and `fresh_matrix` include the
    // whole chain of transforms of parent widgets. This causes complex jittering that changes
    // its behavior depending on which widgets are rendered to textures and whether they've been
    // packed or sent to overflow.
    // Proper solution would probably require some careful test cases.

    SkRect draw_bounds = frame.surface_bounds_local.sk;

    /////////////////////////////////////////////////
    // Map from the local coordinates to surface UV
    /////////////////////////////////////////////////
    // First go from local space (metric) to window space (pixels)
    SkMatrix surface_transform = frame.matrix;
    // Now our surface is axis-aligned.
    // Map the surface bounds to unit square.
    surface_transform.postConcat(
        SkMatrix::RectToRect(SkRect::Make(frame.surface_bounds_root), SkRect::MakeWH(1, 1)));
    // Finally flip the y-axis (Skia uses bottom-left origin, but we use top-left)
    surface_transform.postScale(1, -1, 0, 0.5);

    // Skia puts the origin at the top left corner (going down), but we use bottom left (going
    // up). This flip makes all the textures composite in our coordinate system correctly.
    if (anchor_count) {
      SkMatrix anchor_mapping;
      // Apply the inverse transform to the surface mapping - we want to get the original texture
      // position. Note that this transform uses `draw_texture_anchors` which have been saved
      // during the last RenderToSurface.
      if (anchor_mapping.setPolyToPoly(
              SkSpan<const SkPoint>{&fresh_texture_anchors[0].sk, size_t(anchor_count)},
              SkSpan<const SkPoint>{&frame.texture_anchors[0].sk, size_t(anchor_count)})) {
        surface_transform.preConcat(anchor_mapping);
        SkMatrix inverse;
        (void)anchor_mapping.invert(&inverse);
        inverse.mapRectScaleTranslate(&draw_bounds, draw_bounds);
      }
    }

    Status status;
    static auto effect = resources::CompileShader(embedded::assets_glitch_rt_sksl, status);
    assert(effect);

    SkRuntimeEffectBuilder builder(effect);
    builder.uniform("surfaceResolution") = Vec2(frame.surface->width(), frame.surface->height());
    builder.uniform("surfaceTransform") = surface_transform;
    float time = time::SteadySaw<1.0>();
    builder.uniform("time") = time;
    SkSamplingOptions sampling;
    builder.child("surface") = SkSurfaces::AsImage(frame.surface)
                                   ->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, sampling);
    auto shader = builder.makeShader();
    SkPaint paint;
    paint.setShader(shader);
    canvas->drawRect(draw_bounds, paint);

    if constexpr (kDebugVisual) {
      SkPaint surface_bounds_paint;
      constexpr int kNumColors = 10;
      SkColor4f colors[kNumColors];
      float pos[kNumColors];
      double fraction = time::ToSeconds(last_tick.time_since_epoch() % 4s);
      SkMatrix shader_matrix = SkMatrix::RotateDeg(fraction * -360.0f, surface_size.center());
      for (int i = 0; i < kNumColors; ++i) {
        float hsv[] = {i * 360.0f / kNumColors, 1.0f, 1.0f};
        colors[i] = SkColor4f::FromColor(SkHSVToColor((kNumColors - i) * 255 / kNumColors, hsv));
        pos[i] = (float)i / (kNumColors - 1);
      }
      surface_bounds_paint.setShader(SkShaders::SweepGradient(
          surface_size.center(),
          SkGradient{SkGradient::Colors{colors, pos, SkTileMode::kClamp}, {}}, &shader_matrix));
      surface_bounds_paint.setStyle(SkPaint::kStroke_Style);
      surface_bounds_paint.setStrokeWidth(2.0f);
      canvas->concat(SkMatrix::RectToRect(surface_size, frame.surface_bounds_local.sk));
      canvas->drawRect(surface_size.makeInset(1, 1), surface_bounds_paint);
    }
    canvas->restore();
  } else if (compositor == Widget::Compositor::QUANTUM_REALM) {
    SkMatrix surface_transform = frame.matrix;
    surface_transform.postConcat(
        SkMatrix::RectToRect(SkRect::Make(frame.surface_bounds_root),
                             SkRect::MakeWH(frame.surface->width(), frame.surface->height())));

    float scale = frame.matrix.mapRadius(1);
    float scale_log = log10f(scale);
    float quantum_realm = GetRatio(scale_log, 5, 6);

    if (quantum_realm == 0) {
      SkMatrix inverse;
      (void)surface_transform.invert(&inverse);
      canvas->concat(inverse);
      canvas->drawImage(SkSurfaces::AsImage(frame.surface), 0, 0);
    } else {
      Status status;
      static auto effect = resources::CompileShader(embedded::assets_quantum_realm_sksl, status);
      if (!OK(status)) {
        FATAL << status;
      }

      SkRuntimeEffectBuilder builder(effect);
      builder.uniform("iQuantumRealm") = quantum_realm;
      builder.uniform("iScaleLog10") = scale_log;
      builder.uniform("iLocalToPx") = fresh_matrix;
      builder.uniform("iLocalToSurface") = surface_transform;
      builder.uniform("iResolution") = Vec2(frame.surface_bounds_root.width() - kCanvasMargin * 2,
                                            frame.surface_bounds_root.height() - kCanvasMargin * 2);
      float time = time::SteadySaw<M_PI * 2>();
      builder.uniform("iTime") = time;
      SkSamplingOptions sampling;
      builder.child("iSurface") =
          SkSurfaces::AsImage(frame.surface)
              ->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, kFastSamplingOptions);
      auto shader = builder.makeShader();
      SkPaint paint;
      paint.setShader(shader);
      canvas->drawRect(frame.surface_bounds_local.sk, paint);

      SkPaint green;
      green.setColor(SK_ColorGREEN);
      canvas->drawCircle(50, 50, 20, green);
    }
  }

  if constexpr (kDebugVisual) {
    SkPaint old_anchor_paint;
    old_anchor_paint.setStyle(SkPaint::kStroke_Style);
    old_anchor_paint.setColor(SkColorSetARGB(255, 255, 0, 0));
    SkPaint new_anchor_paint;
    new_anchor_paint.setStyle(SkPaint::kStroke_Style);
    new_anchor_paint.setColor(SkColorSetARGB(255, 0, 0, 255));
    SkPaint bounds_paint;
    bounds_paint.setStyle(SkPaint::kStroke_Style);
    bounds_paint.setColor(SkColorSetARGB(128, 0, 255, 0));

    for (int i = 0; i < anchor_count; ++i) {
      canvas->drawCircle(frame.texture_anchors[i].sk, 1_mm, old_anchor_paint);
      canvas->drawCircle(fresh_texture_anchors[i].sk, 1_mm, new_anchor_paint);
      canvas->drawLine(frame.texture_anchors[i].sk, fresh_texture_anchors[i].sk, new_anchor_paint);
    }
  }

  if constexpr (kDebugVisual) {
    auto& font = GetFont();
    SkPaint text_paint;
    canvas->translate(pack_frame_draw_bounds->left(),
                      min(pack_frame_draw_bounds->top(), pack_frame_draw_bounds->bottom()));
    auto text = f("{:.1f}", average_draw_millis);
    font.DrawText(*canvas, text, text_paint);
  }
}  // namespace automat

WidgetDrawable::WidgetDrawable(uint32_t id) : id(id) {}

struct PackedFrame {
  vector<WidgetDrawable::Update> frame;
  map<uint32_t, Vec<Vec2>> fresh_texture_anchors;
  map<uint32_t, SkMatrix> fresh_matrices;
  struct ShadowCasterRef {
    uint32_t id, baker_id;
    float elevation;
  };
  vector<ShadowCasterRef> shadow_casters;
};

void PackFrame(RootWidget& rw, const PackFrameRequest& request, PackedFrame& pack) {
  rw.timer.Tick();
  auto now = rw.timer.now;
  auto root_widget_bounds_px =
      Rect::MakeAtZero<LeftX, BottomY>(Round(rw.size * rw.display_pixels_per_meter))
          .Outset(kCanvasMargin);
  rw.ValidateHierarchy();
  {
    uint32_t current = vm.wake_counter.load(std::memory_order_relaxed);
    if (current != rw.observed_vm_wake_counter) {
      rw.observed_vm_wake_counter = current;
      rw.WakeAnimationAt(now);
    }
  }
  rw.Poll();

  enum class Verdict {
    Unknown = 0,
    Pack = 1,
    Overflow = 2,
    Skip_Clipped = 3,
    Skip_NoTexture = 4,
    Skip_Rendering = 5,  // either this widget or one of its ancestors is still rendering
  };

  static constexpr const char* kVerdictNames[] = {
      "Unknown", "Pack", "Overflow", "Skip_Clipped", "Skip_NoTexture", "Skip_Rendering",
  };

  struct WidgetTree {
    Widget* widget;
    Verdict verdict = Verdict::Unknown;
    int baker;
    int repack_parent;  // Ancestor this node folds its render cost into.
    int parent;
    bool detached = false;  // an Over()/Under() child, composited into its baker rather than parent
    int prev_job = -1;
    int next_job = -1;
    bool same_scale;
    bool wants_to_draw = false;
    bool surface_reusable = false;  // set to true if existing surface covers the visible area
    float render_area_px2 = 0;      // texture resolution (pixels rasterized if re-rendered)
    float model_render_ms = 0;
    int type_id = -1;
    SkMatrix window_to_local;
    SkIRect surface_bounds_root;  // copied over to Widget, if drawn
    Vec<Vec2> pack_frame_texture_anchors;
    // Bounds (in local coords) which are rendered to the surface.
    Rect new_visible_bounds;
    const RenderResult* render_result = nullptr;
    void SetVerdict(Verdict v) {
      if constexpr (kDebugRendering) {
        if (verdict != Verdict::Unknown) {
          ERROR << "Widget " << widget->Name() << " had verdict " << kVerdictNames[(int)verdict]
                << " and was changed to " << kVerdictNames[(int)v];
        }
      }
      verdict = v;
    }
  };
  vector<WidgetTree> tree;

  auto GetLag = [now](WidgetTree& tree_entry) -> float {
    return time::ToSeconds(max<time::Duration>(0s, now - tree_entry.widget->invalidated));
  };

  auto GetRenderMillis = [](WidgetTree& tree_entry) -> float { return tree_entry.model_render_ms; };

  {  // Step 1 - update the cache entries for widgets rendered by the client

    Optional<LOG_IndentGuard> indent;
    if constexpr (kDebugPackFrame) {
      LOG << "PackFrameRequest:";
      indent.emplace();
      LOG << "measured_render_millis: " << request.real_render_millis << "ms";
    }

    float sum_approx_millis = 0;
    for (auto& rr : request.render_results) sum_approx_millis += rr.approx_render_millis;
    bool calibrate = request.real_render_millis > 0 && sum_approx_millis > 0;
    for (auto& render_result : request.render_results) {
      Widget* widget = Widget::Find(render_result.id);
      if (widget == nullptr) {
        ERROR << "Widget " << render_result.id << " not found!";
        continue;
      }
      if constexpr (kDebugRendering) {
        if (!widget->rendering) {
          FATAL << "Widget " << widget->Name() << " has been returned by client multiple times!";
        }
      }
      float render_millis = calibrate ? request.real_render_millis *
                                            (render_result.approx_render_millis / sum_approx_millis)
                                      : render_result.approx_render_millis;
      if constexpr (kDebugPackFrame) {
        LOG << widget->Name() << ": render_time = " << render_result.approx_render_millis
            << "ms, calibrated_millis = " << render_millis << "ms";
      }
      if (isnan(widget->smooth_render_millis)) {
        widget->smooth_render_millis = render_millis;
      } else {
        widget->smooth_render_millis = 0.9 * widget->smooth_render_millis + 0.1 * render_millis;
      }

      widget->rendering = false;
    }
  }

  {                            // Step 1.5 - update the render model
    if (!cost_model.loaded) {  // warm-start from the persisted weights
      cost_model.loaded = true;
      cost_model.Load(cost_model_path);
    }

    float predicted = cost_model.TrainStep(cost_model_obs, request.real_render_millis);
    cost_model_obs.clear();
    static time::SteadyPoint last_save = time::SteadyNow();
    if (time::ToSeconds(time::SteadyNow() - last_save) > 10.0) {
      last_save = time::SteadyNow();
      cost_model.Save(cost_model_path);
    }
  }

  if (rw.rendering) {
    FATAL << "Root widget wasn't rendered during the last frame.";
  }

  {  // Step 2 - flatten the widget tree for analysis.
    struct DFSVisit {
      int parent;
      Widget* widget;
      bool detached;  // true for children that composite outside their immediate parent
    };
    vector<DFSVisit> dfs;
    dfs.push_back({0, &rw, false});
    while (!dfs.empty()) {
      auto [parent, widget, detached] = std::move(dfs.back());
      dfs.pop_back();

      tree.push_back(WidgetTree{
          .widget = widget,
          .baker = detached ? tree[parent].baker : parent,
          .parent = parent,
          .detached = detached,
      });
      int tree_index = tree.size() - 1;

      auto& node = tree.back();

      while (tree[node.baker].verdict == Verdict::Skip_NoTexture) {
        node.baker = tree[node.baker].baker;
      }
      node.repack_parent = detached ? node.baker : parent;

      if (widget->rendering || tree[node.baker].verdict == Verdict::Skip_Rendering) {
        node.SetVerdict(Verdict::Skip_Rendering);
      }

      {  // Update local_to_window & (maybe) call TransformUpdated()
        SkMatrix local_to_window = widget->local_to_parent.asM33();
        if (parent != tree_index) {
          local_to_window.postConcat(tree[parent].widget->local_to_window);
        }
        (void)local_to_window.invert(&node.window_to_local);
        if (widget->local_to_window != local_to_window) {
          widget->local_to_window = local_to_window;
          widget->TransformUpdated(rw.timer);
        }
      }

      // TICK
      if (node.verdict == Verdict::Unknown && widget->next_tick <= now) {
        auto true_d = rw.timer.d;
        auto fake_d = min(1.0, time::ToSeconds(now - widget->last_tick));
        if (widget->last_tick == time::SteadyPoint::min()) {
          // This is the first time this widget is being ticked - use `true_d` to animate it.
          fake_d = true_d;
          widget->shape_invalid = true;
        }
        rw.timer.d = fake_d;
        auto tock = widget->Tick(rw.timer);
        rw.timer.d = true_d;
        widget->last_tick = now;
        widget->next_tick = tock.next_tick;
        node.wants_to_draw = tock.draw;
        widget->shape_invalid |= tock.shape;
      }

      if (parent != tree_index && widget->local_to_parent != widget->packed_local_to_parent) {
        tree[parent].widget->subtree_shape_invalid = true;
      }
      widget->packed_local_to_parent = widget->local_to_parent;

      if (widget->shape_invalid) {
        widget->shape = widget->Shape();
        widget->shape_invalid = false;
        widget->subtree_shape_invalid = true;
      }

      widget->draw_bounds = widget->DrawBounds();
      if constexpr (build_variant::NotRelease) {
        if (widget->draw_bounds && widget->draw_bounds->sk.isEmpty()) {
          FATAL << widget->Name()
                << " returned an empty rect from DrawBounds - it must return nullopt instead";
        }
      }
      widget->pack_frame_draw_bounds = widget->draw_bounds;
      bool visible = true;
      if (widget->pack_frame_draw_bounds.has_value()) {
        // Note: child widgets are drawn using SkCanvas::drawDrawable(WidgetDrawable),
        // which in turn calls WidgetDrawable::onGetBounds to get the widget's bounds.
        // This creates a problem on the first animation frame because at this point,
        // the WidgetDrawable hasn't received the "Update" packet - and doesn't know
        // where it's bounds are.
        //
        // A proper fix may be to call Update right after a Draw call.
        //
        // Right now a workaround is to directly update the WidgetDrawable bounds.
        auto& widget_drawable =
            static_cast<WidgetDrawable&>(SkDrawableRTTI::Unwrap(*widget->sk_drawable));
        widget_drawable.pack_frame_draw_bounds = *widget->pack_frame_draw_bounds;

        // Compute the bounds of the widget - in local & root coordinates
        SkRect root_bounds;
        widget->local_to_window.mapRect(&root_bounds, *widget->pack_frame_draw_bounds);

        // Clip the `root_bounds` to the root widget bounds;
        if (root_bounds.width() * root_bounds.height() < 512 * 512) {
          // Render small objects without clipping
          visible = SkRect::Intersects(root_bounds, root_widget_bounds_px);
        } else {
          // This mutates the `root_bounds` - they're clipped to `root_widget_bounds_px`!
          visible = root_bounds.intersect(root_widget_bounds_px);
        }

        root_bounds.roundOut(&node.surface_bounds_root);

        node.render_area_px2 =
            (float)node.surface_bounds_root.width() * node.surface_bounds_root.height();

        // TODO: this is overestimating the visible area when window_to_local contains a rotation!
        node.window_to_local.mapRect(&node.new_visible_bounds.sk, root_bounds);
        if (widget->rendered_bounds.has_value()) {
          Rect& old_rendered_bounds = *widget->rendered_bounds;
          node.surface_reusable = old_rendered_bounds.Contains(node.new_visible_bounds);
        } else {
          node.surface_reusable = false;
        }
      } else if (node.verdict == Verdict::Unknown) {
        node.SetVerdict(Verdict::Skip_NoTexture);
      }

      // Advance the parent to current widget & visit its children.
      if (!visible) {
        // Skip_Clipped is more important as it signals that widget's children are not included in
        // the widget tree.
        if (node.verdict == Verdict::Skip_Rendering) {
          node.verdict = Verdict::Unknown;
        }
        node.SetVerdict(Verdict::Skip_Clipped);
      } else {
        auto& layers = widget->layers;
        int baked_begin = layers.bake_begin, baked_end = layers.bake_end;
        for (int k = (int)layers.size() - 1; k >= 0; --k) {
          Widget* child = layers[k];
          if (child->parent != widget) {
            ERROR << "Widget " << widget->Name() << " has child " << child->Name()
                  << " whose parent pointer is "
                  << (child->parent ? child->parent->Name() : StrView("nullptr"))
                  << "; skipping to preserve render tree invariants.";
            continue;
          }
          // Children outside [baked_begin, baked_end) are the Over/Under bands: detached,
          // composited into this node's baker instead of baked into this node's texture.
          dfs.push_back({tree_index, child, k < baked_begin || k >= baked_end});
        }
      }
    }

    // Record anchor positions, after all animations have "Tick"-ed.
    for (auto& node : tree) {
      node.pack_frame_texture_anchors = node.widget->TextureAnchors();
    }

    // Update `subtree_shape` and `subtree_draw_bounds`.
    for (int i = (int)tree.size() - 1; i >= 0; --i) {
      Widget* widget = tree[i].widget;
      if (widget->subtree_shape_invalid) {
        widget->subtree_shape = widget->SubtreeShape();
        widget->subtree_draw_bounds = widget->SubtreeDrawBounds();
        widget->subtree_shape_invalid = false;
        tree[tree[i].parent].widget->subtree_shape_invalid = true;
      }
    }
  }

  if constexpr (kDebugRendering && kDebugRenderEvents) {
    // Debug print the tree every 10 seconds
    static time::SteadyPoint last_print = now - 11s;
    if (now - last_print > 10s) {
      last_print = now;
      vector<bool> last_child = vector<bool>(tree.size(), false);
      vector<bool> last_child_found = vector<bool>(tree.size(), false);
      for (int i = tree.size() - 1; i > 0; --i) {
        int parent = tree[i].baker;
        if (!last_child_found[parent]) {
          last_child_found[parent] = true;
          last_child[i] = true;
        }
      }
      for (int i = 0; i < tree.size(); ++i) {
        Str line;
        for (int j = tree[i].baker; j != 0; j = tree[j].baker) {
          if (last_child[j]) {
            line = "   " + line;
          } else {
            line = " │ " + line;
          }
        }
        if (i) {
          if (last_child[i]) {
            line += " ╰╴";
          } else {
            line += " ├╴";
          }
        }
        line += tree[i].widget->Name();
        LOG << line;
      }
    }
  }

  {  // Step 3 - create a list of render jobs for the updated widgets
    int first_job = -1;
    for (int i = 0; i < tree.size(); ++i) {
      auto& node = tree[i];
      auto& widget = *node.widget;
      node.same_scale = (widget.local_to_window.getScaleX() == widget.rendered_matrix.getScaleX() &&
                         widget.local_to_window.getScaleY() == widget.rendered_matrix.getScaleY() &&
                         widget.local_to_window.getSkewX() == widget.rendered_matrix.getSkewX() &&
                         widget.local_to_window.getSkewY() == widget.rendered_matrix.getSkewY());
    }

    // Propagate `wants_to_draw` of textureless widgets to their parents.
    // Reverse order means that long chains of textureless widgets will eventually mark some parent
    // as `wants_to_draw`.
    for (int i = tree.size() - 1; i >= 0; --i) {
      auto& node = tree[i];
      if (node.verdict == Verdict::Skip_NoTexture && node.wants_to_draw) {
        tree[node.baker].wants_to_draw = true;
      }
    }

    for (int i = 0; i < tree.size(); ++i) {
      auto& node = tree[i];
      auto& widget = *node.widget;
      if (node.verdict == Verdict::Skip_NoTexture) {
        continue;
      }
      if (node.verdict == Verdict::Skip_Clipped) {
        continue;
      }
      if (node.verdict == Verdict::Skip_Rendering) {
        continue;
      }
      if (node.same_scale && node.surface_reusable && !node.wants_to_draw &&
          widget.invalidated == time::SteadyPoint::max()) {
        continue;
      }

      if (widget.invalidated == time::SteadyPoint::max()) {
        widget.invalidated = now;
      }
      node.next_job = first_job;
      node.prev_job = -1;
      if (first_job != -1) {
        tree[first_job].prev_job = i;
      }
      first_job = i;
    }

    for (auto& node : tree) {
      if (node.verdict == Verdict::Skip_NoTexture || node.verdict == Verdict::Skip_Clipped)
        continue;
      node.type_id = cost_model.TypeId(std::string(node.widget->Name()));
      node.model_render_ms = cost_model.RenderCostMs(node.type_id, node.render_area_px2);
    }

    Optional<LOG_IndentGuard> indent;
    if constexpr (kDebugPackFrame) {
      LOG << "PackFrame()";
      indent.emplace();
    }

    float remaining_millis = 1000.0f / (rw.window ? rw.window->screen_refresh_rate : 60);
    if constexpr (kDebugPackFrame) LOG << "frame millis: " << remaining_millis;
    remaining_millis -= 3.0f * cost_model.mae;  // 3 standard deviations of margin
    if constexpr (kDebugPackFrame) LOG << "after stddev margin: " << remaining_millis;
    remaining_millis -= cost_model.floor_millis;  // learned floor of our render costs
    if constexpr (kDebugPackFrame) LOG << "after floor margin: " << remaining_millis;

    auto Pack = [&](int pack_i) {
      float millis = 0;
      for (int i = pack_i; true; i = tree[i].repack_parent) {
        if (tree[i].verdict == Verdict::Pack) {
          break;
        }
        if (tree[i].verdict == Verdict::Skip_NoTexture) {
          continue;
        }
        millis += GetRenderMillis(tree[i]);
        tree[i].SetVerdict(Verdict::Pack);
        // Remove this node from the job list
        if (tree[i].prev_job != -1) {
          tree[tree[i].prev_job].next_job = tree[i].next_job;
        } else if (i == first_job) {
          first_job = tree[i].next_job;
        }
        if (tree[i].next_job != -1) {
          tree[tree[i].next_job].prev_job = tree[i].prev_job;
        }
        if (i == 0) {
          break;
        }
      }
      remaining_millis -= millis;
    };

    Pack(0);
    if constexpr (kDebugPackFrame) LOG << "after RootWidget: " << remaining_millis;

    // Widgets whose texture was never valid - never rendered, or invalidated through
    // RedrawThisFrame - render unconditionally, regardless of the budget.
    for (int i = first_job; i != -1; i = tree[i].next_job) {
      if (tree[i].widget->invalidated == time::SteadyPoint::min()) {
        Pack(i);
      }
    }
    if constexpr (kDebugPackFrame) LOG << "after never-valid widgets: " << remaining_millis;

    bool something_packed = false;
    while (first_job != -1) {
      int best_i = -1;
      float best_factor = -1;
      for (int i = first_job; i != -1; i = tree[i].next_job) {
        float total_lag = GetLag(tree[i]);
        float millis = GetRenderMillis(tree[i]);

        for (int i_parent = tree[i].repack_parent; i_parent;
             i_parent = tree[i_parent].repack_parent) {
          if (tree[i_parent].verdict == Verdict::Pack) {
            break;
          }
          if (tree[i_parent].verdict == Verdict::Skip_NoTexture) {
            continue;
          }
          total_lag += GetLag(tree[i_parent]);
          millis += GetRenderMillis(tree[i_parent]);
        }

        if (something_packed && millis > remaining_millis) {
          // Doesn't fit the budget: drop from the job list.
          if (tree[i].prev_job != -1) {
            tree[tree[i].prev_job].next_job = tree[i].next_job;
          } else {
            first_job = tree[i].next_job;
          }
          if (tree[i].next_job != -1) {
            tree[tree[i].next_job].prev_job = tree[i].prev_job;
          }
          continue;
        }

        float factor = total_lag / millis;
        if (factor > best_factor) {
          best_factor = factor;
          best_i = i;
        }
      }

      if (best_i == -1) {
        break;
      }

      // Pack this job
      Pack(best_i);
      something_packed = true;
      if constexpr (kDebugPackFrame)
        LOG << "after packing " << tree[best_i].widget->Name() << ": " << remaining_millis;
    }
  }

  // Step 4 - walk through the tree and record the draw commands into drawables.
  for (int i = tree.size() - 1; i >= 0; --i) {
    if (tree[i].verdict != Verdict::Pack) {
      continue;
    }
    auto& node = tree[i];
    cost_model_obs.push_back(
        {node.type_id, node.render_area_px2, node.widget->smooth_render_millis});

    auto& widget = *node.widget;

    if (widget.rendering) {
      ERROR << "Widget " << widget.Name() << " has been multi-packed!";
      continue;
    }

    WidgetDrawable::Update update = {};

    update.id = widget.ID();
    if (node.baker != i) {
      update.parent_id = tree[node.baker].widget->ID();
    }

    update.average_draw_millis = widget.smooth_render_millis;
    update.name = widget.Name();
    update.last_tick = widget.last_tick;

    update.surface_bounds_root = node.surface_bounds_root;

    SkPictureRecorder recorder;
    SkCanvas* rec_canvas = recorder.beginRecording(root_widget_bounds_px);
    rec_canvas->setMatrix(widget.local_to_window);
    //////////
    // DRAW //
    //////////
    widget.Draw(*rec_canvas);  // This is where we actually draw stuff!
    if constexpr (kDebugShapes) {
      if (i == 0) {
        for (int j = 0; j < tree.size(); ++j) {
          auto& other = *tree[j].widget;
          rec_canvas->setMatrix(other.local_to_window);
          SkPaint shape_paint;
          shape_paint.setStroke(true);
          auto c = color::HSLuv(20 * 360.f * j / tree.size(), 100, 50);
          shape_paint.setColor(c);
          shape_paint.setStrokeWidth(1_mm);
          shape_paint.setAlphaf(0.5f);
          rec_canvas->drawPath(other.subtree_shape, shape_paint);
        }
      }
    }
    update.compositor = widget.GetCompositor();

    constexpr bool kSerializeRecording = false;
    if constexpr (kSerializeRecording) {
      update.recording_data = recorder.finishRecordingAsDrawable()->serialize();
    } else {
      update.recording_drawable = recorder.finishRecordingAsDrawable();
    }

    update.pack_frame_draw_bounds = widget.pack_frame_draw_bounds;
    update.pack_frame_texture_anchors = node.pack_frame_texture_anchors;

    widget.rendering = true;
    widget.invalidated = time::SteadyPoint::max();
    widget.rendered_matrix = widget.local_to_window;
    widget.rendered_bounds = node.new_visible_bounds;
    pack.frame.push_back(update);
  }

  {  // Update Pack::fresh_matrices
    for (int i = 0; i < tree.size(); ++i) {
      auto& node = tree[i];
      bool include = node.verdict == Verdict::Pack;
      if (node.baker != i) {
        include |= tree[node.baker].verdict == Verdict::Pack;
      }
      if (node.detached) {
        // A detached child is composited during its baker's rasterization, so it needs a fresh
        // matrix whenever the baker repacks - even if its logical parent did not.
        include |= tree[node.baker].verdict == Verdict::Pack;
      }
      if (include) {
        pack.fresh_matrices[node.widget->ID()] = node.widget->local_to_window;
      }
    }
  }

  // Update fresh_texture_anchors for all widgets that will be drawn & their children.
  // This allows WidgetDrawable::onDraw to properly deform the texture.
  {
    for (auto& update : pack.frame) {
      if (pack.fresh_texture_anchors.find(update.id) != pack.fresh_texture_anchors.end()) {
        continue;
      }
      if (auto* widget = Widget::Find(update.id)) {
        pack.fresh_texture_anchors[update.id] = widget->TextureAnchors();
        for (auto* child : widget->layers) {
          if (pack.fresh_texture_anchors.find(child->ID()) != pack.fresh_texture_anchors.end()) {
            continue;
          }
          pack.fresh_texture_anchors[child->ID()] = child->TextureAnchors();
        }
      }
    }
    // Detached children composite into a baker that is not their logical parent, so the loop above
    // (which walks each packed widget's own children) does not reach them. Publish their anchors
    // whenever their baker repacks.
    for (auto& node : tree) {
      if (!node.detached || tree[node.baker].verdict != Verdict::Pack) {
        continue;
      }
      auto id = node.widget->ID();
      if (pack.fresh_texture_anchors.find(id) == pack.fresh_texture_anchors.end()) {
        pack.fresh_texture_anchors[id] = node.widget->TextureAnchors();
      }
    }
  }

  for (auto& node : tree) {
    if (node.widget->shadow_elevation <= 0 || node.verdict == Verdict::Skip_Clipped ||
        node.verdict == Verdict::Skip_NoTexture) {
      continue;
    }
    pack.shadow_casters.push_back(
        {node.widget->ID(), tree[node.baker].widget->ID(), node.widget->shadow_elevation});
  }

  if constexpr (kDebugRendering && kDebugRenderEvents) {
    LOG << "Frame packing:";
    LOG_Indent();
    Str packed_widgets;
    for (auto& update : pack.frame) {
      packed_widgets += update.name;
      packed_widgets += " ";
    }
    LOG << "Packed widgets: " << packed_widgets;
    LOG_Unindent();
  }
}

constexpr bool kPowersave = true;

void RenderFrame(SkCanvas& canvas, ui::RootWidget& rw) {
  ZoneScoped;
  if constexpr (kDebugRendering && kDebugRenderEvents) {
    debug_render_events += "WaitingStart(";
    debug_render_events += to_string(foreground_rendering_jobs);
    debug_render_events += " fg jobs) ";
  }
  {  // Spinlock while GPU is working:
    ZoneScopedN("checkAsyncWorkCompletion");

    while (foreground_rendering_jobs) {
      vk::graphite_context->checkAsyncWorkCompletion();
    }
  }
  if constexpr (kDebugRendering && kDebugRenderEvents) {
    debug_render_events += "WaitingEnd ";
  }

  next_frame_request.real_render_millis = time::ToSeconds(time::SteadyNow() - frame_start) * 1000;

  if (kPowersave) {
    static time::SteadyPoint next_frame = time::kZeroSteady;
    ZoneScopedN("Powersave");
    time::SteadyPoint now = time::SteadyNow();
    // TODO: Adjust next_frame to minimize input latency
    // VK_EXT_present_timing
    // https://github.com/KhronosGroup/Vulkan-Docs/pull/1364
    auto period = time::Duration(1s) / rw.window->screen_refresh_rate;
    if (next_frame <= now) {
      int skipped_frames = (int)(time::ToSeconds(now - next_frame) / time::ToSeconds(period)) + 1;
      next_frame = now + period;
      constexpr bool kLogSkippedFrames = false;
      if (kLogSkippedFrames && skipped_frames > 1) {
        LOG << "Skipped " << (uint64_t)(skipped_frames - 1) << " frames";
      }
    } else {
      // This normally sleeps until T + ~10ms.
      // With timeBeginPeriod(1) it's T + ~1ms.
      // TODO: try condition_variable instead
      std::this_thread::sleep_until(next_frame);
      next_frame += period;
    }
  }

  frame_start = time::SteadyNow();

  PackedFrame pack;
  PackFrameRequest request;
  vector<WidgetDrawable*> frame;
  {  // Pack Frame
    lock_guard lock(rw.mutex);
    request = std::move(next_frame_request);
    if constexpr (kDebugRendering && kDebugRenderEvents) {
      static int frame_number = 0;
      ++frame_number;
      LOG << "====== FRAME " << frame_number << " ======";
      LOG << "Render events: " << debug_render_events;
      Str finished_widgets;
      for (auto result : request.render_results) {
        finished_widgets += Widget::Find(result.id)->Name();
        finished_widgets += " ";
      }
      LOG << "Finished since last frame: " << finished_widgets;
      debug_render_events.clear();
    }
    PackFrame(rw, request, pack);
  }

  // Update all WidgetRenderStates
  for (auto& [id, fresh_texture_anchors] : pack.fresh_texture_anchors) {
    if (auto* cached_widget_drawable = WidgetDrawable::FindOrNull(id)) {
      cached_widget_drawable->fresh_texture_anchors = fresh_texture_anchors;
    }
  }
  for (auto& [id, matrix] : pack.fresh_matrices) {
    if (auto* cached_widget_drawable = WidgetDrawable::FindOrNull(id)) {
      cached_widget_drawable->fresh_matrix = matrix;
    }
  }

  for (auto& update : pack.frame) {
    auto widget_drawable = WidgetDrawable::FindOrNull(update.id);
    frame.push_back(widget_drawable);

    if (update.parent_id) {
      auto* parent = WidgetDrawable::FindOrNull(update.parent_id);
      // Make sure the widget has a semaphore.
      if (!widget_drawable->sk_semaphore.isValid()) {
        Status _;
        widget_drawable->vk_semaphore.Create(_);
        widget_drawable->sk_semaphore = widget_drawable->vk_semaphore;
      }

      // Make the widget signal its semaphore when rendered.
      widget_drawable->signal_semaphore = true;

      // Make the parent wait for the child semaphore before rendering.
      widget_drawable->then_list.push_back(parent);
      parent->wait_list.push_back(widget_drawable);
      parent->wait_count++;
    }
  }

  auto props = canvas.getBaseProps();

  for (auto& update : pack.frame) {
    WidgetDrawableHolder& ref = WidgetDrawableHolder::FindOrMake(update.id);
    auto* w = ref.widget_drawable;
    w->UpdateState(update);

    auto& frame = w->in_progress();
    frame.matrix = w->update_matrix;
    frame.surface_bounds_root = w->update_surface_bounds_root;
    frame.surface_bounds_local = *w->pack_frame_draw_bounds;
    frame.texture_anchors = w->pack_frame_texture_anchors;
    frame.image_info = canvas.imageInfo().makeDimensions(frame.surface_bounds_root.size());
    if (frame.texture.isValid() && frame.texture.dimensions() == frame.image_info.dimensions()) {
      // Reusing existing texture.
    } else {
      if (frame.texture.isValid()) {
        frame.surface->recorder()->deleteBackendTexture(frame.texture);
      }
      auto* recorder = global_foreground_recorder.get();
      frame.texture = recorder->createBackendTexture(frame.image_info.dimensions(), kTextureInfo);
      frame.surface = SkSurfaces::WrapBackendTexture(recorder, frame.texture,
                                                     kBGRA_8888_SkColorType, nullptr, &props);
    }
  }

  for (auto& ref : pack.shadow_casters) {
    if (auto* baker = WidgetDrawable::FindOrNull(ref.baker_id)) {
      baker->shadow_casters.clear();
    }
  }
  for (auto& ref : pack.shadow_casters) {
    auto* w = WidgetDrawable::FindOrNull(ref.id);
    auto* baker = WidgetDrawable::FindOrNull(ref.baker_id);
    if (!w || !baker || !w->rendered().surface) {
      continue;
    }
    w->shadow_elevation = ref.elevation;
    auto& rendered = w->rendered();
    baker->shadow_casters.push_back({SkSurfaces::AsImage(rendered.surface),
                                     rendered.surface_bounds_root, rendered.matrix,
                                     rendered.surface_bounds_local, ref.elevation});
  }

  int pending_recordings = 0;
  for (auto& update : pack.frame) {
    auto w = WidgetDrawable::FindOrNull(update.id);
    recording_queue.enqueue(w);
    ++pending_recordings;
  }

  // Wait for all of the WidgetDrawables with their recordings to be ready.
  // Send them to GPU for rendering in topological order.
  std::vector<WidgetDrawable*> ready_for_gpu;
  while (pending_recordings) {
    WidgetDrawable* w = nullptr;
    recorded_queue.wait_dequeue(w);
    --pending_recordings;
    if (w->wait_count == 0) {
      ready_for_gpu.push_back(w);
    }
  }

  // Submit the child→parent DAG one wave at a time. Every widget that is ready simultaneously is
  // mutually independent (a waiter only becomes ready once all the children it waits on have been
  // processed), so a whole wave shares a single vkQueueSubmit without a parent ever waiting on a
  // sibling in the same batch. Each wave is submitted before the next is inserted, so a parent's
  // wait semaphores resolve against children whose signals are already queued. This collapses the
  // submits from one-per-widget to one-per-DAG-level.
  vector<WidgetDrawable*> wave;
  while (!ready_for_gpu.empty()) {
    wave.swap(ready_for_gpu);
    ready_for_gpu.clear();
    for (auto* w : wave) {
      assert(w->wait_count == 0);
      w->InsertRecording();
    }
    vk::graphite_context->submit();
    for (auto* w : wave) {
      for (auto* then : w->then_list) {
        if ((--then->wait_count) == 0) {
          ready_for_gpu.push_back(then);
        }
      }
      w->then_list.clear();
      w->wait_list.clear();
      w->signal_semaphore = false;
    }
  }

  if constexpr (kDebugRendering) {
    static bool saved = false;
    if (!saved) {
      saved = true;
      for (auto state : frame) {
        struct ReadPixelsContext {
          string webp_path;
          SkImageInfo image_info;
        };

        Status ignored;
        Path("build/debug_widgets/").MakeDirs(ignored);

        auto surface = state->rendered().surface;

        auto read_pixels_context = new ReadPixelsContext{
            .webp_path = f("build/debug_widgets/widget_{:03d}_{}.webp", state->id,
                           std::string(state->name.data(), state->name.size())),
            .image_info = surface->imageInfo(),
        };
        vk::graphite_context->asyncRescaleAndReadPixels(
            surface.get(), surface->imageInfo(),
            SkIRect::MakeSize(surface->imageInfo().dimensions()), SkImage::RescaleGamma::kLinear,
            SkImage::RescaleMode::kNearest,
            [](SkImage::ReadPixelsContext context_arg,
               unique_ptr<const SkImage::AsyncReadResult> result) {
              auto context =
                  unique_ptr<ReadPixelsContext>(static_cast<ReadPixelsContext*>(context_arg));
              // LOG << "      saving to " << context->webp_path;
              SkPixmap pixmap = SkPixmap(context->image_info, result->data(0), result->rowBytes(0));
              SkFILEWStream stream(context->webp_path.c_str());
              SkWebpEncoder::Encode(&stream, pixmap, SkWebpEncoder::Options());
            },
            read_pixels_context);
      }
    }
  }

  canvas.setMatrix(rw.local_to_parent);

  // Final widget in the frame is the RootWidget
  auto* top_level_widget = frame.back();
  top_level_widget->onDraw(&canvas);

  if constexpr (kDebugRendering) {  // bullseye for latency visualisation
    lock_guard lock(rw.mutex);
    if (rw.pointers.size() > 0) {
      auto p = rw.pointers.begin()->pointer_position;
      auto window_transform = ui::TransformUp(rw);
      canvas.resetMatrix();
      SkPaint red;
      red.setColor(SK_ColorRED);
      red.setAntiAlias(true);
      SkPaint orange;
      orange.setColor("#ff8000"_color);
      orange.setAntiAlias(true);
      auto mm = window_transform.mapRadius(1_mm);
      SkPath red_ring = SkPathBuilder()
                            .addCircle(p.x, p.y, 4 * mm)
                            .addCircle(p.x, p.y, 3 * mm, SkPathDirection::kCCW)
                            .detach();
      SkPath orange_ring = SkPathBuilder()
                               .addCircle(p.x, p.y, 2 * mm)
                               .addCircle(p.x, p.y, 1 * mm, SkPathDirection::kCCW)
                               .detach();
      canvas.drawPath(red_ring, red);
      canvas.drawPath(orange_ring, orange);
      SkPaint stroke;
      stroke.setStyle(SkPaint::kStroke_Style);
      canvas.drawLine(p.x, p.y - 5 * mm, p.x, p.y + 5 * mm, stroke);
      canvas.drawLine(p.x - 5 * mm, p.y, p.x + 5 * mm, p.y, stroke);
    }
  }

  // TODO: present should wait for a semaphore from the top_level_widget
  vk::Present();

  EVERY_N_SEC(1) { vk::ReportMemoryStats(); }
}

}  // namespace automat
