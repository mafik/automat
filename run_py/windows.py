''' Utilities for working with Windows. '''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

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


def _redirection_guard_enforced():
    kernel32 = ctypes.windll.kernel32
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    get_policy = kernel32.GetProcessMitigationPolicy
    get_policy.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
    get_policy.restype = ctypes.c_int
    ProcessRedirectionTrustPolicy = 16
    policy = ctypes.c_uint32(0)
    ok = get_policy(kernel32.GetCurrentProcess(), ProcessRedirectionTrustPolicy,
                    ctypes.byref(policy), ctypes.sizeof(policy))
    return bool(ok) and bool(policy.value & 1)


def check_redirection_guard():
    if not _redirection_guard_enforced():
        return
    sys.stderr.write(r'''
This build cannot proceed because Windows RedirectionGuard is active.

Fix it by doing ONE of the following:

  1. Build from a local interactive session instead of over SSH.

  2. In an elevated ("Run as administrator") PowerShell:

       $k = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options'
       Remove-ItemProperty "$k\sshd.exe"      MitigationOptions
       Remove-ItemProperty "$k\ssh-agent.exe" MitigationOptions
       Restart-Service sshd

     Then reconnect over SSH and build again.

Documentation:
  https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-process-mitigation-redirection-trust-policy
  https://www.microsoft.com/en-us/msrc/blog/2025/06/redirectionguard-mitigating-unsafe-junction-traversal-in-windows
''')
    sys.exit(1)


check_redirection_guard()

