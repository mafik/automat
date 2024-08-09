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
  InitRoot();
  window.reset(new Window());
  window->RequestResize = [&](Vec2 new_size) { window->Resize(new_size); };
  window->RequestMaximize = [&](bool horizontally, bool vertically) {
    window->maximized_horizontally = horizontally;
    window->maximized_vertically = vertically;
  };
  gui::keyboard = std::make_unique<gui::Keyboard>(*window);
  LoadState(*window, status);
}

}  // namespace automat
