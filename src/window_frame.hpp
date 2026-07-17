#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// The chrome Automat draws around an embedded client window: a rounded frame with marquee
// lights and the title standing on top of it. Both the Wayland compositor and the X11 server
// dress their windows with it, so an embedded window looks the same whichever protocol its
// client speaks.

#include <include/core/SkCanvas.h>
#include <include/core/SkPath.h>

#include <atomic>

#include "action.hpp"
#include "math.hpp"
#include "menu.hpp"
#include "ptr.hpp"
#include "str.hpp"
#include "units.hpp"

namespace automat {

struct Object;
struct ObjectSerializer;
struct ObjectDeserializer;
struct ClientInputActionBase;

// A window object that can wear the frame: the user's decoration preference plus the hook
// the decoration menu calls after changing it. Both the X11 and the Wayland window objects
// implement it, so the menu below serves either.
struct DecoratedWindow {
  enum class DecorationPreference { Auto = 0, ServerSide = 1, ClientSide = 2 };
  std::atomic<DecorationPreference> decoration_preference{DecorationPreference::Auto};

  // UI thread: the action routing the held button into this window, for StartClientMove.
  ClientInputActionBase* input_action = nullptr;

  virtual ~DecoratedWindow() = default;

  // Called after decoration_preference changes; re-negotiates with the client / repaints.
  virtual void DecorationPreferenceChanged() = 0;

  // The "decoration" key of the owning object's serialized state.
  void SerializeDecoration(ObjectSerializer&) const;
  bool DeserializeDecoration(ObjectDeserializer&, StrView key);
};

// Adds the "Decoration..." submenu (Auto / Automat / App) to an object menu; `window` is
// the menu's window object, which implements DecoratedWindow.
void VisitDecorationOptions(const WeakPtr<Object>& window, const OptionsVisitor&);

struct ClientInputActionBase : Action {
  WeakPtr<Object> window;

  using Action::Action;
  ~ClientInputActionBase() override;
  virtual ui::Widget& InitiatingWidget() = 0;

  void LinkWindow(Object&);  // records this action in the window's input_action
};

bool StartClientMove(DecoratedWindow& window);

}  // namespace automat

namespace automat::ui {

struct Font;

struct WindowFrame {
  Vec2 content_size;
  StrView title;

  static constexpr float kTitleH = 7_mm;
  static constexpr float kFrame = 5_mm;
  static constexpr float kContentRadius = 4_mm;

  static Font& GetFont();

  Rect ContentRect() const { return Rect::MakeAtZero(content_size); }
  RRect ContentRRect() const { return RRect::MakeSimple(ContentRect(), kContentRadius); }
  RRect MidRRect() const { return ContentRRect().Outset(kFrame * 3 / 8); }
  RRect LightsRRect() const { return ContentRRect().Outset(kFrame * 11 / 16); }
  RRect OutRRect() const { return ContentRRect().Outset(kFrame); }

  // The window's footprint: the outer frame united with the (fattened) title text above it.
  SkPath Shape() const;

  // The keyboard-focus underline, shown while keys flow into the client.
  SkPath FocusCaretShape() const;

  void Draw(SkCanvas&) const;
};

}  // namespace automat::ui
