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
#include "gui_connection_widget.hh"
#include "root.hh"
#include "tasks.hh"
#include "thread_name.hh"
#include "timer_thread.hh"
#include "window.hh"

using namespace std;
using namespace maf;

namespace automat {

Location* Machine::LocationAtPoint(Vec2 point) {
  for (auto& loc : locations) {
    Vec2 local_point = (point - loc->position) / loc->scale;
    SkPath shape;
    if (loc->object) {
      shape = loc->object->Shape(nullptr);
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
    auto dist2 = (loc->object ? Rect(loc->object->Shape().getBounds()) : Rect{})
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

const Machine Machine::proto;

int log_executed_tasks = 0;

LogTasksGuard::LogTasksGuard() { ++log_executed_tasks; }
LogTasksGuard::~LogTasksGuard() { --log_executed_tasks; }

std::deque<Task*> queue;
std::unordered_set<Location*> no_scheduling;
std::vector<Task*> global_successors;

channel events;

struct AutodeleteTaskWrapper : Task {
  std::unique_ptr<Task> wrapped;
  AutodeleteTaskWrapper(std::unique_ptr<Task>&& task)
      : Task(task->target), wrapped(std::move(task)) {}
  void Execute() override {
    wrapped->Execute();
    delete this;
  }
};

// Dummy task to wake up the Automat Thread to process the shutdown.
struct ShutdownTask : Task {
  ShutdownTask() : Task(nullptr) {}
  std::string Format() override { return "Shutdown"; }
  void Execute() override {}
};

void RunThread(std::stop_token stop_token) {
  StartTimeThread(stop_token);
  std::stop_callback wakeup_for_shutdown(stop_token,
                                         [] { events.try_send(std::make_unique<ShutdownTask>()); });

  SetThreadName("Automat Loop");
  while (!stop_token.stop_requested()) {
    RunLoop();
    std::unique_ptr<Task> task = events.recv<Task>();
    if (task) {
      auto* wrapper = new AutodeleteTaskWrapper(std::move(task));
      wrapper->Schedule();  // Will delete itself after executing.
    }
  }
  automat_thread_finished = true;
  automat_thread_finished.notify_all();
}

void RunLoop(const int max_iterations) {
  if (log_executed_tasks) {
    LOG << "RunLoop(" << queue.size() << " tasks)";
    LOG_Indent();
  }
  int iterations = 0;
  while (!queue.empty() && (max_iterations < 0 || iterations < max_iterations)) {
    Task* task = queue.front();
    queue.pop_front();
    task->scheduled = false;
    task->Execute();
    ++iterations;
  }
  if (log_executed_tasks) {
    LOG_Unindent();
  }
}
bool NoScheduling(Location* location) {
  return no_scheduling.find(location) != no_scheduling.end();
}

static void DoneRunning(Location& here) {
  if (!here.HasError()) {
    ScheduleNext(here);
  }
}

void LongRunning::Done(Location& here) {
  here.long_running = nullptr;
  DoneRunning(here);
}

void Runnable::Run(Location& here) {
  if (here.long_running) {
    return;
  }
  here.long_running = OnRun(here);
  if (here.long_running == nullptr) {
    DoneRunning(here);
  }
}

void Machine::SerializeState(Serializer& writer, const char* key) const {
  writer.Key(key);
  writer.StartObject();
  writer.Key("name");
  writer.String(name.data(), name.size());
  if (!locations.empty()) {
    writer.Key("locations");
    writer.StartArray();
    std::unordered_map<Location*, int> location_ids;
    for (auto& location : locations) {
      location_ids.emplace(location.get(), location_ids.size());
    }
    for (auto& location : locations) {
      writer.StartObject();
      if (!location->name.empty()) {
        writer.Key("name");
        writer.String(location->name.data(), location->name.size());
      }
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
          writer.Int(location_ids.at(&conn->to));
        }
        writer.EndObject();
      }
      writer.EndObject();
    }
    writer.EndArray();
  }
  writer.EndObject();
}
void Machine::DeserializeState(Location& l, Deserializer& d) {
  Status status;
  for (auto& key : ObjectView(d, status)) {
    if (key == "name") {
      d.Get(name, status);
    } else if (key == "locations") {
      Vec<Location*> location_idx;
      struct ConnectionRecord {
        Str label;
        int from;
        int to;
      };
      Vec<ConnectionRecord> connections;
      for (int i : ArrayView(d, status)) {
        LOG << "Deserializing location " << i;
        auto& l = CreateEmpty();
        location_idx.push_back(&l);
        for (auto& field : ObjectView(d, status)) {
          if (field == "name") {
            d.Get(l.name, status);
          } else if (field == "type") {
            Str type;
            d.Get(type, status);
            if (OK(status)) {
              const Object* proto = FindPrototype(type);
              if (proto == nullptr) {
                l.ReportError(f("Unknown object type: %s", type.c_str()));
                // try to continue parsing
              } else {
                l.Create(*proto);
              }
            }
          } else if (field == "value") {
            if (l.object) {
              l.object->DeserializeState(l, d);
            }
          } else if (field == "x") {
            d.Get(l.position.x, status);
          } else if (field == "y") {
            d.Get(l.position.y, status);
          } else if (field == "connections") {
            for (auto& connection_label : ObjectView(d, status)) {
              int target;
              d.Get(target, status);
              if (OK(status)) {
                connections.push_back({connection_label, i, target});
              }
            }
          }
        }
      }
      for (auto& connection_record : connections) {
        if (connection_record.from < 0 || connection_record.from >= location_idx.size()) {
          l.ReportError(f("Invalid connection source index: %d", connection_record.from));
          continue;
        } else if (connection_record.to < 0 || connection_record.to >= location_idx.size()) {
          l.ReportError(f("Invalid connection target index: %d", connection_record.to));
          continue;
        }
        Location* from = location_idx[connection_record.from];
        from->object->Args([&](Argument& arg) {
          if (arg.name != connection_record.label) {
            return;
          }
          from->ConnectTo(*location_idx[connection_record.to], arg);
        });
      }
    }
  }
  if (!OK(status)) {
    l.ReportError(status.ToStr());
  }
}

Machine::Machine() {}

ControlFlow Machine::VisitChildren(gui::Visitor& visitor) {
  int i = 0;
  Size n = locations.size();
  Widget* arr[n];
  for (auto& it : locations) {
    arr[i++] = it.get();
  }
  if (visitor(maf::SpanOfArr(arr, n)) == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  return ControlFlow::Continue;
}
SkMatrix Machine::TransformToChild(const Widget& child, animation::Display* display) const {
  if (const Location* l = dynamic_cast<const Location*>(&child)) {
    return l->GetTransform(display);
  } else if (const gui::ConnectionWidget* w = dynamic_cast<const gui::ConnectionWidget*>(&child)) {
    return SkMatrix::I();
  }
  return SkMatrix::I();
}

SkPath Machine::Shape(animation::Display* display) const {
  SkPath rect = SkPath::Rect(Rect::MakeWH(100_cm, 100_cm));
  if (display && display->window) {
    auto trash = display->window->TrashShape();
    SkPath rect_minus_trash;
    Op(rect, trash, kDifference_SkPathOp, &rect_minus_trash);
    return rect_minus_trash;
  }
  return rect;
}

SkPaint& GetBackgroundPaint(float px_per_m) {
  static SkRuntimeShaderBuilder builder = []() {
    const char* sksl = R"(
        uniform float px_per_m;

        // Dark theme
        //float4 bg = float4(0.05, 0.05, 0.00, 1);
        //float4 fg = float4(0.0, 0.32, 0.8, 1);

        float4 bg = float4(0.9, 0.9, 0.9, 1);
        float4 fg = float4(0.5, 0.5, 0.5, 1);

        float grid(vec2 coord_m, float dots_per_m, float r_px) {
          float r = r_px / px_per_m;
          vec2 grid_coord = fract(coord_m * dots_per_m + 0.5) - 0.5;
          return smoothstep(r, r - 1/px_per_m, length(grid_coord) / dots_per_m) * smoothstep(1./(3*r), 1./(32*r), dots_per_m);
        }

        half4 main(vec2 fragcoord) {
          float dm_grid = grid(fragcoord, 10, 2);
          float cm_grid = grid(fragcoord, 100, 2) * 0.8;
          float mm_grid = grid(fragcoord, 1000, 1) * 0.8;
          float d = max(max(mm_grid, cm_grid), dm_grid);
          return mix(bg, fg, d);
        }
      )";

    auto [effect, err] = SkRuntimeEffect::MakeForShader(SkString(sksl));
    if (!err.isEmpty()) {
      FATAL << err.c_str();
    }
    SkRuntimeShaderBuilder builder(effect);
    return builder;
  }();
  static SkPaint paint;
  builder.uniform("px_per_m") = px_per_m;
  paint.setShader(builder.makeShader());
  return paint;
}

void Machine::PreDraw(gui::DrawContext& ctx) const {
  auto& canvas = ctx.canvas;
  auto shape = Shape(&ctx.display);
  float px_per_m = ctx.canvas.getLocalToDeviceAs3x3().mapRadius(1);
  SkPaint background_paint = GetBackgroundPaint(px_per_m);
  canvas.drawPath(shape, background_paint);
  SkPaint border_paint;
  border_paint.setColor("#404040"_color);
  border_paint.setStyle(SkPaint::kStroke_Style);
  canvas.drawPath(shape, border_paint);
  PreDrawChildren(ctx);
}

void Machine::SnapPosition(Vec2& position, float& scale, Object* object, Vec2* fixed_point) {
  scale = 1.0;
  Rect rect = object->Shape(nullptr).getBounds();
  if (position.x + rect.left < -0.5) {
    position.x = -rect.left - 0.5;
  }
  if (position.x + rect.right > 0.5) {
    position.x = -rect.right + 0.5;
  }
  if (position.y + rect.bottom < -0.5) {
    position.y = -rect.bottom - 0.5;
  }
  if (position.y + rect.top > 0.5) {
    position.y = -rect.top + 0.5;
  }
  position = Vec2(roundf(position.x * 1000) / 1000., roundf(position.y * 1000) / 1000.);
}

void Machine::DropLocation(std::unique_ptr<Location>&& l) {
  l->parent = here;
  locations.insert(locations.begin(), std::move(l));
  InvalidateDrawCache();
}

std::unique_ptr<Location> Machine::Extract(Location& location) {
  auto it =
      std::find_if(locations.begin(), locations.end(),
                   [&location](const unique_ptr<Location>& l) { return l.get() == &location; });
  if (it != locations.end()) {
    std::unique_ptr<Location> result = std::move(*it);
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
    return result;
  } else {
    return nullptr;
  }
}
}  // namespace automat