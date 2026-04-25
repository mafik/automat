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
    SpacerKind,
    ActionKind,
    MenuKind,
  } kind;
};

struct MenuIcon {
  enum Kind {
    SkiaIconKind,
    WindowsStockIconKind,
    FreedesktopIconKind,
  } kind;
};

struct SkiaIcon {
  MenuIcon icon = {MenuIcon::SkiaIconKind};
  MenuIcon* fallback = nullptr;
  sk_sp<SkImage> image;

  operator MenuIcon*() noexcept { return &icon; }
};

struct WindowsStockIcon {
  MenuIcon icon = {MenuIcon::WindowsStockIconKind};
  MenuIcon* fallback = nullptr;
  int shell_stock_icon_id = 0;

  operator MenuIcon*() noexcept { return &icon; }
};

struct FreedesktopIcon {
  MenuIcon icon = {MenuIcon::FreedesktopIconKind};
  MenuIcon* fallback = nullptr;
  Str name;

  operator MenuIcon*() noexcept { return &icon; }
};

struct Spacer {
  MenuItem item = {MenuItem::SpacerKind};

  operator MenuItem*() noexcept { return &item; }
};

struct Action {
  MenuItem item = {MenuItem::ActionKind};
  Str name;
  MenuIcon* icon = nullptr;
  Fn<void()> on_click;

  operator MenuItem*() noexcept { return &item; }
};

// For the root Menu passed to Icon: `name` becomes the tooltip, `icon` the tray bitmap.
// For nested submenus those fields describe the submenu entry itself.
// `items` are non-owning pointers.
struct Menu {
  MenuItem item = {MenuItem::MenuKind};
  Str name;
  MenuIcon* icon = nullptr;
  Vec<MenuItem*> items;

  operator MenuItem*() noexcept { return &item; }
};

// RAII handle to a system tray icon. `default_item` is highlighted and fires on left-click.
struct Icon {
  struct Impl;
  std::unique_ptr<Impl> impl;

  Icon(const Menu& root_menu, Action* default_item = nullptr);
  ~Icon();
  void Update(const Menu& root_menu, Action* default_item = nullptr);
};

}  // namespace system_tray

#ifdef _WIN32
#ifndef WM_APP
#define WM_APP 0x8000
#endif
constexpr unsigned kSystemTrayMessage = WM_APP + 1;
void OnSystemTrayMessage(unsigned event, int mouse_screen_x, int mouse_screen_y, unsigned icon_uid);
#endif

}  // namespace automat
