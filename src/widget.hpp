#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <include/core/SkCanvas.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>
#include <include/core/SkSurface.h>
#include <src/core/SkReadBuffer.h>
#include <src/core/SkWriteBuffer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <source_location>
#include <typeinfo>

#include "action.hpp"
#include "animation.hpp"
#include "key.hpp"
#include "menu.hpp"
#include "mortal.hpp"
#include "optional.hpp"
#include "ptr.hpp"
#include "span.hpp"
#include "str.hpp"
#include "time.hpp"
#include "vec.hpp"

namespace automat {
struct Object;
struct Argument;
struct Location;
struct Syncable;
struct ToyStore;
}  // namespace automat

namespace automat::ui {

struct Widget;
struct RootWidget;
struct Caret;

Str ToStr(Ptr<Widget> widget);

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

Str ToStr(ActionTrigger);

// Widgets are things that can be drawn to the SkCanvas. They're sometimes produced by Objects
// which can't draw themselves otherwise.
struct Widget : OptionsProvider {
  Widget(Widget* parent);
  Widget(const Widget&) = delete;
  virtual ~Widget();

  virtual StrView Name() const {
    const std::type_info& info = typeid(*this);
    return CleanTypeName(info.name());
  }

  static void CheckAllWidgetsReleased();

  uint32_t ID() const;
  static Widget* Find(uint32_t id);

  MortalCoil mortal_coil;
  MortalPtr<Widget> parent;
  SkM44 local_to_parent = SkM44();

  // TODO: maybe replace with subtree_shape_invalid
  SkM44 packed_local_to_parent = SkM44();  // used by PackFrame to recompute subtree_shape

  // This is updated by renderer, right before the call to Tick.
  //
  // This is same as the CTM in the SkCanvas passed to Draw().
  //
  // If this matrix changes, TransformUpdated() will also be called (also before Tick). This allows
  // widgets to react to changes in their screen position.
  SkMatrix local_to_window = {};

  // When the next Tick is due. The renderer ticks this widget once `now` reaches this
  // point; max means no tick is scheduled. Initially due immediately.
  mutable time::SteadyPoint next_tick = time::SteadyPoint::min();
  // When Tick last ran; the basis for the animation time delta. min means never.
  mutable time::SteadyPoint last_tick = time::SteadyPoint::min();

  bool IsAnimating() const { return next_tick != time::SteadyPoint::max(); }

  // Force the widget to be re-rendered this frame, regardless of the render budget.
  //
  // This will not call `Tick`. Also call `WakeAnimation` if you want to wake up the animation.
  //
  // Marks this widget (or, if it has no texture, its children) as never valid.
  void RedrawThisFrame();

  // Things updated in PackFrame (& Draw)

  sk_sp<SkDrawable> sk_drawable;  // holds a WidgetDrawable
  mutable uint32_t id = 0;
  float smooth_render_millis = NAN;

  // TODO: remove / clean up
  Optional<SkRect> pack_frame_texture_bounds;

  // Whenever PackFrame decides to render a widget, it stores the rendered bounds in this field.
  // This is then used later to check if the old surface can be reused.
  Optional<Rect> rendered_bounds;
  SkMatrix rendered_matrix;
  // Moment when the on-screen texture stopped matching this widget's content.
  time::SteadyPoint invalidated = time::SteadyPoint::min();

  bool rendering = false;     // Whether the widget is currently being rendered by the client.
  bool dead = false;          // Set to true by `MarkDead`.
  bool shape_invalid = true;  // Cleared by `PackFrame`.
  bool subtree_shape_invalid = true;  // Cleared by `PackFrame`.

  // Due to the concurrent nature of Automat's objects the Widgets only keep a weak reference to the
  // underlying objects. When these objects are deleted, on the next lock attempt, the widget
  // obtains nullptr. This is the signal for the widget to mark itself as dead (with `MarkDead`).
  //
  // Some (zombie) widgets may choose to not report their death immediately and instead animate
  // their disappearance. In this case, they should call `MarkDead` when the disappearance animation
  // finishes. Examples are LocationWidget and ConnectionWidget.
  void MarkDead(time::SteadyPoint now) {
    dead = true;
    if (parent) {
      parent->OnChildDead(*this, now);
    }
  }

