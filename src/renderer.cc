// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "renderer.hh"

#include <include/core/SkPictureRecorder.h>

#include <cmath>

#include "font.hh"
#include "widget.hh"
#include "window.hh"

// TODO: remove Location::GetTransform and move it into TransformToChild
// TODO: Move all the animation logic into Widget::Update (only pass timer there)
// TODO: Only pass Canvas into Widget::Draw & remove animation::Phase from its return value
// TODO: Each widget should hold its own transform matrix
// TODO: Remove TransformFromChild & TransformToChild
// TODO: Replace VisitChildren with a child iterator
// TODO: Split Widget from Object (largest change)

// TODO: Some objects still crash when executed (for example macro recorder)
// TODO: Throwing object in the trash doesn't really delete everything
// TODO: Crashes when closing Automat
// -- at this point we should be back in the working state
// TODO: fix objects not being redrawn when panning
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

struct PackedFrame {
  vector<Widget*> frame;
  vector<Widget*> overflow;
  animation::Phase animation_phase;
};

void PackFrame(const PackFrameRequest& request, PackedFrame& pack) {
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
    Skip_StillDrawing = 5,
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
#ifdef DEBUG_RENDERING
      if (widget->recording == nullptr) {
        FATAL << "Widget " << widget->Name() << " has been returned by client multiple times!";
      }
#endif
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
      if (node.widget->recording) {
        node.verdict = Verdict::Skip_StillDrawing;
      } else if (!intersects) {
        node.verdict = Verdict::Skip_Clipped;
      } else {
        parent = tree.size() - 1;
        widget->VisitChildren(visitor);
      }
    }
  }

#ifdef DEBUG_RENDERING
  // Debug print the tree every 10 seconds
  static time::SteadyPoint last_print = time::SteadyPoint::min();
  if (now - last_print > 10s) {
    last_print = now;
    for (int i = 0; i < tree.size(); ++i) {
      Str line;
      for (int j = tree[i].parent; j != 0; j = tree[j].parent) {
        line += " ┃ ";
      }
      if (i) {
        line += " ┣━";
      }
      line += tree[i].widget->Name();
      LOG << line;
    }
  }
#endif

  {  // Step 3 - create a list of render jobs for the updated widgets
    int first_job = -1;
    for (int i = 0; i < tree.size(); ++i) {
      auto& node = tree[i];
      auto& widget = *node.widget;
      if ((node.verdict == Verdict::Skip_NoTexture) || (node.verdict == Verdict::Skip_Clipped) ||
          (node.verdict == Verdict::Skip_StillDrawing)) {
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
    auto packed = tree[i].verdict == Verdict::Pack;
    auto overflowed = tree[i].verdict == Verdict::Overflow;
    if (!packed && !overflowed) {
      continue;
    }
    auto& widget = *tree[i].widget;
    auto true_d = window->timer.d;
    auto fake_d = min(1.0, (now - widget.draw_time).count());
    window->timer.d = fake_d;
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

#ifdef DEBUG_RENDERING
    if (widget.recording) {
      FATAL << "Widget " << widget.Name() << " has been repacked!";
    }
#endif

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
    auto animation_phase = widget.Draw(ctx);  // This is where we actually draw stuff!
    window->timer.d = true_d;
    if (animation_phase == animation::Animating) {
      widget.invalidated = min(widget.invalidated, now);
    }

    widget.recording = recorder.finishRecordingAsDrawable();
    widget.draw_present = packed;
    if (packed) {
      pack.frame.push_back(&widget);
    } else {
      pack.overflow.push_back(&widget);
    }
  }
}

std::deque<Widget*> overflow_queue;
time::SteadyPoint paint_start;

void RenderFrame(SkCanvas& canvas) {
  canvas.recordingContext()->asDirectContext()->submit(GrSyncCpu::kYes);
  paint_start = time::SteadyNow();

  PackedFrame pack;
  PackFrame(next_frame_request, pack);
  next_frame_request.render_results.clear();

  // Render the PackedFrame
  for (auto* widget : pack.frame) {
    widget->RenderToSurface(canvas);
  }
  for (auto* drawable : pack.overflow) {
    overflow_queue.push_back(drawable);
  }

  canvas.resetMatrix();
  canvas.scale(window->display_pixels_per_meter, window->display_pixels_per_meter);
  canvas.save();
  pack.frame.back()->ComposeSurface(&canvas);
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
  while (!overflow_queue.empty()) {
    auto paint_time_so_far = time::SteadyNow() - paint_start;
    if (paint_time_so_far > 15ms) {
      break;
    }
    overflow_queue.front()->RenderToSurface(root_canvas);
    overflow_queue.pop_front();
  }
  root_canvas.recordingContext()->asDirectContext()->submit(GrSyncCpu::kNo);
}

}  // namespace automat
