// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "renderer.hh"

#include <include/core/SkColor.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPathTypes.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkStream.h>
#include <include/effects/SkGradientShader.h>
#include <include/encode/SkWebpEncoder.h>
#include <include/gpu/graphite/BackendSemaphore.h>
#include <include/gpu/graphite/GraphiteTypes.h>
#include <include/gpu/graphite/Surface.h>

#include <cmath>
#include <map>

#include "../build/generated/embedded.hh"
#include "drawable_rtti.hh"
#include "font.hh"
#include "global_resources.hh"
#include "log.hh"
#include "root_widget.hh"
#include "textures.hh"
#include "vk.hh"
#include "widget.hh"

// TODO: replace `root_canvas` with surface properties
// TODO: move the "rendering" logic of Widget into a separate class (intended to run in the Client)
// TODO: use correct bounds in SkPictureRecorder::beginRecording
// TODO: render using a job system (tree of Semaphores)

constexpr bool kDebugRendering = false;
constexpr bool kDebugRenderEvents = false;

using namespace automat::gui;
using namespace std;
using namespace maf;

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

time::SteadyPoint paint_start;

struct WidgetDrawable;

deque<WidgetDrawable*> overflow_queue;

struct WidgetDrawableHolder {
  sk_sp<SkDrawable> sk_drawable;
  WidgetDrawable* widget_drawable;
};

// TODO: replace with a set
map<uint32_t, WidgetDrawableHolder> cached_widget_drawables;

// Holds all the data necessary to render a Widget (without refering to the Widget itself).
struct WidgetDrawable : SkDrawableRTTI {
  const uint32_t id = 0;

  // Debugging
  float average_draw_millis = FP_NAN;
  maf::Str name;

  // Rendering
  SkIRect update_surface_bounds_root;
  SkIRect render_surface_bounds_root;
  sk_sp<SkDrawable> recording = nullptr;
  SkMatrix fresh_matrix;     // the most recent transform
  SkMatrix update_matrix;    // transform at the time of the last UpdateState
  SkMatrix rendered_matrix;  // transform at the time of the last RenderToSurface

  maf::Optional<SkRect> pack_frame_texture_bounds;
  maf::Vec<Vec2> pack_frame_texture_anchors;

  // Bounds of the widget's texture (without any clipping) in its local coordinate space.
  // Note that surface have different dimensions. It may be larger (to account for rounding to full
  // pixels) or smaller (due too clipping).
  Rect draw_texture_bounds;
  maf::Vec<Vec2> draw_texture_anchors;
  maf::Vec<Vec2> fresh_texture_anchors;
  time::SteadyPoint last_tick_time;

  sk_sp<SkSurface> surface = nullptr;

  // Synchronization
  skgpu::graphite::BackendSemaphore semaphore;
  bool signal_semaphore = false;  // only signal semaphore if there is a parent that waits for it
  vector<skgpu::graphite::BackendSemaphore>
      wait_list;  // child widgets that must be rendered first, cleared after every frame

  WidgetDrawable(uint32_t id);
  ~WidgetDrawable() {
    if (semaphore.isValid()) {
      vk::DestroySemaphore(semaphore);
    }
  }

  struct Update {
    uint32_t id;
    uint32_t parent_id;  // used to delay rendering of parents (which must render after children)

    // Debugging
    float average_draw_millis;
    maf::Str name;
    time::SteadyPoint last_tick_time;

    // Rendering
    SkIRect surface_bounds_root;

    // When rendering locally, we prefer passing drawables without serialization.
    // Remote rendering (not implemented yet) requires us to serialize them.
    sk_sp<SkDrawable> recording_drawable;
    sk_sp<SkData> recording_data;

    maf::Optional<SkRect> pack_frame_texture_bounds;
    maf::Vec<Vec2> pack_frame_texture_anchors;
  };

  void UpdateState(const Update&);

  void RenderToSurface(SkCanvas& root_canvas);

  SkRect onGetBounds() override;
  void onDraw(SkCanvas* canvas) override;
  const char* getTypeName() const override { return "WidgetDrawable"; }
  void flatten(SkWriteBuffer& buffer) const override;
  static sk_sp<SkFlattenable> CreateProc(SkReadBuffer& buffer);

  static WidgetDrawable* Find(uint32_t id) {
    if (auto it = cached_widget_drawables.find(id); it != cached_widget_drawables.end()) {
      return it->second.widget_drawable;
    }
    // This may happen when the widget is off screen. Its parent usually isn't aware of that and
    // attempts to compose it regardless. On the client side the result is that we don't find any
    // WidgetDrawable to compose.
    return nullptr;
  }

  // May create a new WidgetDrawable - if none exists for the given id. Otherwise returns a
  // reference to the existing WidgetDrawable.
  static WidgetDrawableHolder& Make(uint32_t id);
};

