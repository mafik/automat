// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include <string_view>

#include "tasks.hh"
#pragma maf main
#pragma maf manifest

#pragma comment(lib, "skia")

#if defined(_WIN32)
#include "win32.hh"
#endif

#include <include/core/SkGraphics.h>

#include <thread>
#include <tracy/Tracy.hpp>

#include "automat.hh"
#include "embedded.hh"
#include "global_resources.hh"
#include "loading_animation.hh"
#include "persistence.hh"
#include "prototypes.hh"
#include "renderer.hh"
#include "root_widget.hh"
#include "system_tray.hh"
#include "textures.hh"
#include "thread_name.hh"
#include "timer_thread.hh"
#include "tracy_client.hh"  // IWYU pragma: keep
#include "vk.hh"

using namespace automat::ui;

#pragma region Main
namespace automat {

std::stop_source stop_source;

std::thread::id main_thread_id;

static std::optional<system_tray::Icon> tray_icon;
static bool tray_hidden = false;
static PersistentImage favicon =
    PersistentImage::MakeFromAsset(embedded::docs_assets_favicon_184_png);

void RefreshTrayIcon() {
  auto toggle_hidden = []() {
    if (tray_hidden) {
      root_widget->RestoreFromTray();
    } else {
      root_widget->MinimizeToTray();
    }
    tray_hidden = !tray_hidden;
    RefreshTrayIcon();
  };

  // From <shlobj_core.h>: SHSTOCKICONID values for the Windows stock icons we want.
  // Hardcoded here so automat.cc doesn't have to pull in the Windows shell headers.
  constexpr int kSIID_FIND = 22;
  constexpr int kSIID_DELETE = 84;

  system_tray::FreedesktopIcon hide_fd{.name = tray_hidden ? "view-reveal-symbolic"
                                                           : "view-conceal-symbolic"};
  system_tray::WindowsStockIcon hide_icon{.fallback = hide_fd, .shell_stock_icon_id = kSIID_FIND};

  system_tray::FreedesktopIcon quit_fd{.name = "application-exit-symbolic"};
  system_tray::WindowsStockIcon quit_icon{.fallback = quit_fd, .shell_stock_icon_id = kSIID_DELETE};

  system_tray::SkiaIcon tray_icon_image{
      .image = *favicon.image,
  };

  system_tray::Action hide_action{
      .name = tray_hidden ? "Show" : "Hide",
      .icon = hide_icon,
      .on_click = toggle_hidden,
  };
  system_tray::Spacer tray_separator{};
  system_tray::Action quit_action{
      .name = "Quit",
      .icon = quit_icon,
      .on_click =
          []() {
#ifdef _WIN32
            PostQuitMessage(0);
#else
            stop_source.request_stop();
#endif
          },
  };

  system_tray::Menu root{
      .name = "Automat",
      .icon = tray_icon_image,
      .items = {hide_action, tray_separator, quit_action},
  };

  if (tray_icon) {
    tray_icon->Update(root, &hide_action);
  } else {
    tray_icon.emplace(root, &hide_action);
  }
}

static int argc;
static char** argv;

int Main() {
  // Process setup
  // Thread name of the main thread is also used as a process name so instead of "Main" (which would
  // be more accurate in the context of Automat's thread) we use "automat" (which is more helpful
  // for the user, checking the process list)
  SetThreadName("Main");
  main_thread_id = std::this_thread::get_id();

#if defined(_WIN32)
  win32::ProcessSetup();
#endif  // _WIN32

#if defined(_WIN32)
  audio::Init();
#else
  audio::Init(&argc, &argv);
#endif
  SkGraphics::Init();

  prototypes.emplace();

  root_widget = make_unique<RootWidget>();
  root_widget->Init();

  vm.root_location = MAKE_PTR(Location);
  vm.root_board = vm.root_location->Create<Board>();
  StartTimeThread(stop_source.get_token());

  RefreshTrayIcon();

  Status status;
  LoadState(*root_widget, status);
  if (!OK(status)) {
    ERROR << "Couldn't load saved state: " << status;
  }
  status.Reset();

  StartWorkerThreads(stop_source.get_token());

  root_widget->loading_animation->LoadingCompleted();

  //////////////////
  // Main Loop - processes OS events
  //////////////////
  root_widget->window->MainLoop(stop_source.get_token());

  // Shutdown
  stop_source.request_stop();

  tray_icon.reset();

  JoinWorkerThreads();

  SaveState(*root_widget, status);
  if (!OK(status)) {
    ERROR << "Failed to save state: " << status;
  }

  vm.root_board->locations.clear();

  root_widget.reset();
  vm.root_board.reset();
  vm.root_location.reset();

  prototypes.reset();

  resources::Release();
  image_provider.reset();
  PersistentImage::ReleaseAll();
  RendererShutdown();

  Widget::CheckAllWidgetsReleased();

  vk::Destroy();

  audio::Stop();

  LOG << "Exiting.";

  return 0;
}

}  // namespace automat

#if defined(_WIN32)
#pragma region WIN32

__attribute__((weak)) int main() { return automat::Main(); }

#elif defined(__linux__)
#pragma region Linux

#pragma comment(lib, "freetype2")

// Unit tests have their own main(), defined in gtest.cc
__attribute__((weak)) int main(int argc, char* argv[]) {
  automat::argc = argc;
  automat::argv = argv;
  return automat::Main();
}

#endif
