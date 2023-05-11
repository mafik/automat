#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <math.h>
#include <memory>
#include <regex>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "algebra.h"
#include "base.h"
#include "format.h"
#include "library_alert.h"
#include "library_increment.h"
#include "library_number.h"
#include "log.h"
#include "treemath.h"

namespace automaton {

struct Integer : Object {
  int32_t i;
  Integer(int32_t i = 0) : i(i) {}
  static const Integer proto;
  string_view Name() const override { return "Integer"; }
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<Integer>(i);
  }
  string GetText() const override { return std::to_string(i); }
  void SetText(Location &error_context, string_view text) override {
    i = std::stoi(string(text));
  }
};

struct Delete : Object {
  static const Delete proto;
  static Argument target_arg;
  string_view Name() const override { return "Delete"; }
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<Delete>();
  }
  void Run(Location &here) override {
    auto target = target_arg.GetLocation(here);
    if (!target.ok) {
      return;
    }
    target.location->Take();
  }
};

struct Set : Object {
  static const Set proto;
  static Argument value_arg;
  static Argument target_arg;
  string_view Name() const override { return "Set"; }
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<Set>();
  }
  void Run(Location &here) override {
    auto value = value_arg.GetObject(here);
    auto target = target_arg.GetLocation(here);
    if (!value.ok || !target.ok) {
      return;
    }
    auto clone = value.object->Clone();
    target.location->Put(std::move(clone));
  }
};

struct Date : Object {
  static const Date proto;
  int year;
  int month;
  int day;
  Date(int year = 0, int month = 0, int day = 0)
      : year(year), month(month), day(day) {}
  string_view Name() const override { return "Date"; }
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<Date>(year, month, day);
  }
  string GetText() const override {
    return f("%04d-%02d-%02d", year, month, day);
  }
  void SetText(Location &error_context, string_view text) override {
    std::regex re(R"((\d{4})-(\d{2})-(\d{2}))");
    std::match_results<string_view::const_iterator> match;
    if (std::regex_match(text.begin(), text.end(), match, re)) {
      year = std::stoi(match[1]);
      month = std::stoi(match[2]);
      day = std::stoi(match[3]);
    } else {
      error_context.ReportError(
          "Invalid date format. The Date object expects dates in "
          "the format YYYY-MM-DD. The provided date was: " +
          string(text) + ".");
    }
  }
  std::partial_ordering
  operator<=>(const Object &other) const noexcept override {
    if (auto other_date = dynamic_cast<const Date *>(&other)) {
      if (year < other_date->year)
        return std::partial_ordering::less;
      if (year > other_date->year)
        return std::partial_ordering::greater;
      if (month < other_date->month)
        return std::partial_ordering::less;
      if (month > other_date->month)
        return std::partial_ordering::greater;
      if (day < other_date->day)
        return std::partial_ordering::less;
      if (day > other_date->day)
        return std::partial_ordering::greater;
      return std::partial_ordering::equivalent;
    }
    return std::partial_ordering::unordered;
  }
};

using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double>;
using TimePoint = std::chrono::time_point<Clock, Duration>;

struct FakeTime {
  TimePoint now;
  std::multimap<TimePoint, Location *> schedule;

  void SetNow(TimePoint time) {
    this->now = time;
    while (!schedule.empty() && schedule.begin()->first <= now) {
      auto [time, location] = *schedule.begin();
      schedule.erase(schedule.begin());
      location->ScheduleRun();
    }
  }
  void RunAfter(Duration duration, Location &location) {
    schedule.emplace(now + duration, &location);
  }
};

// This is a _periodic_ timer. It will run every 1ms.
//
// In addition to periodic timers we could also have two other types of timers:
// 1. _Continuous_ timers - which reschedule their `Run` without any delay.
// 2. _Lazy_ timers - which never `Run` but can be queried with `GetText`.
struct Timer : Object {
  static const Timer proto;
  TimePoint start;
  TimePoint last_tick;
  FakeTime *fake_time = nullptr;
  string_view Name() const override { return "Timer"; }
  std::unique_ptr<Object> Clone() const override {
    auto other = new Timer();
    other->start = start;
    return std::unique_ptr<Object>(other);
  }
  void ScheduleNextRun(Location &here) {
    using namespace std::chrono_literals;
    if (fake_time) {
      fake_time->RunAfter(1ms, here);
    } else {
      // TODO: Use std::this_thread::sleep_for
    }
  }
  string GetText() const override {
    using namespace std::chrono_literals;
    TimePoint now = GetNow();
    Duration elapsed = now - start;
    return f("%.3lf", elapsed.count());
  }
  void Run(Location &here) override {
    using namespace std::chrono_literals;
    TimePoint now = GetNow();
    if (now - last_tick >= 1ms) {
      last_tick = now;
      here.ScheduleUpdate();
    }
    ScheduleNextRun(here);
  }
  void Reset(Location &here) {
    start = GetNow();
    last_tick = start;
    here.ScheduleUpdate();
    ScheduleNextRun(here);
  }
  TimePoint GetNow() const {
    if (fake_time) {
      return fake_time->now;
    } else {
      return Clock::now();
    }
  }
};

