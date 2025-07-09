// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "prototypes.hh"

#include "library_assembler.hh"
#include "library_flip_flop.hh"
#include "library_hotkey.hh"
#include "library_instruction_library.hh"
#include "library_key_presser.hh"
#include "library_macro_recorder.hh"
#include "library_mouse_click.hh"
#include "library_mouse_move.hh"
#include "library_number.hh"
#include "library_tesseract_ocr.hh"
#include "library_timeline.hh"
#include "library_timer.hh"
#include "library_window.hh"
#include "object.hh"

using namespace automat::library;

namespace automat {

Optional<PrototypeLibrary> prototypes;

enum ToolbarVisibility {
  ShowInToolbar,
  HideInToolbar,
};

struct IndexHelper {
  PrototypeLibrary& lib;
  IndexHelper(PrototypeLibrary& lib) : lib(lib) {}
  template <typename T, ToolbarVisibility show_in_toolbar = ShowInToolbar, typename... Args>
  void Register(Args&&... args) {
    auto proto = MakePtr<T>(std::forward<Args>(args)...);
    lib.type_index[typeid(T)] = proto;
    lib.name_index[Str(proto->Name())] = proto;
    if (show_in_toolbar == ShowInToolbar) {
      lib.default_toolbar.push_back(proto);
    }
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
  index.Register<MouseMove>();
  index.Register<Number>();
  index.Register<Timeline>();
  index.Register<InstructionLibrary>();
  index.Register<Instruction, HideInToolbar>();
  index.Register<Register, HideInToolbar>(nullptr, 0);
  index.Register<Assembler>();
  index.Register<Window>();
  index.Register<TesseractOCR>();
}

PrototypeLibrary::~PrototypeLibrary() {
  // Some Objects are also Widgets and they need to have `ForgetParents()` called on them to release
  // all their resources.
  for (auto& [name, proto] : name_index) {
    if (auto widget = dynamic_cast<gui::Widget*>(proto.get())) {
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

Object* PrototypeLibrary::Find(StrView name) {
  auto it = name_index.find(name);
  if (it != name_index.end()) {
    return it->second.get();
  }
  return nullptr;
}

}  // namespace automat
