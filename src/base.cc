module base;

import <memory>;
import <string>;
import <algorithm>;
import <functional>;
import <unordered_set>;
import <source_location>;
import "fmt/format.h";
import log;
import error;

namespace automaton {

Connection *Handle::ConnectTo(Handle &other, string_view label) {
  bool to_direct = false;
  if (LiveObject* live_object = ThisAs<LiveObject>()) {
    live_object->Args([&](LiveArgument& arg) {
      if (arg.name == label && arg.precondition >= Argument::kRequiresConcreteType) {
        std::string error;
        arg.CheckRequirements(*this, &other, other.object.get(), error);
        if (error.empty()) {
          to_direct = true;
        }
      }
    });
  }
  Connection *c = new Connection(*this, other, false, to_direct);
  outgoing.emplace(label, c);
  other.incoming.emplace(label, c);
  object->ConnectionAdded(*this, label, *c);
  return c;
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

Handle &Machine::CreateEmpty(const string &name) {
  auto [it, already_present] = handles.emplace(new Handle(self));
  Handle *h = it->get();
  h->name = name;
  return *h;
}

Handle &Machine::Create(const Object &proto, const std::string &name) {
  auto &h = CreateEmpty(name);
  h.Create(proto);
  return h;
}

std::string_view Machine::Name() const { return name; }

std::unique_ptr<Object> Machine::Clone() const {
  Machine *m = new Machine();
  for (auto &my_it : handles) {
    auto &other_h = m->CreateEmpty(my_it->name);
    other_h.Create(*my_it->object);
  }
  return std::unique_ptr<Object>(m);
}

void Machine::Rehandle(Handle *new_self) {
  self = new_self;
  for (auto &it : handles) {
    it->parent = self;
  }
}

void Machine::Errored(Handle &self, Handle &errored) {
  // If the error hasn't been cleared by other Errored calls, then propagate it
  // to the parent.
  if (errored.HasError()) {
    if (auto parent = self.ParentAs<Machine>()) {
      parent->ReportChildError(self);
    } else {
      Error* error = errored.GetError();
      ERROR(error->location) << error->text;
    }
  }
}

std::string Machine::LoggableString() const {
  return fmt::format("Machine({})", name);
}

Handle *Machine::Front(const std::string &name) {
  for (int i = 0; i < front.size(); ++i) {
    if (front[i]->name == name) {
      return front[i];
    }
  }
  return nullptr;
}

void Machine::AddToFrontPanel(Handle &h) {
  if (std::find(front.begin(), front.end(), &h) == front.end()) {
    front.push_back(&h);
  } else {
    ERROR() << "Attempted to add already present " << h << " to " << *this
          << " front panel";
  }
}

void Machine::ReportChildError(Handle &child) {
  children_with_errors.push_back(&child);
  for (Handle *observer : self->error_observers) {
    observer->ScheduleErrored(child);
  }
  self->ScheduleErrored(child);
}

void Machine::ClearChildError(Handle &child) {
  if (auto it = std::find(children_with_errors.begin(),
                          children_with_errors.end(), &child);
      it != children_with_errors.end()) {
    children_with_errors.erase(it);
    if (!self->HasError()) {
      if (auto parent = self->ParentAs<Machine>()) {
        parent->ClearChildError(*self);
      }
    }
  }
}

void Machine::Diagnostics(
    std::function<void(Handle *, Error &)> error_callback) {
  for (auto &handle : handles) {
    if (handle->error) {
      error_callback(handle.get(), *handle->error);
    }
    if (auto submachine = dynamic_cast<Machine *>(handle->object.get())) {
      submachine->Diagnostics(error_callback);
    }
  }
}

void Pointer::SetText(Handle &error_context, string_view text) {
  if (auto *obj = Follow(error_context)) {
    obj->SetText(error_context, text);
  } else {
    error_context.ReportError("Can't set text on null pointer");
  }
}

} // namespace automaton