struct TimerReset : Object {
  static const TimerReset proto;
  static Argument timer_arg;
  string_view Name() const override { return "TimerReset"; }
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<TimerReset>();
  }
  void Run(Location &here) override {
    auto timer = timer_arg.GetTyped<Timer>(here);
    if (!timer.ok) {
      return;
    }
    timer.typed->Reset(*timer.location);
  }
};

struct EqualityTest : LiveObject {
  static const EqualityTest proto;
  static LiveArgument target_arg;
  bool state = true;
  EqualityTest() {}
  string_view Name() const override { return "Equality Test"; }
  std::unique_ptr<Object> Clone() const override {
    auto other = std::make_unique<EqualityTest>();
    other->state = true;
    return other;
  }
  void Args(std::function<void(LiveArgument &)> cb) override { cb(target_arg); }
  string GetText() const override { return state ? "true" : "false"; }
  void Updated(Location &here, Location &updated) override {
    Object *updated_object = updated.Follow();
    bool new_state = true;
    target_arg.LoopObjects<bool>(here, [&](Object &target_object) {
      if ((target_object <=> *updated_object) != 0) {
        new_state = false;
        return true; // return non-null to break out of LoopObjects
      }
      return false;
    });
    if (state != new_state) {
      state = new_state;
      here.ScheduleUpdate();
    }
  }
};

struct LessThanTest : LiveObject {
  static const LessThanTest proto;
  static LiveArgument less_arg;
  static LiveArgument than_arg;
  bool state = true;
  LessThanTest() {}
  string_view Name() const override { return "Less Than Test"; }
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<LessThanTest>();
  }
  string GetText() const override { return state ? "true" : "false"; }
  void Args(std::function<void(LiveArgument &)> cb) override {
    cb(less_arg);
    cb(than_arg);
  }
  void Updated(Location &here, Location &updated) override {
    auto less = less_arg.GetObject(here);
    auto than = than_arg.GetObject(here);
    if (!less.ok || !than.ok) {
      return;
    }
    bool new_state = *less.object < *than.object;
    if (state != new_state) {
      state = new_state;
      here.ScheduleUpdate();
    }
  }
};

struct StartsWithTest : LiveObject {
  static const StartsWithTest proto;
  static LiveArgument starts_arg;
  static LiveArgument with_arg;
  bool state = true;
  StartsWithTest() {}
  string_view Name() const override { return "Starts With Test"; }
  std::unique_ptr<Object> Clone() const override {
    auto other = std::make_unique<StartsWithTest>();
    other->state = state;
    return other;
  }
  string GetText() const override { return state ? "true" : "false"; }
  void Args(std::function<void(LiveArgument &)> cb) override {
    cb(starts_arg);
    cb(with_arg);
  }
  void Updated(Location &here, Location &updated) override {
    auto starts = starts_arg.GetObject(here);
    auto with = with_arg.GetObject(here);
    if (!starts.ok || !with.ok) {
      return;
    } else {
      here.ClearError();
    }
    bool new_state =
        starts.object->GetText().starts_with(with.object->GetText());
    if (state != new_state) {
      state = new_state;
      here.ScheduleUpdate();
    }
  }
};

struct AllTest : LiveObject {
  static const AllTest proto;
  static LiveArgument test_arg;
  bool state = true;
  AllTest() {}
  string_view Name() const override { return "All Test"; }
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<AllTest>();
  }
  string GetText() const override { return state ? "true" : "false"; }
  void Args(std::function<void(LiveArgument &)> cb) override { cb(test_arg); }
  void Updated(Location &here, Location &updated) override {
    bool found_non_true = test_arg.LoopObjects<bool>(here, [](Object &o) {
      if (o.GetText() != "true") {
        return true; // this breaks the loop
      }
      return false;
    });
    bool new_state = !found_non_true;
    if (state != new_state) {
      state = new_state;
      here.ScheduleUpdate();
    }
  }
};

