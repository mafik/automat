// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
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

#include "automat.hh"
#include "backtrace.hh"
#include "concurrentqueue.hh"
#include "global_resources.hh"
#include "library.hh"  // IWYU pragma: keep
#include "loading_animation.hh"
#include "persistence.hh"
#include "prototypes.hh"
#include "renderer.hh"
#include "root_widget.hh"
#include "textures.hh"
#include "thread_name.hh"
#include "timer_thread.hh"
#include "vk.hh"

using namespace maf;
using namespace automat::gui;

#pragma region Main
namespace automat {

std::stop_source stop_source;
Ptr<Location> root_location;
Ptr<Machine> root_machine;
std::jthread automat_thread;
std::atomic_bool automat_thread_finished = false;

moodycamel::ConcurrentQueue<Task*> queue;
std::mutex automat_threads_mutex;
std::condition_variable automat_threads_cv;

std::thread::id main_thread_id;

std::jthread render_thread;
time::SystemPoint next_frame;
constexpr bool kPowersave = true;

static int argc;
static char** argv;

// TODO: Merge this RunThread
void RunLoop(const int max_iterations) {
  int iterations = 0;
  while (max_iterations < 0 || iterations < max_iterations) {
    Task* task;
    if (!queue.try_dequeue(task)) {
      break;
    }
    task->Execute();
    ++iterations;
  }
}

static void RunThread(std::stop_token stop_token) {
  SetThreadName("Automat Loop");
  while (!stop_token.stop_requested()) {
    Task* task;
    if (!queue.try_dequeue(task)) {
      std::unique_lock lk(automat_threads_mutex);
      if (!queue.try_dequeue(task)) {
        automat_threads_cv.wait(lk);
        continue;
      }
    }
    task->Execute();
  }
  automat_thread_finished = true;
  automat_thread_finished.notify_all();
}

void VulkanPaint() {
  if (!vk::initialized) {
    return;
  }
  if (kPowersave) {
    time::SystemPoint now = time::SystemNow();
    // TODO: Adjust next_frame to minimize input latency
    // VK_EXT_present_timing
    // https://github.com/KhronosGroup/Vulkan-Docs/pull/1364
    if (next_frame <= now) {
      double frame_count =
          ceil((now - next_frame).count() * root_widget->window->screen_refresh_rate);
      next_frame += time::Duration(frame_count / root_widget->window->screen_refresh_rate);
      constexpr bool kLogSkippedFrames = false;
      if (kLogSkippedFrames && frame_count > 1) {
        LOG << "Skipped " << (uint64_t)(frame_count - 1) << " frames";
      }
    } else {
      // This normally sleeps until T + ~10ms.
      // With timeBeginPeriod(1) it's T + ~1ms.
      // TODO: try condition_variable instead
      std::this_thread::sleep_until(next_frame);
      next_frame += time::Duration(1.0 / root_widget->window->screen_refresh_rate);
    }
  }

  {
    auto lock = root_widget->window->Lock();
    Vec2 size_px = Vec2(root_widget->window->client_width, root_widget->window->client_height);
    if (root_widget->window->vk_size != size_px) {
      if (auto err =
              vk::Resize(root_widget->window->client_width, root_widget->window->client_height);
          !err.empty()) {
        FATAL << "Couldn't set window size to " << root_widget->window->client_width << "x"
              << root_widget->window->client_height << ": " << err;
      }
      root_widget->window->vk_size = size_px;
    }
  }

  SkCanvas* canvas = vk::GetBackbufferCanvas();
  if (anim) {
    anim.OnPaint(*canvas, RenderFrame);
  } else {
    RenderFrame(*canvas);
  }
  vk::Present();
  RenderOverflow(*canvas);
}

void RenderThread(std::stop_token stop_token) {
  SetThreadName("Render Thread");
  while (!stop_token.stop_requested()) {
    VulkanPaint();
    static_cast<AutomatImageProvider*>(image_provider.get())->TickCache();
  }
}

void EnqueueTask(Task* task) {
  queue.enqueue(task);
  std::unique_lock lk(automat_threads_mutex);
  automat_threads_cv.notify_one();
}

void AssertAutomatThread() {
  if (automat_thread.get_stop_source().stop_requested()) {
    assert(automat_thread_finished);
  } else {
    assert(std::this_thread::get_id() == automat_thread.get_id());
  }
}

void RunOnAutomatThread(std::function<void()> f) {
  if (std::this_thread::get_id() == automat_thread.get_id()) {
    f();
    return;
  }
  if (automat_thread.get_stop_source().stop_requested()) {
    if (!automat_thread_finished) {
      automat_thread_finished.wait(false);
    }
    f();
    return;
  }
  auto task = new FunctionTask(root_location, [f](Location& l) { f(); });
  task->Schedule();
}

void RunOnAutomatThreadSynchronous(std::function<void()> f) {
  if (std::this_thread::get_id() == automat_thread.get_id()) {
    f();
    return;
  }
  std::atomic_bool done = false;
  RunOnAutomatThread([&]() {
    f();
    // wake the UI thread
    done = true;
    done.notify_all();
  });
  done.wait(false);
}

void StopAutomat(maf::Status&) {
  {
    std::unique_lock lk(automat_threads_mutex);
    stop_source.request_stop();
  }
  automat_threads_cv.notify_all();
}

int Main() {
  // Process setup
#if not defined(NDEBUG)  // disable in "Release" builds
  EnableBacktraceOnSIGSEGV();
#endif
  // Thread name of the main thread is also used as a process name so instead of "Main" (which would
  // be more accurate in the context of Automat's thread) we use "automat" (which is more helpful
  // for the user, checking the process list)
  SetThreadName("automat");
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

  RendererInit();

  prototypes.emplace();

  root_widget = MakePtr<RootWidget>();
  root_widget->InitToolbar();
  gui::keyboard = MakePtr<gui::Keyboard>(*root_widget);
  root_widget->keyboards.emplace_back(gui::keyboard);
  gui::keyboard->parent = root_widget;

  root_location = MakePtr<Location>();
  root_location->name = "Root location";
  root_location->parent = gui::root_widget;
  root_machine = root_location->Create<Machine>();
  root_machine->parent = gui::root_widget;
  root_machine->name = "Root machine";
  StartTimeThread(stop_source.get_token());

  automat_thread = std::jthread(RunThread, stop_source.get_token());
  RunOnAutomatThread([&] {
    // nothing to do here - just make sure that memory allocated in main thread is synchronized to
    // automat thread
  });

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
  if (auto err = vk::Init(); !err.empty()) {
    FATAL << "Failed to initialize Vulkan: " << err;
  }
#endif

  image_provider.reset(new AutomatImageProvider());

  render_thread = std::jthread(RenderThread, stop_source.get_token());

  LoadState(*root_widget, status);
  if (!OK(status)) {
    ERROR << "Couldn't load saved state: " << status;
  }

  anim.LoadingCompleted();

  root_widget->window->MainLoop();

  // Shutdown
  StopAutomat(status);
  if (automat_thread.joinable()) {
    automat_thread.join();
  }
  if (render_thread.joinable()) {
    render_thread.join();
  }

  SaveState(*root_widget, status);
  if (!OK(status)) {
    ERROR << "Failed to save state: " << status;
  }

  root_widget->ForgetParents();
  root_machine->locations.clear();

  keyboard.reset();
  root_widget.reset();
  root_machine.reset();
  root_location.reset();

  prototypes.reset();

  resources::Release();
  image_provider.reset();

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
#pragma comment(lib, "xcb")
#pragma comment(lib, "xcb-xinput")
#pragma comment(lib, "xcb-cursor")

// Unit tests have their own main(), defined in gtest.cc
__attribute__((weak)) int main(int argc, char* argv[]) {
  automat::argc = argc;
  automat::argv = argv;
  return automat::Main();
}

#endif
