// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkCanvas.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>
#include <include/core/SkSurface.h>
#include <include/gpu/GrDirectContext.h>

#include <cmath>
#include <functional>
#include <memory>

#include "action.hh"
#include "animation.hh"
#include "control_flow.hh"
#include "drawable.hh"
#include "keyboard.hh"
#include "optional.hh"
#include "span.hh"
#include "str.hh"
#include "time.hh"

constexpr bool kDebugRendering = true;

namespace automat::gui {

struct Widget;

maf::Str ToStr(std::shared_ptr<Widget> widget);

using Visitor = std::function<ControlFlow(maf::Span<std::shared_ptr<Widget>>)>;

// Transform from the window coordinates to the local coordinates of the widget.
SkMatrix TransformDown(const Widget& to);
// Transform from the local coordinates of the widget to the window coordinates.
SkMatrix TransformUp(const Widget& from);
// Transform from the local coordinates of the widget to the local coordinates of another widget.
SkMatrix TransformBetween(const Widget& from, const Widget& to);

struct RenderResult {
  uint32_t id;
  float render_time;
};

struct PackFrameRequest {
  // Must be sorted by ID!
  std::vector<RenderResult> render_results;
};

extern PackFrameRequest next_frame_request;

struct DrawContext {
  time::Timer& timer;
  SkCanvas& canvas;
  DrawContext(time::Timer& timer, SkCanvas& canvas) : timer(timer), canvas(canvas) {}
  float DeltaT() const { return timer.d; }
  operator GrDirectContext*() const {
    if (auto recording_context = canvas.recordingContext()) {
      return recording_context->asDirectContext();
    }
    return nullptr;
  }
};

struct DropTarget;

enum class PointerButton { Unknown, Left, Middle, Right, Count };

struct ActionTrigger {
  int repr;

  constexpr static int kAnsiKeyStart = 0;
  constexpr static int kAnsiKeyEnd = static_cast<int>(AnsiKey::Count);
  constexpr static int kPointerStart = kAnsiKeyEnd;
  constexpr static int kPointerEnd = kPointerStart + static_cast<int>(PointerButton::Count);

  constexpr ActionTrigger(PointerButton button) : repr(kPointerStart + static_cast<int>(button)) {}
  constexpr ActionTrigger(AnsiKey key) : repr(kAnsiKeyStart + static_cast<int>(key)) {}

  constexpr operator PointerButton() const {
    if (repr < kPointerStart || repr >= kPointerEnd) {
      return PointerButton::Unknown;
    }
    return static_cast<PointerButton>(repr - kPointerStart);
  }

  constexpr operator AnsiKey() const {
    if (repr < kAnsiKeyStart || repr >= kAnsiKeyEnd) {
      return AnsiKey::Unknown;
    }
    return static_cast<AnsiKey>(repr - kAnsiKeyStart);
  }

  constexpr auto operator<=>(const ActionTrigger&) const = default;
  constexpr bool operator==(PointerButton button) const {
    return ActionTrigger(button).repr == repr;
  }
};

// Base class for widgets.
struct Widget : public std::enable_shared_from_this<Widget> {
  Widget();
  virtual ~Widget();

  // IDs are used to identify a Widget across frames.
  uint32_t ID() const;
  static Widget* Find(uint32_t id);

  std::shared_ptr<Widget> parent;

  void RenderToSurface(SkCanvas& root_canvas);

  void ComposeSurface(SkCanvas* canvas) const;

  // Instead of calling ComposeSurface directly (which would use the current surface), we're using a
  // drawable. This delays the actual drawing until the canvas is "flushed" and allows us to use the
  // most recent surface.
  struct ComposeSurfaceDrawable : Drawable {
    Widget& Widget() {
      auto offset = offsetof(struct Widget, compose_surface_drawable);
      return *reinterpret_cast<struct Widget*>(reinterpret_cast<uintptr_t>(this) - offset);
    }
    SkRect onGetBounds() override { return *Widget().texture_bounds; }
    void onDraw(SkCanvas* canvas) override { Widget().ComposeSurface(canvas); }
  };

  mutable ComposeSurfaceDrawable compose_surface_drawable;

  maf::Optional<SkRect> texture_bounds;  // local coordinates
  mutable uint32_t id = 0;
  float average_draw_millis = FP_NAN;

  // The time when the cache entry was first invalidated.
  // Initially this is set to 0 (meaning it was never drawn).
  // When the widget is scheduled, set this to max value.
  mutable time::SteadyPoint invalidated = time::SteadyPoint::min();

