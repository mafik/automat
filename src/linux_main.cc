#include "linux_main.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <xcb/xcb.h>

xcb_connection_t *connection;
xcb_window_t window;
xcb_screen_t *screen;
xcb_atom_t wmProtocols;
xcb_atom_t wmDeleteWin;

const char kWindowName[] = "Automat";

#pragma comment(lib, "xcb")

void RenderLoop() {
  bool running = true;
  xcb_generic_event_t *event;
  xcb_client_message_event_t *cm;

  while (running) {
    event = xcb_wait_for_event(connection);

    switch (event->response_type & ~0x80) {
      case XCB_CLIENT_MESSAGE: {
        cm = (xcb_client_message_event_t *)event;

        if (cm->data.data32[0] == wmDeleteWin)
          running = false;

        break;
      }
    }

    free(event);
  }

  xcb_destroy_window(connection, window);
}

int LinuxMain(int argc, char *argv[]) {
  int screenp = 0;
  connection = xcb_connect(NULL, &screenp);
  if (xcb_connection_has_error(connection)) {
    printf("Failed to connect to X server.\n");
    return 1;
  }

  xcb_screen_iterator_t iter =
      xcb_setup_roots_iterator(xcb_get_setup(connection));
  for (int s = screenp; s > 0; s--)
    xcb_screen_next(&iter);
  screen = iter.data;

  window = xcb_generate_id(connection);
  uint32_t eventMask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  uint32_t valueList[] = {screen->black_pixel, 0};

  xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root, 0,
                    0, 150, 150, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    screen->root_visual, eventMask, valueList);

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
                      XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, sizeof(kWindowName),
                      kWindowName);

  xcb_intern_atom_cookie_t wmDeleteCookie = xcb_intern_atom(
      connection, 0, strlen("WM_DELETE_WINDOW"), "WM_DELETE_WINDOW");
  xcb_intern_atom_cookie_t wmProtocolsCookie =
      xcb_intern_atom(connection, 0, strlen("WM_PROTOCOLS"), "WM_PROTOCOLS");
  xcb_intern_atom_reply_t *wmDeleteReply =
      xcb_intern_atom_reply(connection, wmDeleteCookie, NULL);
  xcb_intern_atom_reply_t *wmProtocolsReply =
      xcb_intern_atom_reply(connection, wmProtocolsCookie, NULL);
  wmDeleteWin = wmDeleteReply->atom;
  wmProtocols = wmProtocolsReply->atom;

  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
                      wmProtocolsReply->atom, 4, 32, 1, &wmDeleteReply->atom);

  xcb_map_window(connection, window);
  xcb_flush(connection);

  RenderLoop();

  return 0;
}
