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

#include "gui_connection_widget.hh"
#include "library_timer.hh"
#include "tasks.hh"
#include "thread_name.hh"

namespace automat {

Location* Machine::LocationAtPoint(Vec2 point) {
  for (auto& loc : locations) {
    Vec2 local_point = point - loc->position;
    SkPath shape;
    if (loc->object) {
      shape = loc->object->Shape();
    }
    if (shape.contains(local_point.x, local_point.y)) {
      return loc.get();
    }
  }
  return nullptr;
}

void Machine::UpdateConnectionWidgets() const {
  for (auto& loc : locations) {
    if (loc->object) {
      loc->object->Args([&](Argument& arg) {
        // Check if this argument already has a widget.
        bool has_widget = false;
        for (auto& widget : connection_widgets) {
          if (&widget->from != loc.get()) {
            continue;
          }
          if (&widget->arg != &arg) {
            continue;
          }
          has_widget = true;
        }
        if (has_widget) {
          return;
        }
        // Create a new widget.
        LOG << "Creating a ConnectionWidget for argument " << arg.name;
        connection_widgets.emplace_back(new gui::ConnectionWidget(*loc, arg));
      });
    }
  }
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
  library::StartTimerHelperThread(stop_token);
  std::stop_callback wakeup_for_shutdown(stop_token,
                                         [] { events.send(std::make_unique<ShutdownTask>()); });

  SetThreadName("Automat Loop");
  while (!stop_token.stop_requested()) {
    RunLoop();
    std::unique_ptr<Task> task = events.recv<Task>();
    if (task) {
      auto* wrapper = new AutodeleteTaskWrapper(std::move(task));
      wrapper->Schedule();  // Will delete itself after executing.
    }
  }
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

void Runnable::RunAndScheduleNext(Location& here) {
  Run(here);
  if (!here.HasError()) {
    ScheduleNext(here);
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
        writer.Key("value");
        auto value = location->object->GetText();
        writer.String(value.data(), value.size());
      }
      writer.Key("x");
      writer.Double(location->position.x);
      writer.Key("y");
      writer.Double(location->position.y);
      if (!location->outgoing.empty()) {
        writer.Key("connections");
        writer.StartObject();
        for (auto& [key, conn] : location->outgoing) {
          writer.Key(key.data(), key.size());
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
  Status list_fields_status;
  for (auto& key : ObjectView(d, list_fields_status)) {
    if (key == "name") {
      Status get_name_status;
      name = d.GetString(get_name_status);
      if (!OK(get_name_status)) {
        l.ReportError(get_name_status.ToStr());
        // NOTE: no return here because we try to continue parsing
      }
    } else if (key == "locations") {
      Status array_status;
      Vec<Location*> location_idx;
      struct ConnectionRecord {
        Str label;
        int from;
        int to;
      };
      Vec<ConnectionRecord> connections;
      for (int i : ArrayView(d, array_status)) {
        LOG << "Deserializing location " << i;
        auto& l = CreateEmpty();
        location_idx.push_back(&l);
        Status list_location_fields_status;
        for (auto& field : ObjectView(d, list_location_fields_status)) {
          if (field == "name") {
            Status get_name_status;
            l.name = d.GetString(get_name_status);
            if (!OK(get_name_status)) {
              l.ReportError(get_name_status.ToStr());
              // try to continue parsing
            }
          } else if (field == "type") {
            Status get_type_status;
            Str type = d.GetString(get_type_status);
            if (!OK(get_type_status)) {
              l.ReportError(get_type_status.ToStr());
              // try to continue parsing
            } else {
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
            Status get_x_status;
            l.position.x = d.GetDouble(get_x_status);
            if (!OK(get_x_status)) {
              l.ReportError(get_x_status.ToStr());
              // try to continue parsing
            }
          } else if (field == "y") {
            Status get_y_status;
            l.position.y = d.GetDouble(get_y_status);
            if (!OK(get_y_status)) {
              l.ReportError(get_y_status.ToStr());
              // try to continue parsing
            }
          } else if (field == "connections") {
            Status list_connections_status;
            for (auto& connection_label : ObjectView(d, list_connections_status)) {
              Status get_target_status;
              int target = d.GetInt(get_target_status);
              if (!OK(get_target_status)) {
                l.ReportError(get_target_status.ToStr());
                // try to continue parsing
              } else {
                connections.push_back({connection_label, i, target});
              }
            }
            if (!OK(list_connections_status)) {
              l.ReportError(list_connections_status.ToStr());
              // try to continue parsing
            }
          }
        }
        if (!OK(list_location_fields_status)) {
          l.ReportError(list_location_fields_status.ToStr());
          // try to continue parsing
        }
      }
      if (!OK(array_status)) {
        l.ReportError(array_status.ToStr());
        // try to continue parsing
      }
      for (auto& connection_record : connections) {
        if (connection_record.from < 0 || connection_record.from >= location_idx.size()) {
          l.ReportError(f("Invalid connection source index: %d", connection_record.from));
          continue;
        } else if (connection_record.to < 0 || connection_record.to >= location_idx.size()) {
          l.ReportError(f("Invalid connection target index: %d", connection_record.to));
          continue;
        }
        location_idx[connection_record.from]->ConnectTo(*location_idx[connection_record.to],
                                                        connection_record.label);
      }
    }
  }
  if (!OK(list_fields_status)) {
    l.ReportError(list_fields_status.ToStr());
  }
}

Machine::Machine() {}

ControlFlow Machine::VisitChildren(gui::Visitor& visitor) {
  UpdateConnectionWidgets();
  int i = 0;
  Size n = locations.size() + connection_widgets.size();
  Widget* arr[n];
  for (auto& it : connection_widgets) {
    arr[i++] = it.get();
  }
  for (auto& it : locations) {
    arr[i++] = it.get();
  }
  if (visitor(maf::SpanOfArr(arr, n)) == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  return ControlFlow::Continue;
}
SkMatrix Machine::TransformToChild(const Widget& child, animation::Context& actx) const {
  if (const Location* l = dynamic_cast<const Location*>(&child)) {
    Vec2 pos = l->AnimatedPosition(&actx);
    return SkMatrix::Translate(-pos.x, -pos.y);
  } else if (const gui::ConnectionWidget* w = dynamic_cast<const gui::ConnectionWidget*>(&child)) {
    return SkMatrix::I();
  }
  return SkMatrix::I();
}
}  // namespace automat