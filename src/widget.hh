#pragma once

#include <include/core/SkCanvas.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>
#include <include/core/SkSurface.h>
#include <include/gpu/GrDirectContext.h>

#include <functional>
#include <memory>

#include "action.hh"
#include "animation.hh"
#include "control_flow.hh"
#include "keyboard.hh"
#include "optional.hh"
#include "span.hh"
#include "str.hh"
#include "time.hh"

namespace automat::gui {

struct Widget;

using Path = std::vector<Widget*>;

maf::Str ToStr(const Path& path);

template <typename T>
T* Closest(const Path& path) {
  for (int i = path.size() - 1; i >= 0; --i) {
    if (auto* result = dynamic_cast<T*>(path[i])) {
      return result;
    }
  }
  return nullptr;
}

using Visitor = std::function<ControlFlow(maf::Span<Widget*>)>;

SkMatrix TransformDown(const Path& path, animation::Display*);
SkMatrix TransformUp(const Path& path, animation::Display*);

// Describes the transform sequence for displaying widgets.
//
// The path is necessary to compute on-screen positions for widgets.
//
// The display is necessary to retrieve per-display cached data (mostly animation state).
struct DisplayContext {
  animation::Display& display;
  Path path;
  float DeltaT() const { return display.timer.d; }
  SkMatrix TransformDown() { return gui::TransformDown(path, &display); }
};

struct DrawCache {
  struct Entry {
    Path path;        // they key for this cache entry
    SkMatrix matrix;  // converts from local coordinates to base layer coordinates
    SkIRect root_bounds;
    sk_sp<SkSurface> surface;
    time::SteadyPoint last_used;
    bool needs_refresh;

    Entry(const Path& path)
        : path(path),
          matrix(),
          root_bounds(),
          surface(nullptr),
          last_used(time::SteadyPoint::min()),
          needs_refresh(true) {}
  };

  // TODO: index by path & last_used
  std::vector<std::unique_ptr<Entry>> entries;

  Entry& operator[](const Path& path) {
    for (auto& entry : entries) {
      if (entry->path == path) {
        return *entry;
      }
    }
    entries.push_back(std::make_unique<Entry>(path));
    return *entries.back();
  }

  void Clean(time::SteadyPoint now) {
    auto deadline = now - 60s;
    auto new_end = std::remove_if(entries.begin(), entries.end(), [deadline](const auto& entry) {
      return entry->last_used < deadline;
    });
    entries.erase(new_end, entries.end());
  }
};

struct DrawContext : DisplayContext {
  SkCanvas& canvas;
  DrawCache& draw_cache;
  DrawContext(animation::Display& display, SkCanvas& canvas, DrawCache& draw_cache)
      : DisplayContext{display, {}}, canvas(canvas), draw_cache(draw_cache) {}
  operator GrDirectContext*() const { return canvas.recordingContext()->asDirectContext(); }
};

struct DropTarget;

enum PointerButton { kButtonUnknown, kMouseLeft, kMouseMiddle, kMouseRight, kButtonCount };

// Base class for widgets.
struct Widget {
  Widget() {}
  virtual ~Widget();

  // The name for objects of this type. English proper noun, UTF-8, capitalized.
  // For example: "Text Editor".
  virtual std::string_view Name() const {
    const std::type_info& info = typeid(*this);
    return info.name();
  }

  virtual void PointerOver(Pointer&, animation::Display&) {}
  virtual void PointerLeave(Pointer&, animation::Display&) {}

  virtual void PreDraw(DrawContext& ctx) const {}
  animation::Phase DrawCached(DrawContext& ctx) const;
  void InvalidateDrawCache() const;

  virtual animation::Phase Draw(DrawContext& ctx) const {
    auto phase = animation::Finished;
    phase |= DrawChildren(ctx);
    return phase;
  }
  virtual SkPath Shape(animation::Display*) const = 0;

  virtual bool CenteredAtZero() const { return false; }

  // DEPRECATED: override PointerVisitChildren to block pointer events from propagating to
  // children.
  virtual std::unique_ptr<Action> CaptureButtonDownAction(Pointer&, PointerButton) {
    return nullptr;
  }
  virtual std::unique_ptr<Action> ButtonDownAction(Pointer&, PointerButton) { return nullptr; }

  // Return true if the widget should be highlighted as draggable.
  virtual bool CanDrag() { return false; }

  virtual DropTarget* CanDrop() { return nullptr; }

  // Used to visit the child widgets in a generic fashion.
  // Widgets are stored in front-to-back order.
  // The function stops once the visitor returns ControlFlow::Stop.
  virtual ControlFlow VisitChildren(Visitor& visitor) { return ControlFlow::Continue; }

  // A variant of `VisitChildren` used by the pointer events (PointerOver, PointerLeave, etc).
  // This can be used to block pointer events from propagating to children.
  virtual ControlFlow PointerVisitChildren(Visitor& visitor) { return VisitChildren(visitor); }

  // If the object should be cached into a texture, return its bounds in local coordinates.
  virtual maf::Optional<Rect> TextureBounds(animation::Display* d) const {
    return Shape(d).getBounds();
  }

  virtual SkMatrix TransformToChild(const Widget& child, animation::Display*) const {
    return SkMatrix::I();
  }

  virtual animation::Phase DrawChildCachced(DrawContext&, const Widget& child) const;

  virtual void PreDrawChildren(DrawContext&) const;

  animation::Phase DrawChildrenSubset(DrawContext&, maf::Span<Widget*> widgets) const;

  animation::Phase DrawChildren(DrawContext&) const;
};

struct LabelMixin {
  virtual void SetLabel(maf::StrView label) = 0;
};

struct PaintMixin {
  SkPaint paint;
  static SkPaint* Get(Widget* widget) {
    if (auto p = dynamic_cast<PaintMixin*>(widget)) {
      return &p->paint;
    }
    return nullptr;
  }
};

}  // namespace automat::gui
