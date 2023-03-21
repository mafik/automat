#pragma once

#include <include/core/SkCanvas.h>
#include <include/core/SkPath.h>

#include "action.h"
#include "math.h"

// GUI allows multiple windows to interact with multiple Automaton objects. GUI
// takes care of drawing things in the right order & correctly routing the
// input.
//
// GUI maintains per-window state (position, zoom, toolbar configuration). When
// a window disconnects it downloads this state & saves it in its local storage.
// When later the same window connects again, it uploads the state to the GUI
// when attaching itself.
namespace automaton::gui {

// API for Windows.

struct Pointer;

enum Key {
  kKeyUnknown,
  kMouseLeft,
  kMouseMiddle,
  kMouseRight,
  kKeyW,
  kKeyA,
  kKeyS,
  kKeyD,
  kKeyCount
};

struct Window final {
  Window(std::string_view initial_state = "");
  ~Window();
  void Resize(vec2 size);
  void Draw(SkCanvas &);
  void KeyDown(Key);
  void KeyUp(Key);
  std::unique_ptr<Pointer> MakePointer(vec2 position);
  std::string_view GetState();

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

struct Pointer final {
  ~Pointer();
  void Move(vec2 position);
  void KeyDown(Key);
  void KeyUp(Key);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

// API for Objects

// Base class for widgets.
struct Widget {
  Widget();
  virtual ~Widget();
  // `hover` is a value between 0 and 1. 0 means that no pointers are hovering
  // over this widget. 1 means that at least one pointer is hovering.
  // Intermediate values are used to animate the transition.
  virtual void Draw(SkCanvas &, float hover, float focus) = 0;
  virtual vec2 GetPosition() = 0;
  virtual SkPath GetShape() = 0;
  virtual std::unique_ptr<Action> KeyDown(Key) { return nullptr; }
  // Return true if the widget should be highlighted as keyboard focusable.
  virtual bool CanFocusKeyboard() { return false; }
  // Return true if the widget should be highlighted as draggable.
  virtual bool CanDrag() { return false; }
};

} // namespace automaton::gui