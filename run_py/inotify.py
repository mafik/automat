#!/usr/bin/env python3
'''Cross-platform inotify replacement.

Does not support recursion - just watches on the provided directory.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

import ctypes, sys

if __name__ != '__main__':
  raise ImportError('This module is not meant to be imported.')

watch_dirs = sys.argv[1:]

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
  for watch_dir in watch_dirs:
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
  import threading, ctypes

  semaphore = threading.Semaphore(0)

  def SetConsoleCtrlHandler(handler, add):
    func = ctypes.windll.kernel32.SetConsoleCtrlHandler
    callback = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_uint)
    func.argtypes = [callback, ctypes.c_bool]
    func.restype = ctypes.c_bool
    return func(callback(handler), add)
  
  def CreateFile(lpFileName:str, dwDesiredAccess:int, dwShareMode:int, lpSecurityAttributes:int, dwCreationDisposition:int, dwFlagsAndAttributes:int, hTemplateFile:int):
    func = ctypes.windll.kernel32.CreateFileW
    func.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    func.restype = ctypes.c_void_p
    return func(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)
  
  def ReadDirectoryChangesW(hDirectory:int, nBufferLength: int, bWatchSubtree:bool, dwNotifyFilter:int):
    func = ctypes.windll.kernel32.ReadDirectoryChangesW
    func.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_bool, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    func.restype = ctypes.c_bool
    buffer = ctypes.create_string_buffer(nBufferLength)
    bytes_returned = ctypes.c_uint32()
    overlapped = ctypes.c_void_p()
    success = func(hDirectory, buffer, nBufferLength, bWatchSubtree, dwNotifyFilter, ctypes.byref(bytes_returned), overlapped, None)
    if not success:
      print(ctypes.GetLastError())
      raise ctypes.WinError()
    results = []
    class FILE_NOTIFY_INFORMATION(ctypes.Structure):
      _fields_ = [
        ('NextEntryOffset', ctypes.c_uint32),
        ('Action', ctypes.c_uint32),
        ('FileNameLength', ctypes.c_uint32)
      ]
    offset = 0
    while offset + ctypes.sizeof(FILE_NOTIFY_INFORMATION) < bytes_returned.value:
      info = FILE_NOTIFY_INFORMATION.from_buffer(buffer, offset)
      if info.FileNameLength:
        first_char = ctypes.addressof(info) + ctypes.sizeof(FILE_NOTIFY_INFORMATION)
        filename = ctypes.wstring_at(first_char, info.FileNameLength // 2)
        results.append((info.Action, filename))
      if info.NextEntryOffset == 0:
        break
      offset += info.NextEntryOffset
    return results
  
  GENERIC_READ = 0x80000000
  FILE_SHARE_READ = 0x00000001
  FILE_SHARE_WRITE = 0x00000002
  FILE_SHARE_DELETE = 0x00000004
  OPEN_EXISTING = 3
  FILE_FLAG_BACKUP_SEMANTICS = 0x02000000
  FILE_NOTIFY_CHANGE_LAST_WRITE = 0x00000010

  SetConsoleCtrlHandler(lambda x: semaphore.release(), True)

  def watch_thread():
    handle = CreateFile(
      watch_dir,
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      None,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS,
      None
    )

    results = ReadDirectoryChangesW(
      handle,
      1024,
      True,
      FILE_NOTIFY_CHANGE_LAST_WRITE
    )

    for action, file in results:
      print(file)

    semaphore.release()
  
  threading.Thread(target=watch_thread, daemon=True).start()
  semaphore.acquire()
