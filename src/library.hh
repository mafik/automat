// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <math.h>

#include <cstdint>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "algebra.hh"
#include "base.hh"
#include "format.hh"
#include "library_alert.hh"           // IWYU pragma: keep
#include "library_flip_flop.hh"       // IWYU pragma: keep
#include "library_hotkey.hh"          // IWYU pragma: keep
#include "library_increment.hh"       // IWYU pragma: keep
#include "library_instruction.hh"     // IWYU pragma: keep
#include "library_key_presser.hh"     // IWYU pragma: keep
#include "library_macro_recorder.hh"  // IWYU pragma: keep
#include "library_mouse_click.hh"     // IWYU pragma: keep
#include "library_number.hh"          // IWYU pragma: keep
#include "library_timeline.hh"        // IWYU pragma: keep
#include "library_timer.hh"           // IWYU pragma: keep
#include "treemath.hh"

namespace automat {

struct Integer : Object {
  int32_t i;
  Integer(int32_t i = 0) : i(i) {}
  string_view Name() const override { return "Integer"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Integer, i); }
  string GetText() const override { return std::to_string(i); }
  void SetText(Location& error_context, string_view text) override { i = std::stoi(string(text)); }
};

struct Delete : Object, Runnable {
  static Argument target_arg;
  string_view Name() const override { return "Delete"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Delete); }
  void OnRun(Location& here, RunTask&) override {
    auto target = target_arg.GetLocation(here);
    if (!target.ok) {
      return;
    }
    target.location->Take();
  }
};

struct Set : Object, Runnable {
  static Argument value_arg;
  static Argument target_arg;
  string_view Name() const override { return "Set"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Set); }
  void OnRun(Location& here, RunTask&) override {
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
  int year;
  int month;
  int day;
  Date(int year = 0, int month = 0, int day = 0) : year(year), month(month), day(day) {}
  string_view Name() const override { return "Date"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Date, year, month, day); }
  string GetText() const override { return f("{:04d}-{:02d}-{:02d}", year, month, day); }
  void SetText(Location& error_context, string_view text) override {
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
  std::partial_ordering operator<=>(const Object& other) const noexcept override {
    if (auto other_date = dynamic_cast<const Date*>(&other)) {
      if (year < other_date->year) return std::partial_ordering::less;
      if (year > other_date->year) return std::partial_ordering::greater;
      if (month < other_date->month) return std::partial_ordering::less;
      if (month > other_date->month) return std::partial_ordering::greater;
      if (day < other_date->day) return std::partial_ordering::less;
      if (day > other_date->day) return std::partial_ordering::greater;
      return std::partial_ordering::equivalent;
    }
    return std::partial_ordering::unordered;
  }
};

struct FakeTime {
  time::SteadyPoint now;
  std::multimap<time::SteadyPoint, Location*> schedule;

  void SetNow(time::SteadyPoint time) {
    this->now = time;
    while (!schedule.empty() && schedule.begin()->first <= now) {
      auto [time, location] = *schedule.begin();
      schedule.erase(schedule.begin());
      location->ScheduleRun();
    }
  }
  void RunAfter(time::Duration duration, Location& location) {
    schedule.emplace(now + duration, &location);
  }
};

// This is a _periodic_ timer. It will run every 1ms.
//
// In addition to periodic timers we could also have two other types of timers:
// 1. _Continuous_ timers - which reschedule their `Run` without any delay.
// 2. _Lazy_ timers - which never `Run` but can be queried with `GetText`.
struct Timer : Object, Runnable {
  time::SteadyPoint start;
  time::SteadyPoint last_tick;
  FakeTime* fake_time = nullptr;
  string_view Name() const override { return "Timer"; }
  Ptr<Object> Clone() const override {
    auto other = MAKE_PTR(Timer);
    other->start = start;
    return other;
  }
  void ScheduleNextRun(Location& here) {
    if (fake_time) {
      fake_time->RunAfter(1ms, here);
    } else {
      // TODO: Use std::this_thread::sleep_for
    }
  }
  string GetText() const override {
    time::SteadyPoint now = GetNow();
    time::Duration elapsed = now - start;
    return f("{}", time::ToSeconds(elapsed));
  }
  void OnRun(Location& here, RunTask&) override {
    time::SteadyPoint now = GetNow();
    if (now - last_tick >= 1ms) {
      last_tick = now;
      here.ScheduleUpdate();
    }
    ScheduleNextRun(here);
  }
  void Reset(Location& here) {
    start = GetNow();
    last_tick = start;
    here.ScheduleUpdate();
    ScheduleNextRun(here);
  }
  time::SteadyPoint GetNow() const {
    if (fake_time) {
      return fake_time->now;
    } else {
      return time::SteadyClock::now();
    }
  }
};

