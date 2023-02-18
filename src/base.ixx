export module base;

import "fmt/format.h";
import <memory>;
import <algorithm>;
import <deque>;
import <string>;
import <cassert>;
import <compare>;
import <functional>;
import <string_view>;
import <unordered_map>;
import <unordered_set>;
import <source_location>;
import <vector>;
import log;
import error;

export namespace automaton {

using std::function;
using std::hash;
using std::string;
using std::string_view;
using std::unique_ptr;
using std::unordered_multimap;
using std::unordered_set;
using std::vector;

struct Handle;
struct Machine;

struct Connection {
  Handle &from, &to;
  // Connections typically follow objects that point somewhere. This can be
  // prevented using the `from_direct` & `to_direct` flags.
  bool from_direct, to_direct;
  Connection(Handle &from, Handle &to, bool from_direct, bool to_direct)
      : from(from), to(to), from_direct(from_direct), to_direct(to_direct) {}
};

struct Pointer;

// Objects are interactive pieces of data & behavior.
//
// Instances of this class provide custom logic & appearance.
struct Object {

  // The name for objects of this type. English proper noun, UTF-8, capitalized.
  // For example: "Text Editor".
  virtual string_view Name() const = 0;

  // Create a copy of this object.
  //
  // Subclasses of Object should have a static `proto` field, holding their own
  // prototype instance. This prototype will be cloned when new objects are
  // created. See the `Machine::Create` function for details.
  virtual std::unique_ptr<Object> Clone() const = 0;

  virtual void Rehandle(Handle *new_self) {}

  // Release the memory occupied by this object.
  virtual ~Object() = default;

  virtual string GetText() const { return ""; }
  virtual void SetText(Handle &error_context, string_view text) {}
  // virtual void SetText(Handle &error_context, string_view text) {
  //   auto error_message = fmt::format("{} doesn't support text input.", Name());
  //   error_context.ReportError(error_message);
  // }

  // Pointer-like objects can be followed.
  virtual Pointer* AsPointer() { return nullptr; }

  virtual void ConnectionAdded(Handle &self, string_view label,
                               Connection &connection) {}
  virtual void Run(Handle &self) {}
  virtual void Updated(Handle &self, Handle &updated) { Run(self); }
  virtual void Errored(Handle &self, Handle &errored) {}
  virtual std::partial_ordering operator<=>(const Object &other) const noexcept {
    return GetText() <=> other.GetText();
  }
};

struct Pointer : Object {
  Handle* self = nullptr;
  virtual Object* Next(Handle& error_context) const = 0;
  virtual void PutNext(Handle& error_context, std::unique_ptr<Object> obj) = 0;
  virtual std::unique_ptr<Object> TakeNext(Handle& error_context) = 0;

  std::pair<Pointer&, Object*> FollowPointers(Handle& error_context) const {
    const Pointer* ptr = this;
    Object *next = Next(error_context);
    while (next != nullptr) {
      if (Pointer* next_ptr = next->AsPointer()) {
        ptr = next_ptr;
        next = next_ptr->Next(error_context);
      } else {
        break;
      }
    }
    return {*const_cast<Pointer*>(ptr), next};
  }
  Object* Follow(Handle& error_context) const {
    return FollowPointers(error_context).second;
  }
  void Put(Handle& error_context, std::unique_ptr<Object> obj) {
    FollowPointers(error_context).first.PutNext(error_context, std::move(obj));
  }
  std::unique_ptr<Object> Take(Handle& error_context) {
    return FollowPointers(error_context).first.TakeNext(error_context);
  }

