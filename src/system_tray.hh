// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkImage.h>

#include <memory>

#include "fn.hh"
#include "str.hh"
#include "vec.hh"

namespace automat {

namespace system_tray {

struct MenuItem {
  enum Kind {
    Spacer,
    Action,
    Menu,
  } kind;
  MenuItem(Kind kind) : kind(kind) {}
  virtual ~MenuItem() = default;
};

struct Spacer : MenuItem {
  Spacer() : MenuItem(Kind::Spacer) {}
};

struct Action : MenuItem {
  Str name;
  sk_sp<SkImage> icon;
  Fn<void()> on_click;

  Action() : MenuItem(Kind::Action) {}
};

// For the root Menu passed to Icon: `name` becomes the tooltip, `icon` the tray bitmap.
// For nested submenus those fields describe the submenu entry itself.
// `items` are non-owning pointers.
struct Menu : MenuItem {
  Str name;
  sk_sp<SkImage> icon;
  Vec<MenuItem*> items;

  Menu() : MenuItem(Kind::Menu) {}
};

// RAII handle to a system tray icon. `on_activate` fires on left-click of the tray icon.
struct Icon {
  struct Impl;
  std::unique_ptr<Impl> impl;

  Icon(const Menu& root_menu, Fn<void()> on_activate);
  ~Icon();
  void Update(const Menu& root_menu, Fn<void()> on_activate);
};

}  // namespace system_tray

#ifdef _WIN32
#ifndef WM_APP
#define WM_APP 0x8000
#endif
constexpr unsigned kSystemTrayMessage = WM_APP + 1;
void OnSystemTrayMessage(unsigned event, int mouse_screen_x, int mouse_screen_y, unsigned icon_uid);

// Loads a Windows stock icon (SHSTOCKICONID / SIID_*). Returns null on failure.
sk_sp<SkImage> LoadSystemIcon(int shell_stock_icon_id);
#endif

}  // namespace automat
