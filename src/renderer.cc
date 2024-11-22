// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "renderer.hh"

#include <include/core/SkColor.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPathTypes.h>
#include <include/core/SkPictureRecorder.h>

#include <cmath>

#include "font.hh"
#include "widget.hh"
#include "window.hh"

// TODO: Fix connection rendering lag
// TODO: Fix objects not being redrawn when panning
// TODO: Move all the animation logic into Widget::Update (only pass timer there)
// TODO: Only pass Canvas into Widget::Draw & remove animation::Phase from its return value
// TODO: Each widget should hold its own transform matrix
// TODO: Remove TransformFromChild & TransformToChild
// TODO: Replace VisitChildren with a child iterator
// TODO: Split Widget from Object (largest change)

// TODO: investigate why some widgets are not packed even when they should be
// TODO: lots of cleanups!
//       - widgets should have pointers to their parents (remove "Paths")
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
  std::lock_guard lock(window->mutex);
  window->timer.Tick();
  auto now = window->timer.now;
  auto window_bounds_px =
      SkRect::MakeWH(round(window->size.width * window->display_pixels_per_meter),
                     round(window->size.width * window->display_pixels_per_meter));

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
    SkMatrix window_to_local;

    SkMatrix local_to_window;     // copied over to Widget, if drawn
    SkIRect surface_bounds_root;  // copied over to Widget, if drawn
  };
  vector<WidgetTree> tree;

  auto GetLag = [now](WidgetTree& tree_entry) -> float {
    return max(time::Duration(0), now - tree_entry.widget->invalidated).count();
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
        while (!renderable_parent->texture_bounds.has_value()) {
          // Root widget (window) can always be rendered to texture so we don't need any extra stop
          // condition here.
          renderable_parent = renderable_parent->parent.get();
        }
        renderable_parent->invalidated = min(renderable_parent->invalidated, widget->draw_time);
      }

      widget->recording.reset();
      widget->draw_present = false;
    }
  }

  window->FixParents();

  {  // Step 2 - flatten the widget tree for analysis.
    // Queue with (parent index, widget) pairs.
    vector<pair<int, std::shared_ptr<Widget>>> q;
    q.push_back(make_pair(0, window));
    int parent;
    Visitor visitor = [&](maf::Span<shared_ptr<Widget>> children) {
      for (auto& child : children) {
        q.push_back(make_pair(parent, child));
      }
      return ControlFlow::Continue;
    };
    while (!q.empty()) {
      auto [parent_tmp, widget] = std::move(q.back());
      parent = parent_tmp;
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
      if (parent != i) {
        node.window_to_local = tree[parent].window_to_local;
        node.window_to_local.postConcat(tree[parent].widget->TransformToChild(*widget));
      } else {
        node.window_to_local.postScale(1 / window->display_pixels_per_meter,
                                       1 / window->display_pixels_per_meter);
      }
      (void)node.window_to_local.invert(&node.local_to_window);

      widget->texture_bounds = widget->TextureBounds();
      bool intersects = true;
      if (widget->texture_bounds.has_value()) {
        // Compute the bounds of the widget - in local & root coordinates
        SkRect root_bounds;
        node.local_to_window.mapRect(&root_bounds, *widget->texture_bounds);

        // Clip the `root_bounds` to the window bounds;
        if (root_bounds.width() * root_bounds.height() < 512 * 512) {
          // Render small objects without clipping
          intersects = SkRect::Intersects(root_bounds, window_bounds_px);
        } else {
          // This mutates the `root_bounds` - they're clipped to `canvas_bounds`!
          intersects = root_bounds.intersect(window_bounds_px);
        }

        root_bounds.roundOut(&node.surface_bounds_root);
      } else {
        node.verdict = Verdict::Skip_NoTexture;
      }

      // Advance the parent to current widget & visit its children.
      if (!intersects) {
        node.verdict = Verdict::Skip_Clipped;
      } else {
        parent = tree.size() - 1;
        widget->VisitChildren(visitor);
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
      if ((node.verdict == Verdict::Skip_NoTexture) || (node.verdict == Verdict::Skip_Clipped)) {
        continue;
      }

      bool same_scale = (node.window_to_local.getScaleX() == widget.window_to_local.getScaleX() &&
                         node.window_to_local.getScaleY() == widget.window_to_local.getScaleY() &&
                         node.window_to_local.getSkewX() == widget.window_to_local.getSkewX() &&
                         node.window_to_local.getSkewY() == widget.window_to_local.getSkewY());

      // Widgets that haven't been invalidated don't have to be re-rendered.
      if (widget.invalidated == time::SteadyPoint::max() && same_scale) {
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
  for (int i = 0; i < tree.size(); ++i) {
    auto verdict = tree[i].verdict;
    if ((verdict != Verdict::Pack) && (verdict != Verdict::Overflow) &&
        (verdict != Verdict::Skip_NoTexture)) {
      continue;
    }
    auto& widget = *tree[i].widget;
    auto true_d = window->timer.d;
    auto fake_d = min(1.0, (now - widget.draw_time).count());
    window->timer.d = fake_d;
    ////////////
    // UPDATE //
    ////////////
    auto animation_phase = widget.Update(window->timer);
    window->timer.d = true_d;
    if (animation_phase == animation::Finished) {
      widget.invalidated = time::SteadyPoint::max();
    } else {
      widget.invalidated = now;
    }
  }
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

    // ACTUALLY the delta time is `draw_time - now`.
    auto true_d = window->timer.d;
    auto fake_d = min(1.0, (now - widget.draw_time).count());
    widget.draw_time = now;
    widget.window_to_local = node.window_to_local;
    widget.surface_bounds_root = node.surface_bounds_root;
    SkPictureRecorder recorder;
    SkCanvas* rec_canvas = recorder.beginRecording(window_bounds_px);
    rec_canvas->setMatrix(node.local_to_window);
    DrawContext ctx(window->timer, *rec_canvas);
    window->timer.d = fake_d;
    //////////
    // DRAW //
    //////////
    auto animation_phase = widget.Draw(ctx);  // This is where we actually draw stuff!
    window->timer.d = true_d;
    if (animation_phase == animation::Animating) {
      widget.invalidated = min(widget.invalidated, now);
    }

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
    std::lock_guard lock(window->mutex);
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
  canvas.scale(window->display_pixels_per_meter, window->display_pixels_per_meter);
  canvas.save();
  // Final widget in the frame is the root window
  if (auto* widget = Widget::Find(pack.frame.back())) {
    widget->ComposeSurface(&canvas);
  }

  if constexpr (kDebugRendering) {  // bullseye for latency visualisation
    std::lock_guard lock(window->mutex);
    if (window->pointers.size() > 0) {
      SkPaint red;
      red.setColor(SK_ColorRED);
      red.setAntiAlias(true);
      SkPaint orange;
      orange.setColor("#ff8000"_color);
      orange.setAntiAlias(true);
      auto p = window->pointers[0]->pointer_position;
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
  canvas.translate(0, window->size.height - 1_cm);
  canvas.scale(2.7, 2.7);
  auto fps_text = f("%.1fms", window->timer.d * 1000);
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