  Pointer* AsPointer() override { return this; }
  void Rehandle(Handle *self) override { this->self = self; }
  string GetText() const override {
    if (auto *obj = Follow(*self)) {
      return obj->GetText();
    } else {
      return "null";
    }
  }
  void SetText(Handle &error_context, string_view text) override;
};

template <typename T> struct ptr_hash {
  using is_transparent = void;

  auto operator()(T *p) const { return hash<T *>{}(p); }
  auto operator()(const unique_ptr<T> &p) const { return hash<T *>{}(p.get()); }
};

template <typename T> struct ptr_equal {
  using is_transparent = void;

  template <typename LHS, typename RHS>
  auto operator()(const LHS &lhs, const RHS &rhs) const {
    return AsPtr(lhs) == AsPtr(rhs);
  }

private:
  static const T *AsPtr(const T *p) { return p; }
  static const T *AsPtr(const unique_ptr<T> &p) { return p.get(); }
};

template<typename T>
std::unique_ptr<Object> Create() {
  return T::proto.Clone();
}

// 2D Canvas holding objects & a spaghetti of connections.
struct Machine : Object {
  static const Machine proto;
  Machine() = default;
  Handle *self = nullptr;
  string name = "";
  unordered_set<unique_ptr<Handle>> handles;
  vector<Handle *> front;
  vector<Handle *> children_with_errors;

  Handle &CreateEmpty(const string &name = "");
  Handle &Create(const Object &prototype, const string &name = "");

  // Create an instance of T and return its handle.
  //
  // The new instance is created from a prototype instance in `T::proto`.
  template <typename T> Handle &Create(const string &name = "") {
    return Create(T::proto, name);
  }

  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Rehandle(Handle *parent) override;
  void Errored(Handle &self, Handle &errored) override;
  string LoggableString() const;
  Handle *Front(const string &name);

  Handle *operator[](const string &name) {
    auto h = Front(name);
    if (h == nullptr) {
      ERROR() << "Component \"" << name << "\" of " << this->name << " is null!";
    }
    return h;
  }

  void AddToFrontPanel(Handle &h);

  // Report all errors that occured within this machine.
  //
  // This function will return all errors held by handles of this machine &
  // recurse into submachines.
  void Diagnostics(function<void(Handle *, Error &)> error_callback);

  void ReportChildError(Handle &child);
  void ClearChildError(Handle &child);
};

struct string_equal {
  using is_transparent = std::true_type;

  bool operator()(string_view l, string_view r) const noexcept {
    return l == r;
  }
};

struct string_hash {
  using is_transparent = std::true_type;

  auto operator()(string_view str) const noexcept {
    return hash<string_view>()(str);
  }
};

template <typename Value>
using string_multimap =
    unordered_multimap<string, Value, string_hash, string_equal>;

struct Task;

// Each Container holds its inner objects in Handles.
//
// Handle specifies location & relations of an object.
//
// Handles provide common interface for working with Containers of various types
// (2d canvas, 3d space, list, hashmap, etc.). In that sense they are similar
// to C++ iterators.
//
// Implementations of this interface would typically extend it with
// container-specific functions.
struct Handle {
  Handle *parent;

  unique_ptr<Object> object;

  // Name of this Handle.
  string name;

  // Connections of this Handle.
  // Connection is owned by both incoming & outgoing handles.
  string_multimap<Connection *> outgoing;
  string_multimap<Connection *> incoming;

  unordered_set<Handle *> update_observers;
  unordered_set<Handle *> observing_updates;

  unordered_set<Handle *> error_observers;
  unordered_set<Handle *> observing_errors;

  Handle(Handle *parent);

  Object *Create(const Object &prototype);

  template <typename T> T *Create() {
    return dynamic_cast<T *>(Create(T::proto));
  }

  // Remove the objects held by this handle.
  //
  // Some containers may not allow empty handles so this function may also
  // delete the handle. Check the return value.
  Handle *Clear();

  Object* Follow();
  void Put(unique_ptr<Object> obj);
  unique_ptr<Object> Take();

  // Find a related object.
  //
  // Each Container can alter they way that object properties are looked up.
  // This function holds the container-specific look up overrides.
  Handle *Find(string_view property);
  void *FindAll(string_view property, function<void *(Handle &)> callback);

  // Iterate over all nearby objects (including this object).
  //
  // Return non-null from the callback to stop the search.
  void *Nearby(function<void *(Handle &)> callback);

  // This function should register a connection from this handle to the `other`
  // so that subsequent calls to `Find` will return `other`.
  //
  // This function should also notify the object with the `ConnectionAdded`
  // call.
  void ConnectTo(Handle &other, string_view property);

  // Immediately execute this object's Updated function.
  void Updated(Handle &updated);

  // Schedule this object's Updated function to be executed with the `updated`
  // argument.
  void ScheduleLocalUpdate(Handle &updated);

