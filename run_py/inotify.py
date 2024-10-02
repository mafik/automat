#!/usr/bin/env python3
'''Cross-platform inotify replacement.

Does not support recursion - just watches on the provided directory.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

import ctypes, sys

if __name__ != '__main__':
  raise ImportError('This module is not meant to be imported.')

watch_dir = sys.argv[1]

if sys.platform == 'linux':
  libc = ctypes.cdll.LoadLibrary('libc.so.6')

  libc.inotify_init.argtypes = []
  libc.inotify_init.restype = ctypes.c_int

  libc.inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
  libc.inotify_add_watch.restype = ctypes.c_int

  libc.read.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
  libc.read.restype = ctypes.c_int

  libc.strerror.argtypes = [ctypes.c_int]
  libc.strerror.restype = ctypes.c_char_p

  IN_CLOSE_WRITE = 0x008

  def strerror(errno):
    return libc.strerror(errno).decode()

  fd = libc.inotify_init()
  wd = libc.inotify_add_watch(fd, watch_dir.encode(), IN_CLOSE_WRITE)
  
  buf = ctypes.create_string_buffer(1024)
  # blocks until an event occurs
  n = libc.read(fd, buf, 1024)
  if n == -1:
    raise OSError(ctypes.get_errno(), strerror(ctypes.get_errno()))
  buf = buf.raw[:n]
  buf = buf[16:].decode()
  print(buf)

elif sys.platform == 'win32':
  import win32api, win32file, win32con, threading

  semaphore = threading.Semaphore(0)

  win32api.SetConsoleCtrlHandler(lambda x: semaphore.release(), True)

  def watch_thread():
    handle = win32file.CreateFile(
      watch_dir,
      win32file.GENERIC_READ,
      win32file.FILE_SHARE_READ | win32file.FILE_SHARE_WRITE | win32file.FILE_SHARE_DELETE,
      None,
      win32file.OPEN_EXISTING,
      win32file.FILE_FLAG_BACKUP_SEMANTICS,
      None
    )

    results = win32file.ReadDirectoryChangesW(
      handle,
      1024,
      True,
      win32con.FILE_NOTIFY_CHANGE_LAST_WRITE,
      None,
      None
    )

    for action, file in results:
      print(file)

    semaphore.release()
  
  threading.Thread(target=watch_thread, daemon=True).start()
  semaphore.acquire()
