// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "base.hh"

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMetrics.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPathEffect.h>
#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/pathops/SkPathOps.h>

#include "drag_action.hh"
#include "embedded.hh"
#include "format.hh"
#include "global_resources.hh"
#include "location.hh"
// #include "log.hh"
#include "math.hh"
#include "root_widget.hh"
#include "tasks.hh"
#include "textures.hh"
#include "timer_thread.hh"
#include "ui_connection_widget.hh"
#include "widget.hh"

using namespace std;

namespace automat {

void Machine::ConnectAtPoint(Object& start, Argument& arg, Vec2 point) {
  bool connected = false;
  auto TryConnect = [&](Object& end, Part& part) {
    if (connected) return;
    if (arg.CanConnect(start, part)) {
      arg.Connect(start, NestedPtr<Part>(end.AcquirePtr(), &part));
      connected = true;
    }
  };
  for (auto& loc : locations) {
    Vec2 local_point = (point - loc->position) / loc->scale;
    SkPath shape = loc->ToyForObject().Shape();
    if (!shape.contains(local_point.x, local_point.y)) {
      continue;
    }
    auto& obj = *loc->object;
    TryConnect(obj, obj);
    if (connected) return;
    obj.Parts([&](Part& part) { TryConnect(obj, part); });
  }
}

Location* Machine::LocationAtPoint(Vec2 point) {
  for (auto& loc : locations) {
    Vec2 local_point = (point - loc->position) / loc->scale;
    SkPath shape;
    if (loc->object) {
      shape = loc->ToyForObject().Shape();
    }
    if (shape.contains(local_point.x, local_point.y)) {
      return loc.get();
    }
  }
  return nullptr;
}

void* Machine::Nearby(Vec2 start, float radius, std::function<void*(Location&)> callback) {
  float radius2 = radius * radius;
  for (auto& loc : locations) {
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

void Machine::NearbyCandidates(Location& here, const Argument& arg, float radius,
                               std::function<void(Location&, Vec<Vec2AndDir>&)> callback) {
  // Check the currently dragged object
  auto& root_widget = here.FindRootWidget();
  for (auto* action : root_widget.active_actions) {
    if (auto* drag_location_action = dynamic_cast<DragLocationAction*>(action)) {
      for (auto& location : drag_location_action->locations) {
        if (location.get() == &here) {
          continue;
        }
        if (!arg.CanConnect(*here.object, *location->object)) {
          continue;
        }
        Vec<Vec2AndDir> to_points;
        location->ToyForObject().ConnectionPositions(to_points);
        callback(*location, to_points);
      }
    }
  }
  // Query nearby objects in the machine
  Vec2 center = here.ToyForObject().ArgStart(arg, this).pos;
  Nearby(center, radius, [&](Location& other) -> void* {
    if (&other == &here) {
      return nullptr;
    }
    if (!arg.CanConnect(*here.object, *other.object)) {
      return nullptr;
    }
    Vec<Vec2AndDir> to_points;
    other.ToyForObject().ConnectionPositions(to_points);
    callback(other, to_points);
    return nullptr;
  });
}

void LongRunning::Done(Location& here) {
  if (long_running_task == nullptr) {
    FATAL << "LongRunning::Done called while long_running_task == null.";
  }
  long_running_task->DoneRunning(*here.object);
  long_running_task.reset();
  NotifyTurnedOff();
}

void LongRunning::OnTurnOn() {
  auto object = OnFindRunnable();
  if (object == nullptr) {
    ERROR << "LongRunning::OnFindRunnable didn't return any Object!";
    return;
  }
  object->here->ScheduleRun();
}

void Machine::SerializeState(ObjectSerializer& writer) const {
  writer.Key("name");
  writer.String(name);
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

bool Machine::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  if (key == "name") {
    d.Get(name, status);
  } else if (key == "locations") {
    for (auto& object_name : ObjectView(d, status)) {
      auto* object = d.LookupObject(object_name);

      auto& loc = CreateEmpty();
      {  // Place the new location below all the others.
        locations.emplace_back(std::move(locations.front()));
        locations.pop_front();
      }
      object->here = &loc;
      if (auto widget = dynamic_cast<ui::Widget*>(object)) {
        widget->parent = &loc;
      }
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

Machine::Machine(ui::Widget* parent) : ui::Widget(parent) {}

void Machine::FillChildren(Vec<Widget*>& children) {
  int i = 0;
  Size n = locations.size();
  children.reserve(n);
  for (auto& l : locations) {
    children.push_back(l.get());
  }
}

SkPath Machine::Shape() const {
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

void Machine::PreDraw(SkCanvas& canvas) const {
  auto shape = Shape();
  float px_per_m = canvas.getLocalToDeviceAs3x3().mapRadius(1);
  SkPaint background_paint = GetBackgroundPaint(px_per_m);
  canvas.drawPath(shape, background_paint);
  SkPaint border_paint;
  border_paint.setColor("#404040"_color);
  border_paint.setStyle(SkPaint::kStroke_Style);
  canvas.drawPath(shape, border_paint);
}

SkMatrix Machine::DropSnap(const Rect& rect_ref, Vec2 bounds_origin, Vec2* fixed_point) {
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

void Machine::DropLocation(Ptr<Location>&& l) {
  l->parent = this;
  l->parent_location = here;
  locations.insert(locations.begin(), std::move(l));
  audio::Play(embedded::assets_SFX_canvas_drop_wav);
  locations.front()->object->ForEachToy(
      [](ui::RootWidget&, ui::Widget& w) { w.RedrawThisFrame(); });
}

Vec<Ptr<Location>> Machine::ExtractStack(Location& base) {
  auto base_it = std::find_if(locations.begin(), locations.end(),
                              [&base](const Ptr<Location>& l) { return l.get() == &base; });
  if (base_it != locations.end()) {
    int base_index = std::distance(locations.begin(), base_it);
    SkPath base_shape = base.GetShapeRecursive();
    base_shape.transform(ui::TransformUp(base));  // top-level coordinates

    Vec<Ptr<Location>> result;
    result.push_back(std::move(*base_it));
    locations.erase(base_it);

    // Move all the objects that are on top of the `base_shape`.
    for (int atop_index = base_index - 1; atop_index >= 0; --atop_index) {
      Location& atop = *locations[atop_index];
      SkPath atop_shape = atop.GetShapeRecursive();
      atop_shape.transform(ui::TransformUp(atop));  // top-level coordinates
      SkPath intersection;
      bool op_success = Op(atop_shape, base_shape, kIntersect_SkPathOp, &intersection);
      if (op_success && intersection.countVerbs() > 0) {
        result.insert(result.begin(), std::move(locations[atop_index]));
        locations.erase(locations.begin() + atop_index);
        // Expand the base shape to include the atop shape.
        Op(base_shape, atop_shape, kUnion_SkPathOp, &base_shape);
      }
    }

    for (int i = 0; i < front.size(); ++i) {
      for (auto& r : result) {
        if (front[i] == r.get()) {
          front.erase(front.begin() + i);
        }
      }
    }
    WakeAnimation();
    audio::Play(embedded::assets_SFX_canvas_pick_wav);
    return result;
  } else {
    return {};
  }
}

Ptr<Location> Machine::Extract(Location& location) {
  auto it = std::find_if(locations.begin(), locations.end(),
                         [&location](const auto& l) { return l.get() == &location; });
  if (it != locations.end()) {
    auto result = std::move(*it);
    locations.erase(it);
    for (int i = 0; i < front.size(); ++i) {
      if (front[i] == result.get()) {
        front.erase(front.begin() + i);
        break;
      }
    }
    WakeAnimation();
    audio::Play(embedded::assets_SFX_canvas_pick_wav);
    return result;
  } else {
    return nullptr;
  }
}

Location& Machine::CreateEmpty() {
  auto& it = locations.emplace_front(new Location(this, here));
  Location* h = it.get();
  return *h;
}
void Machine::Relocate(Location* parent) {
  Object::Relocate(parent);
  for (auto& it : locations) {
    it->parent_location = here;
    it->parent = this;
  }
}
}  // namespace automat
