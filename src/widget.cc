// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "widget.hh"

#include <include/core/SkColor.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkDrawable.h>
#include <include/core/SkFlattenable.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkRect.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkSerialProcs.h>
#include <include/core/SkShader.h>
#include <include/effects/SkGradientShader.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/gpu/graphite/Context.h>
#include <include/gpu/graphite/Surface.h>
#include <include/pathops/SkPathOps.h>

#include <ranges>

#include "log.hh"
#include "renderer.hh"
#include "root_widget.hh"
#include "time.hh"

using namespace std;

namespace automat::gui {

void Widget::PreDrawChildren(SkCanvas& canvas) const {
  for (auto& widget : ranges::reverse_view(Children())) {
    canvas.save();
    canvas.concat(widget->local_to_parent);
    widget->PreDraw(canvas);
    canvas.restore();
  }
}

void Widget::DrawCached(SkCanvas& canvas) const {
  if (pack_frame_texture_bounds == nullopt) {
    return Draw(canvas);
  }

  canvas.drawDrawable(sk_drawable.get());
}

void Widget::WakeAnimation() const {
  auto now = time::SteadyNow();
  if (wake_time == time::SteadyPoint::max()) {
    // When a widget is woken up after a long sleep, we assume that it was just rendered. This
    // prevents the animation from thinking that the initial frame took a very long time.
    last_tick_time = now;
  }
  wake_time = min(wake_time, now);
}

void Widget::DrawChildCachced(SkCanvas& canvas, const Widget& child) const {
  canvas.save();
  canvas.concat(child.local_to_parent);
  child.DrawCached(canvas);
  canvas.restore();
}

void Widget::DrawChildrenSpan(SkCanvas& canvas, Span<Ptr<Widget>> widgets) const {
  std::ranges::reverse_view rv{widgets};
  for (auto& widget : rv) {
    DrawChildCachced(canvas, *widget);
  }  // for each Widget
}

void Widget::DrawChildren(SkCanvas& canvas) const {
  PreDrawChildren(canvas);
  for (auto& child : ranges::reverse_view(Children())) {
    DrawChildCachced(canvas, *child);
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
  // TODO: optimize by finding the closest common parent
  auto up = TransformUp(from);
  auto down = TransformDown(to);
  return SkMatrix::Concat(down, up);
}

Str ToStr(Ptr<Widget> widget) {
  Str ret;
  while (widget) {
    ret = Str(widget->Name()) + (ret.empty() ? "" : " -> " + ret);
  }
  return ret;
}

std::map<uint32_t, Widget*>& GetWidgetIndex() {
  static std::map<uint32_t, Widget*> widget_index = {};
  return widget_index;
}

Widget::Widget() {
  GetWidgetIndex()[ID()] = this;
  sk_drawable = MakeWidgetDrawable(*this);
}
Widget::~Widget() { GetWidgetIndex().erase(ID()); }

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
    redraw_this_frame = true;
  } else {
    for (auto& child : Children()) {
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

void Widget::FixParents() {
  for (auto& child : Children()) {
    if (child->parent.get() != this) {
      // TODO: uncomment this and fix all instances of this error
      // ERROR << "Widget " << child->Name() << " has parent " << f("{}",
      // static_cast<void*>(child->parent.get()))
      //       << " but should have " << this->Name() << f(" ({})", static_cast<void*>(this));
      child->parent = this->AcquirePtr();
    }
    child->FixParents();
  }
}
void Widget::ForgetParents() {
  parent = nullptr;
  for (auto& child : Children()) {
    child->ForgetParents();
  }
}

Ptr<Widget> Widget::ForObject(Object& object, const Widget& parent) {
  return parent.FindRootWidget().widgets.For(object, parent);
}

SkPath Widget::GetShapeRecursive() const {
  SkPath shape = Shape();
  if (shape.isEmpty()) {  // only descend into children if the parent widget has no shape
    for (auto& child : Children()) {
      SkPath child_shape = child->GetShapeRecursive();
      child_shape.transform(child->local_to_parent.asM33());
      shape.addPath(child_shape);
    }
  }
  return shape;
}

bool Widget::Intersects(const Widget& a, const Widget& b) {
  SkPath a_shape = a.GetShapeRecursive();
  SkPath b_shape = b.GetShapeRecursive();
  a_shape.transform(TransformBetween(a, b));
  SkPath intersection;
  bool result = Op(a_shape, b_shape, kIntersect_SkPathOp, &intersection);
  return result && intersection.countVerbs() > 0;
}

void Widget::ConnectionPositions(Vec<Vec2AndDir>& out_positions) const {
  // By default just one position on the top of the bounding box.
  auto shape = Shape();
  Rect bounds = shape.getBounds();
  out_positions.push_back(Vec2AndDir{
      .pos = bounds.TopCenter(),
      .dir = -90_deg,
  });
}

Vec2AndDir Widget::ArgStart(const Argument& arg) {
  SkPath shape;
  if (arg.field) {
    shape = FieldShape(*arg.field);
  }
  if (shape.isEmpty()) {
    shape = Shape();
  }
  Rect bounds = shape.getBounds();
  return Vec2AndDir{
      .pos = bounds.BottomCenter(),
      .dir = -90_deg,
  };
}

RootWidget& Widget::FindRootWidget() const {
  Widget* w = const_cast<Widget*>(this);
  while (w->parent.get()) {
    w = w->parent.get();
  }
  auto* root = dynamic_cast<struct RootWidget*>(w);
  assert(root);
  return *root;
}

std::unique_ptr<Action> Widget::FindAction(Pointer& pointer, ActionTrigger btn) {
  if (btn == PointerButton::Right) {
    LOG << "Right click on " << Name();
    return nullptr;
  }
  return nullptr;
}

}  // namespace automat::gui