struct TimerReset : Object, Runnable {
  static Argument timer_arg;
  string_view Name() const override { return "TimerReset"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(TimerReset); }
  void OnRun(Location& here, RunTask&) override {
    auto timer = timer_arg.GetTyped<Timer>(here);
    if (!timer.ok) {
      return;
    }
    timer.typed->Reset(*timer.location);
  }
};

struct EqualityTest : LiveObject {
  static LiveArgument target_arg;
  bool state = true;
  EqualityTest() {}
  string_view Name() const override { return "Equality Test"; }
  Ptr<Object> Clone() const override {
    auto other = MAKE_PTR(EqualityTest);
    other->state = true;
    return other;
  }
  void Args(std::function<void(Argument&)> cb) override { cb(target_arg); }
  string GetText() const override { return state ? "true" : "false"; }
  void Updated(Location& here, Location& updated) override {
    Object* updated_object = updated.Follow();
    bool new_state = true;
    target_arg.LoopObjects<bool>(here, [&](Object& target_object) {
      if ((target_object <=> *updated_object) != 0) {
        new_state = false;
        return true;  // return non-null to break out of LoopObjects
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
  static LiveArgument less_arg;
  static LiveArgument than_arg;
  bool state = true;
  LessThanTest() {}
  string_view Name() const override { return "Less Than Test"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(LessThanTest); }
  string GetText() const override { return state ? "true" : "false"; }
  void Args(std::function<void(Argument&)> cb) override {
    cb(less_arg);
    cb(than_arg);
  }
  void Updated(Location& here, Location& updated) override {
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
  static LiveArgument starts_arg;
  static LiveArgument with_arg;
  bool state = true;
  StartsWithTest() {}
  string_view Name() const override { return "Starts With Test"; }
  Ptr<Object> Clone() const override {
    auto other = MAKE_PTR(StartsWithTest);
    other->state = state;
    return other;
  }
  string GetText() const override { return state ? "true" : "false"; }
  void Args(std::function<void(Argument&)> cb) override {
    cb(starts_arg);
    cb(with_arg);
  }
  void Updated(Location& here, Location& updated) override {
    auto starts = starts_arg.GetObject(here);
    auto with = with_arg.GetObject(here);
    if (!starts.ok || !with.ok) {
      return;
    } else {
      here.ClearError();
    }
    bool new_state = starts.object->GetText().starts_with(with.object->GetText());
    if (state != new_state) {
      state = new_state;
      here.ScheduleUpdate();
    }
  }
};

struct AllTest : LiveObject {
  static LiveArgument test_arg;
  bool state = true;
  AllTest() {}
  string_view Name() const override { return "All Test"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(AllTest); }
  string GetText() const override { return state ? "true" : "false"; }
  void Args(std::function<void(Argument&)> cb) override { cb(test_arg); }
  void Updated(Location& here, Location& updated) override {
    bool found_non_true = test_arg.LoopObjects<bool>(here, [](Object& o) {
      if (o.GetText() != "true") {
        return true;  // this breaks the loop
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
  static LiveArgument target_arg;
  LiveArgument case_arg = LiveArgument("case", Argument::kRequiresObject);
  string_view Name() const override { return "Switch"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Switch); }
  void Args(std::function<void(Argument&)> cb) override {
    cb(target_arg);
    cb(case_arg);
  }
  string GetText() const override {
    auto case_ = case_arg.GetObject(*here.lock());
    if (!case_.ok) {
      return "";
    }
    return case_.object->GetText();
  }
  void Updated(Location& here, Location& updated) override {
    // When "target" changes the name of the case argument changes.
    auto target = target_arg.GetObject(here);
    if (!target.ok) {
      return;
    }
    if (&updated == target.location) {  // target changed
      case_arg.Rename(here, target.object->GetText());
      here.ScheduleUpdate();
      return;
    }
    auto case_ = case_arg.GetLocation(here);
    if (!case_.ok) {
      return;
    }
    if (&updated == case_.location) {  // case changed
      here.ScheduleUpdate();
      return;
    }
  }
};

struct ErrorReporter : LiveObject {
  static LiveArgument test_arg;
  static LiveArgument message_arg;
  string_view Name() const override { return "Error Reporter"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(ErrorReporter); }
  void Args(std::function<void(Argument&)> cb) override {
    cb(test_arg);
    cb(message_arg);
  }
  void Updated(Location& here, Location& updated) override {
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
  string_view Name() const override { return "Parent"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Parent); }
  void Args(std::function<void(Argument&)> cb) override {}
  Object* Next(Location& error_context) const override {
    if (auto h = here.lock()) {
      if (auto p = h->parent_location.lock()) {
        return p->object.get();
      }
    }
    return nullptr;
  }
  void PutNext(Location& error_context, Ptr<Object> obj) override {
    if (auto h = here.lock()) {
      if (auto p = h->parent_location.lock()) {
        p->Put(std::move(obj));
        return;
      }
    }
    auto err = error_context.ReportError("No parent to put to");
    err->saved_object = std::move(obj);
  }
  Ptr<Object> TakeNext(Location& error_context) override {
    if (auto h = here.lock()) {
      if (auto p = h->parent_location.lock()) {
        return p->Take();
      }
    }
    auto err = error_context.ReportError("No parent to take from");
    return nullptr;
  }
};

struct HealthTest : Object {
  static Argument target_arg;
  bool state = true;
  HealthTest() {}
  Ptr<Object> Clone() const override { return MAKE_PTR(HealthTest); }
  void UpdateState(Location* here) {
    auto target = target_arg.GetFinalLocation(*here);
    if (target.final_location) {
      here->ObserveErrors(*target.final_location);
      state = !target.final_location->HasError();
    } else {
      state = true;
    }
  }
  void Relocate(Location* here) override { UpdateState(here); }
  void ConnectionAdded(Location& here, Connection& connection) override {
    if (&connection.argument == &target_arg) {
      UpdateState(&here);
    }
  }
  string_view Name() const override { return "Health Test"; }
  string GetText() const override { return state ? "true" : "false"; }
  void Errored(Location& here, Location& errored) override {
    state = false;
    here.ScheduleUpdate();
  }
};

struct ErrorCleaner : Object {
  static Argument target_arg;
  ErrorCleaner() {}
  Ptr<Object> Clone() const override { return MAKE_PTR(ErrorCleaner); }
  void ObserveErrors(Location* here) {
    if (!here) {
      return;
    }
    auto target = target_arg.GetFinalLocation(*here);
    if (target.final_location) {
      here->ObserveErrors(*target.final_location);
    }
  }
  void Relocate(Location* here) override { ObserveErrors(here); }
  void ConnectionAdded(Location& here, Connection& connection) override {
    if (&connection.argument == &target_arg) {
      ObserveErrors(&here);
    }
  }
  string_view Name() const override { return "Error Cleaner"; }
  void Errored(Location& here, Location& errored) override { errored.ClearError(); }
};

struct AbstractList {
  virtual Error* GetAtIndex(int index, Object*& obj) = 0;
  virtual Error* PutAtIndex(int index, bool overwrite, Ptr<Object> obj) = 0;
  virtual Error* TakeAtIndex(int index, bool leave_null, Ptr<Object>& obj) = 0;
  virtual Error* GetSize(int& size) = 0;
};

struct Append : Object, Runnable {
  static Argument to_arg;
  static Argument what_arg;
  string_view Name() const override { return "Append"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Append); }
  void OnRun(Location& here, RunTask&) override {
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
    return;
  }
};

struct List : Object, AbstractList {
  Location* here = nullptr;
  vector<Ptr<Object>> objects;
  string_view Name() const override { return "List"; }
  Ptr<Object> Clone() const override {
    auto list = MAKE_PTR(List);
    for (auto& object : objects) {
      list->objects.emplace_back(object->Clone());
    }
    return list;
  }
  void Relocate(Location* here) override { this->here = here; }
  Error* GetAtIndex(int index, Object*& obj) override {
    if (index < 0 || index >= objects.size()) {
      return here->ReportError("Index out of bounds.");
    }
    obj = objects[index].get();
    return nullptr;
  }
  Error* PutAtIndex(int index, bool overwrite, Ptr<Object> obj) override {
    if (index < 0 || (overwrite ? index >= objects.size() : index > objects.size())) {
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
  Error* TakeAtIndex(int index, bool keep_null, Ptr<Object>& obj) override {
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
  Error* GetSize(int& size) override {
    size = objects.size();
    return nullptr;
  }
};

struct Iterator {
  virtual Object* GetCurrent() const = 0;
};

struct CurrentElement;
struct Filter : LiveObject, Iterator, AbstractList, Runnable {
  enum class Phase { kSequential, kDone };

  static LiveArgument list_arg;
  static LiveArgument element_arg;
  static LiveArgument test_arg;
  Phase phase = Phase::kDone;
  int index = 0;
  vector<Object*> objects;
  vector<int> indices;
  string_view Name() const override { return "Filter"; }
  Ptr<Object> Clone() const override {
    auto filter = MAKE_PTR(Filter);
    filter->phase = Phase::kDone;
    filter->index = 0;
    return filter;
  }
  void Args(std::function<void(Argument&)> cb) override {
    cb(list_arg);
    cb(element_arg);
    cb(test_arg);
  }
  void OnRun(Location& here, RunTask&) override {
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
      here.ReportError("Tried to Run this Filter but filtering is already completed.");
    }
    return;
  }
  void StartFiltering() {
    objects.clear();
    phase = Phase::kSequential;
    index = 0;
    BeginNextIteration(*here.lock());
  }
  void BeginNextIteration(Location& here) {
    int list_size = 0;
    auto list = list_arg.GetTyped<AbstractList>(here);
    if (!list.ok) {
      return;
    }
    list.typed->GetSize(list_size);
    if (index < list_size) {
      vector<Task*> successors;
      if (here.run_task) {
        successors.emplace_back(here.run_task.get());
      }
      NextGuard next_guard(std::move(successors));
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
  Object* GetCurrent() const override {
    auto list = list_arg.GetTyped<AbstractList>(*here.lock());
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
    Object* obj = nullptr;
    if (list.typed->GetAtIndex(index, obj)) {
      return nullptr;
    }
    return obj;
  }
  void Updated(Location& here, Location& updated) override;
  // AbstractList interface
  Error* GetAtIndex(int index, Object*& obj) override {
    if (index < 0 || index >= objects.size()) {
      return here.lock()->ReportError("Index out of bounds.");
    }
    obj = objects[index];
    return nullptr;
  }
  Error* PutAtIndex(int index, bool overwrite, Ptr<Object> obj) override {
    // Filter doesn't own the objects it contains, so it must pass them to its
    // parent list. In order to insert them at the right position in the parent
    // list, it must also know the mapping between filtered & unfiltered
    // indices. This is not yet implemented. NOTE: object dropped here!
    return here.lock()->ReportError("Not implemented yet.");
  }
  Error* TakeAtIndex(int index, bool leave_null, Ptr<Object>& obj) override {
    if (index < 0 || index >= objects.size()) {
      return here.lock()->ReportError("Index out of bounds.");
    }
    int orig_index = indices[index];
    auto list = list_arg.GetTyped<AbstractList>(*here.lock());
    if (!list.ok) {
      return here.lock()->GetError();
    }
    if (Error* err = list.typed->TakeAtIndex(orig_index, leave_null, obj)) {
      return err;
    }
    objects.erase(objects.begin() + index);
    indices.erase(indices.begin() + index);
    for (int i = index; i < indices.size(); ++i) {
      --indices[i];
    }
    return nullptr;
  }
  Error* GetSize(int& size) override {
    size = objects.size();
    return nullptr;
  }
};

struct CurrentElement : Pointer {
  static LiveArgument of_arg;
  string_view Name() const override { return "Current Element"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(CurrentElement); }
  void Args(std::function<void(Argument&)> cb) override { cb(of_arg); }
  Object* Next(Location& error_context) const override {
    auto of = of_arg.GetTyped<Iterator>(*here.lock());
    if (!of.ok) {
      return nullptr;
    }
    return of.typed->GetCurrent();
  }
  void PutNext(Location& error_context, Ptr<Object> obj) override {
    here.lock()->ReportError("Tried to put an object to Current Element but it's not possible.");
  }
  Ptr<Object> TakeNext(Location& error_context) override {
    here.lock()->ReportError("Tried to take an object from Current Element but it's not possible.");
    return nullptr;
  }
};

inline void Filter::Updated(Location& here, Location& updated) {
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
  std::unordered_map<std::string, Ptr<Object>> objects;
  string_view Name() const override { return "Complex"; }
  Ptr<Object> Clone() const override {
    auto c = MAKE_PTR(Complex);
    for (auto& [name, obj] : objects) {
      c->objects.emplace(name, obj->Clone());
    }
    return c;
  }
};

struct ComplexField : Pointer {
  static LiveArgument complex_arg;
  static LiveArgument label_arg;
  string_view Name() const override { return "Complex Field"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(ComplexField); }
  void Args(std::function<void(Argument&)> cb) override {
    cb(complex_arg);
    cb(label_arg);
  }
  Object* Next(Location& unused_error_context) const override {
    auto [complex, label] = FollowComplex(*here.lock());
    if (complex == nullptr) {
      return nullptr;
    }
    auto it = complex->objects.find(label);
    if (it == complex->objects.end()) {
      return nullptr;
    }
    return it->second.get();
  }
  void PutNext(Location& error_context, Ptr<Object> obj) override {
    auto [complex, label] = FollowComplex(*here.lock());
    if (complex == nullptr) {
      return;
    }
    complex->objects[label] = std::move(obj);
  }
  Ptr<Object> TakeNext(Location& error_context) override {
    auto [complex, label] = FollowComplex(*here.lock());
    auto it = complex->objects.find(label);
    if (it == complex->objects.end()) {
      return nullptr;
    }
    auto obj = std::move(it->second);
    complex->objects.erase(it);
    return obj;
  }
  void Updated(Location& here, Location& updated) override {
    // Complex was updated - so let's propagate the update.
    here.ScheduleUpdate();
  }

 private:
  // Return Complex pointed by this object. If not possible report error and
  // return nullptr.
  static std::pair<Complex*, std::string> FollowComplex(Location& here) {
    auto label = label_arg.GetObject(here);
    std::string label_text = "";
    Complex* return_complex = nullptr;
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

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

struct Text : LiveObject {
  struct RefChunk {
    LiveArgument arg;
  };
  using Chunk = std::variant<string, RefChunk>;
  vector<Chunk> chunks;
  static auto Parse(string_view text) -> std::vector<Chunk> {
    std::vector<Chunk> chunks;
    std::regex ref_placeholder_re(R"(\{([^\}]+)\})");
    std::regex_iterator<string_view::const_iterator> begin(text.begin(), text.end(),
                                                           ref_placeholder_re);
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
  static LiveArgument target_arg;
  string_view Name() const override { return "Text Editor"; }
  Ptr<Object> Clone() const override {
    auto other = MAKE_PTR(Text);
    other->chunks = chunks;
    return other;
  }
  void Args(std::function<void(Argument&)> cb) override {
    cb(target_arg);
    for (Chunk& chunk : chunks) {
      if (auto ref = std::get_if<RefChunk>(&chunk)) {
        cb(ref->arg);
      }
    }
  }
  string GetText() const override {
    string buffer;
    for (const std::variant<string, RefChunk>& chunk : chunks) {
      std::visit(overloaded{[&](const string& text) { buffer += text; },
                            [&](const RefChunk& ref) {
                              auto arg = ref.arg.GetObject(*here.lock());
                              if (arg.ok) {
                                buffer += arg.object->GetText();
                              } else {
                                buffer += f("{{{}}}", ref.arg.name);
                              }
                            }},
                 chunk);
    }
    return buffer;
  }
  void SetText(Location& error_context, string_view new_text) override {
    string old_text = GetText();
    if (old_text == new_text) return;
    chunks.clear();
    chunks = Parse(new_text);
    // chunks.emplace_back(string(new_text));
    if (auto h = here.lock()) {
      auto target = target_arg.GetLocation(*h);
      if (target.location) {
        target.location->SetText(new_text);
      }
    }
  }
  void Updated(Location& here, Location& updated) override {
    auto target = target_arg.GetLocation(here);
    if (target.location == &updated) {
      SetText(here, updated.GetText());
    }
  }
};

struct Button : Object, Runnable {
  string label;
  static Argument enabled_arg;
  string_view Name() const override { return "Button"; }
  Ptr<Object> Clone() const override {
    auto other = MAKE_PTR(Button);
    other->label = label;
    return other;
  }
  string GetText() const override { return label; }
  void SetText(Location& error_context, string_view new_label) override { label = new_label; }
  void OnRun(Location& h, RunTask&) override {
    auto enabled = enabled_arg.GetObject(h);
    if (enabled.object && enabled.object->GetText() == "false") {
      h.ReportError("Button is disabled.");
    }
  }
};

struct ComboBox : LiveObject {
  static LiveArgument options_arg;
  Location* here = nullptr;
  Location* selected = nullptr;
  string_view Name() const override { return "Combo Box"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(ComboBox); }
  void Args(std::function<void(Argument&)> cb) override { cb(options_arg); }
  void Relocate(Location* here) override { this->here = here; }
  string GetText() const override { return selected->GetText(); }
  void SetText(Location& error_context, string_view new_text) override {
    selected = options_arg.LoopLocations<Location*>(*here, [&](Location& option) -> Location* {
      if (option.GetText() == new_text) {
        return &option;
      }
      return nullptr;
    });
    if (selected == nullptr) {
      error_context.ReportError(
          f("No option named {}", std::string(new_text.data(), new_text.size())));
    }
  }
  void ConnectionAdded(Location& here, Connection& connection) override {
    LiveObject::ConnectionAdded(here, connection);
    if (selected == nullptr && &connection.argument == &options_arg) {
      auto option = options_arg.GetLocation(here);
      selected = option.location;
    }
  }
};

struct Slider : LiveObject {
  static LiveArgument min_arg;
  static LiveArgument max_arg;
  double value = 0;
  string_view Name() const override { return "Slider"; }
  Ptr<Object> Clone() const override {
    auto s = MAKE_PTR(Slider);
    s->value = value;
    return s;
  }
  void Args(std::function<void(Argument&)> cb) override {
    cb(min_arg);
    cb(max_arg);
  }
  void Updated(Location& here, Location& updated) override {
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
  void SetText(Location& error_context, string_view new_text) override {
    double new_value = std::stod(string(new_text));
    auto min = min_arg.GetLocation(*here.lock());
    if (min.location) {
      double min_val = min.location->GetNumber();
      new_value = std::max(new_value, min_val);
    }
    auto max = max_arg.GetLocation(*here.lock());
    if (max.location) {
      double max_val = max.location->GetNumber();
      new_value = std::min(new_value, max_val);
    }
    value = new_value;
  }
};

struct ProgressBar : library::Number {
  ProgressBar(gui::Widget* parent) : library::Number(parent) {}
  string_view Name() const override { return "Progress Bar"; }
  Ptr<Object> Clone() const override {
    auto bar = MAKE_PTR(ProgressBar, parent);
    bar->value = value;
    return bar;
  }
  void Draw(SkCanvas& canvas) const override { Object::FallbackWidget::Draw(canvas); }
  SkPath Shape() const override { return Object::FallbackWidget::Shape(); }
};

struct ListView : Pointer {
  static LiveArgument list_arg;
  int index = -1;
  string_view Name() const override { return "List View"; }
  Ptr<Object> Clone() const override {
    Ptr<Object> clone = MAKE_PTR(ListView);
    dynamic_cast<ListView&>(*clone).index = index;
    return clone;
  }
  void Args(std::function<void(Argument&)> cb) override { cb(list_arg); }
  void Select(int new_index) {
    if (new_index != index) {
      index = new_index;
      here.lock()->ScheduleUpdate();
    }
  }
  Object* Next(Location& error_context) const override {
    if (index < 0) return nullptr;
    auto list = list_arg.GetTyped<AbstractList>(error_context);
    if (!list.ok) {
      return nullptr;
    }
    int size = 0;
    if (auto err = list.typed->GetSize(size)) {
      return nullptr;
    }
    if (index >= size) return nullptr;
    Object* obj = nullptr;
    if (auto err = list.typed->GetAtIndex(index, obj)) {
      return nullptr;
    }
    return obj;
  }
  void PutNext(Location& error_context, Ptr<Object> obj) override {
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
  Ptr<Object> TakeNext(Location& error_context) override {
    auto list = list_arg.GetTyped<AbstractList>(error_context);
    if (!list.ok) {
      return nullptr;
    }
    int size = 0;
    list.typed->GetSize(size);
    if (index < 0 || index >= size) return nullptr;
    Ptr<Object> obj;
    list.typed->TakeAtIndex(index, false, obj);
    if (index >= size) --index;
    return obj;
  }
};

////////////////
// Algebra
////////////////

// Interface for algebra callbacks.
struct AlgebraContext : algebra::Context {
  Location* location;
  AlgebraContext(Location* location) : location(location) {}
  double RetrieveVariable(string_view variable) override {
    auto target = Argument(variable, Argument::kRequiresObject).GetObject(*location);
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
  std::unique_ptr<algebra::Statement> statement = nullptr;
  string_view Name() const override { return "Formula"; }
  Ptr<Object> Clone() const override {
    auto other = MAKE_PTR(Blackboard);
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
  void SetText(Location& error_context, string_view text) override {
    statement = algebra::ParseStatement(text);
  }
};

struct BlackboardUpdater : LiveObject {
  std::unordered_map<string, std::unique_ptr<algebra::Expression>> formulas;
  std::map<string, LiveArgument> independent_variable_args;
  static Argument const_arg;

  Ptr<Object> Clone() const override { return MAKE_PTR(BlackboardUpdater); }
  void Args(std::function<void(Argument&)> cb) override {
    for (auto& arg : independent_variable_args) {
      cb(arg.second);
    }
  }
  void Relocate(Location* here) override {
    // 1. Find nearby blackboards & register as an observer.
    // 2. Extract variables from math statements.
    if (auto parent_machine = here->ParentAs<Machine>()) {
      parent_machine->Nearby(here->position, HUGE_VALF, [&](Location& other) -> void* {
        if (Blackboard* blackboard = other.As<Blackboard>()) {
          here->ObserveUpdates(other);
          if (algebra::Equation* equation =
                  dynamic_cast<algebra::Equation*>(blackboard->statement.get())) {
            // fmt::print("Found equation: {}\n", equation->GetText());
            treemath::Tree tree(*equation);
            vector<algebra::Variable*> variables = ExtractVariables(equation);
            std::unordered_set<algebra::Variable*> independent_variables;
            // 3. Derive formulas for each of the variables.
            for (algebra::Variable* variable : variables) {
              if (treemath::Variable* tree_var = tree.FindVariable(variable->name)) {
                if (auto expr = tree_var->DeriveExpression(nullptr)) {
                  // fmt::print("Derived formula for {}: {}\n", variable->name,
                  // expr->GetText());
                  formulas[variable->name] = std::move(expr);
                  for (algebra::Variable* independent :
                       ExtractVariables(formulas[variable->name].get())) {
                    independent_variables.insert(independent);
                  }
                }
              }
            }
            // 4. Observe all of the independent variables.
            for (algebra::Variable* variable : independent_variables) {
              independent_variable_args.emplace(
                  variable->name, LiveArgument(variable->name, Argument::kRequiresObject));
            }
          }
        }
        return nullptr;
      });
    }
    LiveObject::Relocate(here);
  }
  string_view Name() const override { return "Blackboard Updater"; }
  void Updated(Location& here, Location& updated) override {
    // LOG << "MathEngine::Updated(" << here.HumanReadableName() << ", " <<
    // updated.HumanReadableName() << ")";
    NoSchedulingGuard guard(here);

    // FIXME: Find the name that the user assigned to the updated object!
    Str updated_name = updated.ToStr();  // placeholder just to make it compile

    if (double num = updated.GetNumber(); !std::isnan(num)) {
      // The list of variables that have changed in response could be
      // precomputed.
      for (auto& [name, expr] : formulas) {
        vector<algebra::Variable*> independent_variables = algebra::ExtractVariables(expr.get());
        for (algebra::Variable* independent_var : independent_variables) {
          if (independent_var->name == updated_name) {
            AlgebraContext context{&here};
            auto arg_it = independent_variable_args.find(name);
            if (arg_it == independent_variable_args.end()) {
              here.ReportError(
                  "Couldn't find LiveArgument for a variable. "
                  "This shouldn't happen.");
              continue;
            }
            auto target = arg_it->second.GetObject(here);
            if (target.object) {
              if (target.location->incoming.find(&const_arg) != target.location->incoming.end()) {
                // LOG << "  would write to " << *target
                //     << " but it's marked as const.";

              } else {
                double new_value = expr->Eval(&context);
                if (isnan(new_value)) {
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

}  // namespace automat
