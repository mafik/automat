// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "board.hh"

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/pathops/SkPathOps.h>

#include "control_flow.hh"
#include "drag_action.hh"
#include "embedded.hh"
#include "global_resources.hh"
#include "location.hh"
#include "math.hh"
#include "root_widget.hh"
#include "textures.hh"
#include "ui_connection_widget.hh"

using namespace std;

namespace automat {

Board::Board() {}

std::unique_ptr<ObjectToy> Board::MakeToy(ui::Widget* parent) {
  return std::make_unique<BoardWidget>(parent, *this);
}

void Board::SerializeState(ObjectSerializer& writer) const {
  if (!locations.empty()) {
    writer.Key("locations");
    writer.StartObject();

    // Serialize the locations.
    for (auto& location : locations) {
      auto& name = writer.ResolveName(*location->object);
      writer.Key(name);
      writer.StartArray();
      writer.Double(round(location->position.x * 1000000.) /
                    1000000.);  // round to 6 decimal places
      writer.Double(round(location->position.y * 1000000.) / 1000000.);
      writer.EndArray();
    }
    writer.EndObject();
  }
}

bool Board::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  if (key == "locations") {
    for (auto& object_name : ObjectView(d, status)) {
      auto* object = d.LookupObject(object_name);

      auto& loc = CreateEmpty();
      {  // Place the new location below all the others.
        locations.emplace_back(std::move(locations.front()));
        locations.pop_front();
      }
      object->here = &loc;
      loc.InsertHere(object->AcquirePtr());

      // Read the [x, y] position array
      for (auto i : ArrayView(d, status)) {
        if (i == 0) {
          d.Get(loc.position.x, status);
        } else if (i == 1) {
          d.Get(loc.position.y, status);
        } else {
          d.Skip();
        }
      }
    }
  } else {
    return false;
  }
  if (!OK(status)) {
    ReportError(status.ToStr());
  }
  return true;
}

Ptr<Location> Board::Extract(Location& location) {
  auto it = std::find_if(locations.begin(), locations.end(),
                         [&location](const auto& l) { return l.get() == &location; });
  if (it != locations.end()) {
    auto result = std::move(*it);
    locations.erase(it);
    WakeToys();
    return result;
  }
  return nullptr;
}

Location& Board::CreateEmpty() {
  auto& it = locations.emplace_front(new Location(here));
  Location* h = it.get();
  return *h;
}

void Board::Relocate(Location* parent) {
  Object::Relocate(parent);
  for (auto& it : locations) {
    it->parent_location = here;
  }
}

BoardWidget::BoardWidget(ui::Widget* parent, Board& board) : ObjectToy(parent, board) {}

SkPath BoardWidget::Shape() const {
  SkPath rect = SkPath::Rect(Rect::MakeCenterZero(100_cm, 100_cm));
  auto& root = FindRootWidget();
  auto trash = root.TrashShape();
  SkPath rect_minus_trash;
  Op(rect, trash, kDifference_SkPathOp, &rect_minus_trash);
  return rect_minus_trash;
}

// Turns the background green - to make it easier to isolate elements of Automat in screenshots.
constexpr bool kGreenScreen = false;

SkPaint& GetBackgroundPaint(float px_per_m) {
  if constexpr (kGreenScreen) {
    static SkPaint paint;
    paint.setColor(SK_ColorGREEN);
    return paint;
  }
  static PersistentImage bg =
      PersistentImage::MakeFromAsset(embedded::assets_bg_webp, PersistentImage::MakeArgs{
                                                                   .height = 100_cm,
                                                               });
  Status status;
  static auto shader = resources::CompileShader(embedded::assets_bg_sksl, status);
  if (!OK(status)) {
    ERROR << status;
    return bg.paint;
  }
  static SkPaint paint;
  SkRuntimeEffectBuilder builder(shader);
  builder.uniform("px_per_m") = px_per_m;
  builder.uniform("background_px") = (float)bg.heightPx();
  builder.child("background_image") = *bg.shader;
  const int kThumbSize = 64;
  auto thumb_info = (*bg.image)->imageInfo().makeWH(kThumbSize, kThumbSize);
  static auto thumb_image = (*bg.image)->makeScaled(thumb_info, kDefaultSamplingOptions);
  static auto thumb_shader = thumb_image->makeShader(
      kDefaultSamplingOptions,
      SkMatrix::Scale(1. / kThumbSize, -1. / kThumbSize).postTranslate(0, 1));
  builder.child("background_thumb") = thumb_shader;
  paint.setShader(builder.makeShader());
  return paint;
}