struct Switch : LiveObject {
  static const Switch proto;
  static LiveArgument target_arg;
  LiveArgument case_arg = LiveArgument("case", Argument::kRequiresObject);
  string_view Name() const override { return "Switch"; }
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<Switch>();
  }
  void Args(std::function<void(LiveArgument &)> cb) override {
    cb(target_arg);
    cb(case_arg);
  }
  string GetText() const override {
    auto case_ = case_arg.GetObject(*here);
    if (!case_.ok) {
      return "";
    }
    return case_.object->GetText();
  }
  void Updated(Location &here, Location &updated) override {
    // When "target" changes the name of the case argument changes.
    auto target = target_arg.GetObject(here);
    if (!target.ok) {
      return;
    }
    if (&updated == target.location) { // target changed
      case_arg.Rename(here, target.object->GetText());
      here.ScheduleUpdate();
      return;
    }
    auto case_ = case_arg.GetLocation(here);
    if (!case_.ok) {
      return;
    }
    if (&updated == case_.location) { // case changed
      here.ScheduleUpdate();
      return;
    }
  }
};

struct ErrorReporter : LiveObject {
  static const ErrorReporter proto;
  static LiveArgument test_arg;
  static LiveArgument message_arg;
  string_view Name() const override { return "Error Reporter"; }
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<ErrorReporter>();
  }
  void Args(std::function<void(LiveArgument &)> cb) override {
    cb(test_arg);
    cb(message_arg);
  }
  void Updated(Location &here, Location &updated) override {
    here.ClearError();
    auto test = test_arg.GetObject(here);
    if (!test.ok || test.object->GetText() != "true") {
      return;
    }
    auto message = message_arg.GetObject(here);
    if (!message.ok) {
      return;
    }
    std::string error_text = "Error reported by ErrorReporter";
    if (message.object) {
      error_text = message.object->GetText();
    }
    auto err = here.ReportError(error_text);
  }
};

struct Parent : Pointer {
  static const Parent proto;
  string_view Name() const override { return "Parent"; }
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<Parent>();
  }
  void Args(std::function<void(LiveArgument &)> cb) override {}
  Object *Next(Location &error_context) const override {
    if (here && here->parent) {
      return here->parent->object.get();
    }
    return nullptr;
  }
  void PutNext(Location &error_context, std::unique_ptr<Object> obj) override {
    if (here && here->parent) {
      here->parent->Put(std::move(obj));
    } else {
      auto err = error_context.ReportError("No parent to put to");
      err->saved_object = std::move(obj);
    }
  }
  std::unique_ptr<Object> TakeNext(Location &error_context) override {
    if (here && here->parent) {
      return here->parent->Take();
    }
    auto err = error_context.ReportError("No parent to take from");
    return nullptr;
  }
};

struct HealthTest : Object {
  static const HealthTest proto;
  static Argument target_arg;
  bool state = true;
  HealthTest() {}
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<HealthTest>();
  }
  void UpdateState(Location *here) {
    auto target = target_arg.GetFinalLocation(*here);
    if (target.final_location) {
      here->ObserveErrors(*target.final_location);
      state = !target.final_location->HasError();
    } else {
      state = true;
    }
  }
  void Relocate(Location *here) override { UpdateState(here); }
  void ConnectionAdded(Location &here, string_view label,
                       Connection &connection) override {
    if (label == "target") {
      UpdateState(&here);
    }
  }
  string_view Name() const override { return "Health Test"; }
  string GetText() const override { return state ? "true" : "false"; }
  void Errored(Location &here, Location &errored) override {
    state = false;
    here.ScheduleUpdate();
  }
};

struct ErrorCleaner : Object {
  static const ErrorCleaner proto;
  static Argument target_arg;
  ErrorCleaner() {}
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<ErrorCleaner>();
  }
  void ObserveErrors(Location *here) {
    if (!here) {
      return;
    }
    auto target = target_arg.GetFinalLocation(*here);
    if (target.final_location) {
      here->ObserveErrors(*target.final_location);
    }
  }
  void Relocate(Location *here) override { ObserveErrors(here); }
  void ConnectionAdded(Location &here, string_view label,
                       Connection &connection) override {
    if (label == "target") {
      ObserveErrors(&here);
    }
  }
  string_view Name() const override { return "Error Cleaner"; }
  void Errored(Location &here, Location &errored) override {
    errored.ClearError();
  }
};

struct AbstractList {
  virtual Error *GetAtIndex(int index, Object *&obj) = 0;
  virtual Error *PutAtIndex(int index, bool overwrite,
                            std::unique_ptr<Object> obj) = 0;
  virtual Error *TakeAtIndex(int index, bool leave_null,
                             std::unique_ptr<Object> &obj) = 0;
  virtual Error *GetSize(int &size) = 0;
};

