#if defined(_WIN32)

#include "win_main.h"

int main() {
  return WinMain(GetModuleHandle(NULL), NULL, GetCommandLine(), SW_SHOW);
}

#endif