void BoardWidget::Draw(SkCanvas& canvas) const {
  auto shape = Shape();
  float px_per_m = canvas.getLocalToDeviceAs3x3().mapRadius(1);
  SkPaint background_paint = GetBackgroundPaint(px_per_m);
  canvas.drawPath(shape, background_paint);
  SkPaint border_paint;
  border_paint.setColor("#404040"_color);
  border_paint.setStyle(SkPaint::kStroke_Style);
  canvas.drawPath(shape, border_paint);
  DrawChildren(canvas);
}

SkMatrix BoardWidget::DropSnap(const Rect& rect_ref, Vec2 bounds_origin, Vec2* fixed_point) {
  Rect rect = rect_ref;
  SkMatrix matrix;
  Vec2 grid_snap = RoundToMilimeters(bounds_origin) - bounds_origin;
  matrix.postTranslate(grid_snap.x, grid_snap.y);
  matrix.mapRectScaleTranslate(&rect.sk, rect.sk);
  if (rect.left < -50_cm) {
    matrix.postTranslate(-rect.left - 50_cm, 0);
  }
  if (rect.right > 50_cm) {
    matrix.postTranslate(50_cm - rect.right, 0);
  }
  if (rect.bottom < -50_cm) {
    matrix.postTranslate(0, -rect.bottom - 50_cm);
  }
  if (rect.top > 50_cm) {
    matrix.postTranslate(0, 50_cm - rect.top);
  }
  return matrix;
}

void BoardWidget::DropLocation(Ptr<Location>&& l) {
  auto board = LockBoard();
  if (!board) return;
  l->parent_location = board->here;
  board->locations.insert(board->locations.begin(), std::move(l));
  audio::Play(embedded::assets_SFX_canvas_drop_wav);
  Location& dropped = *board->locations.front();
  dropped.object->ForEachToy([](ui::RootWidget&, automat::Toy& w) { w.RedrawThisFrame(); });
  // Walk over connections that start/end in the dropped location.
  // If the other end of the connection is obscured by another location, raise that obscurer
  // (and its whole stack) to the front.
  auto& root = FindRootWidget();
  for (auto& [key, toy] : root.toys.container) {
    auto* conn = dynamic_cast<ui::ConnectionWidget*>(toy.get());
    if (!conn) continue;
    Location* start_loc = conn->StartLocation();
    Location* end_loc = conn->EndLocation();
    Location* other = nullptr;
    if (start_loc == &dropped) {
      other = end_loc;
    } else if (end_loc == &dropped) {
      other = start_loc;
    }
    if (!other) continue;
    auto other_it = std::find_if(board->locations.begin(), board->locations.end(),
                                 [&](const Ptr<Location>& loc) { return loc.get() == other; });
    if (other_it == board->locations.end()) continue;
    int other_index = std::distance(board->locations.begin(), other_it);
    SkPath other_shape = other->widget->ShapeRigid();
    // Check if any location above `other` obscures it.
    for (int i = other_index - 1; i >= 0; --i) {
      Location& above = *board->locations[i];
      if (&above == &dropped) continue;
      SkPath above_shape = above.widget->ShapeRigid();
      SkPath intersection;
      if (Op(above_shape, other_shape, kIntersect_SkPathOp, &intersection) &&
          intersection.countVerbs() > 0) {
        RaiseStack(above);
        break;
      }
    }
  }
}

void BoardWidget::ConnectAtPoint(Object& start, Argument& arg, Vec2 point) {
  auto board = LockBoard();
  if (!board) return;
  bool connected = false;
  auto TryConnect = [&](Object& end_obj, Interface& end_iface) {
    if (connected) return;
    if (arg.CanConnect(start, end_obj, end_iface)) {
      arg.Connect(start, end_obj, end_iface);
      connected = true;
    }
  };
  for (auto& loc : board->locations) {
    Vec2 local_point = (point - loc->position) / loc->scale;
    SkPath shape = loc->ToyForObject().Shape();
    if (!shape.contains(local_point.x, local_point.y)) {
      continue;
    }
    auto& obj = *loc->object;
    TryConnect(obj, Object::toplevel_interface);
    if (connected) return;
    obj.Interfaces([&](Interface& iface) {
      TryConnect(obj, iface);
      if (connected) {
        return LoopControl::Break;
      }
      return LoopControl::Continue;
    });
  }
}