struct Append : Object {
  static const Append proto;
  static Argument to_arg;
  static Argument what_arg;
  string_view Name() const override { return "Append"; }
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<Append>();
  }
  void Run(Location &here) override {
    auto to = to_arg.GetTyped<AbstractList>(here);
    if (!to.ok) {
      return;
    }
    auto list_object = to.typed;
    int size = 0;
    if (auto error = list_object->GetSize(size)) {
      here.error.reset(error);
      return;
    }
    auto what = what_arg.GetLocation(here);
    if (!what.ok) {
      return;
    }
    if (auto obj = what.location->Take()) {
      if (auto error = list_object->PutAtIndex(size, false, obj->Clone())) {
        here.error.reset(error);
        return;
      }
    }
  }
};

struct List : Object, AbstractList {
  static const List proto;
  Location *here = nullptr;
  vector<unique_ptr<Object>> objects;
  string_view Name() const override { return "List"; }
  std::unique_ptr<Object> Clone() const override {
    auto list = std::make_unique<List>();
    for (auto &object : objects) {
      list->objects.emplace_back(object->Clone());
    }
    return list;
  }
  void Relocate(Location *here) override { this->here = here; }
  Error *GetAtIndex(int index, Object *&obj) override {
    if (index < 0 || index >= objects.size()) {
      return here->ReportError("Index out of bounds.");
    }
    obj = objects[index].get();
    return nullptr;
  }
  Error *PutAtIndex(int index, bool overwrite,
                    std::unique_ptr<Object> obj) override {
    if (index < 0 ||
        (overwrite ? index >= objects.size() : index > objects.size())) {
      // TODO: save the object in the error - it shouldn't be destroyed!
      return here->ReportError("Index out of bounds.");
    }
    if (overwrite) {
      objects[index] = std::move(obj);
    } else {
      objects.insert(objects.begin() + index, std::move(obj));
    }
    here->ScheduleUpdate();
    return nullptr;
  }
  Error *TakeAtIndex(int index, bool keep_null,
                     std::unique_ptr<Object> &obj) override {
    if (index < 0 || index >= objects.size()) {
      return here->ReportError("Index out of bounds.");
    }
    obj = std::move(objects[index]);
    if (!keep_null) {
      objects.erase(objects.begin() + index);
    }
    here->ScheduleUpdate();
    return nullptr;
  }
  Error *GetSize(int &size) override {
    size = objects.size();
    return nullptr;
  }
};

struct Iterator {
  virtual Object *GetCurrent() const = 0;
};

struct CurrentElement;
struct Filter : LiveObject, Iterator, AbstractList {
  enum class Phase { kSequential, kDone };
  static const Filter proto;
  static LiveArgument list_arg;
  static LiveArgument element_arg;
  static LiveArgument test_arg;
  Phase phase = Phase::kDone;
  int index = 0;
  vector<Object *> objects;
  vector<int> indices;
  string_view Name() const override { return "Filter"; }
  std::unique_ptr<Object> Clone() const override {
    auto filter = std::make_unique<Filter>();
    filter->phase = Phase::kDone;
    filter->index = 0;
    return filter;
  }
  void Args(std::function<void(LiveArgument &)> cb) override {
    cb(list_arg);
    cb(element_arg);
    cb(test_arg);
  }
  void Run(Location &here) override {
    if (phase == Phase::kSequential) {
      // Check the value of test, possibly copying element from list to output.
      // Then increment index and schedule another iteration.
      auto test = test_arg.GetObject(here);
      if (!test.ok) {
        return;
      }
      if (test.object->GetText() == "true") {
        if (auto obj = GetCurrent()) {
          objects.emplace_back(obj);
          indices.emplace_back(index);
        }
      }
      ++index;
      BeginNextIteration(here);
    } else {
      here.ReportError(
          "Tried to Run this Filter but filtering is already completed.");
    }
  }
  void StartFiltering() {
    objects.clear();
    phase = Phase::kSequential;
    index = 0;
    BeginNextIteration(*here);
  }
  void BeginNextIteration(Location &here) {
    int list_size = 0;
    auto list = list_arg.GetTyped<AbstractList>(here);
    if (!list.ok) {
      return;
    }
    list.typed->GetSize(list_size);
    if (index < list_size) {
      ThenGuard then({&here.run_task});
      auto element = element_arg.GetLocation(here);
      if (!element.ok) {
        return;
      }
      element.location->ScheduleUpdate();
    } else {
      phase = Phase::kDone;
    }
  }
  // Iterator interface
  Object *GetCurrent() const override {
    auto list = list_arg.GetTyped<AbstractList>(*here);
    if (!list.ok) {
      return nullptr;
    }
    int size = 0;
    if (list.typed->GetSize(size)) {
      return nullptr;
    }
    if (index >= size) {
      return nullptr;
    }
    Object *obj = nullptr;
    if (list.typed->GetAtIndex(index, obj)) {
      return nullptr;
    }
    return obj;
  }
  void Updated(Location &here, Location &updated) override;
  // AbstractList interface
  Error *GetAtIndex(int index, Object *&obj) override {
    if (index < 0 || index >= objects.size()) {
      return here->ReportError("Index out of bounds.");
    }
    obj = objects[index];
    return nullptr;
  }
  Error *PutAtIndex(int index, bool overwrite,
                    std::unique_ptr<Object> obj) override {
    // Filter doesn't own the objects it contains, so it must pass them to its
    // parent list. In order to insert them at the right position in the parent
    // list, it must also know the mapping between filtered & unfiltered
    // indices. This is not yet implemented. NOTE: object dropped here!
    return here->ReportError("Not implemented yet.");
  }
  Error *TakeAtIndex(int index, bool leave_null,
                     std::unique_ptr<Object> &obj) override {
    if (index < 0 || index >= objects.size()) {
      return here->ReportError("Index out of bounds.");
    }
    int orig_index = indices[index];
    auto list = list_arg.GetTyped<AbstractList>(*here);
    if (!list.ok) {
      return here->GetError();
    }
    if (Error *err = list.typed->TakeAtIndex(orig_index, leave_null, obj)) {
      return err;
    }
    objects.erase(objects.begin() + index);
    indices.erase(indices.begin() + index);
    for (int i = index; i < indices.size(); ++i) {
      --indices[i];
    }
    return nullptr;
  }
  Error *GetSize(int &size) override {
    size = objects.size();
    return nullptr;
  }
};

