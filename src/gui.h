#pragma once

#include <functional>

#include <include/core/SkCanvas.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>

#include "action.h"
#include "animation.h"
#include "math.h"
#include "product_ptr.h"
#include "time.h"

// GUI allows multiple windows to interact with multiple Automaton objects. GUI
// takes care of drawing things in the right order & correctly routing the
// input.
//
// GUI maintains per-window state (position, zoom, toolbar configuration). When
// a window disconnects it downloads this state & saves it in its local storage.
// When later the same window connects again, it uploads the state to the GUI
// when attaching itself.
namespace automaton::gui {

using namespace automaton;

// API for Windows.

struct Pointer;

enum Key { kKeyUnknown, kKeyW, kKeyA, kKeyS, kKeyD, kKeyCount };

enum Button {
  kButtonUnknown,
  kMouseLeft,
  kMouseMiddle,
  kMouseRight,
  kButtonCount
};

struct Window final {
  Window(vec2 size, float pixels_per_meter,
         std::string_view initial_state = "");
  ~Window();
  void Resize(vec2 size);
  void DisplayPixelDensity(float pixels_per_meter);
  void Draw(SkCanvas &);
  void KeyDown(Key);
  void KeyUp(Key);
  std::string_view GetState();

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
  friend struct Pointer;
};

struct Pointer final {
  Pointer(Window &, vec2 position);
  ~Pointer();
  void Move(vec2 position);
  void Wheel(float delta);
  void ButtonDown(Button);
  void ButtonUp(Button);

  enum IconType { kIconArrow, kIconHand, kIconIBeam };

  IconType Icon() const;
  void PushIcon(IconType);
  void PopIcon();

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
  friend struct Window;
};

// API for Objects

struct Widget;

enum class VisitResult { kContinue, kStop };

struct WidgetVisitor {
  virtual ~WidgetVisitor() {}
  virtual VisitResult operator()(Widget &, const SkMatrix &transform) = 0;
};

using WidgetVisitorFunc =
    std::function<VisitResult(Widget &, const SkMatrix &)>;

// Base class for widgets.
struct Widget {
  Widget() {}
  virtual ~Widget() {}
  virtual void PointerOver(Pointer &, animation::State &) {}
  virtual void PointerLeave(Pointer &, animation::State &) {}
  virtual void OnFocus(bool focus, animation::State &animation_state) {}
  virtual void Draw(SkCanvas &, animation::State &animation_state) const = 0;
  virtual SkPath Shape() const = 0;
  virtual std::unique_ptr<Action> KeyDownAction(Key) { return nullptr; }
  virtual std::unique_ptr<Action> ButtonDownAction(Button, vec2 contact_point) {
    return nullptr;
  }
  // Return true if the widget should be highlighted as keyboard focusable.
  virtual bool CanFocusKeyboard() { return false; }
  // Return true if the widget should be highlighted as draggable.
  virtual bool CanDrag() { return false; }
  // Iterate over direct child widgets in front-to-back order.
  virtual VisitResult VisitImmediateChildren(WidgetVisitor &visitor) {
    return VisitResult::kContinue;
  }
  void VisitAll(WidgetVisitor &visitor);
  void VisitAll(WidgetVisitorFunc visitor);
  void VisitAtPoint(vec2 point, WidgetVisitor &visitor);
  void VisitAtPoint(vec2 point, WidgetVisitorFunc visitor);
};

} // namespace automaton::gui