// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkCanvas.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>
#include <include/core/SkSurface.h>
#include <src/core/SkReadBuffer.h>
#include <src/core/SkWriteBuffer.h>

#include <cmath>
#include <memory>

#include "action.hh"
#include "animation.hh"
#include "key.hh"
#include "menu.hh"
#include "optional.hh"
#include "ptr.hh"
#include "span.hh"
#include "str.hh"
#include "time.hh"
#include "vec.hh"

namespace automat {
struct Object;
struct Argument;
struct Location;
struct Syncable;
}  // namespace automat

namespace automat::ui {

struct Widget;
struct RootWidget;
struct ToyStore;

Str ToStr(Ptr<Widget> widget);

inline Span<Widget*> WidgetPtrSpan(Span<std::unique_ptr<Widget>> vec) {
  return {reinterpret_cast<Widget**>(vec.data()), vec.size()};
}

// Transform from the RootWidget coordinates to the local coordinates of the widget.
SkMatrix TransformDown(const Widget& to);
// Transform from the local coordinates of the widget to the RootWidget coordinates.
SkMatrix TransformUp(const Widget& from);
// Transform from the local coordinates of the widget to the local coordinates of another widget.
SkMatrix TransformBetween(const Widget& from, const Widget& to);

struct DropTarget;

enum class PointerButton { Unknown, Left, Middle, Right, Back, Forward, Count };

Str ToStr(PointerButton);

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

// Widgets are things that can be drawn to the SkCanvas. They're sometimes produced by Objects
// which can't draw themselves otherwise.
struct Widget : Trackable, OptionsProvider {
  Widget(Widget* parent);
  Widget(const Widget&) = delete;
  virtual ~Widget();

  static void CheckAllWidgetsReleased();

  uint32_t ID() const;
  static Widget* Find(uint32_t id);

  TrackedPtr<Widget> parent;
  SkM44 local_to_parent = SkM44();

  // This is updated by renderer, right before the call to Tick.
  //
  // This is same as the CTM in the SkCanvas passed to Draw().
  //
  // If this matrix changes, TransformUpdated() will also be called (also before Tick). This allows
  // widgets to react to changes in their screen position.
  SkMatrix local_to_window = {};

  // The time when the animation should wake up.
  // Initially this is set to 0 (meaning it should wake up immediately).
  // When the widget's animation finishes, set this to max value.
  mutable time::SteadyPoint wake_time = time::SteadyPoint::min();
  // The time when the Tick was last called. Updated right after Tick.
  mutable time::SteadyPoint last_tick_time;

  // Set to true if the widget should be redrawn (without the need for animation `Tick`).
  // This may be the case when its children are changed but its animation state is otherwise
  // unaffected.
  bool needs_draw = false;

  bool redraw_this_frame = false;

  // Force the widget to be redrawn ASAP (without offscreen rendering).
  //
  // This will not call `Tick`. Also call `WakeAnimation` if you want to wake up the animation.
  //
  // Sets force_packing to true on either this widget or (if it has no texture) its children.
  void RedrawThisFrame();

  // Things updated in PackFrame (& Draw)

  sk_sp<SkDrawable> sk_drawable;  // holds a WidgetDrawable
  mutable uint32_t id = 0;
  float average_draw_millis = NAN;

  // TODO: remove / clean up
  Optional<SkRect> pack_frame_texture_bounds;

  // Whenever PackFrame decides to render a widget, it stores the rendered bounds in this field.
  // This is then used later to check if the old surface can be reused.
  Optional<Rect> rendered_bounds;
  SkMatrix rendered_matrix;
  bool rendering = false;  // Whether the widget is currently being rendered by the client.
  bool rendering_to_screen = false;  // Whether the current render job is going to be presented.

  RootWidget& FindRootWidget() const;
  ToyStore& ToyStore() const;

  // Validates that the parent/children hierarchy is correctly maintained (in non-release builds).
  void ValidateHierarchy();

  virtual void PointerOver(Pointer&) {}
  virtual void PointerLeave(Pointer&) {}

  // Called when the widget's total transform (any of the `local_to_parent` in its ancestry) has
  // changed.
  //
  // This may be used by some widgets to trigger re-rendering.
  virtual void TransformUpdated() {}

  virtual void PreDraw(SkCanvas&) const {}
  void DrawCached(SkCanvas&) const;
  void WakeAnimation() const;
  void WakeAnimationAt(time::SteadyPoint) const;

  // Called for visible widgets while they're being animated.
  // Use this function to update the widget's animation state.
  // Once a widget finishes its animation, it's Tick is no longer being called. Wake it up again by
  // calling WakeAnimation.
  virtual animation::Phase Tick(time::Timer&) { return animation::Finished; }

  virtual void Draw(SkCanvas& canvas) const { return DrawChildren(canvas); }

  // Compositor decides how the widget's texture is going to be copied onto the parent texture.
  // If GPU is overloaded, the texture may not be ready in time. Compositor's job is then to take
  // the (possibly) old texture and deform it using anchor points to reduce latency.
  //
  // Compositors are a client-side feature and are implemented in WidgetDrawable::onDraw
  // (renderer.cc).
  enum class Compositor {
    // Copy this surface to the parent canvas. This ignores any anchor points & widget's
    // local_to_parent transform.
    COPY_RAW,
    // Fill in the missing part of the widget using CRT-like glitch effect.
    GLITCH,
    // Deform the image based on returned anchor points.
    ANCHOR_WARP,
    // Limit scale using fancy zoom in effect.
    QUANTUM_REALM,
    // TODO: Add a value that will prevent rendering to separate texture.
  };

