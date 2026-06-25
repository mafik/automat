// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "widget.hpp"

#include <include/core/SkColor.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkDrawable.h>
#include <include/core/SkFlattenable.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkRect.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkSerialProcs.h>
#include <include/core/SkShader.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/gpu/graphite/Context.h>
#include <include/gpu/graphite/Surface.h>
#include <include/pathops/SkPathOps.h>
#include <llvm/ADT/SmallVector.h>

#include <ranges>

#include "build_variant.hpp"
#include "log.hpp"
#include "renderer.hpp"
#include "root_widget.hpp"
#include "time.hpp"

using namespace std;

namespace automat::ui {

Str ToStr(PointerButton btn) {
  switch (btn) {
    case PointerButton::Unknown:
      return "Unknown";
    case PointerButton::Left:
      return "Left";
    case PointerButton::Middle:
      return "Middle";
    case PointerButton::Right:
      return "Right";
    case PointerButton::Back:
      return "Back";
    case PointerButton::Forward:
      return "Forward";
    case PointerButton::Count:
      return "Count";
  }
}

Str ToStr(ActionTrigger trigger) {
  if (trigger.repr >= ActionTrigger::kPointerStart && trigger.repr < ActionTrigger::kPointerEnd) {
    return "PointerButton::" + ToStr(static_cast<PointerButton>(trigger));
  } else if (trigger.repr >= ActionTrigger::kAnsiKeyStart &&
             trigger.repr < ActionTrigger::kAnsiKeyEnd) {
    return Str("AnsiKey::") + Str(ToStr(static_cast<AnsiKey>(trigger)));
  } else {
    return "Invalid ActionTrigger";
  }
}

void Widget::Reparent(Widget& new_parent) {
  auto fix = TransformBetween(*parent, new_parent);
  SkM44 fix44(fix);
  OnReparent(new_parent, fix44);
  Widget* old_parent = parent;
  parent = &new_parent;
  if (old_parent != &new_parent) {
    if (old_parent) {
      old_parent->layers.Remove(this);
      old_parent->OnChildReparentedAway(*this);
    }
    new_parent.layers.InsertFront(this);  // front by default; new parent reorders if it cares
  }
}

void Widget::DrawCached(SkCanvas& canvas) const {
  if (pack_frame_texture_bounds == nullopt) {
    Draw(canvas);
  } else {
    canvas.drawDrawable(sk_drawable.get());
  }
}

void Widget::WakeAnimation() const {
  auto now = time::SteadyNow();
  WakeAnimationAt(now);
}

void Widget::WakeAnimationAt(time::SteadyPoint when) const {
  if (next_tick == time::SteadyPoint::max()) {
    // When a widget is woken up after a long sleep, we assume that it was just rendered. This
    // prevents the animation from thinking that the initial frame took a very long time.
    last_tick = when;
  }
  next_tick = min(next_tick, when);
}

void Widget::BakeChildStack(SkCanvas& canvas, const Widget& child) {
  canvas.save();
  canvas.concat(child.local_to_parent);
  // The child's Over()/Under() bands are not baked into the child's texture; they composite here,
  // in the child's baker (this surface), wrapped around the child. Recursing lands a whole detached
  // subtree in its nearest baking ancestor.
  for (auto* under : ranges::reverse_view(child.layers.Under())) {
    BakeChildStack(canvas, *under);
  }
  child.DrawCached(canvas);
  for (auto* over : ranges::reverse_view(child.layers.Over())) {
    BakeChildStack(canvas, *over);
  }
  canvas.restore();
}

void Widget::BakeChildren(SkCanvas& canvas) const {
  for (auto* child : ranges::reverse_view(layers.Baked())) {
    BakeChildStack(canvas, *child);
  }
}

static void PathToRoot(const Widget* start, std::vector<const Widget*>& out_path) {
  do {
    out_path.push_back(start);
    start = start->parent;
  } while (start);
}

bool Widget::IsAbove(Widget& other) const {
  std::vector<const Widget*> path_this;
  std::vector<const Widget*> path_other;
  PathToRoot(this, path_this);
  PathToRoot(&other, path_other);
  // Final element of both paths is guaranteed to be RootWidget
  int n_common = 1;
  // Descend into the path until paths differ
  while (path_this.size() > n_common && path_other.size() > n_common &&
         path_this[path_this.size() - n_common - 1] ==
             path_other[path_other.size() - n_common - 1]) {
    ++n_common;
  }
  if (n_common == path_this.size()) {
    // `other` is a descendant of `this`
    return true;
  } else if (n_common == path_other.size()) {
    // `other` is an ancestor of `this`
    return false;
  } else {
    // `other` & `this` share an ancestor
    auto* shared_ancestor = path_this[path_this.size() - n_common];
    auto* this_ancestor = path_this[path_this.size() - n_common - 1];
    auto* other_ancestor = path_other[path_other.size() - n_common - 1];
    for (auto* child : shared_ancestor->layers) {
      if (child == this_ancestor) {
        return true;
      } else if (child == other_ancestor) {
        return false;
      }
    }
    ERROR_ONCE << "Cannot determine which widget is on top because " << shared_ancestor->Name()
               << " (which is a shared ancestor of " << this->Name() << " and " << other.Name()
               << ") didn't report either of their unique ancestors (" << this_ancestor->Name()
               << " and " << other_ancestor->Name() << ") as its children";
    return true;
  }
}

SkMatrix TransformDown(const Widget& to) {
  auto up = TransformUp(to);
  SkMatrix down;
  (void)up.invert(&down);
  return down;
}

SkMatrix TransformUp(const Widget& from) {
  SkMatrix up = from.local_to_parent.asM33();
  if (from.parent) {
    up.postConcat(TransformUp(*from.parent));
  }
  return up;
}

SkMatrix TransformBetween(const Widget& from, const Widget& to) {
  // Reference implementation: (simple but expensive and less numerically stable)
  // auto up = TransformUp(from);
  // auto down = TransformDown(to);
  // return SkMatrix::Concat(down, up);
  SmallVec<Widget*, 8> path_from;
  for (auto* w : from.Parents()) {
    path_from.push_back(w);
  }

  SmallVec<Widget*, 8> path_to;
  for (auto* w : to.Parents()) {
    path_to.push_back(w);
  }

  int n_shared_ancestors = 0;
  while (n_shared_ancestors < path_from.size() && n_shared_ancestors < path_to.size()) {
    auto* anc_from = path_from[path_from.size() - 1 - n_shared_ancestors];
    auto* anc_to = path_to[path_to.size() - 1 - n_shared_ancestors];
    // same widget OR same transform
    // Avoiding different widgets with the same transform improves numerical stability.
    // This is probably due to multiplying & inverting a matrix not always preserving all the bits.
    if ((anc_from != anc_to) && (anc_from->local_to_parent != anc_to->local_to_parent)) {
      break;
    }
    ++n_shared_ancestors;
  }

  auto to_up = SkMatrix::I();
  for (int i = 0; i < path_to.size() - n_shared_ancestors; ++i) {
    to_up.postConcat(path_to[i]->local_to_parent.asM33());
  }
  SkMatrix between;
  (void)to_up.invert(&between);

  for (int i = path_from.size() - n_shared_ancestors - 1; i >= 0; --i) {
    between.preConcat(path_from[i]->local_to_parent.asM33());
  }

  return between;
}

Str ToStr(Widget* widget) {
  Str ret;
  while (widget) {
    ret = Str(widget->Name()) + (ret.empty() ? "" : " -> " + ret);
    widget = widget->parent;
  }
  return ret;
}

std::map<uint32_t, Widget*>& GetWidgetIndex() {
  static std::map<uint32_t, Widget*> widget_index = {};
  return widget_index;
}

Widget::Widget(Widget* parent) : parent(parent) {
  GetWidgetIndex()[ID()] = this;
  sk_drawable = MakeWidgetDrawable(*this);
  if (parent) {
    parent->layers.InsertFront(this);
  }
}

Widget::~Widget() {
  if (parent) {
    parent->layers.Remove(this);
  }
  GetWidgetIndex().erase(ID());
}

void Widget::CheckAllWidgetsReleased() {
  auto& widget_index = GetWidgetIndex();
  if (widget_index.empty()) {
    return;
  }
  ERROR << "Leaked references to " << widget_index.size() << " widget(s):";
  for (auto& [id, widget] : widget_index) {
    auto name = widget->Name();
    ERROR << f("  {} with ID {} with name {}", static_cast<void*>(widget), id,
               std::string(name.data(), name.size()));
  }
}

void Widget::RedrawThisFrame() {
  if (pack_frame_texture_bounds) {
    invalidated = time::SteadyPoint::min();
  } else {
    for (auto* child : layers) {
      child->RedrawThisFrame();
    }
  }
}

uint32_t Widget::ID() const {
  static atomic<uint32_t> id_counter = 0;
  if (id == 0) {
    id = ++id_counter;
  }
  return id;
}

Widget* Widget::Find(uint32_t id) {
  if (auto it = GetWidgetIndex().find(id); it != GetWidgetIndex().end()) {
    return it->second;
  } else {
    return nullptr;
  }
}

void Widget::ValidateHierarchy(std::source_location location) {
  if constexpr (build_variant::NotRelease) {
    for (auto* child : layers) {
      if (child->parent != this) {
        LogEntry(LogLevel::Error, location)
            << "Widget " << child->Name() << " has parent "
            << (child->parent ? child->parent->Name() : "nullptr")
            << f(" ({})", static_cast<void*>(child->parent)) << " but should have " << this->Name()
            << f(" ({})", static_cast<void*>(this));
      }
      child->ValidateHierarchy(location);
    }
  }
}

SkPath Widget::ShapeRecursive() const {
  SkPath shape = Shape();
  if (shape.isEmpty()) {  // only descend into children if the parent widget has no shape
    SkPathBuilder builder;
    builder.addPath(shape);
    for (auto* child : layers) {
      SkPath child_shape = child->ShapeRigid().makeTransform(child->local_to_parent.asM33());
      builder.addPath(child_shape);
    }
    shape = builder.detach();
  }
  return shape;
}

SkPath Widget::ShapeRigid() const { return ShapeRecursive(); }

bool Widget::Intersects(const Widget& a, const Widget& b) {
  SkPath a_shape = a.ShapeRigid().makeTransform(TransformBetween(a, b));
  SkPath b_shape = b.ShapeRigid();
  SkPath intersection;
  bool result = Op(a_shape, b_shape, kIntersect_SkPathOp, &intersection);
  return result && intersection.countVerbs() > 0;
}

RootWidget& Widget::FindRootWidget() const {
  Widget* w = const_cast<Widget*>(this);
  while (w->parent) {
    w = w->parent;
  }
  auto* root = dynamic_cast<struct RootWidget*>(w);
  assert(root);
  return *root;
}

ToyStore& Widget::ToyStore() const { return FindRootWidget().toys; }

std::unique_ptr<Action> Widget::FindAction(Pointer& pointer, ActionTrigger btn) {
  if (btn == PointerButton::Right) {
    LOG << "Right click on " << Name();
    return nullptr;
  }
  return nullptr;
}

void DebugCheckParents(Widget& widget) {
  using namespace mortal_priv;
  if (Widget* parent = widget.parent) {
    Ref* ref = Untag(parent->mortal_coil.head);
    bool found = false;
    while (ref) {
      if (ref == static_cast<Ref*>(&widget.parent)) {
        found = true;
        break;
      }
      ref = Untag(ref->next);
    }
    if (!found) {
      ERROR << widget.Name() << " is not known by its parent!";
      LOG << "  Widget 'parent' ptr is located at: " << f("{}", (void*)&widget.parent);
      LOG << "  Parent's ref list:";
      for (Ref* r = Untag(parent->mortal_coil.head); r; r = Untag(r->next)) {
        LOG << "    " << f("{}", (void*)r);
      }
    }
  }
  for (auto* child : widget.layers) {
    DebugCheckParents(*child);
  }
}

Str Widget::ParentsView::ToStr() const {
  constexpr auto kSep = " → "sv;
  auto start_name = start->Name();

  // Go over parents and find `n` - how many chars we need to store the result
  int n = start_name.size();
  auto* parent = start->parent.Get();
  while (parent) {
    n += kSep.size() + parent->Name().size();
    parent = parent->parent.Get();
  }

  // Go over parents again and fill the result string
  Str result;
  result.resize(n);
  int pos = n;
  auto ReplaceBack = [&](StrView sv) {
    auto size = sv.size();
    pos -= size;
    result.replace(pos, size, sv);
  };
  ReplaceBack(start_name);
  parent = start->parent.Get();
  while (parent) {
    ReplaceBack(kSep);
    ReplaceBack(parent->Name());
    parent = parent->parent.Get();
  }
  return result;
}

}  // namespace automat::ui