sk_sp<SkDrawable> MakeWidgetDrawable(Widget& widget) {
  auto& drawable_holder = WidgetDrawable::Make(widget.ID());
  return drawable_holder.sk_drawable;
}

void RendererInit() { SkFlattenable::Register("WidgetDrawable", WidgetDrawable::CreateProc); }

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
#ifndef NDEBUG
  static Size warning_threshold = 10 * 1024;
  if (size > warning_threshold) {
    LOG << "Warning: Widget " << name << " drew a frame of size " << size / 1024 << "kB";
    warning_threshold = size;  // prevent spamming the log
  }
#endif
}

SkRect WidgetDrawable::onGetBounds() { return *pack_frame_texture_bounds; }
void WidgetDrawable::flatten(SkWriteBuffer& buffer) const {
  // Normally this shouldn't be called. There is no point serializing the render state.
  buffer.writeInt(id);
}
WidgetDrawableHolder& WidgetDrawable::Make(uint32_t id) {
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
  auto& ref = Make(id);
  return ref.sk_drawable;
}

void WidgetDrawable::UpdateState(const Update& update) {
  average_draw_millis = update.average_draw_millis;
  update_surface_bounds_root = update.surface_bounds_root;

  name = update.name;
  last_tick_time = update.last_tick_time;

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
  pack_frame_texture_bounds = update.pack_frame_texture_bounds;
  pack_frame_texture_anchors = update.pack_frame_texture_anchors;
}

void WidgetDrawable::RenderToSurface(SkCanvas& root_canvas) {
  rendered_matrix = update_matrix;
  render_surface_bounds_root = update_surface_bounds_root;
  draw_texture_anchors = pack_frame_texture_anchors;
  draw_texture_bounds = *pack_frame_texture_bounds;

  skgpu::graphite::RecorderOptions options;
  options.fImageProvider = image_provider;
  auto recorder = vk::graphite_context->makeRecorder(options);
  auto cpu_started = time::SteadyNow();
  auto root_surface = root_canvas.getSurface();
  auto image_info = root_surface->imageInfo().makeDimensions(render_surface_bounds_root.size());

  surface = SkSurfaces::RenderTarget(recorder.get(), image_info, skgpu::Mipmapped::kNo,
                                     &root_surface->props(), name);

  auto graphite_canvas = surface->getCanvas();
  graphite_canvas->clear(SK_ColorTRANSPARENT);
  graphite_canvas->translate(-render_surface_bounds_root.left(), -render_surface_bounds_root.top());
  // Remove all Drawables by converting the commands into SkPicture
  recording->makePictureSnapshot()->playback(graphite_canvas);

  if constexpr (kDebugRendering && kDebugRenderEvents) {
    debug_render_events += "BeginFlush(";
    debug_render_events += name;
    debug_render_events += ") ";
  }
  auto graphite_recording = recorder->snap();
  skgpu::graphite::InsertRecordingInfo insert_recording_info;
  insert_recording_info.fRecording = graphite_recording.get();

  if (signal_semaphore) {
    insert_recording_info.fSignalSemaphores = &semaphore;
    insert_recording_info.fNumSignalSemaphores = 1;
  }

  if (!wait_list.empty()) {
    insert_recording_info.fWaitSemaphores = wait_list.data();
    insert_recording_info.fNumWaitSemaphores = wait_list.size();
  }

  struct FinishedContext : public ReferenceCounted {
    uint32_t id;
    time::SteadyPoint gpu_started;
    float cpu_time;
    string name;
    mutex mutex;
  };

  auto finished_context = MakePtr<FinishedContext>();
  finished_context->id = id;
  finished_context->gpu_started = time::SteadyPoint::min();
  finished_context->cpu_time = 0;
  finished_context->name = name;

  // Note that we're creating a dynamically allocated Ptr here. This is needed because we
  // must store it inside `void* fFinishedContext`.
  insert_recording_info.fFinishedContext = new Ptr<FinishedContext>(finished_context);
  insert_recording_info.fFinishedProc = [](skgpu::graphite::GpuFinishedContext context,
                                           skgpu::CallbackResult) {
    auto* finished_context_ptr = static_cast<Ptr<FinishedContext>*>(context);
    auto finished_context =
        std::move(*finished_context_ptr);  // create a stack-based copy of the Ptr
    delete finished_context_ptr;           // free the heap allocated Ptr

    auto lock = lock_guard(finished_context->mutex);
    float gpu_time;
    if (finished_context->gpu_started == time::SteadyPoint::min()) {
      gpu_time = 0;
    } else {
      gpu_time = (float)(time::SteadyNow() - finished_context->gpu_started).count();
    }
    float render_time = max(gpu_time, finished_context->cpu_time);
    if (render_time > 1) {
      LOG << "Widget " << finished_context->name << " took " << render_time << "s to render";
    }
    next_frame_request.render_results.push_back({finished_context->id, render_time});
    if constexpr (kDebugRendering && kDebugRenderEvents) {
      debug_render_events += "Finished(";
      debug_render_events += finished_context->name;
      debug_render_events += ") ";
    }
  };

  vk::graphite_context->insertRecording(insert_recording_info);

  wait_list.clear();
  signal_semaphore = false;

  vk::graphite_context->submit();

  {
    auto lock = lock_guard(finished_context->mutex);
    auto now = time::SteadyNow();
    finished_context->gpu_started = now;
    finished_context->cpu_time = (now - cpu_started).count();
  }

  if constexpr (kDebugRendering && kDebugRenderEvents) {
    debug_render_events += "EndFlush(";
    debug_render_events += name;
    debug_render_events += ") ";
  }
}

