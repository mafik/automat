#include "library_toolbar.hh"

#include <include/core/SkMatrix.h>

#include "gui_constants.hh"
#include "span.hh"
#include "widget.hh"
#include "window.hh"

using namespace std::literals;

namespace automat::gui {
std::unique_ptr<Action> PrototypeButton::ButtonDownAction(gui::Pointer& pointer,
                                                          gui::PointerButton btn) {
  if (btn != gui::kMouseLeft) {
    return nullptr;
  }
  auto drag_action = std::make_unique<DragObjectAction>(pointer, proto->Clone());
  drag_action->contact_point = pointer.PositionWithin(*this);
  return drag_action;
}
}  // namespace automat::gui

namespace automat::library {

std::unique_ptr<Object> Toolbar::Clone() const {
  auto new_toolbar = std::make_unique<Toolbar>();
  for (const auto& prototype : prototypes) {
    new_toolbar->AddObjectPrototype(prototype.get());
  }
  return new_toolbar;
}

SkPath Toolbar::Shape(animation::Display*) const {
  float width = CalculateWidth();
  float height = gui::kToolbarIconSize;
  auto rect = Rect(-width / 2, 0, width / 2, height);
  return SkPath::Rect(rect);
}

void Toolbar::Draw(gui::DrawContext& dctx) const {
  float width_targets[buttons.size()];
  for (size_t i = 0; i < buttons.size(); ++i) {
    width_targets[i] = gui::kToolbarIconSize;
  }

  auto my_transform = gui::TransformDown(dctx.path, &dctx.display);

  float width = CalculateWidth();
  for (auto* pointer : dctx.display.window->pointers) {
    float x = -width / 2;
    Vec2 pointer_position = my_transform.mapPoint(pointer->pointer_position);
    if (pointer_position.x < -width / 2 || pointer_position.x > width / 2) {
      continue;
    }
    if (pointer_position.y > gui::kToolbarIconSize) {
      continue;
    }
    for (int i = 0; i < buttons.size(); ++i) {
      if (x <= pointer_position.x && pointer_position.x <= x + buttons[i]->width) {
        width_targets[i] = gui::kToolbarIconSize * 2;
        break;
      }
      x += buttons[i]->width;
    }
  }

  for (int i = 0; i < buttons.size(); ++i) {
    // width.SpringTowards(width_target, ctx.DeltaT(), 0.3, 0.05);
    buttons[i]->width.SineTowards(width_targets[i], dctx.DeltaT(), 0.4);
  }

  auto my_shape = Shape(&dctx.display);
  SkPaint debug_paint;
  debug_paint.setStyle(SkPaint::kStroke_Style);
  dctx.canvas.drawPath(my_shape, debug_paint);
  DrawChildren(dctx);
}

ControlFlow Toolbar::VisitChildren(gui::Visitor& visitor) {
  Widget* arr[buttons.size()];
  for (size_t i = 0; i < buttons.size(); ++i) {
    arr[i] = buttons[i].get();
  }
  if (auto ret = visitor(maf::SpanOfArr(arr, buttons.size())); ret == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  return ControlFlow::Continue;
}

void Toolbar::AddObjectPrototype(const Object* new_proto) {
  prototypes.push_back(new_proto->Clone());
  buttons.emplace_back(std::make_unique<gui::PrototypeButton>(prototypes.back().get()));
}

SkMatrix Toolbar::TransformToChild(const Widget& child, animation::Display* display) const {
  float width = CalculateWidth();

  float x = -width / 2;

  for (int i = 0; i < buttons.size(); ++i) {
    if (buttons[i].get() == &child) {
      Rect src = prototypes[i]->CoarseBounds(display).rect;
      float size = buttons[i]->width;
      x += size / 2;
      Rect dst = Rect::MakeWH(size, size).MoveBy({x, size / 2});
      auto child_to_parent = SkMatrix::RectToRect(src, dst, SkMatrix::kCenter_ScaleToFit);
      SkMatrix parent_to_child;
      if (child_to_parent.invert(&parent_to_child)) {
        return parent_to_child;
      }
      return SkMatrix::I();
    }
    x += buttons[i]->width;
  }
  return SkMatrix::I();
}
float Toolbar::CalculateWidth() const {
  float width = 0;
  for (const auto& button : buttons) {
    width += button->width;
  }
  return width;
}
maf::StrView Toolbar::Name() const { return "Toolbar"sv; }
}  // namespace automat::library