  // Call this function when the value of the object has changed.
  //
  // It will notify all observers & call their `Updated` function.
  //
  // The `Updated` function will not be called immediately but will be scheduled
  // using the task queue.
  void ScheduleUpdate();

  void ObserveUpdates(Handle &other);

  void ObserveErrors(Handle &other);

  string LoggableString() const;

  string GetText();
  double GetNumber();

  // Immediately execute this object's Run function.
  void Run();

  // Add this object to the task queue. Once it's turn comes, its `Run` method
  // will be executed.
  void ScheduleRun();

  // Immediately execute this object's Errored function.
  void Errored(Handle &errored);

  // Execute this object's Errored function using the task queue.
  void ScheduleErrored(Handle &errored);

  Handle *Rename(string_view new_name);

  template <typename T> T *ThisAs() {
    return dynamic_cast<T *>(object.get());
  }
  template <typename T> T *As() {
    return dynamic_cast<T *>(Follow());
  }
  template <typename T> T *ParentAs() const {
    return parent ? dynamic_cast<T *>(parent->object.get()) : nullptr;
  }

  void SetText(string_view text);
  void SetNumber(double number);

  // First error caught by this Handle.
  unique_ptr<Error> error;

  bool HasError();
  Error *GetError();
  Error* ReportError(string_view message, std::source_location location = std::source_location::current()) {
    if (error == nullptr) {
      error.reset(new Error(message, location));
      if (auto machine = ParentAs<Machine>()) {
        machine->ReportChildError(*this);
      }
    }
    return error.get();
  }
  void ClearError();

  // Shorthand function for reporting that a required property couldn't be
  // found.
  void ReportMissing(string_view property);

  string HumanReadableName() const {
    if (name == "") {
      return string(object->Name());
    } else {
      return fmt::format("{0} \"{1}\"", object->Name(), name);
    }
  }

