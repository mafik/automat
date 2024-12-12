// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "prototypes.hh"

#include "library_flip_flop.hh"
#include "library_hotkey.hh"
#include "library_key_presser.hh"
#include "library_macro_recorder.hh"
#include "library_mouse_click.hh"
#include "library_number.hh"
#include "library_timeline.hh"
#include "library_timer.hh"
#include "object.hh"

using namespace maf;
using namespace automat::library;

namespace automat {

maf::Optional<PrototypeLibrary> prototypes;

struct IndexHelper {
  PrototypeLibrary& lib;
  IndexHelper(PrototypeLibrary& lib) : lib(lib) {}
  template <typename T, typename... Args>
  void Register(Args&&... args) {
    auto proto = std::make_shared<T>(std::forward<Args>(args)...);
    lib.type_index[typeid(T)] = proto;
    lib.name_index[Str(proto->Name())] = proto;
  }
};

PrototypeLibrary::PrototypeLibrary() {
  IndexHelper index(*this);

  index.Register<FlipFlop>();
  index.Register<MacroRecorder>();
  index.Register<TimerDelay>();
  index.Register<HotKey>();
  index.Register<KeyPresser>();
  index.Register<MouseClick>(gui::PointerButton::Left, true);
  index.Register<MouseClick>(gui::PointerButton::Left, false);
  index.Register<MouseClick>(gui::PointerButton::Right, true);
  index.Register<MouseClick>(gui::PointerButton::Right, false);
  index.Register<Number>();
  index.Register<Timeline>();
}

PrototypeLibrary::~PrototypeLibrary() {
  // Some Objects are also Widgets and they need to have `ForgetParents()` called on them to release
  // all their resources.
  for (auto& [name, proto] : name_index) {
    if (auto widget = dynamic_cast<Widget*>(proto.get())) {
      widget->ForgetParents();
    }
  }
}

Object* PrototypeLibrary::Find(const std::type_info& type) {
  auto it = type_index.find(type);
  if (it != type_index.end()) {
    return it->second.get();
  }
  return nullptr;
}

Object* PrototypeLibrary::Find(maf::StrView name) {
  auto it = name_index.find(name);
  if (it != name_index.end()) {
    return it->second.get();
  }
  return nullptr;
}

}  // namespace automat