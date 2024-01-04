#pragma maf main

#pragma comment(lib, "skia")

#if defined(_WIN32)

#include "win_main.hh"

int main() { return WinMain(GetModuleHandle(NULL), NULL, GetCommandLine(), SW_SHOW); }

#elif defined(__linux__)

#include "linux_main.hh"

#pragma comment(lib, "webp")
#pragma comment(lib, "webpdemux")
#pragma comment(lib, "brotlicommon")
#pragma comment(lib, "brotlidec")
#pragma comment(lib, "skottie")
#pragma comment(lib, "sksg")
#pragma comment(lib, "jpeg")
#pragma comment(lib, "png")
#pragma comment(lib, "z")
#pragma comment(lib, "fontconfig")
#pragma comment(lib, "freetype")
#pragma comment(lib, "expat")
#pragma comment(lib, "xcb")
#pragma comment(lib, "Xau")
#pragma comment(lib, "icuuc")
#pragma comment(lib, "icudata")
#pragma comment(lib, "Xdmcp")
#pragma comment(lib, "uuid")

int main(int argc, char* argv[]) { return LinuxMain(argc, argv); }

#endif
