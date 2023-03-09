#if defined(_WIN32)

#include "win_main.h"

int main() {
  return wWinMain(GetModuleHandle(NULL), NULL, GetCommandLine(), SW_SHOW);
}

#endif