void WidgetDrawable::onDraw(SkCanvas* canvas) {
  if constexpr (kDebugRendering) {
    SkPaint texture_bounds_paint;  // translucent black
    texture_bounds_paint.setStyle(SkPaint::kStroke_Style);
    texture_bounds_paint.setColor(SkColorSetARGB(128, 0, 0, 0));
    canvas->drawRect(draw_texture_bounds.sk, texture_bounds_paint);
  }

  if (surface == nullptr) {
    // LOG << "Missing surface for " << Name();
  } else {
    SkRect surface_size = SkRect::MakeWH(surface->width(), surface->height());

    auto anchor_count = min<int>(draw_texture_anchors.size(), fresh_texture_anchors.size());

    if (anchor_count == 2) {
      SkSamplingOptions sampling;
      static auto builder =
          resources::RuntimeEffectBuilder(embedded::assets_anchor_warp_rt_sksl.content);

      SkMatrix root_to_local;
      (void)rendered_matrix.invert(&root_to_local);
      Rect local_surface_bounds = SkRect::Make(render_surface_bounds_root);
      root_to_local.mapRect(&local_surface_bounds.sk);
      builder->uniform("surfaceOrigin") = local_surface_bounds.BottomLeftCorner();
      builder->uniform("surfaceSize") = local_surface_bounds.Size();
      builder->uniform("surfaceResolution") = Vec2(surface->width(), surface->height());
      builder->uniform("anchorsLast").set(&draw_texture_anchors[0], anchor_count);
      builder->uniform("anchorsCurr").set(&fresh_texture_anchors[0], anchor_count);
      builder->child("surface") = SkSurfaces::AsImage(surface)->makeShader(sampling);

      auto shader = builder->makeShader();
      SkPaint paint;
      paint.setShader(shader);

      // Heuristic for finding same texture bounds (guaranteed to contain the whole widget):
      // - for every anchor move the old texture bounds by its displacement
      // - compute a union of all the moved bounds
      Rect new_anchor_bounds = Rect::MakeEmptyAt(fresh_texture_anchors[0]);
      for (int i = 0; i < anchor_count; ++i) {
        Vec2 delta = fresh_texture_anchors[i] - draw_texture_anchors[i];
        Rect offset_bounds = draw_texture_bounds.MoveBy(delta);
        new_anchor_bounds.ExpandToInclude(offset_bounds);
      }
      canvas->drawRect(new_anchor_bounds.sk, paint);
    } else {
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

      SkRect draw_bounds = draw_texture_bounds.sk;

      /////////////////////////////////////////////////
      // Map from the local coordinates to surface UV
      /////////////////////////////////////////////////
      // First go from local space (metric) to window space (pixels)
      SkMatrix surface_transform = rendered_matrix;
      // Now our surface is axis-aligned.
      // Map the surface bounds to unit square.
      surface_transform.postConcat(
          SkMatrix::RectToRect(SkRect::Make(render_surface_bounds_root), SkRect::MakeWH(1, 1)));
      // Finally flip the y-axis (Skia uses bottom-left origin, but we use top-left)
      surface_transform.postScale(1, -1, 0, 0.5);

      // Skia puts the origin at the top left corner (going down), but we use bottom left (going
      // up). This flip makes all the textures composite in our coordinate system correctly.
      if (anchor_count) {
        SkMatrix anchor_mapping;
        // Apply the inverse transform to the surface mapping - we want to get the original texture
        // position. Note that this transform uses `draw_texture_anchors` which have been saved
        // during the last RenderToSurface.
        if (anchor_mapping.setPolyToPoly(&fresh_texture_anchors[0].sk, &draw_texture_anchors[0].sk,
                                         anchor_count)) {
          surface_transform.preConcat(anchor_mapping);
          SkMatrix inverse;
          (void)anchor_mapping.invert(&inverse);
          inverse.mapRectScaleTranslate(&draw_bounds, draw_bounds);
        }
      }

      static auto builder =
          resources::RuntimeEffectBuilder(embedded::assets_glitch_rt_sksl.content);
      builder->uniform("surfaceResolution") = Vec2(surface->width(), surface->height());
      builder->uniform("surfaceTransform") = surface_transform;
      float time = fmod(time::SteadyNow().time_since_epoch().count(), 1.0);
      builder->uniform("time") = time;
      SkSamplingOptions sampling;
      builder->child("surface") = SkSurfaces::AsImage(surface)->makeShader(
          SkTileMode::kClamp, SkTileMode::kClamp, sampling);
      auto shader = builder->makeShader();
      SkPaint paint;
      paint.setShader(shader);
      canvas->drawRect(draw_bounds, paint);

      if constexpr (kDebugRendering) {
        SkPaint surface_bounds_paint;
        constexpr int kNumColors = 10;
        SkColor colors[kNumColors];
        float pos[kNumColors];
        double integer_ignored;
        double fraction = modf(last_tick_time.time_since_epoch().count() / 4, &integer_ignored);
        SkMatrix shader_matrix = SkMatrix::RotateDeg(fraction * -360.0f, surface_size.center());
        for (int i = 0; i < kNumColors; ++i) {
          float hsv[] = {i * 360.0f / kNumColors, 1.0f, 1.0f};
          colors[i] = SkHSVToColor((kNumColors - i) * 255 / kNumColors, hsv);
          pos[i] = (float)i / (kNumColors - 1);
        }
        surface_bounds_paint.setShader(
            SkGradientShader::MakeSweep(surface_size.centerX(), surface_size.centerY(), colors, pos,
                                        kNumColors, 0, &shader_matrix));
        surface_bounds_paint.setStyle(SkPaint::kStroke_Style);
        surface_bounds_paint.setStrokeWidth(2.0f);
        canvas->concat(SkMatrix::RectToRect(surface_size, draw_texture_bounds.sk));
        canvas->drawRect(surface_size.makeInset(1, 1), surface_bounds_paint);
      }
      canvas->restore();
    }

    if constexpr (kDebugRendering) {
      SkPaint old_anchor_paint;
      old_anchor_paint.setStyle(SkPaint::kStroke_Style);
      old_anchor_paint.setColor(SkColorSetARGB(128, 128, 0, 0));
      SkPaint new_anchor_paint;
      new_anchor_paint.setStyle(SkPaint::kStroke_Style);
      new_anchor_paint.setColor(SkColorSetARGB(128, 0, 0, 128));
      SkPaint bounds_paint;
      bounds_paint.setStyle(SkPaint::kStroke_Style);
      bounds_paint.setColor(SkColorSetARGB(128, 0, 128, 0));

      for (int i = 0; i < anchor_count; ++i) {
        canvas->drawCircle(draw_texture_anchors[i].sk, 1_mm, old_anchor_paint);
        canvas->drawCircle(fresh_texture_anchors[i].sk, 1_mm, new_anchor_paint);
        canvas->drawLine(draw_texture_anchors[i].sk, fresh_texture_anchors[i].sk, new_anchor_paint);
      }
    }

    if constexpr (kDebugRendering) {
      auto& font = GetFont();
      SkPaint text_paint;
      canvas->translate(pack_frame_texture_bounds->left(),
                        min(pack_frame_texture_bounds->top(), pack_frame_texture_bounds->bottom()));
      auto text = f("%.1f", average_draw_millis);
      font.DrawText(*canvas, text, text_paint);
    }
  }
}

