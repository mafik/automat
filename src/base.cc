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
#include "global_resources.hh"
#include "location.hh"
#include "math.hh"
#include "root_widget.hh"
#include "tasks.hh"
#include "textures.hh"
#include "timer_thread.hh"
#include "ui_connection_widget.hh"
#include "widget.hh"

using namespace std;

namespace automat {

Location* Machine::LocationAtPoint(Vec2 point) {
  for (auto& loc : locations) {
    Vec2 local_point = (point - loc->position) / loc->scale;
    SkPath shape;
    if (loc->object) {
      shape = loc->WidgetForObject().Shape();
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
    auto dist2 = (loc->object ? loc->WidgetForObject().CoarseBounds().rect : Rect{})
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

static void DoneRunning(Location& here, RunTask& run_task) {
  if (run_task.schedule_next && !here.HasError()) {
    ScheduleNext(here);
  }
}

void LongRunning::Done(Location& here) {
  if (long_running_task == nullptr) {
    FATAL << "LongRunning::Done called without a long_running_task";
  }
  DoneRunning(here, *long_running_task);
  long_running_task = nullptr;
}

void LongRunning::BeginLongRunning(Location& here, RunTask& run_task) {
  this->long_running_task = &run_task;
}

void LongRunning::On() {
  LOG << "Calling LongRunning::On";
  auto live_object = dynamic_cast<LiveObject*>(this);
  if (live_object == nullptr) {
    ERROR << "LongRunning::On called on a non-LiveObject";
    return;
  }
  auto location = live_object->here.Lock();
  location->ScheduleRun();
}

void Runnable::Run(Location& here, RunTask& run_task) {
  LongRunning* long_running = here.object->AsLongRunning();
  if (long_running && long_running->IsRunning()) {
    return;
  }
  OnRun(here, run_task);
  if (long_running && long_running->IsRunning()) {
    return;
  }
  DoneRunning(here, run_task);
}

void Machine::SerializeState(Serializer& writer, const char* key) const {
  writer.Key(key);
  writer.StartObject();
  writer.Key("name");
  writer.String(name.data(), name.size());
  if (!locations.empty()) {
    writer.Key("locations");
    writer.StartObject();

    // Pick unique names for locations.
    std::unordered_set<Str> assigned_names;
    std::unordered_map<Location*, Str> location_ids;
    for (auto& location : locations) {
      auto base_name = Str(location->object->Name());
      auto name = base_name;
      int i = 2;
      while (assigned_names.count(name)) {
        name = f("{} #{}", base_name, i++);
      }
      location_ids.emplace(location.get(), name);
      assigned_names.insert(name);
    }

    // Serialize the locations.
    for (auto& location : locations) {
      auto& name = location_ids.at(location.get());
      writer.Key(name.data(), name.size());
      writer.StartObject();
      if (location->object) {
        writer.Key("type");
        auto type = location->object->Name();
        writer.String(type.data(), type.size());
        location->object->SerializeState(writer, "value");
      }
      writer.Key("x");
      writer.Double(round(location->position.x * 1000000.) /
                    1000000.);  // round to 6 decimal places
      writer.Key("y");
      writer.Double(round(location->position.y * 1000000.) / 1000000.);
      if (!location->outgoing.empty()) {
        writer.Key("connections");
        writer.StartObject();
        for (auto* conn : location->outgoing) {
          writer.Key(conn->argument.name.data(), conn->argument.name.size());
          auto& to_name = location_ids.at(&conn->to);
          writer.String(to_name.data(), to_name.size());
        }
        writer.EndObject();
      }
      writer.EndObject();
    }
    writer.EndObject();
  }
  writer.EndObject();
}

void Machine::DeserializeState(Location& l, Deserializer& d) {
  Status status;
  for (auto& key : ObjectView(d, status)) {
    if (key == "name") {
      d.Get(name, status);
    } else if (key == "locations") {
      std::unordered_map<Str, Location*> location_idx;
      struct ConnectionRecord {
        Str label;
        Location* from;
        Str to_name;
      };
      Vec<ConnectionRecord> connections;
      for (auto& location_name : ObjectView(d, status)) {
        auto& l = CreateEmpty();

        {  // Place the new location below all the others.
          // Normally new locations are created at the top of all the others (front of the locations
          // deque). We actually want to recreate the same order as the original save file.
          locations.emplace_back(std::move(locations.front()));
          locations.pop_front();
        }

        location_idx.emplace(location_name, &l);
        Ptr<Object> object;
        for (auto& field : ObjectView(d, status)) {
          if (field == "type") {
            Str type;
            d.Get(type, status);
            if (OK(status)) {
              auto proto = prototypes->Find(type);
              if (proto == nullptr) {
                l.ReportError(f("Unknown object type: {}", type));
                // try to continue parsing
              } else {
                object = proto->Clone();
                if (auto widget = dynamic_cast<Widget*>(object.get())) {
                  widget->parent = &l;
                }
              }
            }
          } else if (field == "value") {
            if (object) {
              object->DeserializeState(l, d);
            }
          } else if (field == "x") {
            d.Get(l.position.x, status);
          } else if (field == "y") {
            d.Get(l.position.y, status);
          } else if (field == "connections") {
            for (auto& connection_label : ObjectView(d, status)) {
              Str to_name;
              d.Get(to_name, status);
              if (OK(status)) {
                connections.push_back({connection_label, &l, to_name});
              }
            }
          }
        }
        // Objects are inserted into the location only after their state has been deserialized -
        // this allows
        if (object) {
          l.InsertHere(std::move(object));
        }
      }
      for (auto& connection_record : connections) {
        auto to_it = location_idx.find(connection_record.to_name);
        if (to_it == location_idx.end()) {
          l.ReportError(f("Missing connection target: {}", connection_record.to_name));
          continue;
        }
        Location* from = connection_record.from;
        Location* to = to_it->second;
        from->object->Args([&](Argument& arg) {
          if (arg.name != connection_record.label) {
            return;
          }
          from->ConnectTo(*to, arg);
        });
      }
    }
  }
  if (!OK(status)) {
    l.ReportError(status.ToStr());
  }
  // Objects may have been rendered in their incomplete state - re-render them all.
  for (auto& loc : locations) {
    loc->WakeAnimation();
  }
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

SkPaint& GetBackgroundPaint(float px_per_m) {
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
  locations.front()->object->ForEachWidget(
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
    for (int i = 0; i < children_with_errors.size(); ++i) {
      for (auto& r : result) {
        if (children_with_errors[i] == r.get()) {
          children_with_errors.erase(children_with_errors.begin() + i);
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
    for (int i = 0; i < children_with_errors.size(); ++i) {
      if (children_with_errors[i] == result.get()) {
        children_with_errors.erase(children_with_errors.begin() + i);
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

void LiveObject::Relocate(Location* new_here) {
  Args([old_here = here, new_here](Argument& arg) {
    if (auto live_arg = dynamic_cast<LiveArgument*>(&arg)) {
      live_arg->Relocate(old_here.lock().get(), new_here);
    }
  });
  here = new_here;
}

Location& Machine::CreateEmpty() {
  auto& it = locations.emplace_front(new Location(this, here));
  Location* h = it.get();
  return *h;
}
void Machine::Relocate(Location* parent) {
  LiveObject::Relocate(parent);
  for (auto& it : locations) {
    it->parent_location = here;
    it->parent = this;
  }
}
}  // namespace automat