struct CurrentElement : Pointer {
  static const CurrentElement proto;
  static LiveArgument of_arg;
  string_view Name() const override { return "Current Element"; }
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<CurrentElement>();
  }
  void Args(std::function<void(LiveArgument &)> cb) override { cb(of_arg); }
  Object *Next(Location &error_context) const override {
    auto of = of_arg.GetTyped<Iterator>(*here);
    if (!of.ok) {
      return nullptr;
    }
    return of.typed->GetCurrent();
  }
  void PutNext(Location &error_context, std::unique_ptr<Object> obj) override {
    here->ReportError(
        "Tried to put an object to Current Element but it's not possible.");
  }
  std::unique_ptr<Object> TakeNext(Location &error_context) override {
    here->ReportError(
        "Tried to take an object from Current Element but it's not possible.");
    return nullptr;
  }
};

inline void Filter::Updated(Location &here, Location &updated) {
  auto list = list_arg.GetTyped<AbstractList>(here);
  auto element = element_arg.GetTyped<CurrentElement>(here);
  auto test = test_arg.GetObject(here);
  if (!list.ok || !element.ok || !test.ok) {
    return;
  }
  // TODO: Cancel the outstanding RunTask
  bool filtering_completed = phase == Phase::kDone;
  if (filtering_completed) {
    StartFiltering();
  }
}

// Object with subobjects.
//
// The structure contains named fields and is here-descriptive.
struct Complex : Object {
  static const Complex proto;
  std::unordered_map<std::string, unique_ptr<Object>> objects;
  string_view Name() const override { return "Complex"; }
  std::unique_ptr<Object> Clone() const override {
    auto c = new Complex();
    for (auto &[name, obj] : objects) {
      c->objects.emplace(name, obj->Clone());
    }
    return std::unique_ptr<Object>(c);
  }
};

struct ComplexField : Pointer {
  static const ComplexField proto;
  static LiveArgument complex_arg;
  static LiveArgument label_arg;
  string_view Name() const override { return "Complex Field"; }
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<ComplexField>();
  }
  void Args(std::function<void(LiveArgument &)> cb) override {
    cb(complex_arg);
    cb(label_arg);
  }
  Object *Next(Location &unused_error_context) const override {
    auto [complex, label] = FollowComplex(*here);
    if (complex == nullptr) {
      return nullptr;
    }
    auto it = complex->objects.find(label);
    if (it == complex->objects.end()) {
      return nullptr;
    }
    return it->second.get();
  }
  void PutNext(Location &error_context, std::unique_ptr<Object> obj) override {
    auto [complex, label] = FollowComplex(*here);
    if (complex == nullptr) {
      return;
    }
    complex->objects[label] = std::move(obj);
  }
  std::unique_ptr<Object> TakeNext(Location &error_context) override {
    auto [complex, label] = FollowComplex(*here);
    auto it = complex->objects.find(label);
    if (it == complex->objects.end()) {
      return nullptr;
    }
    auto obj = std::move(it->second);
    complex->objects.erase(it);
    return obj;
  }
  void Updated(Location &here, Location &updated) override {
    // Complex was updated - so let's propagate the update.
    here.ScheduleUpdate();
  }