void* BoardWidget::Nearby(Vec2 start, float radius, std::function<void*(Location&)> callback) {
  auto board = LockBoard();
  if (!board) return nullptr;
  float radius2 = radius * radius;
  for (auto& loc : board->locations) {
    auto dist2 = (loc->object ? loc->ToyForObject().CoarseBounds().rect : Rect{})
                     .MoveBy(loc->position)
                     .DistanceSquared(start);
    if (dist2 > radius2) {
      continue;
    }
    if (auto ret = callback(*loc)) {
      return ret;
    }
  }
  return nullptr;
}

void BoardWidget::NearbyCandidates(
    Location& here, const Argument& arg, float radius,
    std::function<void(ObjectToy&, Interface&, Vec<Vec2AndDir>&)> callback) {
  // Check the currently dragged object
  auto& root_widget = *ui::root_widget;
  for (auto* action : root_widget.active_actions) {
    if (auto* drag_location_action = dynamic_cast<DragLocationAction*>(action)) {
      for (auto& location : drag_location_action->locations) {
        if (location.get() == &here) {
          continue;
        }
        Interface* iface = arg.CanConnect(*here.object, *location->object);
        if (!iface) {
          continue;
        }
        auto& toy = location->ToyForObject();
        Vec<Vec2AndDir> to_points;
        toy.ConnectionPositions(to_points);
        callback(toy, *iface, to_points);
      }
    }
  }
  // Query nearby objects in the board
  Vec2 center = here.ToyForObject().ArgStart(arg, this).pos;
  Nearby(center, radius, [&](Location& other) -> void* {
    if (&other == &here) {
      return nullptr;
    }
    Interface* iface = arg.CanConnect(*here.object, *other.object);
    if (!iface) {
      return nullptr;
    }
    auto& toy = other.ToyForObject();
    Vec<Vec2AndDir> to_points;
    toy.ConnectionPositions(to_points);
    callback(toy, *iface, to_points);
    return nullptr;
  });
}

void BoardWidget::ForStack(Location& base, std::function<void(Location&, int index)> callback) {
  auto board = LockBoard();
  if (!board) return;
  auto base_it = std::find_if(board->locations.begin(), board->locations.end(),
                              [&base](const Ptr<Location>& l) { return l.get() == &base; });
  if (base_it == board->locations.end()) return;
  int base_index = std::distance(board->locations.begin(), base_it);
  SkPath base_shape = base.widget->ShapeRigid();
  callback(base, base_index);
  for (int atop_index = base_index - 1; atop_index >= 0; --atop_index) {
    Location& atop = *board->locations[atop_index];
    SkPath atop_shape = atop.widget->ShapeRigid();
    SkPath intersection;
    if (Op(atop_shape, base_shape, kIntersect_SkPathOp, &intersection) &&
        intersection.countVerbs() > 0) {
      callback(atop, atop_index);
      Op(base_shape, atop_shape, kUnion_SkPathOp, &base_shape);
    }
  }
}

SkPath BoardWidget::StackShape(Location& base) {
  SkPath stack_shape;
  ForStack(base, [&](Location& loc, int) {
    if (&loc != &base) {
      Op(stack_shape, loc.widget->ShapeRecursive(), kUnion_SkPathOp, &stack_shape);
    }
  });
  return stack_shape;
}

Vec<Ptr<Location>> BoardWidget::ExtractStack(Location& base) {
  auto board = LockBoard();
  if (!board) return {};
  Vec<int> stack_indices;
  ForStack(base, [&](Location&, int index) { stack_indices.push_back(index); });
  if (stack_indices.empty()) return {};

  Vec<Ptr<Location>> result;
  // Indices are in decreasing order (base is always first/highest), so erasing in order is safe.
  for (int idx : stack_indices) {
    result.insert(result.begin(), std::move(board->locations[idx]));
    board->locations.erase(board->locations.begin() + idx);
  }

  WakeAnimation();
  audio::Play(embedded::assets_SFX_canvas_pick_wav);
  return result;
}

void BoardWidget::RaiseStack(Location& base) {
  auto board = LockBoard();
  if (!board) return;
  Vec<int> stack_indices;
  ForStack(base, [&](Location&, int index) { stack_indices.push_back(index); });
  if (stack_indices.empty()) return;

  Vec<Ptr<Location>> stack;
  // Indices are in decreasing order (highest first), so erasing in order is safe.
  for (int idx : stack_indices) {
    stack.insert(stack.begin(), std::move(board->locations[idx]));
    board->locations.erase(board->locations.begin() + idx);
  }

  // Re-insert at the front (top of Z-order).
  for (int i = stack.size() - 1; i >= 0; --i) {
    board->locations.insert(board->locations.begin(), std::move(stack[i]));
  }
}

}  // namespace automat
