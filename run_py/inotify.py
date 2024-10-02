#!/usr/bin/env python3
'''Cross-platform inotify replacement.

Does not support recursion - just watches on the provided directory.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

import ctypes, sys

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

  def inotify_init():
    return libc.inotify_init()

  def inotify_add_watch(fd, path, mask):
    return libc.inotify_add_watch(fd, path.encode(), mask)

  def inotify_wait(fd):
    buf = ctypes.create_string_buffer(1024)
    n = libc.read(fd, buf, 1024)
    if n == -1:
      raise OSError(ctypes.get_errno(), strerror(ctypes.get_errno()))
    return buf.raw[:n]

  def strerror(errno):
    return libc.strerror(errno).decode()

  if __name__ == '__main__':
    fd = inotify_init()
    wd = inotify_add_watch(fd, sys.argv[1], IN_CLOSE_WRITE)
    buf = inotify_wait(fd)[16:].decode()
    print(buf)

elif sys.platform == 'win32':
  pass