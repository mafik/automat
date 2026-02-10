// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "tasks.hh"
#pragma maf main

#pragma comment(lib, "skia")

#if defined(_WIN32)
#include "win32.hh"
#include "win32_window.hh"
#elif defined(__linux__)
#include "xcb_window.hh"
#endif

#include <include/core/SkGraphics.h>

#include <atomic>
#include <condition_variable>
#include <thread>
#include <tracy/Tracy.hpp>

#include "automat.hh"
#include "concurrentqueue.hh"
#include "global_resources.hh"
#include "loading_animation.hh"
#include "persistence.hh"
#include "prototypes.hh"
#include "renderer.hh"
#include "root_widget.hh"
#include "system_tray.hh"
#include "textures.hh"
#include "thread_name.hh"
#include "time.hh"
#include "timer_thread.hh"
#include "tracy_client.hh"  // IWYU pragma: keep
#include "vk.hh"

using namespace automat::ui;

#pragma region Main
namespace automat {

std::stop_source stop_source;
Ptr<Location> root_location;
Ptr<Machine> root_machine;

std::thread::id main_thread_id;

std::jthread render_thread;
time::SteadyPoint next_frame = time::kZeroSteady;
constexpr bool kPowersave = true;

static int argc;
static char** argv;

void VulkanPaint() {
  ZoneScoped;
  if (!vk::initialized) {
    return;
  }
  if (kPowersave) {
    ZoneScopedN("Powersave");
    time::SteadyPoint now = time::SteadyNow();
    // TODO: Adjust next_frame to minimize input latency
    // VK_EXT_present_timing
    // https://github.com/KhronosGroup/Vulkan-Docs/pull/1364
    if (next_frame <= now) {
      auto frame_count =
          time::ToSeconds(now - next_frame) / root_widget->window->screen_refresh_rate;
      next_frame = now + time::Duration(1s) / root_widget->window->screen_refresh_rate;
      constexpr bool kLogSkippedFrames = false;
      if (kLogSkippedFrames && frame_count > 1) {
        LOG << "Skipped " << (uint64_t)(frame_count - 1) << " frames";
      }
    } else {
      // This normally sleeps until T + ~10ms.
      // With timeBeginPeriod(1) it's T + ~1ms.
      // TODO: try condition_variable instead
      std::this_thread::sleep_until(next_frame);
      next_frame += time::Duration(1s) / root_widget->window->screen_refresh_rate;
    }
  }

  {
    ZoneScopedN("Resize");
    auto lock = root_widget->window->Lock();
    Vec2 size_px = Vec2(root_widget->window->client_width, root_widget->window->client_height);
    if (root_widget->window->vk_size != size_px) {
      Status status;
      vk::Resize(root_widget->window->client_width, root_widget->window->client_height, status);
      if (!OK(status)) {
        FATAL << "Couldn't set window size to " << root_widget->window->client_width << "x"
              << root_widget->window->client_height << ": " << status;
      }
      root_widget->window->vk_size = size_px;
    }
  }

  SkCanvas* canvas = vk::AcquireCanvas();
  // When window is resized continously, vulkan may return VK_ERROR_OUT_OF_DATE_KHR and it may be
  // hard to obtain a valid surface. When this happens, we just skip the paint for this frame.
  if (canvas == nullptr) {
    return;
  }
  {
    ZoneScopedN("RenderFrame");
    RenderFrame(*canvas);
  }
  FrameMark;
}

void RenderThread(std::stop_token stop_token) {
  SetThreadName("Render Thread");
  while (!stop_token.stop_requested()) {
    VulkanPaint();
    {
      ZoneScopedN("ImageProvider TickCache");
      static_cast<AutomatImageProvider*>(image_provider.get())->TickCache();
    }
  }
}

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
  root_widget->loading_animation = std::make_unique<HypnoRect>();
  root_widget->InitToolbar();

  root_location = MAKE_PTR(Location);
  root_machine = root_location->Create<Machine>();
  root_machine->name = "Root machine";
  StartTimeThread(stop_source.get_token());

  InitSystemTray();

  Status status;
#ifdef __linux__
  root_widget->window = xcb::XCBWindow::Make(*root_widget, status);
#else
  root_widget->window = Win32Window::Make(*root_widget, status);
#endif
  if (!OK(status)) {
    FATAL << "Couldn't create main window: " << status;
  }

#ifdef CPU_RENDERING
  // nothing to do here
#else
  vk::Init(status);
  if (!OK(status)) {
    FATAL << "Failed to initialize Vulkan: " << status;
  }
#endif
  image_provider.reset(new AutomatImageProvider());
  RendererInit();
  PersistentImage::PreloadAll();

  render_thread = std::jthread(RenderThread, stop_source.get_token());

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
  if (render_thread.joinable()) {
    render_thread.join();
  }

  SaveState(*root_widget, status);
  if (!OK(status)) {
    ERROR << "Failed to save state: " << status;
  }

  root_machine->locations.clear();

  root_widget.reset();
  root_machine.reset();
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