  // Things updated in PackFrame (& Draw)
  mutable time::SteadyPoint draw_time = time::SteadyPoint::min();
  SkIRect surface_bounds_root;
  sk_sp<SkDrawable> recording = nullptr;
  SkMatrix window_to_local;
  bool draw_present = false;  // Whether the current draw job is going to be presented.

  // Things updated in RenderToSurface
  float cpu_time;                 // Used by the client to measure rendering time
  time::SteadyPoint gpu_started;  // Used by the client to measure rendering time
  SkRect surface_bounds_local;
  sk_sp<SkSurface> surface = nullptr;

  mutable std::string name_cached;

  // The name for objects of this type. English proper noun, UTF-8, capitalized.
  // For example: "Text Editor".
  virtual std::string_view Name() const {
    if (name_cached.empty()) {
      const std::type_info& info = typeid(*this);
      name_cached = maf::Demangle(info.name());
    }
    return name_cached;
  }

  Widget& RootWidget() const {
    Widget* root = const_cast<Widget*>(this);
    while (root->parent.get()) {
      root = root->parent.get();
    }
    return *root;
  }

  // Each widget needs to have a pointer to its parent.
  // Because widgets share inheritance hierarchy with Objects (and objects must use shared_ptr), the
  // widgets also must use shared_ptr for their references.
  // While an object is constructed using `make_shared`, it doesn't know its own shared_ptr. This is
  // a limitation of enable_shared_from_this - its inner pointer is initialized only after
  // construction.
  // In order to properly initialize the parents we currently use this workaround, that should be
  // called after a widget hierarchy is constructed.
  // Once Widget & Object classes are separated, and Widgets no longer use shared_ptr, this should
  // be replaced with a proper `parent` initialization in the Widget constructor.
  void FixParents();

  virtual void PointerOver(Pointer&) {}
  virtual void PointerLeave(Pointer&) {}

  virtual animation::Phase PreDraw(DrawContext& ctx) const { return animation::Finished; }
  animation::Phase DrawCached(DrawContext& ctx) const;
  virtual void InvalidateDrawCache() const;

  std::weak_ptr<Widget> WeakPtr() const {
    // For some reason, `static_pointer_cast` doesn't work with weak_ptr.
    return const_cast<Widget*>(this)->weak_from_this();
  }

  template <typename T = Widget>
  std::shared_ptr<T> SharedPtr() const {
    return static_pointer_cast<T>(const_cast<Widget*>(this)->shared_from_this());
  }

  // Called before Draw & PreDraw. The widgets can use this to update their state.
  // Only widgets that are being drawn will have this called.
  virtual animation::Phase Update(time::Timer&) { return animation::Finished; }

  virtual animation::Phase Draw(DrawContext& ctx) const { return DrawChildren(ctx); }
  virtual SkPath Shape() const = 0;

  virtual bool CenteredAtZero() const { return false; }

  virtual std::unique_ptr<Action> FindAction(Pointer&, ActionTrigger) { return nullptr; }

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
  virtual maf::Optional<Rect> TextureBounds() const { return Shape().getBounds(); }

  virtual SkMatrix TransformToChild(const Widget& child) const { return SkMatrix::I(); }

  SkMatrix TransformFromChild(const Widget& child) const {
    auto to_child = TransformToChild(child);
    SkMatrix from_child = SkMatrix::I();
    (void)to_child.invert(&from_child);
    return from_child;
  }

  virtual animation::Phase DrawChildCachced(DrawContext&, const Widget& child) const;

  virtual animation::Phase PreDrawChildren(DrawContext&) const;

  animation::Phase DrawChildrenSpan(DrawContext&, maf::Span<std::shared_ptr<Widget>> widgets) const;

  animation::Phase DrawChildren(DrawContext&) const;

  struct ParentsView {
    std::shared_ptr<Widget> start;

    struct end_iterator {};

    struct iterator {
      std::shared_ptr<Widget> widget;
      iterator(std::shared_ptr<Widget> widget) : widget(widget) {}
      std::shared_ptr<Widget>& operator*() { return widget; }
      iterator& operator++() {
        widget = widget->parent;
        return *this;
      }
      bool operator!=(const end_iterator&) { return widget.get() != nullptr; }
    };

    iterator begin() { return iterator(start); }
  };

  ParentsView Parents() const { return ParentsView{SharedPtr<Widget>()}; }
};

template <typename T>
T* Closest(Widget& widget) {
  Widget* w = &widget;
  while (w) {
    if (auto* result = dynamic_cast<T*>(w)) {
      return result;
    }
    w = w->parent.get();
  }
  return nullptr;
}

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
