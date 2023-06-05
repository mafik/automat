#include "window.h"

#include <memory>

#include "window_impl.h"

namespace automat::gui {

Window::Window(vec2 size, float pixels_per_meter, std::string_view initial_state)
    : impl(std::make_unique<WindowImpl>(size, pixels_per_meter)) {}
Window::~Window() {}
void Window::Resize(vec2 size) { impl->Resize(size); }
void Window::DisplayPixelDensity(float pixels_per_meter) {
  impl->DisplayPixelDensity(pixels_per_meter);
}
void Window::Draw(SkCanvas& canvas) { impl->Draw(canvas); }
std::string_view Window::GetState() { return {}; }

}  // namespace automat::gui
