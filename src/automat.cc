// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma maf main

#pragma comment(lib, "skia")

#include "automat.hh"

#include "persistence.hh"
#include "root.hh"
#include "window.hh"

using namespace automat::gui;

#pragma region Main
#if defined(_WIN32)

#include "win_main.hh"

int main() { return WinMain(GetModuleHandle(NULL), NULL, GetCommandLine(), SW_SHOW); }

#elif defined(__linux__)

#include "linux_main.hh"

#pragma comment(lib, "freetype2")

int main(int argc, char* argv[]) { return LinuxMain(argc, argv); }

#endif

#pragma region Initialization
namespace automat {
void InitAutomat(maf::Status& status) {
  window = std::make_shared<Window>();
  window->InitToolbar();
  window->RequestResize = [&](Vec2 new_size) { window->Resize(new_size); };
  window->RequestMaximize = [&](bool horizontally, bool vertically) {
    window->maximized_horizontally = horizontally;
    window->maximized_vertically = vertically;
  };
  InitRoot();
  gui::keyboard = std::make_shared<gui::Keyboard>(*window);
  window->keyboards.emplace_back(gui::keyboard);
  gui::keyboard->parent = window;
  LoadState(*window, status);
  RunOnAutomatThread([&] {
    // nothing to do here - just make sure that memory allocated in main thread is synchronized to
    // automat thread
  });
}

}  // namespace automat
