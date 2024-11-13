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

namespace automat::gui {

struct Widget;

maf::Str ToStr(std::shared_ptr<Widget> widget);

using Visitor = std::function<ControlFlow(maf::Span<std::shared_ptr<Widget>>)>;

// `to` is the widget located below `from` in the widget hierarchy.
// `from` can be nullptr - if so then the transform starts at the root layer (takes in pixel
// coordinates).
SkMatrix TransformDown(const Widget& to, const Widget* from = nullptr,
                       animation::Display* = nullptr);

// `from` is the widget located below `to` in the widget hierarchy.
// `to` can be nullptr - if so then the transform ends at the root layer (produces pixel
// coordinates).
SkMatrix TransformUp(const Widget& from, const Widget* to = nullptr, animation::Display* = nullptr);

// A type of drawable that draws the Widget using the most recently cached texture.
// A choppy drawable can be asked to "update" itself, which will update the cached texture.
struct ChoppyDrawable : Drawable {
  Widget* widget;

  ChoppyDrawable(Widget* widget) : widget(widget) {}

  void Render(SkCanvas& canvas);

  SkRect onGetBounds() override;

  void onDraw(SkCanvas* canvas) override;
};

struct DrawContext {
  animation::Display& display;
  SkCanvas& canvas;
  DrawContext(animation::Display& display, SkCanvas& canvas) : display(display), canvas(canvas) {}
  float DeltaT() const { return display.timer.d; }
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
  virtual ~Widget();

  std::shared_ptr<Widget> parent;

  // OLD entries
  // TODO: delete them once choppy animations are in order

  SkMatrix draw_matrix;  // converts from local coordinates to base layer coordinates
  time::SteadyPoint last_used = time::SteadyPoint::min();

  // NEW entries
  // The time when the cache entry was first invalidated.
  // Initially this is set to 0 (meaning it was never drawn).
  // When the widget is scheduled, set this to max value.
  mutable time::SteadyPoint invalidated = time::SteadyPoint::min();

  // The timestamp of the currently presented frame.
  // This may actually be very old - for example if the widget wasn't invalidated for a long time
  // or was very slow to draw.
  time::SteadyPoint presented_time = time::SteadyPoint::min();

  bool draw_to_texture = false;
  // The ID of the current draw job.
  ChoppyDrawable choppy_drawable;
  SkRect local_bounds;          // local coordinates
  SkRect root_bounds;           // root coordinates, clipped to the window viewport
  SkIRect root_bounds_rounded;  // same as above, but rounded to integer pixels
  // The time when the current draw job was started.
  time::SteadyPoint draw_time = time::SteadyPoint::min();
  SkRect draw_bounds;  // Area of the widget which was drawn (local coordinates)
  // The recording that is being drawn.
  sk_sp<SkDrawable> recording = nullptr;
  // The surface that is being drawn to.
  sk_sp<SkSurface> surface = nullptr;
  // Whether the current draw job is going to be presented.
  bool draw_present = false;

  // How long, on average it takes to draw this widget.
  float draw_millis = FP_NAN;

  Widget() : choppy_drawable(this) {}

  // The name for objects of this type. English proper noun, UTF-8, capitalized.
  // For example: "Text Editor".
  virtual std::string_view Name() const {
    const std::type_info& info = typeid(*this);
    return info.name();
  }

  virtual void PointerOver(Pointer&, animation::Display&) {}
  virtual void PointerLeave(Pointer&, animation::Display&) {}

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

  virtual animation::Phase Draw(DrawContext& ctx) const {
    auto phase = animation::Finished;
    phase |= DrawChildren(ctx);
    return phase;
  }
  virtual SkPath Shape(animation::Display*) const = 0;

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
  virtual maf::Optional<Rect> TextureBounds(animation::Display* d) const {
    return Shape(d).getBounds();
  }

  virtual SkMatrix TransformToChild(const Widget& child, animation::Display*) const {
    return SkMatrix::I();
  }

  SkMatrix TransformFromChild(const Widget& child, animation::Display* d) {
    auto to_child = TransformToChild(child, d);
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
T* Closest(std::shared_ptr<Widget>& widget) {
  Widget* w = widget.get();
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
