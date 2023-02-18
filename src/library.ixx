export module library;

import "fmt/format.h";
import <memory>;
import <regex>;
import <chrono>;
import <math.h>;
import <cstdio>;
import <string_view>;
import <variant>;
import <cstdint>;
import <map>;
import <string>;
import <unordered_map>;
import <unordered_set>;
import <vector>;
import <source_location>;
import algebra;
import base;
import treemath;
import error;
import log;

export namespace automaton {

struct Integer : Object {
  int32_t i;
  Integer(int32_t i = 0) : i(i) {}
  static const Integer proto;
  string_view Name() const override { return "Integer"; }
  std::unique_ptr<Object> Clone() const override { return std::make_unique<Integer>(i); }
  string GetText() const override { return std::to_string(i); }
  void SetText(Handle &error_context, string_view text) override {
    i = std::stoi(string(text));
  }
};

struct Number : Object {
  double value;
  Number(double x = 0) : value(x) {}
  static const Number proto;
  string_view Name() const override { return "Number"; }
  std::unique_ptr<Object> Clone() const override { return std::make_unique<Number>(value); }
  string GetText() const override { return std::to_string(value); }
  void SetText(Handle &error_context, string_view text) override {
    value = std::stod(string(text));
  }
};

struct Increment : Object {
  static const Increment proto;
  string_view Name() const override { return "Increment"; }
  std::unique_ptr<Object> Clone() const override { return std::make_unique<Increment>(); }
  void Run(Handle &h) override {
    if (auto target = h.Find("target")) {
      if (auto i = dynamic_cast<Integer *>(target->object.get())) {
        i->i++;
        target->ScheduleUpdate();
      }
    }
  }
};

struct Delete : Object {
  static const Delete proto;
  string_view Name() const override { return "Delete"; }
  std::unique_ptr<Object> Clone() const override { return std::make_unique<Delete>(); }
  void Run(Handle &self) override { self.Find("target")->Take(); }
};

struct Set : Object {
  static const Set proto;
  string_view Name() const override { return "Set"; }
  std::unique_ptr<Object> Clone() const override { return std::make_unique<Set>(); }
  void Run(Handle &self) override {
    auto clone = self.Find("value")->Follow()->Clone();
    self.Find("target")->Put(std::move(clone));
  }
};

struct Date : Object {
  static const Date proto;
  int year;
  int month;
  int day;
  Date(int year, int month, int day) : year(year), month(month), day(day) {}
  string_view Name() const override { return "Date"; }
  std::unique_ptr<Object> Clone() const override { return std::make_unique<Date>(year, month, day); }
  string GetText() const override {
    return fmt::format("{:04d}-{:02d}-{:02d}", year, month, day);
  }
  void SetText(Handle &error_context, string_view text) override {
    std::regex re(R"((\d{4})-(\d{2})-(\d{2}))");
    std::match_results<string_view::const_iterator> match;
    if (std::regex_match(text.begin(), text.end(), match, re)) {
      year = std::stoi(match[1]);
      month = std::stoi(match[2]);
      day = std::stoi(match[3]);
    } else {
      error_context.ReportError("Invalid date format. The Date object expects dates in "
                      "the format YYYY-MM-DD. The provided date was: " +
                      string(text) + ".");
    }
  }
  std::partial_ordering operator<=>(const Object &other) const noexcept override {
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
  std::multimap<TimePoint, Handle *> schedule;

  void SetNow(TimePoint time) {
    this->now = time;
    while (!schedule.empty() && schedule.begin()->first <= now) {
      auto [time, handle] = *schedule.begin();
      schedule.erase(schedule.begin());
      handle->ScheduleRun();
    }
  }
  void RunAfter(Duration duration, Handle &handle) {
    schedule.emplace(now + duration, &handle);
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
  void ScheduleNextRun(Handle &self) {
    using namespace std::chrono_literals;
    if (fake_time) {
      fake_time->RunAfter(1ms, self);
    } else {
      // TODO: Use std::this_thread::sleep_for
    }
  }
  string GetText() const override {
    using namespace std::chrono_literals;
    TimePoint now = fake_time ? fake_time->now : Clock::now();
    Duration elapsed = now - start;
    return fmt::format("{:.3f}", elapsed.count());
  }
  void Run(Handle &self) override {
    using namespace std::chrono_literals;
    TimePoint now = fake_time ? fake_time->now : Clock::now();
    if (now - last_tick >= 1ms) {
      last_tick = now;
      self.ScheduleUpdate();
    }
    ScheduleNextRun(self);
  }
  void Reset(Handle &self) {
    if (fake_time) {
      start = fake_time->now;
    } else {
      start = Clock::now();
    }
    last_tick = start;
    self.ScheduleUpdate();
    ScheduleNextRun(self);
  }
};

struct TimerReset : Object {
  static const TimerReset proto;
  string_view Name() const override { return "TimerReset"; }
  std::unique_ptr<Object> Clone() const override { return std::make_unique<TimerReset>(); }
  void Run(Handle &self) override {
    if (auto timer = self.Find("timer")) {
      if (auto timer_object = timer->As<Timer>()) {
        timer_object->Reset(*timer);
      }
    }
  }
};

struct EqualityTest : Object {
  static const EqualityTest proto;
  bool state = true;
  EqualityTest() {}
  string_view Name() const override { return "Equality Test"; }
  std::unique_ptr<Object> Clone() const override {
    auto other = std::make_unique<EqualityTest>();
    other->state = state;
    return other;
  }
  string GetText() const override { return state ? "true" : "false"; }
  void ConnectionAdded(Handle &self, string_view label,
                                    Connection &connection) override {
    if (label != "target")
      return;
    self.ObserveUpdates(connection.to);
    self.ScheduleLocalUpdate(connection.to);
  }
  void Updated(Handle &self, Handle &updated) override {
    bool new_state = true;
    self.FindAll("target", [&](Handle &target) -> void * {
      if (&target == &updated) {
        return nullptr;
      }
      if ((target <=> updated) != 0) {
        new_state = false;
        return &target; // return non-null to break out of FindAll
      }
      return nullptr;
    });
    if (state != new_state) {
      state = new_state;
      self.ScheduleUpdate();
    }
  }
};

struct LessThanTest : Object {
  static const LessThanTest proto;
  bool state = true;
  LessThanTest() {}
  string_view Name() const override { return "Less Than Test"; }
  std::unique_ptr<Object> Clone() const override {
    auto other = std::make_unique<LessThanTest>();
    other->state = state;
    return other;
  }
  string GetText() const override { return state ? "true" : "false"; }
  void ConnectionAdded(Handle &self, string_view label,
                                    Connection &connection) override {
    if (label == "less" || label == "than") {
      self.ObserveUpdates(connection.to);
      self.ScheduleLocalUpdate(connection.to);
    }
  }
  void Updated(Handle &self, Handle &updated) override {
    Handle *less = self.Find("less");
    if (less == nullptr)
      return;
    Handle *than = self.Find("than");
    if (than == nullptr)
      return;
    bool new_state = *less->object < *than->object;
    if (state != new_state) {
      state = new_state;
      self.ScheduleUpdate();
    }
  }
};

struct StartsWithTest : Object {
  static const StartsWithTest proto;
  bool state = true;
  StartsWithTest() {}
  string_view Name() const override { return "Starts With Test"; }
  std::unique_ptr<Object> Clone() const override {
    auto other = std::make_unique<StartsWithTest>();
    other->state = state;
    return other;
  }
  string GetText() const override { return state ? "true" : "false"; }
  void ConnectionAdded(Handle &self, string_view label,
                                    Connection &connection) override {
    if (label == "starts" || label == "with") {
      self.ObserveUpdates(connection.to);
      self.ScheduleLocalUpdate(connection.to);
    }
  }
  void Updated(Handle &self, Handle &updated) override {
    Handle *starts = self.Find("starts");
    if (starts == nullptr)
      return;
    Handle *with = self.Find("with");
    if (with == nullptr)
      return;
    Object *starts_obj = starts->Follow();
    if (starts_obj == nullptr)
      return;
    Object *with_obj = with->Follow();
    if (with_obj == nullptr)
      return;
    bool new_state = starts_obj->GetText().starts_with(with_obj->GetText());
    if (state != new_state) {
      state = new_state;
      self.ScheduleUpdate();
    }
  }
};

struct AllTest : Object {
  static const AllTest proto;
  bool state = true;
  AllTest() {}
  string_view Name() const override { return "All Test"; }
  std::unique_ptr<Object> Clone() const override {
    auto other = std::make_unique<AllTest>();
    other->state = state;
    return other;
  }
  string GetText() const override { return state ? "true" : "false"; }
  void ConnectionAdded(Handle &self, string_view label,
                                Connection &connection) override {
    if (label != "test")
      return;
    self.ObserveUpdates(connection.to);
    self.ScheduleLocalUpdate(connection.to);
  }
  void Updated(Handle &self, Handle &updated) override {
    bool new_state = true;
    self.FindAll("test", [&](Handle &test) -> void * {
      if (test.GetText() != "true") {
        new_state = false;
        return &test; // return non-null to break out of FindAll
      }
      return nullptr;
    });
    if (state != new_state) {
      state = new_state;
      self.ScheduleUpdate();
    }
  }
};

struct Switch : Object {
  static const Switch proto;
  Handle *self = nullptr;
  string_view Name() const override { return "Switch"; }
  std::unique_ptr<Object> Clone() const override { return std::make_unique<Switch>(); }
  void Rehandle(Handle *parent) override { self = parent; }
  string GetText() const override {
    if (auto target = self->Find("target")) {
      auto value = target->GetText();
      if (auto case_ = self->Find(value)) {
        return case_->GetText();
      }
      self->ReportError("Switch object has no case for value: " + value + ".");
    } else {
      self->ReportError("Switch object has no target.");
    }
    return "";
  }
};

struct ErrorReporter : Object {
  static const ErrorReporter proto;
  string_view Name() const override { return "Error Reporter"; }
  std::unique_ptr<Object> Clone() const override { return std::make_unique<ErrorReporter>(); }
  void ConnectionAdded(Handle &self, string_view label,
                                      Connection &connection) override {
    if (label == "message" || label == "test" || label == "target") {
      self.ObserveUpdates(connection.to);
      self.ScheduleLocalUpdate(connection.to);
    }
  }
  void Updated(Handle &self, Handle &updated) override {
    self.ClearError();
    if (auto test = self.Find("test")) {
      if (test->GetText() == "true") {
        if (auto message = self.Find("message")) {
          self.ReportError(message->GetText());
        } else {
          self.ReportError("Error reported by Error Reporter object.");
        }
      }
    }
  }
};

struct HealthTest : Object {
  static const HealthTest proto;
  bool state = true;
  HealthTest() {}
  std::unique_ptr<Object> Clone() const override { return std::make_unique<HealthTest>(); }
  void Rehandle(Handle *self) override {
    if (self->parent) {
      self->ObserveErrors(*self->parent);
      state = !self->parent->HasError();
    }
  }
  string_view Name() const override { return "Health Test"; }
  string GetText() const override { return state ? "true" : "false"; }
  void Errored(Handle &self, Handle &errored) override {
    state = false;
    self.ScheduleUpdate();
  }
};

struct AbstractList {
  virtual Error* GetAtIndex(int index, Object*& obj) = 0;
  virtual Error* PutAtIndex(int index, bool overwrite, std::unique_ptr<Object> obj) = 0;
  virtual Error* TakeAtIndex(int index, bool leave_null, std::unique_ptr<Object>& obj) = 0;
  virtual Error* GetSize(int& size) = 0;
};

struct Append : Object {
  static const Append proto;
  string_view Name() const override { return "Append"; }
  std::unique_ptr<Object> Clone() const override { return std::make_unique<Append>(); }
  void Run(Handle &self) override {
    if (auto list = self.Find("to")) {
      if (auto list_object = list->As<AbstractList>()) {
        int size = 0;
        if (auto error = list_object->GetSize(size)) {
          self.error.reset(error);
          return;
        }
        if (auto what = self.Find("what")) {
          if (auto obj = what->Take()) {
            if (auto error = list_object->PutAtIndex(size, false, obj->Clone())) {
              self.error.reset(error);
              return;
            }
          }
        }
      }
    }
  }
};

struct List : Object, AbstractList {
  static const List proto;
  Handle *self = nullptr;
  vector<unique_ptr<Object>> objects;
  string_view Name() const override { return "List"; }
  std::unique_ptr<Object> Clone() const override {
    auto list = std::make_unique<List>();
    for (auto &object : objects) {
      list->objects.emplace_back(object->Clone());
    }
    return list;
  }
  void Rehandle(Handle *self) override { this->self = self; }
  Error* GetAtIndex(int index, Object*& obj) override {
    if (index < 0 || index >= objects.size()) {
      return self->ReportError("Index out of bounds.");
    }
    obj = objects[index].get();
    return nullptr;
  }
  Error* PutAtIndex(int index, bool overwrite, std::unique_ptr<Object> obj) override {
    if (index < 0 || (overwrite ? index >= objects.size() : index > objects.size())) {
      // TODO: save the object in the error - it shouldn't be destroyed!
      return self->ReportError("Index out of bounds.");
    }
    if (overwrite) {
      objects[index] = std::move(obj);
    } else {
      objects.insert(objects.begin() + index, std::move(obj));
    }
    self->ScheduleUpdate();
    return nullptr;
  }
  Error* TakeAtIndex(int index, bool keep_null, std::unique_ptr<Object>& obj) override {
    if (index < 0 || index >= objects.size()) {
      return self->ReportError("Index out of bounds.");
    }
    obj = std::move(objects[index]);
    if (!keep_null) {
      objects.erase(objects.begin() + index);
    }
    self->ScheduleUpdate();
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

struct Filter : Object, Iterator, AbstractList {
  enum class Phase {
    kSequential,
    kDone
  };
  static const Filter proto;
  Handle *self = nullptr;
  Phase phase = Phase::kDone;
  int index = 0;
  vector<Object*> objects;
  vector<int> indices;
  string_view Name() const override { return "Filter"; }
  std::unique_ptr<Object> Clone() const override {
    auto filter = std::make_unique<Filter>();
    filter->phase = Phase::kDone;
    filter->index = 0;
    return filter;
  }
  void Rehandle(Handle *self) override { this->self = self; }
  void ConnectionAdded(Handle &self, string_view label,
                        Connection &connection) override {
    if (label != "list" && label != "element" && label != "test") {
      return;
    }
    if (label == "list") {
      self.ObserveUpdates(connection.to);
    }
    auto list = self.Find("list");
    if (list == nullptr) {
      return;
    }
    auto element = self.Find("element");
    if (element == nullptr) {
      return;
    }
    auto test = self.Find("test");
    if (test == nullptr) {
      return;
    }
    StartFiltering();
  }
  void Run(Handle &self) override {
    if (phase == Phase::kSequential) {
      // Check the value of test, possibly copying element from list to output.
      // Then increment index and schedule another iteration.
      auto test = self.Find("test");
      if (test == nullptr) {
        self.ReportError("Tried to Run this Filter but `test` is not connected anywhere.");
        return;
      }
      if (test->GetText() == "true") {
        if (auto obj = GetCurrent()) {
          objects.emplace_back(obj);
          indices.emplace_back(index);
        }
      }
      ++index;
      BeginNextIteration(self);
    } else {
      self.ReportError("Tried to Run this Filter but filtering is already completed.");
    }
  }
  void StartFiltering() {
    objects.clear();
    phase = Phase::kSequential;
    index = 0;
    BeginNextIteration(*self);
  }
  void BeginNextIteration(Handle &self) {
    int list_size = 0;
    self.Find("list")->As<AbstractList>()->GetSize(list_size);
    if (index < list_size) {
      ThenGuard then(std::make_unique<RunTask>(&self));
      self.Find("element")->ScheduleUpdate();
    } else {
      phase = Phase::kDone;
    }
  }
  // Iterator interface
  Object* GetCurrent() const override {
    auto list = self->Find("list")->As<AbstractList>();
    int size = 0;
    if (list->GetSize(size)) {
      return nullptr;
    }
    if (index >= size) {
      return nullptr;
    }
    Object* obj = nullptr;
    if (list->GetAtIndex(index, obj)) {
      return nullptr;
    }
    return obj;
  }
  void Updated(Handle &self, Handle &updated) override {
    // TODO: Cancel the outstanding RunTask
    StartFiltering();
  }
  // AbstractList interface
  Error* GetAtIndex(int index, Object*& obj) override {
    if (index < 0 || index >= objects.size()) {
      return self->ReportError("Index out of bounds.");
    }
    obj = objects[index];
    return nullptr;
  }
  Error* PutAtIndex(int index, bool overwrite, std::unique_ptr<Object> obj) override {
    // Filter doesn't own the objects it contains, so it must pass them to its parent list.
    // In order to insert them at the right position in the parent list, it must also know the mapping between filtered & unfiltered indices.
    // This is not yet implemented.
    // NOTE: object dropped here!
    return self->ReportError("Not implemented yet.");
  }
  Error* TakeAtIndex(int index, bool leave_null, std::unique_ptr<Object>& obj) override {
    if (index < 0 || index >= objects.size()) {
      return self->ReportError("Index out of bounds.");
    }
    int orig_index = indices[index];
    if (Error* err = self->Find("list")->As<AbstractList>()->TakeAtIndex(orig_index, leave_null, obj)) {
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
  static const CurrentElement proto;
  string_view Name() const override { return "Current Element"; }
  std::unique_ptr<Object> Clone() const override { return std::make_unique<CurrentElement>(); }
  Object* Next(Handle& error_context) const override {
    if (auto it = GetIterator(*self)) {
      return it->GetCurrent();
    }
    return nullptr;
  }
  void PutNext(Handle& error_context, std::unique_ptr<Object> obj) override {
    self->ReportError("Tried to put an object to Current Element but it's not possible.");
  }
  std::unique_ptr<Object> TakeNext(Handle& error_context) override {
    self->ReportError("Tried to take an object from Current Element but it's not possible.");
    return nullptr;
  }
 private:
  static Iterator* GetIterator(Handle& self) {
    auto of = self.Find("of");
    if (of == nullptr) {
      self.ReportError("Tried to get the next element but `of` is not connected anywhere.");
      return nullptr;
    }
    auto it = of->As<Iterator>();
    if (it == nullptr) {
      self.ReportError("Tried to get the next element but `of` is not an Iterator.");
      return nullptr;
    }
    return it;
  }
};

// Object with subobjects.
//
// The structure contains named fields and is self-descriptive.
struct Complex : Object {
  static const Complex proto;
  Handle *self;
  Complex() : self(nullptr) {}

  std::unordered_map<std::string, unique_ptr<Object>> objects;
  string_view Name() const override { return "Complex"; }
  std::unique_ptr<Object> Clone() const override {
    auto c = new Complex();
    for (auto &[name, obj] : objects) {
      c->objects.emplace(name, obj->Clone());
    }
    return std::unique_ptr<Object>(c);
  }
  void Rehandle(Handle *new_self) override { self = new_self; }
};

struct ComplexField : Pointer {
  static const ComplexField proto;
  string_view Name() const override { return "Complex Field"; }
  std::unique_ptr<Object> Clone() const override { return std::make_unique<ComplexField>(); }
  Object *Next(Handle &unused_error_context) const override {
    auto [complex, label] = FollowComplex(*self);
    if (complex == nullptr) {
      return nullptr;
    }
    auto it = complex->objects.find(label);
    if (it == complex->objects.end()) {
      return nullptr;
    }
    return it->second.get();
  }
  void PutNext(Handle &error_context, std::unique_ptr<Object> obj) override {
    auto [complex, label] = FollowComplex(*self);
    if (complex == nullptr) {
      return;
    }
    complex->objects[label] = std::move(obj);
  }
  std::unique_ptr<Object> TakeNext(Handle &error_context) override {
    auto [complex, label] = FollowComplex(*self);
    auto it = complex->objects.find(label);
    if (it == complex->objects.end()) {
      return nullptr;
    }
    auto obj = std::move(it->second);
    complex->objects.erase(it);
    return obj;
  }
  void ConnectionAdded(Handle &self, string_view label,
                        Connection &connection) override {
    // TODO: also monitor "label"
    if (label == "complex") {
      self.ObserveUpdates(connection.to);
    }
  }
  void Updated(Handle &self, Handle &updated) override {
    // Complex was updated - so let's propagate the update.
    self.ScheduleUpdate();
  }

 private:
  // Return Complex pointed by this object. If not possible report error and return nullptr.
  static std::pair<Complex*, std::string> FollowComplex(Handle &self) {
    if (auto label = self.Find("label")) {
      std::string label_text = label->GetText();
      if (auto complex = self.Find("complex")) {
        if (auto complex_obj = complex->As<Complex>()) {
          return std::make_pair(complex_obj, label_text);
        } else {
          self.ReportError("The \"complex\" connection should point to a Complex object.");
          return std::make_pair(nullptr, "");
        }
      } else {
        self.ReportError("I need a \"complex\" connection.");
        return std::make_pair(nullptr, "");
      }
    } else {
      self.ReportError("I need a \"label\" connection.");
      return std::make_pair(nullptr, "");
    }
  }
};

//////////////
// Widgets
//////////////

template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

struct Text : Object {
  struct RefChunk {
    string label;
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
        string text_chunk(
            text.substr(parsed_to, match.position() - parsed_to));
        chunks.emplace_back(text_chunk);
      }
      chunks.push_back(RefChunk{match[1].str()});
      parsed_to = match.position() + match.length();
    }
    if (parsed_to < text.size()) {
      string text_chunk(text.substr(parsed_to));
      chunks.emplace_back(text_chunk);
    }
    return chunks;
  }
  static const Text proto;
  Handle *self = nullptr;
  string_view Name() const override { return "Text Editor"; }
  std::unique_ptr<Object> Clone() const override {
    auto other = std::make_unique<Text>();
    other->chunks = chunks;
    return other;
  }
  void Rehandle(Handle *self) override {
    this->self = self;
  }
  string GetText() const override {
    string buffer;
    for (const std::variant<string, RefChunk> &chunk : chunks) {
      std::visit(overloaded{[&](const string &text) { buffer += text; },
                            [&](const RefChunk &ref) {
                              if (auto target = self->Find(ref.label)) {
                                buffer += target->GetText();
                              } else {
                                buffer += fmt::format("{{{}:}}", ref.label);
                              }
                            }},
                chunk);
    }
    return buffer;
  }
  void SetText(Handle &error_context, string_view new_text) override {
    string old_text = GetText();
    if (old_text == new_text)
      return;
    chunks.clear();
    chunks = Parse(new_text);
    // chunks.emplace_back(string(new_text));
    if (self) {
      if (auto target = self->Find("target")) {
        target->SetText(new_text);
      }
    }
  }
  void ConnectionAdded(Handle &self, string_view label,
                            Connection &connection) override {
    if (label != "target")
      return;
    self.ObserveUpdates(connection.to);
    self.ScheduleLocalUpdate(connection.to);
  }
  void Run(Handle &self) override {}
  void Updated(Handle &self, Handle &updated) override {
    SetText(self, updated.GetText());
  }
};

struct Button : Object {
  string label;
  static const Button proto;
  string_view Name() const override { return "Button"; }
  std::unique_ptr<Object> Clone() const override {
    auto other = std::make_unique<Button>();
    other->label = label;
    return other;
  }
  string GetText() const override { return label; }
  void SetText(Handle &error_context, string_view new_label) override {
    label = new_label;
  }
  void Run(Handle &h) override {
    if (auto enabled = h.Find("enabled")) {
      if (enabled->GetText() == "false") {
        h.ReportError("Button is disabled.");
        return;
      }
    }
    if (auto next = h.Find("click")) {
      next->ScheduleRun();
    }
  }
};

struct ComboBox : Object {
  static const ComboBox proto;
  Handle *self = nullptr;
  Handle *selected = nullptr;
  string_view Name() const override { return "Combo Box"; }
  std::unique_ptr<Object> Clone() const override { return std::make_unique<ComboBox>(); }
  void Rehandle(Handle *self) override { this->self = self; }
  string GetText() const override { return selected->GetText(); }
  void SetText(Handle &error_context, string_view new_text) override {
    auto ret = self->FindAll("option", [&](Handle &option) -> void * {
      if (option.GetText() == new_text) {
        selected = &option;
        return &option;
      }
      return nullptr;
    });
    if (ret == nullptr) {
      error_context.ReportError(fmt::format("No option named {}", new_text));
    }
  }
  void ConnectionAdded(Handle &self, string_view label,
                                Connection &connection) override {
    if (selected == nullptr && label == "option") {
      selected = self.Find("option");
    }
  }
};

struct Slider : Number {
  static const Slider proto;
  double min = 0;
  double max = 1;
  string_view Name() const override { return "Slider"; }
  std::unique_ptr<Object> Clone() const override {
    auto s = std::make_unique<Slider>();
    s->min = min;
    s->max = max;
    s->value = value;
    return s;
  }
  void Rehandle(Handle *self) override {
    if (auto min = self->Find("min")) {
      self->ObserveUpdates(*min);
    }
    if (auto max = self->Find("max")) {
      self->ObserveUpdates(*max);
    }
  }
  void Updated(Handle &self, Handle &updated) override {
    if (auto min = self.Find("min")) {
      if (&updated == min) {
        this->min = min->GetNumber();
        value = std::max(value, this->min);
      }
    }
    if (auto max = self.Find("max")) {
      if (&updated == max) {
        this->max = max->GetNumber();
        value = std::min(value, this->max);
      }
    }
  }
};

struct ProgressBar : Number {
  static const ProgressBar proto;
  string_view Name() const override { return "Progress Bar"; }
  std::unique_ptr<Object> Clone() const override {
    auto bar = std::make_unique<ProgressBar>();
    bar->value = value;
    return bar;
  }
};

struct Alert : Object {
  static const Alert proto;
  vector<string> alerts_for_tests;
  string_view Name() const override { return "Alert"; }
  std::unique_ptr<Object> Clone() const override { return std::make_unique<Alert>(); }
  void Run(Handle &self) override {
    if (auto message = self.Find("message")) {
      string text = message->GetText();
      alerts_for_tests.push_back(text);
    }
  }
};

struct ListView : Pointer {
  static const ListView proto;
  Handle* self = nullptr;
  int index = -1;
  string_view Name() const override { return "List View"; }
  std::unique_ptr<Object> Clone() const override {
    std::unique_ptr<Object> clone = std::make_unique<ListView>();
    dynamic_cast<ListView&>(*clone).index = index;
    return clone;
  }
  void Rehandle(Handle *self) override { this->self = self; }
  void Select(int new_index) {
    if (new_index != index) {
      index = new_index;
      self->ScheduleUpdate();
    }
  }
  Object* Next(Handle& error_context) const override {
    if (index < 0) return nullptr;
    auto list_arg = self->Find("list");
    if (list_arg == nullptr) {
      error_context.ReportError("ListView has no \"list\" target.");
      LOG() << "ListView has no \"list\" target.";
      return nullptr;
    }
    auto list = list_arg->As<AbstractList>();
    if (list == nullptr) {
      auto error_msg = fmt::format("ListView \"list\" target is not an AbstractList but a {}.",
                                   list_arg->HumanReadableName());
      error_context.ReportError(error_msg);
      LOG() << error_msg;
      return nullptr;
    }
    int size = 0;
    if (auto err = list->GetSize(size)) {
      return nullptr;
    }
    if (index >= size) return nullptr;
    Object* obj = nullptr;
    if (auto err = list->GetAtIndex(index, obj)) {
      return nullptr;
    }
    return obj;
  }
  void PutNext(Handle& error_context, std::unique_ptr<Object> obj) override {
    auto list = self->Find("list")->As<AbstractList>();
    int size = 0;
    list->GetSize(size);
    if (index < 0) {
      list->PutAtIndex(0, false, std::move(obj));
    } else if (index >= size) {
      list->PutAtIndex(size, false, std::move(obj));
      ++index;
    } else {
      list->PutAtIndex(index, false, std::move(obj));
    }
  }
  std::unique_ptr<Object> TakeNext(Handle& error_context) override {
    auto list = self->Find("list")->As<AbstractList>();
    int size = 0;
    list->GetSize(size);
    if (index < 0 || index >= size) return nullptr;
    std::unique_ptr<Object> obj;
    list->TakeAtIndex(index, false, obj);
    if (index >= size) --index;
    return obj;
  }
};

////////////////
// Algebra
////////////////

// Interface for algebra callbacks.
struct AlgebraContext : algebra::Context {
  Handle *handle;
  AlgebraContext(Handle *handle) : handle(handle) {}
  double RetrieveVariable(string_view variable) override {
    if (auto target = handle->Find(variable)) {
      string s = target->GetText();
      return std::stod(s);
    } else {
      handle->ReportMissing(variable);
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
  void SetText(Handle &error_context, string_view text) override {
    statement = algebra::ParseStatement(text);
  }
};

/////////////
// Misc
/////////////

struct BlackboardUpdater : Object {
  static const BlackboardUpdater proto;
  std::unordered_map<string, unique_ptr<algebra::Expression>> formulas;

  std::unique_ptr<Object> Clone() const override { return std::make_unique<BlackboardUpdater>(); }
  void Rehandle(Handle *self) override {
    // 1. Find nearby blackboards & register as an observer.
    // 2. Extract variables from math statements.
    self->Nearby([&](Handle &other) -> void * {
      if (Blackboard *blackboard = other.As<Blackboard>()) {
        self->ObserveUpdates(other);
        if (algebra::Equation *equation = dynamic_cast<algebra::Equation *>(
                blackboard->statement.get())) {
          // fmt::print("Found equation: {}\n", equation->GetText());
          treemath::Tree tree(*equation);
          vector<algebra::Variable *> variables =
              ExtractVariables(equation);
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
            if (Handle *handle = self->Find(variable->name)) {
              self->ObserveUpdates(*handle);
            }
          }
        }
      }
      return nullptr;
    });
  }
  string_view Name() const override { return "Blackboard Updater"; }
  void Run(Handle &self) override {}
  void Updated(Handle &self, Handle &updated) override {
    // LOG << "MathEngine::Updated(" << self.HumanReadableName() << ", " <<
    // updated.HumanReadableName() << ")";
    NoSchedulingGuard guard(self);
    if (double num = updated.GetNumber(); !std::isnan(num)) {
      // The list of variables that have changed in response could be
      // precomputed.
      for (auto &[name, expr] : formulas) {
        vector<algebra::Variable *> independent_variables =
            algebra::ExtractVariables(expr.get());
        for (algebra::Variable *independent_var : independent_variables) {
          if (independent_var->name == updated.name) {
            AlgebraContext context{&self};
            if (auto *target = self.Find(name)) {
              if (target->incoming.find("const") != target->incoming.end()) {
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
                  target->SetNumber(new_value);
                }
              }
            }
          }
        }
      }
    }
  }
};

/////////////////
// Prototypes
/////////////////

const Integer Integer::proto;
const Number Number::proto;
const Increment Increment::proto;
const Delete Delete::proto;
const Set Set::proto;
const Date Date::proto(2022, 1, 1);
const Timer Timer::proto;
const TimerReset TimerReset::proto;
const EqualityTest EqualityTest::proto;
const LessThanTest LessThanTest::proto;
const StartsWithTest StartsWithTest::proto;
const AllTest AllTest::proto;
const Switch Switch::proto;
const ErrorReporter ErrorReporter::proto;
const HealthTest HealthTest::proto;
const Text Text::proto;
const Button Button::proto;
const ComboBox ComboBox::proto;
const Slider Slider::proto;
const ProgressBar ProgressBar::proto;
const Alert Alert::proto;
const ListView ListView::proto;
const Blackboard Blackboard::proto;
const BlackboardUpdater BlackboardUpdater::proto;
const Append Append::proto;
const List List::proto;
const Filter Filter::proto;
const CurrentElement CurrentElement::proto;
const Complex Complex::proto;
const ComplexField ComplexField::proto;

} // namespace automaton