  std::partial_ordering operator<=>(const Handle &other) const noexcept;
};

struct Task {
  Handle* target;
  std::shared_ptr<Task> waiting_task;
  Task(Handle *target);
  virtual ~Task();
  // Add this task to the task queue. This function takes ownership of the task.
  void Schedule();
  void PreExecute();
  void PostExecute();
  virtual std::string Format();
  virtual void Execute() = 0;
  virtual Task* Clone() = 0;
};

struct RunTask : Task {
  RunTask(Handle *target);
  std::string Format() override;
  void Execute() override;
  Task* Clone() override;
};

struct UpdateTask : Task {
  Handle *updated;
  UpdateTask(Handle *target, Handle *updated);
  std::string Format() override;
  void Execute() override;
  Task* Clone() override;
};

struct ErroredTask : Task {
  Handle *errored;
  ErroredTask(Handle *target, Handle *errored);
  std::string Format() override;
  void Execute() override;
  Task* Clone() override;
};

struct ThenGuard {
  std::shared_ptr<Task> then;
  std::shared_ptr<Task>* old_global_then;
  ThenGuard(std::unique_ptr<Task>&& then);
  ~ThenGuard();
};

struct NoSchedulingGuard {
  Handle &handle;
  NoSchedulingGuard(Handle &);
  ~NoSchedulingGuard();
};

void RunLoop();

inline std::ostream &operator<<(std::ostream &os, const Handle &e) {
  return os << e.HumanReadableName();
}


// End of header



Handle::Handle(Handle *parent) : parent(parent) {}

Object *Handle::Create(const Object &prototype) {
  object = prototype.Clone();
  object->Rehandle(this);
  return object.get();
}

Handle *Handle::Clear() {
  object.reset();
  return this;
}

Object *Handle::Follow() {
  if (Pointer *ptr = object->AsPointer()) {
    return ptr->Follow(*this);
  }
  return object.get();
}
void Handle::Put(unique_ptr<Object> obj) {
  if (object == nullptr) {
    object = std::move(obj);
    return;
  }
  if (Pointer *ptr = object->AsPointer()) {
    ptr->Put(*this, std::move(obj));
  } else {
    object = std::move(obj);
  }
}
unique_ptr<Object> Handle::Take() {
  if (Pointer *ptr = object->AsPointer()) {
    return ptr->Take(*this);
  }
  return std::move(object);
}

std::string Handle::LoggableString() const {
  if (name.empty()) {
    return std::string(object->Name());
  } else {
    return fmt::format("{} \"{}\"", object->Name(), name);
  }
}

Handle *Handle::Rename(std::string_view new_name) {
  name = new_name;
  return this;
}

void Handle::ConnectTo(Handle &other, std::string_view label) {
  Connection *c = new Connection(*this, other, false, false);
  outgoing.emplace(label, c);
  other.incoming.emplace(label, c);
  object->ConnectionAdded(*this, label, *c);
}

// Types of objects that sholud work nicely with data updates:
//
// - stateful functions (e.g. X + Y => Z)      Solution: function adds itself to
// the observers list, gets activated up by NotifyUpdated, recalculates its
// value & (maybe) calls NotifyUpdated on itself (or its output object)
// - bi-directional functions (X + 1 = Z)      Solution: same as above but the
// function activation must include direction (!)
// - lazy functions                            Solution: NotifyUpdated traverses
// all lazy nodes & activates their observers
//
// Complexity: O(connections + observers)

void Handle::ObserveUpdates(Handle &other) {
  other.update_observers.insert(this);
  observing_updates.insert(&other);
}

void Handle::ObserveErrors(Handle &other) {
  other.error_observers.insert(this);
  observing_errors.insert(&other);
}

Handle *Handle::Find(std::string_view label) {
  auto frame_it = outgoing.find(label);
  // explicit connection
  if (frame_it != outgoing.end()) {
    return &frame_it->second->to;
  }
  // otherwise, search for other handles in this machine
  return reinterpret_cast<Handle *>(Nearby([&](Handle &other) -> void * {
    if (other.name == label) {
      return &other;
    }
    return nullptr;
  }));
}

void *Handle::FindAll(std::string_view label,
                      std::function<void *(Handle &)> callback) {
  auto [begin, end] = outgoing.equal_range(label);
  for (auto it = begin; it != end; ++it) {
    if (auto ret = callback(it->second->to)) {
      return ret;
    }
  }
  return nullptr;
}

void *Handle::Nearby(std::function<void *(Handle &)> callback) {
  if (auto parent_machine = ParentAs<Machine>()) {
    // TODO: sort by distance
    for (auto &other : parent_machine->handles) {
      if (auto ret = callback(*other)) {
        return ret;
      }
    }
  }
  return nullptr;
}

std::string Handle::GetText() {
  auto *follow = Follow();
  if (follow == nullptr) {
    return "";
  }
  return follow->GetText();
}
double Handle::GetNumber() { return std::stod(GetText()); }

void Handle::SetText(std::string_view text) {
  std::string current_text = GetText();
  if (current_text == text) {
    return;
  }
  Follow()->SetText(*this, text);
  ScheduleUpdate();
}

void Handle::SetNumber(double number) { SetText(fmt::format("{}", number)); }

void Handle::Run() { object->Run(*this); }
void Handle::Updated(Handle &updated) { object->Updated(*this, updated); }
void Handle::Errored(Handle &errored) { object->Errored(*this, errored); }

bool log_executed_tasks = false;

void TaskLogging(bool enable) { log_executed_tasks = enable; }

std::deque<unique_ptr<Task>> queue;
std::unordered_set<Handle *> no_scheduling;
std::shared_ptr<Task> *global_then = nullptr;

bool NoScheduling(Handle *handle) {
  return no_scheduling.find(handle) != no_scheduling.end();
}

void Task::Schedule() {
  if (NoScheduling(target)) {
    delete this;
    return;
  }
  if (log_executed_tasks) {
    LOG() << "Scheduling " << Format();
  }
  queue.emplace_back(this);
}

Task::Task(Handle *target) : target(target) {
  if (global_then) {
    waiting_task = *global_then;
  }
}
Task::~Task() {}
void Task::PreExecute() {
  if (log_executed_tasks) {
    LOG() << Format();
    LOG_Indent();
  }
  if (waiting_task) {
    global_then = &waiting_task;
  }
}
void Task::PostExecute() {
  if (global_then) {
    assert(global_then == &waiting_task);
    global_then = nullptr;
    if (waiting_task.unique()) {
      // std::shared_ptr doesn't permit moving data out of it so we have to make
      // a copy of the task for scheduling.
      waiting_task->Clone()->Schedule();
    }
  }
  if (log_executed_tasks) {
    LOG_Unindent();
  }
}
std::string Task::Format() { return "Task()"; }
RunTask::RunTask(Handle *target) : Task(target) {}
std::string RunTask::Format() {
  return fmt::format("RunTask({})", target->LoggableString());
}
void RunTask::Execute() {
  PreExecute();
  target->Run();
  if (auto then = target->Find("then")) {
    then->ScheduleRun();
  }
  PostExecute();
}
Task *RunTask::Clone() {
  auto t = new RunTask(target);
  t->waiting_task = waiting_task;
  return t;
}
UpdateTask::UpdateTask(Handle *target, Handle *updated)
    : Task(target), updated(updated) {}
std::string UpdateTask::Format() {
  return fmt::format("UpdateTask({}, {})", target->LoggableString(),
                     updated->LoggableString());
}
void UpdateTask::Execute() {
  PreExecute();
  target->Updated(*updated);
  PostExecute();
}
Task *UpdateTask::Clone() {
  auto t = new UpdateTask(target, updated);
  t->waiting_task = waiting_task;
  return t;
}
ErroredTask::ErroredTask(Handle *target, Handle *errored)
    : Task(target), errored(errored) {}
std::string ErroredTask::Format() {
  return fmt::format("ErroredTask({}, {})", target->LoggableString(),
                     errored->LoggableString());
}
void ErroredTask::Execute() {
  PreExecute();
  target->Errored(*errored);
  PostExecute();
}
Task *ErroredTask::Clone() {
  auto t = new ErroredTask(target, errored);
  t->waiting_task = waiting_task;
  return t;
}

ThenGuard::ThenGuard(std::unique_ptr<Task> &&then) : then(std::move(then)) {
  old_global_then = global_then;
  global_then = &this->then;
}
ThenGuard::~ThenGuard() {
  assert(global_then == &then);
  global_then = old_global_then;
  if (then.unique()) {
    then->Clone()->Schedule();
  }
}

NoSchedulingGuard::NoSchedulingGuard(Handle &handle) : handle(handle) {
  no_scheduling.insert(&handle);
}
NoSchedulingGuard::~NoSchedulingGuard() { no_scheduling.erase(&handle); }

void Handle::ScheduleRun() { (new RunTask(this))->Schedule(); }

void Handle::ScheduleLocalUpdate(Handle &updated) {
  (new UpdateTask(this, &updated))->Schedule();
}

void Handle::ScheduleUpdate() {
  for (auto observer : update_observers) {
    observer->ScheduleLocalUpdate(*this);
  }
}

void Handle::ScheduleErrored(Handle &errored) {
  (new ErroredTask(this, &errored))->Schedule();
}

void RunLoop() {
  if (log_executed_tasks) {
    LOG() << "RunLoop(" << queue.size() << " tasks)";
    LOG_Indent();
  }
  while (!queue.empty()) {
    queue.front()->Execute();
    queue.pop_front();
  }
  if (log_executed_tasks) {
    LOG_Unindent();
  }
}

bool Handle::HasError() {
  if (error != nullptr)
    return true;
  if (auto machine = As<Machine>()) {
    if (!machine->children_with_errors.empty())
      return true;
  }
  return false;
}

Error *Handle::GetError() {
  if (error != nullptr)
    return error.get();
  if (auto machine = As<Machine>()) {
    if (!machine->children_with_errors.empty())
      return (*machine->children_with_errors.begin())->GetError();
  }
  return nullptr;
}

void Handle::ClearError() {
  if (error == nullptr) {
    return;
  }
  error.reset();
  if (auto machine = ParentAs<Machine>()) {
    machine->ClearChildError(*this);
  }
}

void Handle::ReportMissing(std::string_view property) {
  auto error_message =
      fmt::format("Couldn't find \"{}\". You can create a connection or rename "
                  "one of the nearby objects to fix this.",
                  property);
  ReportError(error_message);
}

std::partial_ordering Handle::operator<=>(const Handle &other) const noexcept {
  return *object <=> *other.object;
}

const Machine Machine::proto;

} // namespace automaton