  // Used to notify the parent that one of its children has expired. The parent should remove it
  // from its children list in the response.
  //
  // Default implementation only wakes the animation.
  //
  // This will typically be called AFTER parent's Tick & DURING the child's Tick.
  virtual void OnChildDead(Widget& child, time::SteadyPoint now) { WakeAnimationAt(now); }

  // Called on the *old* parent immediately after one of its children was reparented to a different
  // widget. The old parent should remove `child` from any list/slot it stores it in so that
  // subsequent `FillChildren` calls don't return a widget whose `parent` no longer points at us.
  virtual void OnChildReparentedAway(Widget& child) {}

  // Can be used by parent to store some data. Helps in avoiding allocations
  int index;

  RootWidget& FindRootWidget() const;
  ToyStore& ToyStore() const;

  // Validates that the parent/children hierarchy is correctly maintained (in non-release builds).
  void ValidateHierarchy(std::source_location location = std::source_location::current());

  virtual void PointerHover(Pointer&) {}
  virtual void PointerUnhover(Pointer&) {}

  virtual void PointerEnter(Pointer&) {}
  virtual void PointerLeave(Pointer&) {}

  // Scroll wheel over this widget. `delta` is in notches, positive when the finger
  // moves up. Return true to consume it.
  virtual bool PointerWheel(Pointer&, float delta) { return false; }

  // Caret events. A widget that requests a Caret (via Keyboard::RequestCaret) overrides these
  // to receive keyboard input and a release notification.
  virtual void ReleaseCaret(Caret&) {}
  virtual void KeyDown(Caret&, Key) {}
  virtual void KeyUp(Caret&, Key) {}

  // Called when the widget's total transform (any of the `local_to_parent` in its ancestry) has
  // changed.
  //
  // This may be used by some widgets to trigger re-rendering.
  virtual void TransformUpdated(time::Timer&) {}

  void DrawCached(SkCanvas&) const;
  void WakeAnimation() const;
  void WakeAnimationAt(time::SteadyPoint) const;

  // Encodes decisions whether to (1) draw and/or (2) to keep ticking.
  struct Tock {
    struct DrawingProxy {
      void operator|=(animation::Progress);
      void operator|=(bool keep_going);
    } drawing [[no_unique_address]];  // bool-like that sets both `next_tick` & `draw`
    struct ShapingProxy {
      void operator|=(animation::Progress);
      void operator|=(bool keep_going);
    } shaping [[no_unique_address]];  // bool-like that sets both `next_tick`, `draw` & `shape`
    struct TickingProxy {
      void operator|=(bool keep_going);
      explicit operator bool() const;
    } ing [[no_unique_address]];  // bool-like for whether widget is animating
    time::SteadyPoint next_tick = time::SteadyPoint::max();
    bool draw = false;
    bool shape = false;

    Tock& operator|=(Tock o) {
      draw |= o.draw;
      shape |= o.shape;
      next_tick = std::min(next_tick, o.next_tick);
      return *this;
    }
    friend Tock operator|(Tock a, Tock b) { return a |= b; }

    static const Tock Draw;     // repaint once, then sleep
    static const Tock Drawing;  // repaint and tick again next frame
    static const Tock Shape;    // update shape & repaint once, then sleep
    static const Tock Shaping;  // update shape & repaint and tick again next frame
    static const Tock Ing;      // tick again next frame without repainting
  };

  // Called for visible widgets while they're being animated.
  // Use this function to update the widget's animation state.
  // Once a widget finishes its animation, its Tick is no longer being called. Wake it up again by
  // calling WakeAnimation.
  virtual Tock Tick(time::Timer&) { return {}; }

  virtual void Draw(SkCanvas& canvas) const { return BakeChildren(canvas); }

  // Compositor decides how the widget's texture is going to be copied onto the parent texture.
  // If GPU is overloaded, the texture may not be ready in time. Compositor's job is then to take
  // the (possibly) old texture and deform it using anchor points to reduce latency.
  //
  // Compositors are a client-side feature and are implemented in WidgetDrawable::onDraw
  // (renderer.cpp).
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

  // Each Widget has a shape that defines its region of reactivity, in local coordinates.
  virtual SkPath Shape() const = 0;

  SkPath shape;          // Cached result of the last `Shape()` call.
  SkPath subtree_shape;  // Union of `shape` and every child's `subtree_shape`

  void RecomputeSubtreeShape();