  virtual Compositor GetCompositor() const { return Compositor::GLITCH; }

  // Each Widget has a shape that defines its region of reactivity.
  // The Shape of a widget is used to constrain the search process.
  // If children extend beyond the shape of their parent, they are not traversed.
  // Widgets with empty shape do not react themselves but are still traversed, allowing their
  // children to extend to arbitrary shapes.
  virtual SkPath Shape() const = 0;

  // Widgets that don't have shape (infinite layers) but which have some child widgets may attempt
  // to report some shape by taking a union of their child shapes.
  //
  // This function defaults to Shape() - if not empty.
  // Otherwise it combines the shapes of this Widget's children.
  SkPath ShapeRecursive() const;

  // Each Widget has some shape where it can function as a rigid base for other Widgets.
  // This is used for ExtractStack - to grab a whole stack of objects.
  //
  // This defaults to ShapeRecursive().
  virtual SkPath ShapeRigid() const;

  // Can be overridden to provide a more efficient alternative to `Shape()->getBounds()`.
  // When not overridden, `Shape().getBounds()` is used.
  // Local (metric) coordinates.
  virtual RRect CoarseBounds() const {
    auto shape = Shape();
    RRect ret{};
    if (shape.isRect(&ret.rect.sk)) {
      ret.type = SkRRect::kRect_Type;
    } else if (shape.isRRect(&ret.sk)) {
      // cool
    } else if (shape.isOval(&ret.rect.sk)) {
      auto r = ret.rect.Size() / 2;
      ret.radii[0] = ret.radii[1] = ret.radii[2] = ret.radii[3] = r;
      ret.type = SkRRect::kOval_Type;
    } else {
      ret.rect = shape.getBounds();
      ret.type = SkRRect::kRect_Type;
    }
    return ret;
  }

  // Can be overridden to signal that the widget can be effectively centered by moving its origin to
  // zero. This is useful for widgets that have a natural center point that is not the center of
  // their bounds.
  virtual bool CenteredAtZero() const { return false; }

  // Anchor, around which various transforms are applied. This should correspond to the center of
  // the widget's mass.
  virtual Vec2 LocalAnchor() const {
    if (CenteredAtZero()) {
      return Vec2(0, 0);
    }
    return CoarseBounds().Center();
  }

  void VisitOptions(const OptionsVisitor&) const override {}
  virtual std::unique_ptr<Action> FindAction(Pointer&, ActionTrigger);

  // Return true if the widget should be highlighted as draggable.
  virtual bool CanDrag() { return false; }

  virtual DropTarget* AsDropTarget() { return nullptr; }

  // If the object should be cached into a texture, return its bounds in local coordinates.
  virtual Optional<Rect> TextureBounds() const { return Shape().getBounds(); }
  virtual Vec<Vec2> TextureAnchors() { return {}; }

  // This will draw the given child widget using it's precomputed texture (if available).
  //
  // Can be overridden to change how child's textures are composited (or prevent children from being
  // drawn entirely).
  virtual void DrawChildCached(SkCanvas&, const Widget& child) const;

  virtual void PreDrawChildren(SkCanvas&) const;

  void DrawChildrenSpan(SkCanvas&, Span<Widget*> widgets) const;

  void DrawChildren(SkCanvas&) const;

  // Used to obtain references to the child widgets in a generic fashion.
  // Widgets are stored in front-to-back order.
  virtual void FillChildren(Vec<Widget*>& children) {}

  mutable Vec<Widget*> children_cache;

  Span<Widget*> Children() const {
    children_cache.clear();  // keeping Vec around prevents allocations
    const_cast<Widget*>(this)->FillChildren(children_cache);
    return children_cache;
  }

  bool IsAbove(Widget& other) const;

  // This can be used to block pointer events from propagating to children.
  virtual bool AllowChildPointerEvents(Widget& child) const { return true; }

  struct ParentsView {
    Widget* start;

    struct end_iterator {};

    struct iterator {
      Widget* widget;
      iterator(Widget* widget) : widget(widget) {}
      Widget* operator*() { return widget; }
      iterator& operator++() {
        widget = widget->parent;
        return *this;
      }
      bool operator!=(const end_iterator&) { return widget != nullptr; }
    };

    iterator begin() { return iterator(start); }
    end_iterator end() { return end_iterator(); }

    Str ToStr() const;
  };

  ParentsView Parents() const { return ParentsView{const_cast<Widget*>(this)}; }

  // Checks if the two widgets intersect (their shapes). This is accurate but also very slow so
  // avoid it if possible.
  static bool Intersects(const Widget& a, const Widget& b);
};

template <typename T>
T* Closest(Widget& widget) {
  Widget* w = &widget;
  while (w) {
    if (auto* result = dynamic_cast<T*>(w)) {
      return result;
    }
    w = w->parent;
  }
  return nullptr;
}

struct LabelMixin {
  virtual void SetLabel(StrView label) = 0;
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

// Use this function if the Widget hierarchy enters an invalid state (for example a widget's parent
// pointer points to invalid memory or widget destruction fails somewhere in TrackedPtr).
void DebugCheckParents(Widget& widget);

}  // namespace automat::ui
