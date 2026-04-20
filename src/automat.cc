// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include <string_view>

#include "tasks.hh"
#pragma maf main

#pragma comment(lib, "skia")

#if defined(_WIN32)
#include "win32.hh"
#endif

#include <include/core/SkGraphics.h>

#include <thread>
#include <tracy/Tracy.hpp>

#include "automat.hh"
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
Ptr<Location> root_location;
Ptr<Board> root_board;

std::thread::id main_thread_id;

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

  root_location = MAKE_PTR(Location);
  root_board = root_location->Create<Board>();
  StartTimeThread(stop_source.get_token());

  InitSystemTray();

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

  JoinWorkerThreads();

  SaveState(*root_widget, status);
  if (!OK(status)) {
    ERROR << "Failed to save state: " << status;
  }

  root_board->locations.clear();

  root_widget.reset();
  root_board.reset();
  root_location.reset();

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