private:
  // Return Complex pointed by this object. If not possible report error and
  // return nullptr.
  static std::pair<Complex *, std::string> FollowComplex(Location &here) {
    auto label = label_arg.GetObject(here);
    std::string label_text = "";
    Complex *return_complex = nullptr;
    if (label.object) {
      label_text = label.object->GetText();
      auto complex = complex_arg.GetTyped<Complex>(here);
      if (complex.typed) {
        return_complex = complex.typed;
      }
    }
    return std::make_pair(return_complex, label_text);
  }
};

//////////////
// Widgets
//////////////

template <class... Ts> struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

struct Text : LiveObject {
  struct RefChunk {
    LiveArgument arg;
  };
  using Chunk = std::variant<string, RefChunk>;
  vector<Chunk> chunks;
  static auto Parse(string_view text) -> std::vector<Chunk> {
    std::vector<Chunk> chunks;
    std::regex ref_placeholder_re(R"(\{([^\}]+)\})");
    std::regex_iterator<string_view::const_iterator> begin(
        text.begin(), text.end(), ref_placeholder_re);
    std::regex_iterator<string_view::const_iterator> end;
    int parsed_to = 0;
    for (auto i = begin; i != end; ++i) {
      std::match_results<string_view::const_iterator> match = *i;
      if (match.position() - parsed_to > 0) {
        string text_chunk(text.substr(parsed_to, match.position() - parsed_to));
        chunks.emplace_back(text_chunk);
      }
      LiveArgument arg(match[1].str(), Argument::kOptional);
      chunks.push_back(RefChunk{arg});
      parsed_to = match.position() + match.length();
    }
    if (parsed_to < text.size()) {
      string text_chunk(text.substr(parsed_to));
      chunks.emplace_back(text_chunk);
    }
    return chunks;
  }
  static const Text proto;
  static LiveArgument target_arg;
  string_view Name() const override { return "Text Editor"; }
  std::unique_ptr<Object> Clone() const override {
    auto other = std::make_unique<Text>();
    other->chunks = chunks;
    return other;
  }
  void Args(std::function<void(LiveArgument &)> cb) override {
    cb(target_arg);
    for (Chunk &chunk : chunks) {
      if (auto ref = std::get_if<RefChunk>(&chunk)) {
        cb(ref->arg);
      }
    }
  }
  string GetText() const override {
    string buffer;
    for (const std::variant<string, RefChunk> &chunk : chunks) {
      std::visit(overloaded{[&](const string &text) { buffer += text; },
                            [&](const RefChunk &ref) {
                              auto arg = ref.arg.GetObject(*here);
                              if (arg.ok) {
                                buffer += arg.object->GetText();
                              } else {
                                buffer += f("{%s}", ref.arg.name.c_str());
                              }
                            }},
                 chunk);
    }
    return buffer;
  }
  void SetText(Location &error_context, string_view new_text) override {
    string old_text = GetText();
    if (old_text == new_text)
      return;
    chunks.clear();
    chunks = Parse(new_text);
    // chunks.emplace_back(string(new_text));
    if (here) {
      auto target = target_arg.GetLocation(*here);
      if (target.location) {
        target.location->SetText(new_text);
      }
    }
  }
  void Run(Location &here) override {}
  void Updated(Location &here, Location &updated) override {
    auto target = target_arg.GetLocation(here);
    if (target.location == &updated) {
      SetText(here, updated.GetText());
    }
  }
};

struct Button : Object {
  string label;
  static const Button proto;
  static Argument enabled_arg;
  string_view Name() const override { return "Button"; }
  std::unique_ptr<Object> Clone() const override {
    auto other = std::make_unique<Button>();
    other->label = label;
    return other;
  }
  string GetText() const override { return label; }
  void SetText(Location &error_context, string_view new_label) override {
    label = new_label;
  }
  void Run(Location &h) override {
    auto enabled = enabled_arg.GetObject(h);
    if (enabled.object && enabled.object->GetText() == "false") {
      h.ReportError("Button is disabled.");
      return;
    }
  }
};

