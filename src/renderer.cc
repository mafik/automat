// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "renderer.hh"

#include <include/core/SkColor.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPathTypes.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkStream.h>
#include <include/encode/SkWebpEncoder.h>
#include <include/gpu/graphite/GraphiteTypes.h>

#include <cmath>

#include "font.hh"
#include "log.hh"
#include "root_widget.hh"
#include "vk.hh"
#include "widget.hh"

// TODO: replace `root_canvas` with surface properties
// TODO: move the "rendering" logic of Widget into a separate class (intended to run in the Client)
// TODO: use correct bounds in SkPictureRecorder::beginRecording
// TODO: render using a job system (tree of Semaphores)

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

std::string debug_render_events;

struct PackedFrame {
  vector<WidgetRenderState::Update> frame;
  vector<WidgetRenderState::Update> overflow;
  map<uint32_t, Vec<Vec2>> fresh_texture_anchors;
  animation::Phase animation_phase;
};

void PackFrame(const PackFrameRequest& request, PackedFrame& pack) {
  root_widget->timer.Tick();
  auto now = root_widget->timer.now;
  auto root_widget_bounds_px =
      SkRect::MakeWH(round(root_widget->size.width * root_widget->display_pixels_per_meter),
                     round(root_widget->size.height * root_widget->display_pixels_per_meter));

  enum class Verdict {
    Unknown = 0,
    Pack = 1,
    Overflow = 2,
    Skip_Clipped = 3,
    Skip_NoTexture = 4,
  };

  struct WidgetTree {
    std::shared_ptr<Widget> widget;
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
        while (!ancestor_with_texture->pack_frame_texture_bounds.has_value()) {
          // RootWidget can always be rendered to texture so we don't need any extra stop
          // condition here.
          ancestor_with_texture = ancestor_with_texture->parent.get();
        }
        ancestor_with_texture->wake_time =
            min(ancestor_with_texture->wake_time, widget->last_tick_time);
      }

      widget->rendering = false;
      widget->rendering_to_screen = false;
    }
  }

  root_widget->FixParents();

  {  // Step 2 - flatten the widget tree for analysis.
    // Queue with (parent index, widget) pairs.
    vector<pair<int, std::shared_ptr<Widget>>> q;
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
      node.pack_frame_texture_anchors = widget->TextureAnchors();
      bool visible = true;
      if (widget->pack_frame_texture_bounds.has_value()) {
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

        Rect new_visible_bounds;
        node.window_to_local.mapRect(&new_visible_bounds.sk, root_bounds);
        Rect& old_rendered_bounds = widget->surface_bounds_local;
        node.surface_reusable = old_rendered_bounds.Contains(new_visible_bounds);
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
  }

  if constexpr (kDebugRendering) {
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
      node.same_scale = (node.window_to_local.getScaleX() == widget.window_to_local.getScaleX() &&
                         node.window_to_local.getScaleY() == widget.window_to_local.getScaleY() &&
                         node.window_to_local.getSkewX() == widget.window_to_local.getSkewX() &&
                         node.window_to_local.getSkewY() == widget.window_to_local.getSkewY());
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

    WidgetRenderState::Update update;

    update.id = widget.ID();

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
    update.recording = recorder.finishRecordingAsDrawable()->serialize();

    update.window_to_local = node.window_to_local;
    widget.window_to_local = node.window_to_local;
    update.pack_frame_texture_bounds = widget.pack_frame_texture_bounds;
    update.pack_frame_texture_anchors = node.pack_frame_texture_anchors;

    widget.rendering = true;
    widget.rendering_to_screen = packed;
    if (packed) {
      pack.frame.push_back(update);
    } else {
      pack.overflow.push_back(update);
    }
  }

  // Update fresh_texture_anchors for all widgets that will be drawn & their children.
  // This allows ComposeSurface to properly deform the texture.
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

// TODO: replace with a set
std::map<uint32_t, WidgetRenderState> widget_render_states;

std::deque<WidgetRenderState*> overflow_queue;

time::SteadyPoint paint_start;

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
  std::vector<WidgetRenderState*> frame;
  {
    std::lock_guard lock(root_widget->mutex);
    request = std::move(next_frame_request);
    if constexpr (kDebugRendering && kDebugRenderEvents) {
      LOG << "Render events: " << debug_render_events;
      debug_render_events.clear();
    }

    // TODO: remove this
    // (Temporary) copy back some of the properties from WidgetRenderState to their original
    // Widgets.
    for (auto& result : request.render_results) {
      if (auto widget = Widget::Find(result.id)) {
        if (auto state_it = widget_render_states.find(result.id);
            state_it != widget_render_states.end()) {
          widget->surface_bounds_local = state_it->second.surface_bounds_local;
        }
      }
    }

    PackFrame(request, pack);
  }

  // Update all WidgetRenderStates
  for (auto& update : Concat(pack.frame, pack.overflow)) {
    WidgetRenderState* state;
    if (auto state_it = widget_render_states.find(update.id);
        state_it != widget_render_states.end()) {
      state = &state_it->second;
    } else {
      state = &widget_render_states.emplace(update.id, update.id).first->second;
    }
    state->UpdateState(update);
  }
  for (auto& [id, fresh_texture_anchors] : pack.fresh_texture_anchors) {
    if (auto state_it = widget_render_states.find(id); state_it != widget_render_states.end()) {
      state_it->second.fresh_texture_anchors = fresh_texture_anchors;
    }
  }
  for (auto& update : pack.frame) {
    if (auto state_it = widget_render_states.find(update.id);
        state_it != widget_render_states.end()) {
      frame.push_back(&state_it->second);
    }
  }
  for (auto& update : pack.overflow) {
    if (auto state_it = widget_render_states.find(update.id);
        state_it != widget_render_states.end()) {
      overflow_queue.push_back(&state_it->second);
    }
  }

  // Render the PackedFrame
  for (auto widget_render_state : frame) {
    widget_render_state->RenderToSurface(canvas);
  }

  if constexpr (kDebugRendering) {
    static bool saved = false;
    if (!saved) {
      saved = true;
      for (auto state : frame) {
        struct ReadPixelsContext {
          std::string webp_path;
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
               std::unique_ptr<const SkImage::AsyncReadResult> result) {
              auto context =
                  std::unique_ptr<ReadPixelsContext>(static_cast<ReadPixelsContext*>(context_arg));
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
  root_widget_copy->ComposeSurface(&canvas);

  if constexpr (kDebugRendering) {  // bullseye for latency visualisation
    std::lock_guard lock(root_widget->mutex);
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

  auto& font = GetFont();
  canvas.save();
  canvas.translate(0, root_widget->size.height - 1_cm);
  canvas.scale(2.7, 2.7);
  auto fps_text = f("%.1fms", root_widget->timer.d * 1000);
  font.DrawText(canvas, fps_text, SkPaint());
  canvas.restore();
}

void RenderOverflow(SkCanvas& root_canvas) {
  // Render at least one widget from the overflow queue.
  if (!overflow_queue.empty()) {
    auto widget_render_state = overflow_queue.front();
    widget_render_state->RenderToSurface(root_canvas);
    overflow_queue.pop_front();
  }
  for (int i = 0; i < overflow_queue.size(); ++i) {
    auto widget_render_state = overflow_queue[i];
    auto expected_total_paint_time =
        time::SteadyNow() - paint_start +
        time::Duration(widget_render_state->average_draw_millis / 1000);
    if (expected_total_paint_time > 16.6ms) {
      continue;
    }
    widget_render_state->RenderToSurface(root_canvas);
    overflow_queue.erase(overflow_queue.begin() + i);
    --i;
  }
}

}  // namespace automat
