#include "base.h"

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

#include <set>
#include <unordered_map>

#include "color.h"
#include "font.h"
#include "text_field.h"
#include "thread_name.h"

namespace automat {

Location* Machine::LocationAtPoint(vec2 point) {
  for (auto& loc : locations) {
    vec2 local_point = point - loc->position;
    if (loc->Shape().contains(local_point.X, local_point.Y)) {
      return loc.get();
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

void RunThread() {
  SetThreadName("Automaton Loop");
  while (true) {
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
    LOG() << "RunLoop(" << queue.size() << " tasks)";
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
}  // namespace automat