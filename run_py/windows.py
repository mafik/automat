# Utilities for working with Windows

import sys

if sys.platform != 'win32':
  raise Exception('This module is only for Windows')

import ctypes

user32 = ctypes.windll.user32
GetWindowThreadProcessId = user32.GetWindowThreadProcessId
EnumWindows = user32.EnumWindows
EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int))
GetWindowText = user32.GetWindowTextW
GetWindowTextLength = user32.GetWindowTextLengthW
GetParent = user32.GetParent
IsWindowVisible = user32.IsWindowVisible


def is_main(hwnd):
    return GetParent(hwnd) == 0


def is_visible(hwnd):
    return IsWindowVisible(hwnd)


def get_pid(hwnd):
    pid = ctypes.c_ulong()
    GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


'''Sends a WM_CLOSE message to the main window of the process with the given PID.

Returns True if the window was found and WM_CLOSE sent, False otherwise.'''
def close_window(pid):
    hwnds = []
    def foreach_window(hwnd, lParam):
        if not is_main(hwnd) or not is_visible(hwnd):
            return True
        # length = GetWindowTextLength(hwnd)
        # buff = ctypes.create_unicode_buffer(length + 1)
        # GetWindowText(hwnd, buff, length + 1)
        # hwnd_title = buff.value
        hwnd_pid = get_pid(hwnd)
        if pid == hwnd_pid:
            hwnds.append(hwnd)
        return True
    EnumWindows(EnumWindowsProc(foreach_window), 0)
    if len(hwnds) != 1:
        return False
    hwnd = hwnds[0]
    user32.PostMessageW(hwnd, 0x0010, 0, 0)
    return True

