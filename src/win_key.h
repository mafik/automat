#pragma once

#include "keyboard.h"

automaton::gui::AnsiKey ScanCodeToKey(uint32_t scan_code);
automaton::gui::AnsiKey VirtualKeyToKey(uint8_t virtual_key);
