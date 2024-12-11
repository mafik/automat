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
#include "thread_name.hh"
#include "timer_thread.hh"
#include "vk.hh"
#include "window.hh"

using namespace maf;
using namespace automat::gui;

#pragma region Main
namespace automat {

std::stop_source stop_source;
std::shared_ptr<Location> root_location;
std::shared_ptr<Machine> root_machine;
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
          ceil((now - next_frame).count() * window->os_window->screen_refresh_rate);
      next_frame += time::Duration(frame_count / window->os_window->screen_refresh_rate);
      constexpr bool kLogSkippedFrames = false;
      if (kLogSkippedFrames && frame_count > 1) {
        LOG << "Skipped " << (uint64_t)(frame_count - 1) << " frames";
      }
    } else {
      // This normally sleeps until T + ~10ms.
      // With timeBeginPeriod(1) it's T + ~1ms.
      // TODO: try condition_variable instead
      std::this_thread::sleep_until(next_frame);
      next_frame += time::Duration(1.0 / window->os_window->screen_refresh_rate);
    }
  }

  {
    auto lock = window->os_window->Lock();
    Vec2 size_px = Vec2(window->os_window->client_width, window->os_window->client_height);
    if (window->os_window->vk_size != size_px) {
      if (auto err = vk::Resize(window->os_window->client_width, window->os_window->client_height);
          !err.empty()) {
        FATAL << "Couldn't set window size to " << window->os_window->client_width << "x"
              << window->os_window->client_height << ": " << err;
      }
      window->os_window->vk_size = size_px;
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
  // TODO: fix backtraces
  // EnableBacktraceOnSIGSEGV();
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

  auto& prototypes = Prototypes();
  stable_sort(prototypes.begin(), prototypes.end(),
              [](const auto& a, const auto& b) { return a->Name() < b->Name(); });
  for (auto& proto : prototypes) {
    LOG << "Prototype: " << proto->Name();
  }

  window = std::make_shared<Window>();
  window->InitToolbar();
  window->RequestResize = [&](Vec2 new_size) { window->Resize(new_size); };
  window->RequestMaximize = [&](bool horizontally, bool vertically) {
    window->maximized_horizontally = horizontally;
    window->maximized_vertically = vertically;
  };
  gui::keyboard = std::make_shared<gui::Keyboard>(*window);
  window->keyboards.emplace_back(gui::keyboard);
  gui::keyboard->parent = window;

  root_location = std::make_shared<Location>();
  root_location->name = "Root location";
  root_location->parent = gui::window;
  root_machine = root_location->Create<Machine>();
  root_machine->parent = gui::window;
  root_machine->name = "Root machine";
  StartTimeThread(stop_source.get_token());

  Status status;
  LoadState(*window, status);
  if (!OK(status)) {
    ERROR << "Couldn't load saved state: " << status;
  }

  automat_thread = std::jthread(RunThread, stop_source.get_token());
  RunOnAutomatThread([&] {
    // nothing to do here - just make sure that memory allocated in main thread is synchronized to
    // automat thread
  });
  anim.LoadingCompleted();

#ifdef __linux__
  window->os_window = xcb::XCBWindow::Make(*window, status);
#else
  window->os_window = Win32Window::Make(*window, status);
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

  render_thread = std::jthread(RenderThread, stop_source.get_token());

  window->os_window->MainLoop();

  // Shutdown
  StopAutomat(status);
  if (automat_thread.joinable()) {
    automat_thread.join();
  }
  if (render_thread.joinable()) {
    render_thread.join();
  }

  SaveState(*window, status);
  if (!OK(status)) {
    ERROR << "Failed to save state: " << status;
  }

  window->ForgetParents();
  root_machine->locations.clear();

  keyboard.reset();
  window.reset();
  root_machine.reset();
  root_location.reset();

  // TODO: Initialize (& deinitialize) prototypes in a better place.
  for (auto& proto : Prototypes()) {
    if (auto widget = dynamic_cast<Widget*>(proto.get())) {
      widget->ForgetParents();
    }
    proto.reset();
  }
  Prototypes().clear();
  Machine::proto->ForgetParents();
  Machine::proto.reset();

  resources::Release();

  Widget::CheckAllWidgetsReleased();

  vk::Destroy();

  audio::Stop();

  LOG << "Exiting.";

  return 0;
}

}  // namespace automat

#if defined(_WIN32)
#pragma region WIN32

int main() { return automat::Main(); }

#elif defined(__linux__)
#pragma region Linux

#pragma comment(lib, "freetype2")
#pragma comment(lib, "xcb")
#pragma comment(lib, "xcb-xinput")

int main(int argc, char* argv[]) {
  automat::argc = argc;
  automat::argv = argv;
  return automat::Main();
}

#endif
