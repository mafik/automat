#pragma once

#include "keyboard.h"

automat::gui::AnsiKey ScanCodeToKey(uint32_t scan_code);
automat::gui::AnsiKey VirtualKeyToKey(uint8_t virtual_key);