  // Can be overridden to provide a more efficient alternative to `shape.getBounds()`.
  // Local (metric) coordinates.
  virtual RRect CoarseBounds() const {
    // `shape` is only populated by PackFrame; before the first tick (e.g. when a parent lays out
    // freshly created children in its constructor) it is empty, so fall back to a live `Shape()`.
    SkPath fresh;
    const SkPath* s = &shape;
    if (s->isEmpty()) {
      fresh = Shape();
      s = &fresh;
    }
    RRect ret{};
    if (s->isRect(&ret.rect.sk)) {
      ret.type = SkRRect::kRect_Type;
    } else if (s->isRRect(&ret.sk)) {
      // cool
    } else if (s->isOval(&ret.rect.sk)) {
      auto r = ret.rect.Size() / 2;
      ret.radii[0] = ret.radii[1] = ret.radii[2] = ret.radii[3] = r;
      ret.type = SkRRect::kOval_Type;
    } else {
      ret.rect = s->getBounds();
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

  // Called when this Widget is reparented.
  //
  // The `fix` matrix may be applied to local_to_parent in order to keep the widget at the same
  // screen position (default implementation does exactly that).
  virtual void OnReparent(Widget& new_parent, SkM44& fix) { local_to_parent.postConcat(fix); }

  void Reparent(Widget& new_parent);

  virtual DropTarget* AsDropTarget() { return nullptr; }

  // If the object should be cached into a texture, return its bounds in local coordinates.
  virtual Optional<Rect> TextureBounds() const { return shape.getBounds(); }
  virtual Vec<Vec2> TextureAnchors() { return {}; }

  // This will draw the given child widget using it's precomputed texture (if available).
  //
  // Can be overridden to change how child's textures are composited (or prevent children from being
  // drawn entirely).
  static void BakeChildStack(SkCanvas&, const Widget& child);

  // Baba Yaga approves.
  void BakeChildren(SkCanvas&) const;

  struct LayerStack {
    int bake_begin = 0;
    int bake_end = 0;

    // Iterate all children front-to-back (Over, then Baked, then Under).
    auto begin() { return vec.begin(); }
    auto end() { return vec.end(); }
    auto begin() const { return vec.begin(); }
    auto end() const { return vec.end(); }
    size_t size() const { return vec.size(); }
    Widget* operator[](size_t i) const { return vec[i]; }

    // Move `child` (already a member) into the Baked() range.
    void OrderInside(Widget* child) {
      int from = IndexOf(child);
      if (from >= bake_begin && from < bake_end) return;
      Place(from, bake_end - (from < bake_end), Layer::Baked);
    }

    // Place `child` (already a member) just in front of `reference`, joining its range.
    // `reference == nullptr` means the parent: `child` lands at the bottom of the Over range.
    void OrderAbove(Widget* child, Widget* reference = nullptr) {
      int from = IndexOf(child);
      if (reference == nullptr) {
        Place(from, bake_begin - (from < bake_begin), Layer::Over);
      } else {
        int ref = IndexOf(reference);
        if (ref == (int)vec.size()) return;
        Place(from, ref - (ref > from), LayerAt(ref));
      }
    }

    // Place `child` (already a member) just behind `reference`, joining its range.
    // `reference == nullptr` means the parent: `child` lands at the top of the Under range.
    void OrderBelow(Widget* child, Widget* reference = nullptr) {
      int from = IndexOf(child);
      if (reference == nullptr) {
        Place(from, bake_end - (from < bake_end), Layer::Under);
      } else {
        int ref = IndexOf(reference);
        if (ref == (int)vec.size()) return;
        Place(from, ref + (ref < from), LayerAt(ref));
      }
    }

    // Move `child` (already a member) to the very front, the top of the Over range.
    void OrderTop(Widget* child) { Place(IndexOf(child), 0, Layer::Over); }

    // Move `child` (already a member) to the very back, the bottom of the Under range.
    void OrderBottom(Widget* child) { Place(IndexOf(child), (int)vec.size() - 1, Layer::Under); }

    // Range of child widgets which are composited over their parent.
    Span<Widget*> Over() { return Span<Widget*>(vec).Resize(bake_begin); }

    // Range of child widgets which are composited within their parent's Draw().
    //
    // This allows clipping, under/over-draw & fancy special effects through shaders.
    Span<Widget*> Baked() {
      return Span<Widget*>(vec.begin() + bake_begin, vec.begin() + bake_end);
    }

    // Range of child widgets which are composited under their parent.
    Span<Widget*> Under() { return Span<Widget*>(vec).RemovePrefix(bake_end); }

   private:
    SmallVec<Widget*> vec{};

    enum class Layer { Over, Baked, Under };

    int IndexOf(Widget* w) const { return (int)(std::ranges::find(vec, w) - vec.begin()); }

    Layer LayerAt(int i) const {
      return i < bake_begin ? Layer::Over : i < bake_end ? Layer::Baked : Layer::Under;
    }

    // Move an element from 'from' to 'to'/'layer' while keeping 'bake_*' range correct.
    void Place(int from, int to, Layer layer) {
      Layer was = LayerAt(from);
      auto b = vec.begin();
      if (from < to) {
        std::ranges::rotate(b + from, b + from + 1, b + to + 1);
      } else if (to < from) {
        std::ranges::rotate(b + to, b + from, b + from + 1);
      }
      bake_begin += (layer == Layer::Over) - (was == Layer::Over);
      bake_end += (layer != Layer::Under) - (was != Layer::Under);
    }

    // Add at the top of the Over() range.
    void InsertFront(Widget* w) {
      vec.insert(vec.begin(), w);
      ++bake_begin;
      ++bake_end;
      InvalidateSubtreeShape();
    }

    void Remove(Widget* w) {
      int i = IndexOf(w);
      if (i == (int)vec.size()) return;
      if (i < bake_begin) --bake_begin;
      if (i < bake_end) --bake_end;
      vec.erase(vec.begin() + i);
      InvalidateSubtreeShape();
    }

    void InvalidateSubtreeShape() {
      reinterpret_cast<Widget*>(reinterpret_cast<uintptr_t>(this) - offsetof(Widget, layers))
          ->subtree_shape_invalid = true;
    }

    friend struct Widget;

  } mutable layers;  // protected by RootWidget::mutex;

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

using Tock = Widget::Tock;

static_assert(offsetof(Tock, drawing) == 0, "DrawingProxy assumes offset = 0");
inline void Tock::DrawingProxy::operator|=(animation::Progress p) {
  auto& t = *reinterpret_cast<Tock*>(this);
  if (p.value_changed) t.draw = true;
  if (!p.settled) t.next_tick = time::SteadyPoint::min();
}
inline void Tock::DrawingProxy::operator|=(bool keep_going) {
  if (!keep_going) return;
  auto& t = *reinterpret_cast<Tock*>(this);
  t.draw = true;
  t.next_tick = time::SteadyPoint::min();
}

static_assert(offsetof(Tock, shaping) == 0, "ShapingProxy assumes offset = 0");
inline void Tock::ShapingProxy::operator|=(animation::Progress p) {
  auto& t = *reinterpret_cast<Tock*>(this);
  if (p.value_changed) {
    t.draw = true;
    t.shape = true;
  }
  if (!p.settled) t.next_tick = time::SteadyPoint::min();
}
inline void Tock::ShapingProxy::operator|=(bool keep_going) {
  if (!keep_going) return;
  auto& t = *reinterpret_cast<Tock*>(this);
  t.draw = true;
  t.shape = true;
  t.next_tick = time::SteadyPoint::min();
}

static_assert(offsetof(Tock, ing) == 0, "TickingProxy assumes offset = 0");
inline void Tock::TickingProxy::operator|=(bool keep_going) {
  if (!keep_going) return;
  auto& t = *reinterpret_cast<Tock*>(this);
  t.next_tick = time::SteadyPoint::min();
}
inline Tock::TickingProxy::operator bool() const {
  auto& t = *reinterpret_cast<const Tock*>(this);
  return t.next_tick != time::SteadyPoint::max();
}

inline const Tock Tock::Draw{.draw = true};
inline const Tock Tock::Drawing{.next_tick = time::SteadyPoint::min(), .draw = true};
inline const Tock Tock::Shape{.draw = true, .shape = true};
inline const Tock Tock::Shaping{.next_tick = time::SteadyPoint::min(), .draw = true, .shape = true};
inline const Tock Tock::Ing{.next_tick = time::SteadyPoint::min()};

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
// pointer points to invalid memory or widget destruction fails somewhere in its MortalCoil).
void DebugCheckParents(Widget& widget);

}  // namespace automat::ui
