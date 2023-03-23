#include "gui.h"

#include <vector>

namespace automaton::gui {

struct Window::Impl {
  vec2 position; // center of the window
  vec2 size;
  float zoom = 1.0f;
};

std::vector<Window*> windows;

Window::Window(vec2 size, std::string_view initial_state) : impl(std::make_unique<Window::Impl>(Vec2(0, 0), size, 1.0f)) {
  windows.push_back(this);
}
Window::~Window() {
  windows.erase(std::find(windows.begin(), windows.end(), this));
}
void Window::Resize(vec2 size) {
  impl->size = size;
}
void Window::Draw(SkCanvas &) {}
void Window::KeyDown(Key) {}
void Window::KeyUp(Key) {}
std::unique_ptr<Pointer> Window::MakePointer(vec2 position) {

}
std::string_view Window::GetState() {}

} // namespace automaton::gui