struct ComboBox : LiveObject {
  static const ComboBox proto;
  static LiveArgument options_arg;
  Location *here = nullptr;
  Location *selected = nullptr;
  string_view Name() const override { return "Combo Box"; }
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<ComboBox>();
  }
  void Args(std::function<void(LiveArgument &)> cb) override {
    cb(options_arg);
  }
  void Relocate(Location *here) override { this->here = here; }
  string GetText() const override { return selected->GetText(); }
  void SetText(Location &error_context, string_view new_text) override {
    selected = options_arg.LoopLocations<Location *>(
        *here, [&](Location &option) -> Location * {
          if (option.GetText() == new_text) {
            return &option;
          }
          return nullptr;
        });
    if (selected == nullptr) {
      error_context.ReportError(
          f("No option named %*s", new_text.size(), new_text.data()));
    }
  }
  void ConnectionAdded(Location &here, string_view label,
                       Connection &connection) override {
    LiveObject::ConnectionAdded(here, label, connection);
    if (selected == nullptr && label == "option") {
      auto option = options_arg.GetLocation(here);
      selected = option.location;
    }
  }
};

struct Slider : LiveObject {
  static const Slider proto;
  static LiveArgument min_arg;
  static LiveArgument max_arg;
  double value = 0;
  string_view Name() const override { return "Slider"; }
  std::unique_ptr<Object> Clone() const override {
    auto s = std::make_unique<Slider>();
    s->value = value;
    return s;
  }
  void Args(std::function<void(LiveArgument &)> cb) override {
    cb(min_arg);
    cb(max_arg);
  }
  void Updated(Location &here, Location &updated) override {
    auto min = min_arg.GetLocation(here);
    if (min.location && &updated == min.location) {
      double min_val = min.location->GetNumber();
      value = std::max(value, min_val);
    }
    auto max = max_arg.GetLocation(here);
    if (max.location && &updated == max.location) {
      double max_val = max.location->GetNumber();
      value = std::min(value, max_val);
    }
  }
  string GetText() const override { return std::to_string(value); }
  void SetText(Location &error_context, string_view new_text) override {
    double new_value = std::stod(string(new_text));
    auto min = min_arg.GetLocation(*here);
    if (min.location) {
      double min_val = min.location->GetNumber();
      new_value = std::max(new_value, min_val);
    }
    auto max = max_arg.GetLocation(*here);
    if (max.location) {
      double max_val = max.location->GetNumber();
      new_value = std::min(new_value, max_val);
    }
    value = new_value;
  }
};

struct ProgressBar : library::Number {
  static const ProgressBar proto;
  string_view Name() const override { return "Progress Bar"; }
  std::unique_ptr<Object> Clone() const override {
    auto bar = std::make_unique<ProgressBar>();
    bar->value = value;
    return bar;
  }
};

struct ListView : Pointer {
  static const ListView proto;
  static LiveArgument list_arg;
  int index = -1;
  string_view Name() const override { return "List View"; }
  std::unique_ptr<Object> Clone() const override {
    std::unique_ptr<Object> clone = std::make_unique<ListView>();
    dynamic_cast<ListView &>(*clone).index = index;
    return clone;
  }
  void Args(std::function<void(LiveArgument &)> cb) override { cb(list_arg); }
  void Select(int new_index) {
    if (new_index != index) {
      index = new_index;
      here->ScheduleUpdate();
    }
  }
  Object *Next(Location &error_context) const override {
    if (index < 0)
      return nullptr;
    auto list = list_arg.GetTyped<AbstractList>(error_context);
    if (!list.ok) {
      return nullptr;
    }
    int size = 0;
    if (auto err = list.typed->GetSize(size)) {
      return nullptr;
    }
    if (index >= size)
      return nullptr;
    Object *obj = nullptr;
    if (auto err = list.typed->GetAtIndex(index, obj)) {
      return nullptr;
    }
    return obj;
  }
  void PutNext(Location &error_context, std::unique_ptr<Object> obj) override {
    auto list = list_arg.GetTyped<AbstractList>(error_context);
    if (!list.ok) {
      return;
    }
    int size = 0;
    list.typed->GetSize(size);
    if (index < 0) {
      list.typed->PutAtIndex(0, false, std::move(obj));
    } else if (index >= size) {
      list.typed->PutAtIndex(size, false, std::move(obj));
      ++index;
    } else {
      list.typed->PutAtIndex(index, false, std::move(obj));
    }
  }
  std::unique_ptr<Object> TakeNext(Location &error_context) override {
    auto list = list_arg.GetTyped<AbstractList>(error_context);
    if (!list.ok) {
      return nullptr;
    }
    int size = 0;
    list.typed->GetSize(size);
    if (index < 0 || index >= size)
      return nullptr;
    std::unique_ptr<Object> obj;
    list.typed->TakeAtIndex(index, false, obj);
    if (index >= size)
      --index;
    return obj;
  }
};

////////////////
// Algebra
////////////////

