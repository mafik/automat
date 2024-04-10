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

}  // namespace automat