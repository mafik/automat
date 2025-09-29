// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "prototypes.hh"

#include "library_assembler.hh"
#include "library_flip_flop.hh"
#include "library_hotkey.hh"
#include "library_instruction_library.hh"
#include "library_key_presser.hh"
#include "library_macro_recorder.hh"
#include "library_mouse.hh"
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
    auto proto = MAKE_PTR(T, std::forward<Args>(args)...);
    lib.type_index[typeid(T)] = proto;
    lib.name_index[Str(proto->Name())] = proto;
    if (show_in_toolbar == ShowInToolbar) {
      lib.default_toolbar.push_back(proto);
    }
  }
};

PrototypeLibrary::PrototypeLibrary() {
  IndexHelper index(*this);

  // TODO: Remove this once Objects are split from Widgets.
  ui::Widget* null_parent = nullptr;

  index.Register<FlipFlop>(null_parent);
  index.Register<MacroRecorder>(null_parent);
  index.Register<TimerDelay>(null_parent);
  index.Register<HotKey>(null_parent);
  index.Register<KeyPresser>(null_parent);
  index.Register<Mouse>();
  index.Register<MouseMove, HideInToolbar>();
  index.Register<MouseButtonEvent, HideInToolbar>(ui::PointerButton::Unknown, false);
  index.Register<Number>(null_parent);
  index.Register<Timeline>();
  index.Register<InstructionLibrary>();
  index.Register<Instruction, HideInToolbar>();
  index.Register<Register, HideInToolbar>(nullptr, 0);
  index.Register<Assembler>();
  index.Register<Window>();
  index.Register<TesseractOCR>();
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