// Interface for algebra callbacks.
struct AlgebraContext : algebra::Context {
  Location *location;
  AlgebraContext(Location *location) : location(location) {}
  double RetrieveVariable(string_view variable) override {
    auto target =
        Argument(variable, Argument::kRequiresObject).GetObject(*location);
    if (target.object) {
      string s = target.object->GetText();
      return std::stod(s);
    } else {
      location->ReportMissing(variable);
      return NAN;
    }
  }
};

struct Blackboard : Object {
  static const Blackboard proto;
  unique_ptr<algebra::Statement> statement = nullptr;
  string_view Name() const override { return "Formula"; }
  std::unique_ptr<Object> Clone() const override {
    auto other = std::make_unique<Blackboard>();
    if (statement) {
      other->statement = statement->Clone();
    }
    return other;
  }
  string GetText() const override {
    if (!statement) {
      return "";
    }
    return statement->GetText();
  }
  void SetText(Location &error_context, string_view text) override {
    statement = algebra::ParseStatement(text);
  }
};

struct BlackboardUpdater : LiveObject {
  static const BlackboardUpdater proto;
  std::unordered_map<string, unique_ptr<algebra::Expression>> formulas;
  std::map<string, LiveArgument> independent_variable_args;

  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<BlackboardUpdater>();
  }
  void Args(std::function<void(LiveArgument &)> cb) override {
    for (auto &arg : independent_variable_args) {
      cb(arg.second);
    }
  }
  void Relocate(Location *here) override {
    // 1. Find nearby blackboards & register as an observer.
    // 2. Extract variables from math statements.
    here->Nearby([&](Location &other) -> void * {
      if (Blackboard *blackboard = other.As<Blackboard>()) {
        here->ObserveUpdates(other);
        if (algebra::Equation *equation = dynamic_cast<algebra::Equation *>(
                blackboard->statement.get())) {
          // fmt::print("Found equation: {}\n", equation->GetText());
          treemath::Tree tree(*equation);
          vector<algebra::Variable *> variables = ExtractVariables(equation);
          std::unordered_set<algebra::Variable *> independent_variables;
          // 3. Derive formulas for each of the variables.
          for (algebra::Variable *variable : variables) {
            if (treemath::Variable *tree_var =
                    tree.FindVariable(variable->name)) {
              if (auto expr = tree_var->DeriveExpression(nullptr)) {
                // fmt::print("Derived formula for {}: {}\n", variable->name,
                // expr->GetText());
                formulas[variable->name] = std::move(expr);
                for (algebra::Variable *independent :
                     ExtractVariables(formulas[variable->name].get())) {
                  independent_variables.insert(independent);
                }
              }
            }
          }
          // 4. Observe all of the independent variables.
          for (algebra::Variable *variable : independent_variables) {
            independent_variable_args.emplace(
                variable->name,
                LiveArgument(variable->name, Argument::kRequiresObject));
          }
        }
      }
      return nullptr;
    });
    LiveObject::Relocate(here);
  }
  string_view Name() const override { return "Blackboard Updater"; }
  void Run(Location &here) override {}
  void Updated(Location &here, Location &updated) override {
    // LOG << "MathEngine::Updated(" << here.HumanReadableName() << ", " <<
    // updated.HumanReadableName() << ")";
    NoSchedulingGuard guard(here);
    if (double num = updated.GetNumber(); !std::isnan(num)) {
      // The list of variables that have changed in response could be
      // precomputed.
      for (auto &[name, expr] : formulas) {
        vector<algebra::Variable *> independent_variables =
            algebra::ExtractVariables(expr.get());
        for (algebra::Variable *independent_var : independent_variables) {
          if (independent_var->name == updated.name) {
            AlgebraContext context{&here};
            auto arg_it = independent_variable_args.find(name);
            if (arg_it == independent_variable_args.end()) {
              here.ReportError("Couldn't find LiveArgument for a variable. "
                               "This shouldn't happen.");
              continue;
            }
            auto target = arg_it->second.GetObject(here);
            if (target.object) {
              if (target.location->incoming.find("const") !=
                  target.location->incoming.end()) {
                // LOG << "  would write to " << *target
                //     << " but it's marked as const.";

              } else {
                double new_value = expr->Eval(&context);
                if (std::isnan(new_value)) {
                  // LOG << "  would write to " << *target
                  //     << " but the value would be NaN.";
                } else {
                  // LOG << "  writing to " << target->HumanReadableName()
                  //     << " <- " << new_value;
                  target.location->SetNumber(new_value);
                }
              }
            }
          }
        }
      }
    }
  }
};

/////////////
// Misc
/////////////

#undef DEFINE_PROTO

} // namespace automaton
