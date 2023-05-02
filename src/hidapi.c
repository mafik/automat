#if defined(_WIN32)

#pragma comment(lib, "hid")

#include <wtypes.h>

#include <winioctl.h>

#include "win_hidapi.c"

#elif defined(__linux__)

#include "linux_hidapi.c"

#endif
