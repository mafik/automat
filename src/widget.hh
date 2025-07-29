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
}  // namespace automat

namespace automat::ui {

struct Widget;
struct RootWidget;

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

  // The name for objects of this type. English proper noun, UTF-8, capitalized.
  // For example: "Text Editor".
  virtual StrView Name() const {
    const std::type_info& info = typeid(*this);
    return CleanTypeName(info.name());
  }

  RootWidget& FindRootWidget() const;

  // Validates that the parent/children hierarchy is correctly maintained (in non-release builds).
  void ValidateHierarchy();

  virtual void PointerOver(Pointer&) {}
  virtual void PointerLeave(Pointer&) {}

  // Called when the widget's total transform (any of the `local_to_parent` in its ancestry) has
  // changed.
  //
  // This may be used by some widgets to trigger re-rendering.
  virtual void TransformUpdated() {}

  void RecursiveTransformUpdated() {
    TransformUpdated();
    for (auto& child : Children()) {
      child->RecursiveTransformUpdated();
    }
  }

  virtual void PreDraw(SkCanvas&) const {}
  void DrawCached(SkCanvas&) const;
  virtual void WakeAnimation() const;

  // Called for visible widgets while they're being animated.
  // Use this function to update the widget's animation state.
  // Once a widget finishes its animation, it's Tick is no longer being called. Wake it up again by
  // calling WakeAnimation.
  virtual animation::Phase Tick(time::Timer&) { return animation::Finished; }

  virtual void Draw(SkCanvas& canvas) const { return DrawChildren(canvas); }
  virtual SkPath Shape() const = 0;

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

  virtual Vec2 ScalePivot() const {
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
  virtual Vec<Vec2> TextureAnchors() const { return {}; }

  virtual void DrawChildCachced(SkCanvas&, const Widget& child) const;

  virtual void PreDrawChildren(SkCanvas&) const;

  void DrawChildrenSpan(SkCanvas&, Span<Widget*> widgets) const;

  void DrawChildren(SkCanvas&) const;

  // Used to obtain references to the child widgets in a generic fashion.
  // Widgets are stored in front-to-back order.
  virtual void FillChildren(Vec<Widget*>& children) {}

  Vec<Widget*> Children() const {
    Vec<Widget*> children;
    const_cast<Widget*>(this)->FillChildren(children);
    return children;
  }

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
  };

  ParentsView Parents() const { return ParentsView{const_cast<Widget*>(this)}; }

  // Methods related to Widgets that represent Objects.
  // TODO: Move them to a separate class (ObjectWidget)

  // Describes the area of the widget where the given field is located.
  // Local (metric) coordinates.
  virtual SkPath FieldShape(Object&) const { return SkPath(); }

  // Returns the start position of the given argument.
  // Local (metric) coordinates.
  virtual Vec2AndDir ArgStart(const Argument&);

  // Places where the connections to this widget may terminate.
  // Local (metric) coordinates.
  virtual void ConnectionPositions(Vec<Vec2AndDir>& out_positions) const;

  static Widget& ForObject(Object&, const Widget* parent);

  // Find the shape of this widget. If it's shape is empty, combine its children's shapes.
  SkPath GetShapeRecursive() const;

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

}  // namespace automat::ui
