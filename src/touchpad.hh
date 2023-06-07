#include <stdint.h>

#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <optional>
#endif  // _WIN32

#include "math.hh"

namespace automat::touchpad {

struct Touch {
  uint32_t id;
  Vec2 pos;
};

struct TouchPad {
  double width_m;
  double height_m;
  std::vector<bool> buttons;
  std::vector<Touch> touches;

  bool panning = false;
  Vec2 pan = Vec2(0, 0);
  float zoom = 1;
};

// Automat uses touchpad to pan around & zoom, but Windows translates those
// actions into mouse wheel events. This function helps with ignoring the right
// events.

// It's not perfect because it ignores all scroll events based on the time of
// last two-finger pan. Ideally it should only ignore the events that come from
// the touchpad that is panning, but that would need more work.
//
// Note: `GetCurrentInputMessageSource` doesn't seem to work.
// https://stackoverflow.com/questions/69193249/how-to-distinguish-mouse-and-touchpad-events-using-getcurrentinputmessagesource
//
// INPUT_MESSAGE_SOURCE source;
// if (GetCurrentInputMessageSource(&source) == TRUE) {
//   LOG << "WM_MOUSEWHEEL source deviceType: " << source.deviceType <<
//   " originId: " << source.originId;
// }
//
// TODO: MAYBE switch to `WM_POINTER` events with `EnableMouseInPointer`.
// Automat should work nicely with all input devices (mice, touch screens,
// tablets, etc.) but it might be more robust to rely on direct HID input rather
// than Windows-specific messages.
bool ShouldIgnoreScrollEvents();

extern std::mutex touchpads_mutex;
extern std::vector<TouchPad*> touchpads;

void Init();

#if defined(_WIN32)
std::optional<int64_t> ProcessEvent(uint32_t msg, uint64_t wParam, int64_t lParam);
#endif  // _WIN32

}  // namespace automat::touchpad