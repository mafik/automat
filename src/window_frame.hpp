#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// The chrome Automat draws around an embedded client window: a rounded frame with marquee
// lights and the title standing on top of it. Both the Wayland compositor and the X11 server
// dress their windows with it, so an embedded window looks the same whichever protocol its
// client speaks.

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPath.h>

#include <atomic>

#include "action.hpp"
#include "keyboard.hpp"
#include "math.hpp"
#include "menu.hpp"
#include "pointer.hpp"
#include "ptr.hpp"
#include "str.hpp"
#include "ui_beta.hpp"
#include "units.hpp"
#include "vec.hpp"

namespace automat {

struct Object;
struct ObjectSerializer;
struct ObjectDeserializer;
struct ClientInputActionBase;
struct Launch;

namespace library {
struct ClientWindow;
}  // namespace library

namespace ui {
struct WindowFrame;
}  // namespace ui

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

// One window of a running program, drawn on the board inside the shared
// chrome: caret-gated keyboard, pointer pass-through.
struct ClientWindowToy : ui::beta::ObjectToy, ui::PointerMoveCallback {
  Str title_;
  bool client_gone_ = false;
  bool client_decorated_ = false;
  DecoratedWindow::DecorationPreference pref_ = DecoratedWindow::DecorationPreference::Auto;
  sk_sp<SkImage> image_;
  SkISize content_size_ = {};
  ui::Caret* caret_ = nullptr;

  // One client pixel on the board, matching the Wayland compositor's scale.
  static constexpr float kPx = 0.20_mm;
  static constexpr float kMinContent = 3_cm;

  using ui::beta::ObjectToy::ObjectToy;
  ~ClientWindowToy() override;

  Ptr<library::ClientWindow> LockClientWindow() const;

  void PullState();
  virtual void PullMore(library::ClientWindow&) {}
  virtual bool TickMore(time::Timer&) { return false; }
  virtual bool Decorated() const;

  Vec2 ContentSize() const;
  Rect ContentRect() const { return Rect::MakeAtZero(ContentSize()); }
  ui::WindowFrame Chrome() const;
  SkPath FocusCaretShape() const;
  Vec2 ToSurfacePx(Vec2 local) const;

  bool CenteredAtZero() const override { return true; }
  SkPath Shape() const override;
  Tock Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  void VisitOptions(const OptionsVisitor&) const override;

  void FocusClient(ui::Pointer&);
  void ReleaseCaret(ui::Caret&) override;

  void PointerMove(ui::Pointer&, Vec2) override;
  void PointerEnter(ui::Pointer&) override;
  void PointerLeave(ui::Pointer&) override;
  void KeyDown(ui::Caret&, ui::Key) override;
  void KeyUp(ui::Caret&, ui::Key) override;

  virtual void SendMotion(Vec2 px) = 0;
  virtual void SendCrossing(bool enter, Vec2 px) {}
  virtual void SendKey(ui::Key, bool pressed) = 0;
  virtual void SendFocus(bool) {}

  virtual bool AllowClientPress(ui::Pointer&) { return true; }
  virtual std::unique_ptr<Action> BeginClientPress(ui::Pointer&) = 0;
  std::unique_ptr<Action> FindAction(ui::Pointer&, ui::ActionTrigger) override;
};

struct ClientArrivals {
  Vec<std::pair<Ptr<library::ClientWindow>, Ptr<Launch>>> appeared;
  Vec<Ptr<library::ClientWindow>> disappeared;
  Vec<WeakPtr<library::ClientWindow>> move_requests;

  ClientArrivals();
  ~ClientArrivals();
  void Process();
};

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
