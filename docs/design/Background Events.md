# Background Events

This document describes the lessons learned while attempting to send events to background windows, without activating them.

# Windows

Windows allows input events to be sent to message queues of arbitrary windows but many apps use different APIs, which don't really rely on the events being sent. This applies especially to games, which tend to "query device state" at regular intervals.

See: https://stackoverflow.com/questions/1220820/how-do-i-send-key-strokes-to-a-window-without-having-to-activate-it-using-window

# Linux

Same issue as on Windows.

Linux is more modular - the X11 server & Wayland compositor can be replaced - although that would require running games in under supervision of Automat. It seems like this solution would work, although it would require a significant effort to turn Automat into a compositor. See: https://wayland-book.com/

A big problem that prevents Automat from sending precise events seem to be "passive grabs" that the X server performs when a key is being pressed. See: https://www.x.org/releases/X11R7.7/doc/inputproto/XI2proto.txt

See: https://unix.stackexchange.com/questions/782869/send-a-mouseclick-to-an-x-window-in-the-background-without-bringing-it-to-the-f

Note: it might also be possible to embed the game's window using XEmbed (which would help with control over some events) but that doesn't address the issue of raw events or querying device state.

## Sending key presses directly

Here is a sample code that can send X11 key press events (the Core version) to arbitrary xcb_window.

```C++
auto machine = location.ParentAs<Machine>();
library::Window* window = (library::Window*)machine->Nearby(
    location.position, 100_cm,
    [](Location& other) -> void* { return other.As<library::Window>(); });
if (window) {
  xcb_window_t target = window->xcb_window;
  if (target != XCB_WINDOW_NONE) {
    xcb_key_press_event_t event = {
        .response_type = XCB_KEY_PRESS,
        .detail = (xcb_keycode_t)x11::KeyToX11KeyCode(key),
        .sequence = 0,
        .time = XCB_CURRENT_TIME,
        .root = xcb::screen->root,
        .event = target,
        .child = XCB_NONE,
        .state = 0,
        .same_screen = 1,
    };
    xcb_send_event(xcb::connection, true, target, 0, (const char*)&event);
    event_sent_directly_to_window = true;
  }
}
```

## Skyrim/Proton experiments

Running Skyrim under Wine/Proton has shown that the game actually grabs all of the raw input events on all devices (although it seemed to discard the events from extra master devices - specifically MPX - Multi-Pointer-X virtual devices).

## Focus

X11 seems to have several kinds of focus - the X11 one (XGetInputFocus), XInput2 one (XIGetInputFocus), and one more, controlled by the window manager (_NET_ACTIVE_WINDOW). The latter seems to be the most reliable way of accessing the focus.

See: https://stackoverflow.com/questions/31800880/xlib-difference-between-net-active-window-and-xgetinputfocus

## Current solution

The most robust way of sending virtual input to apps seems to be the XTEST extension: https://www.x.org/releases/X11R7.7/doc/xextproto/xtest.html

It's going to stay as the main way of sending events until Automat can work as a proper compositor.

# Big Issues

*Focus* many apps keep track of their focus state and only process events if they're properly focused.

*Out-of-band input protocols* are another issue, especially for games. They tend to ignore the events and instead get the input state from other libraries, often reading it directly from input devices.

# Conclusion

For the time being keep emulating events at a global level (which also provides correct focus tracking & stays in sync with out-of-band protocols). 

In the future - explore mechanisms for running apps under more controlled environment (custom compositor, hooking the apps).
