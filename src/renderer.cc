// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "renderer.hh"

#include <include/core/SkColor.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPathTypes.h>
#include <include/core/SkPictureRecorder.h>
#include <include/gpu/GrDirectContext.h>

#include <cmath>

#include "font.hh"
#include "root_widget.hh"
#include "widget.hh"


// TODO: lots of cleanups!
//       - remove redundancy between WidgetTree & Widget & DrawCache::Entry
//       - the three phases of rendering should be put in sequence (locally)
//         (record / Draw / Update) -> (render) -> (present / DrawCached / onDraw)
//       - move frame packing & execution elsewhere
// TODO: use correct bounds in SkPictureRecorder::beginRecording
// TODO: render this using a job system

using namespace automat::gui;
using namespace std;
using namespace maf;

namespace automat {

std::string debug_render_events;

struct PackedFrame {
  vector<U32> frame;
  vector<U32> overflow;
  animation::Phase animation_phase;
};

void PackFrame(const PackFrameRequest& request, PackedFrame& pack) {
  std::lock_guard lock(root_widget->mutex);
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
        if (widget->recording == nullptr) {
          FATAL << "Widget " << widget->Name() << " has been returned by client multiple times!";
        }
      }
      float draw_millis = render_result.render_time * 1000;
      if (isnan(widget->average_draw_millis)) {
        widget->average_draw_millis = draw_millis;
      } else {
        widget->average_draw_millis = 0.9 * widget->average_draw_millis + 0.1 * draw_millis;
      }

      if (widget->draw_present == false) {
        // Find the closest parent that can be rendered to texture.
        Widget* renderable_parent = widget->parent.get();
        while (!renderable_parent->pack_frame_texture_bounds.has_value()) {
          // RootWidget can always be rendered to texture so we don't need any extra stop
          // condition here.
          renderable_parent = renderable_parent->parent.get();
        }
        renderable_parent->wake_time = min(renderable_parent->wake_time, widget->last_tick_time);
      }

      widget->recording.reset();
      widget->draw_present = false;
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

      if (widget->recording) {
        continue;
      }

      tree.push_back(WidgetTree{
          .widget = widget,
          .parent = parent,
          .window_to_local = SkMatrix::I(),
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

      if (parent != i) {
        node.local_to_window = tree[parent].local_to_window;
      } else {
        node.local_to_window = SkMatrix::I();
      }
      node.local_to_window.preConcat(widget->local_to_parent.asM33());
      (void)node.local_to_window.invert(&node.window_to_local);

      widget->pack_frame_texture_bounds = widget->TextureBounds();
      widget->pack_frame_texture_anchors = widget->TextureAnchors();
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
      if (widget.recording) {
        FATAL << "Widget " << widget.Name() << " has been repacked!";
      }
    }

    widget.window_to_local = node.window_to_local;
    widget.surface_bounds_root = node.surface_bounds_root;
    SkPictureRecorder recorder;
    SkCanvas* rec_canvas = recorder.beginRecording(root_widget_bounds_px);
    rec_canvas->setMatrix(node.local_to_window);
    //////////
    // DRAW //
    //////////
    widget.Draw(*rec_canvas);  // This is where we actually draw stuff!

    widget.recording = recorder.finishRecordingAsDrawable();
    widget.draw_present = packed;
    if (packed) {
      pack.frame.push_back(widget.ID());
    } else {
      pack.overflow.push_back(widget.ID());
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
    for (auto id : pack.frame) {
      packed_widgets += Widget::Find(id)->Name();
      packed_widgets += " ";
    }
    LOG << "Packed widgets: " << packed_widgets;
    Str overflow_widgets;
    for (auto id : pack.overflow) {
      overflow_widgets += Widget::Find(id)->Name();
      overflow_widgets += " ";
    }
    LOG << "Overflow widgets: " << overflow_widgets;
    LOG_Unindent();
  }
}

std::deque<U32> overflow_queue;
time::SteadyPoint paint_start;

void RenderFrame(SkCanvas& canvas) {
  if constexpr (kDebugRendering && kDebugRenderEvents) {
    debug_render_events += "BeginSubmit(synced) ";
  }
  canvas.recordingContext()->asDirectContext()->submit(GrSyncCpu::kYes);
  if constexpr (kDebugRendering && kDebugRenderEvents) {
    debug_render_events += "EndSubmit(synced) ";
  }
  paint_start = time::SteadyNow();

  PackedFrame pack;
  PackFrameRequest request;
  {
    std::lock_guard lock(root_widget->mutex);
    request = std::move(next_frame_request);
    if constexpr (kDebugRendering && kDebugRenderEvents) {
      LOG << "Render events: " << debug_render_events;
      debug_render_events.clear();
    }
  }
  PackFrame(request, pack);

  // Render the PackedFrame
  for (auto id : pack.frame) {
    if (auto* widget = Widget::Find(id)) {
      widget->RenderToSurface(canvas);
    }
  }
  for (auto id : pack.overflow) {
    overflow_queue.push_back(id);
  }

  canvas.resetMatrix();
  canvas.scale(root_widget->display_pixels_per_meter, root_widget->display_pixels_per_meter);
  canvas.save();
  // Final widget in the frame is the RootWidget
  if (auto* widget = Widget::Find(pack.frame.back())) {
    widget->ComposeSurface(&canvas);
  }

  if constexpr (kDebugRendering) {  // bullseye for latency visualisation
    std::lock_guard lock(root_widget->mutex);
    if (root_widget->pointers.size() > 0) {
      SkPaint red;
      red.setColor(SK_ColorRED);
      red.setAntiAlias(true);
      SkPaint orange;
      orange.setColor("#ff8000"_color);
      orange.setAntiAlias(true);
      auto p = root_widget->pointers[0]->pointer_position;
      SkPath red_ring;
      red_ring.addCircle(p.x, p.y, 4_mm);
      red_ring.addCircle(p.x, p.y, 3_mm, SkPathDirection::kCCW);
      SkPath orange_ring;
      orange_ring.addCircle(p.x, p.y, 2_mm);
      orange_ring.addCircle(p.x, p.y, 1_mm, SkPathDirection::kCCW);
      canvas.drawPath(red_ring, red);
      canvas.drawPath(orange_ring, orange);
      SkPaint stroke;
      stroke.setStyle(SkPaint::kStroke_Style);
      canvas.drawLine(p.x, p.y - 5_mm, p.x, p.y + 5_mm, stroke);
      canvas.drawLine(p.x - 5_mm, p.y, p.x + 5_mm, p.y, stroke);
    }
  }
  canvas.restore();

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
    if (auto* widget = Widget::Find(overflow_queue.front())) {
      widget->RenderToSurface(root_canvas);
    }
    overflow_queue.pop_front();
  }
  for (int i = 0; i < overflow_queue.size(); ++i) {
    auto* widget = Widget::Find(overflow_queue[i]);
    if (widget) {
      auto paint_time_so_far =
          time::SteadyNow() - paint_start + time::Duration(widget->average_draw_millis / 1000);
      if (paint_time_so_far > 16.6ms) {
        continue;
      }
      widget->RenderToSurface(root_canvas);
    }
    overflow_queue.erase(overflow_queue.begin() + i);
    --i;
  }
  if constexpr (kDebugRendering && kDebugRenderEvents) {
    debug_render_events += "BeginSubmit(unsynced) ";
  }
  root_canvas.recordingContext()->asDirectContext()->submit(GrSyncCpu::kNo);
  if constexpr (kDebugRendering && kDebugRenderEvents) {
    debug_render_events += "EndSubmit(unsynced) ";
  }
}

}  // namespace automat