WidgetDrawable::WidgetDrawable(uint32_t id) : id(id) {}

struct PackedFrame {
  vector<WidgetDrawable::Update> frame;
  vector<WidgetDrawable::Update> overflow;
  map<uint32_t, Vec<Vec2>> fresh_texture_anchors;
  map<uint32_t, SkMatrix> fresh_matrices;
  animation::Phase animation_phase;
};

void PackFrame(const PackFrameRequest& request, PackedFrame& pack) {
  root_widget->timer.Tick();
  auto now = root_widget->timer.now;
  auto root_widget_bounds_px = Rect::MakeAtZero<LeftX, BottomY>(
                                   Round(root_widget->size * root_widget->display_pixels_per_meter))
                                   .Outset(64);  // 64px margin around screen

  enum class Verdict {
    Unknown = 0,
    Pack = 1,
    Overflow = 2,
    Skip_Clipped = 3,
    Skip_NoTexture = 4,
  };

  struct WidgetTree {
    Ptr<Widget> widget;
    Verdict verdict = Verdict::Unknown;
    int parent;
    int prev_job = -1;
    int next_job = -1;
    bool same_scale;
    bool wants_to_draw = false;
    bool surface_reusable = false;  // set to true if existing surface covers the visible area
    SkMatrix window_to_local;
    SkMatrix local_to_window;     // copied over to Widget, if drawn
    SkIRect surface_bounds_root;  // copied over to Widget, if drawn
    maf::Vec<Vec2> pack_frame_texture_anchors;
    // Bounds (in local coords) which are rendered to the surface.
    Rect new_visible_bounds;
  };
  vector<WidgetTree> tree;

  auto GetLag = [now](WidgetTree& tree_entry) -> float {
    return max(time::Duration(0), now - tree_entry.widget->wake_time).count();
  };

  auto GetRenderTime = [](WidgetTree& tree_entry) {
    return isnan(tree_entry.widget->average_draw_millis)
               ? 0
               : tree_entry.widget->average_draw_millis / 1000;
  };

  {  // Step 1 - update the cache entries for widgets rendered by the client
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
      float draw_millis = render_result.render_time * 1000;
      if (isnan(widget->average_draw_millis)) {
        widget->average_draw_millis = draw_millis;
      } else {
        widget->average_draw_millis = 0.9 * widget->average_draw_millis + 0.1 * draw_millis;
      }

      if (widget->rendering_to_screen == false) {
        // Find the closest ancestor that can be rendered to texture.
        Widget* ancestor_with_texture = widget->parent.get();
        while (ancestor_with_texture &&
               !ancestor_with_texture->pack_frame_texture_bounds.has_value()) {
          // RootWidget can always be rendered to texture so we don't need any extra stop
          // condition here.
          ancestor_with_texture = ancestor_with_texture->parent.get();
        }
        if (ancestor_with_texture == nullptr) {
          ERROR << "Widget " << widget->Name()
                << " (which just finished background rendering) has no parent to wake up!";
        } else {
          ancestor_with_texture->wake_time =
              min(ancestor_with_texture->wake_time, widget->last_tick_time);
        }
      }

      widget->rendering = false;
      widget->rendering_to_screen = false;
    }
  }

  root_widget->FixParents();

  {  // Step 2 - flatten the widget tree for analysis.
    // Queue with (parent index, widget) pairs.
    vector<pair<int, Ptr<Widget>>> q;
    q.push_back(make_pair(0, root_widget));
    while (!q.empty()) {
      auto [parent, widget] = std::move(q.back());
      q.pop_back();

      if (widget->rendering) {
        continue;
      }

      tree.push_back(WidgetTree{
          .widget = widget,
          .parent = parent,
      });
      int i = tree.size() - 1;

      auto& node = tree.back();

      // UPDATE
      if (widget->wake_time != time::SteadyPoint::max()) {
        node.wants_to_draw = true;
        auto true_d = root_widget->timer.d;
        auto fake_d = min(1.0, (now - widget->last_tick_time).count());
        if (widget->wake_time == time::SteadyPoint::min()) {
          // This is the first time this widget is being rendered - use `true_d` to animate it.
          fake_d = true_d;
        }
        root_widget->timer.d = fake_d;
        auto animation_phase = widget->Tick(root_widget->timer);
        root_widget->timer.d = true_d;
        widget->last_tick_time = now;
        if (animation_phase == animation::Finished) {
          widget->wake_time = time::SteadyPoint::max();
        } else {
          widget->wake_time = now;
        }
      }

      node.local_to_window = widget->local_to_parent.asM33();
      if (parent != i) {
        node.local_to_window.postConcat(tree[parent].local_to_window);
      }
      (void)node.local_to_window.invert(&node.window_to_local);

      widget->pack_frame_texture_bounds = widget->TextureBounds();
      bool visible = true;
      if (widget->pack_frame_texture_bounds.has_value()) {
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
        widget_drawable.pack_frame_texture_bounds = *widget->pack_frame_texture_bounds;

        // Compute the bounds of the widget - in local & root coordinates
        SkRect root_bounds;
        node.local_to_window.mapRect(&root_bounds, *widget->pack_frame_texture_bounds);

        // Clip the `root_bounds` to the root widget bounds;
        if (root_bounds.width() * root_bounds.height() < 512 * 512) {
          // Render small objects without clipping
          visible = SkRect::Intersects(root_bounds, root_widget_bounds_px);
        } else {
          // This mutates the `root_bounds` - they're clipped to `root_widget_bounds_px`!
          visible = root_bounds.intersect(root_widget_bounds_px);
        }

        root_bounds.roundOut(&node.surface_bounds_root);

        // TODO: this is overestimating the visible area when window_to_local contains a rotation!
        node.window_to_local.mapRect(&node.new_visible_bounds.sk, root_bounds);
        if (widget->rendered_bounds.has_value()) {
          Rect& old_rendered_bounds = *widget->rendered_bounds;
          node.surface_reusable = old_rendered_bounds.Contains(node.new_visible_bounds);
        } else {
          node.surface_reusable = false;
        }
      } else {
        node.verdict = Verdict::Skip_NoTexture;
      }

      // Advance the parent to current widget & visit its children.
      if (!visible) {
        node.verdict = Verdict::Skip_Clipped;
      } else {
        for (auto& child : widget->Children()) {
          q.push_back(make_pair(i, child));
        }
      }
    }

    // Record anchor positions, after all animations have "Tick"-ed.
    for (auto& node : tree) {
      node.pack_frame_texture_anchors = node.widget->TextureAnchors();
    }
  }

  if constexpr (kDebugRendering && kDebugRenderEvents) {
    // Debug print the tree every 10 seconds
    static time::SteadyPoint last_print = time::SteadyPoint::min();
    if (now - last_print > 10s) {
      last_print = now;
      vector<bool> last_child = vector<bool>(tree.size(), false);
      vector<bool> last_child_found = vector<bool>(tree.size(), false);
      for (int i = tree.size() - 1; i > 0; --i) {
        int parent = tree[i].parent;
        if (!last_child_found[parent]) {
          last_child_found[parent] = true;
          last_child[i] = true;
        }
      }
      for (int i = 0; i < tree.size(); ++i) {
        Str line;
        for (int j = tree[i].parent; j != 0; j = tree[j].parent) {
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
      node.same_scale = (node.local_to_window.getScaleX() == widget.rendered_matrix.getScaleX() &&
                         node.local_to_window.getScaleY() == widget.rendered_matrix.getScaleY() &&
                         node.local_to_window.getSkewX() == widget.rendered_matrix.getSkewX() &&
                         node.local_to_window.getSkewY() == widget.rendered_matrix.getSkewY());
    }

    // Propagate `wants_to_draw` of textureless widgets to their parents.
    // Reverse order means that long chains of textureless widgets will eventually mark some parent
    // as `wants_to_draw`.
    for (int i = tree.size() - 1; i >= 0; --i) {
      auto& node = tree[i];
      if (node.verdict == Verdict::Skip_NoTexture && node.wants_to_draw) {
        tree[node.parent].wants_to_draw = true;
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
      if (node.same_scale && node.surface_reusable && !node.wants_to_draw) {
        continue;
      }

      node.next_job = first_job;
      node.prev_job = -1;
      if (first_job != -1) {
        tree[first_job].prev_job = i;
      }
      first_job = i;
    }

    float remaining_time = 1.0f / 60;

    auto Pack = [&](int pack_i) {
      float render_time = 0;
      for (int i = pack_i; true; i = tree[i].parent) {
        if (tree[i].verdict == Verdict::Pack) {
          break;
        }
        if (tree[i].verdict == Verdict::Skip_NoTexture) {
          continue;
        }
        render_time += GetRenderTime(tree[i]);
        tree[i].verdict = Verdict::Pack;
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
      remaining_time -= render_time;
    };

    Pack(0);

    while (first_job != -1) {
      int best_i = -1;
      float best_factor = -1;
      float best_render_time = 0;
      for (int i = first_job; i != -1; i = tree[i].next_job) {
        float self_lag = GetLag(tree[i]);
        float self_render_time = GetRenderTime(tree[i]);

        float total_lag = self_lag;
        float total_render_time = self_render_time;
        bool ancestor_overflowed = false;

        for (int i_parent = tree[i].parent; true; i_parent = tree[i_parent].parent) {
          if (tree[i_parent].verdict == Verdict::Pack) {
            break;
          }
          if (tree[i_parent].verdict == Verdict::Overflow) {
            ancestor_overflowed = true;
            break;
          }
          if (tree[i_parent].verdict == Verdict::Skip_NoTexture) {
            continue;
          }
          total_lag += GetLag(tree[i_parent]);
          total_render_time += GetRenderTime(tree[i_parent]);
          if (i_parent == 0) {
            break;
          }
        }

        total_render_time = max(total_render_time, 0.000001f);

        if (ancestor_overflowed || total_render_time > remaining_time) {
          tree[i].verdict = Verdict::Overflow;
          if (tree[i].prev_job != -1) {
            tree[tree[i].prev_job].next_job = tree[i].next_job;
          } else {
            first_job = tree[i].next_job;
          }
          if (tree[i].next_job != -1) {
            tree[tree[i].next_job].prev_job = tree[i].prev_job;
          }
        } else {
          float factor = total_lag / total_render_time;
          if (factor > best_factor) {
            best_factor = factor;
            best_i = i;
            best_render_time = total_render_time;
          }
        }
      }

      if (best_i == -1) {
        break;
      }

      // Pack this job
      Pack(best_i);

    }  // while (!jobs.empty())
  }

  // Step 4 - walk through the tree and record the draw commands into drawables.
  for (int i = tree.size() - 1; i >= 0; --i) {
    auto packed = tree[i].verdict == Verdict::Pack;
    auto overflowed = tree[i].verdict == Verdict::Overflow;
    if (!packed && !overflowed) {
      continue;
    }
    auto& node = tree[i];
    auto& widget = *node.widget;

    if constexpr (kDebugRendering) {
      if (widget.rendering) {
        FATAL << "Widget " << widget.Name() << " has been repacked!";
      }
    }

    WidgetDrawable::Update update = {};

    update.id = widget.ID();
    if (node.parent != i) {
      update.parent_id = tree[node.parent].widget->ID();
    }

    update.average_draw_millis = widget.average_draw_millis;
    update.name = widget.Name();
    update.last_tick_time = widget.last_tick_time;

    update.surface_bounds_root = node.surface_bounds_root;

    SkPictureRecorder recorder;
    SkCanvas* rec_canvas = recorder.beginRecording(root_widget_bounds_px);
    rec_canvas->setMatrix(node.local_to_window);
    //////////
    // DRAW //
    //////////
    widget.Draw(*rec_canvas);  // This is where we actually draw stuff!

    constexpr bool kSerializeRecording = false;
    if constexpr (kSerializeRecording) {
      update.recording_data = recorder.finishRecordingAsDrawable()->serialize();
    } else {
      update.recording_drawable = recorder.finishRecordingAsDrawable();
    }

    update.pack_frame_texture_bounds = widget.pack_frame_texture_bounds;
    update.pack_frame_texture_anchors = node.pack_frame_texture_anchors;

    widget.rendering = true;
    widget.rendering_to_screen = packed;
    widget.rendered_matrix = node.local_to_window;
    widget.rendered_bounds = node.new_visible_bounds;
    if (packed) {
      pack.frame.push_back(update);
    } else {
      pack.overflow.push_back(update);
    }
  }

  {  // Update Pack::fresh_matrices
    for (int i = 0; i < tree.size(); ++i) {
      auto& node = tree[i];
      bool include = false;
      include |= node.verdict == Verdict::Pack;
      include |= node.verdict == Verdict::Overflow;
      if (node.parent != i) {
        auto& parent = tree[node.parent];
        include |= parent.verdict == Verdict::Pack;
        include |= parent.verdict == Verdict::Overflow;
      }
      if (include) {
        pack.fresh_matrices[node.widget->ID()] = node.local_to_window;
      }
    }
  }

  // Update fresh_texture_anchors for all widgets that will be drawn & their children.
  // This allows WidgetDrawable::onDraw to properly deform the texture.
  {
    for (auto& update : Concat(pack.frame, pack.overflow)) {
      if (pack.fresh_texture_anchors.find(update.id) != pack.fresh_texture_anchors.end()) {
        continue;
      }
      if (auto* widget = Widget::Find(update.id)) {
        pack.fresh_texture_anchors[update.id] = widget->TextureAnchors();
        for (auto& child : widget->Children()) {
          if (pack.fresh_texture_anchors.find(child->ID()) != pack.fresh_texture_anchors.end()) {
            continue;
          }
          pack.fresh_texture_anchors[child->ID()] = child->TextureAnchors();
        }
      }
    }
  }

  if constexpr (kDebugRendering && kDebugRenderEvents) {
    LOG << "Frame packing:";
    LOG_Indent();
    Str finished_widgets;
    for (auto result : request.render_results) {
      finished_widgets += Widget::Find(result.id)->Name();
      finished_widgets += " ";
    }
    LOG << "Finished since last frame: " << finished_widgets;
    Str packed_widgets;
    for (auto& update : pack.frame) {
      packed_widgets += update.name;
      packed_widgets += " ";
    }
    LOG << "Packed widgets: " << packed_widgets;
    Str overflow_widgets;
    for (auto& update : pack.overflow) {
      overflow_widgets += update.name;
      overflow_widgets += " ";
    }
    LOG << "Overflow widgets: " << overflow_widgets;
    LOG_Unindent();
  }
}

void RenderFrame(SkCanvas& canvas) {
  if constexpr (kDebugRendering && kDebugRenderEvents) {
    debug_render_events += "BeginSubmit(synced) ";
  }
  vk::graphite_context->submit(skgpu::graphite::SyncToCpu::kYes);
  if constexpr (kDebugRendering && kDebugRenderEvents) {
    debug_render_events += "EndSubmit(synced) ";
  }
  paint_start = time::SteadyNow();

  PackedFrame pack;
  PackFrameRequest request;
  vector<WidgetDrawable*> frame;
  {
    lock_guard lock(root_widget->mutex);
    request = std::move(next_frame_request);
    if constexpr (kDebugRendering && kDebugRenderEvents) {
      LOG << "Render events: " << debug_render_events;
      debug_render_events.clear();
    }

    PackFrame(request, pack);
  }

  // Update all WidgetRenderStates
  for (auto& [id, fresh_texture_anchors] : pack.fresh_texture_anchors) {
    if (auto* cached_widget_drawable = WidgetDrawable::Find(id)) {
      cached_widget_drawable->fresh_texture_anchors = fresh_texture_anchors;
    }
  }
  for (auto& [id, matrix] : pack.fresh_matrices) {
    if (auto* cached_widget_drawable = WidgetDrawable::Find(id)) {
      cached_widget_drawable->fresh_matrix = matrix;
    }
  }
  for (auto& update : Concat(pack.frame, pack.overflow)) {
    WidgetDrawableHolder& ref = WidgetDrawable::Make(update.id);
    ref.widget_drawable->UpdateState(update);
  }
  for (auto& update : pack.frame) {
    auto widget_drawable = WidgetDrawable::Find(update.id);
    frame.push_back(widget_drawable);

    if (update.parent_id) {
      // Make sure the widget has a semaphore.
      if (!widget_drawable->semaphore.isValid()) {
        widget_drawable->semaphore = vk::CreateSemaphore();
      }

      // Make the widget signal its semaphore when rendered.
      widget_drawable->signal_semaphore = true;

      // Make the parent wait for the child semaphore before rendering.
      WidgetDrawable::Find(update.parent_id)->wait_list.push_back(widget_drawable->semaphore);
    }
  }
  for (auto& update : pack.overflow) {
    overflow_queue.push_back(WidgetDrawable::Find(update.id));
  }

  // Render the PackedFrame
  for (auto widget_drawable : frame) {
    widget_drawable->RenderToSurface(canvas);
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

        maf::Path("build/debug_widgets/").MakeDirs(nullptr);

        auto read_pixels_context = new ReadPixelsContext{
            .webp_path = f("build/debug_widgets/widget_%03d_%*s.webp", state->id,
                           state->name.size(), state->name.data()),
            .image_info = state->surface->imageInfo(),
        };
        vk::graphite_context->asyncRescaleAndReadPixels(
            state->surface.get(), state->surface->imageInfo(),
            SkIRect::MakeSize(state->surface->imageInfo().dimensions()),
            SkImage::RescaleGamma::kLinear, SkImage::RescaleMode::kNearest,
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

  canvas.setMatrix(root_widget->local_to_parent);

  // Final widget in the frame is the RootWidget
  auto root_widget_copy = frame.back();
  root_widget_copy->onDraw(&canvas);

  if constexpr (kDebugRendering) {  // bullseye for latency visualisation
    lock_guard lock(root_widget->mutex);
    if (root_widget->pointers.size() > 0) {
      auto p = root_widget->pointers[0]->pointer_position;
      auto window_transform = canvas.getTotalMatrix();
      canvas.resetMatrix();
      SkPaint red;
      red.setColor(SK_ColorRED);
      red.setAntiAlias(true);
      SkPaint orange;
      orange.setColor("#ff8000"_color);
      orange.setAntiAlias(true);
      SkPath red_ring;
      auto mm = window_transform.mapRadius(1_mm);
      red_ring.addCircle(p.x, p.y, 4 * mm);
      red_ring.addCircle(p.x, p.y, 3 * mm, SkPathDirection::kCCW);
      SkPath orange_ring;
      orange_ring.addCircle(p.x, p.y, 2 * mm);
      orange_ring.addCircle(p.x, p.y, 1 * mm, SkPathDirection::kCCW);
      canvas.drawPath(red_ring, red);
      canvas.drawPath(orange_ring, orange);
      SkPaint stroke;
      stroke.setStyle(SkPaint::kStroke_Style);
      canvas.drawLine(p.x, p.y - 5 * mm, p.x, p.y + 5 * mm, stroke);
      canvas.drawLine(p.x - 5 * mm, p.y, p.x + 5 * mm, p.y, stroke);
      canvas.setMatrix(window_transform);
    }
  }
}

void RenderOverflow(SkCanvas& root_canvas) {
  // Render at least one widget from the overflow queue.
  if (!overflow_queue.empty()) {
    auto cached_widget_drawable = overflow_queue.front();
    cached_widget_drawable->RenderToSurface(root_canvas);
    overflow_queue.pop_front();
  }
  for (int i = 0; i < overflow_queue.size(); ++i) {
    auto cached_widget_drawable = overflow_queue[i];
    auto expected_total_paint_time =
        time::SteadyNow() - paint_start +
        time::Duration(cached_widget_drawable->average_draw_millis / 1000);
    if (expected_total_paint_time > 16.6ms) {
      continue;
    }
    cached_widget_drawable->RenderToSurface(root_canvas);
    overflow_queue.erase(overflow_queue.begin() + i);
    --i;
  }
}

}  // namespace automat
