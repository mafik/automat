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

  # Windows API constants
  GENERIC_READ = 0x80000000
  FILE_SHARE_READ = 0x00000001
  FILE_SHARE_WRITE = 0x00000002
  FILE_SHARE_DELETE = 0x00000004
  OPEN_EXISTING = 3
  FILE_FLAG_BACKUP_SEMANTICS = 0x02000000
  FILE_FLAG_OVERLAPPED = 0x40000000
  FILE_NOTIFY_CHANGE_LAST_WRITE = 0x00000010
  WAIT_OBJECT_0 = 0x00000000
  WAIT_TIMEOUT = 0x00000102
  WAIT_ABANDONED = 0x00000080
  WAIT_FAILED = 0xFFFFFFFF
  INVALID_HANDLE_VALUE = -1

  # Define Windows structures
  class OVERLAPPED(ctypes.Structure):
    _fields_ = [
      ('Internal', ctypes.POINTER(ctypes.c_ulong)),
      ('InternalHigh', ctypes.POINTER(ctypes.c_ulong)),
      ('Offset', ctypes.c_ulong),
      ('OffsetHigh', ctypes.c_ulong),
      ('hEvent', ctypes.c_void_p)
    ]

  class FILE_NOTIFY_INFORMATION(ctypes.Structure):
    _fields_ = [
      ('NextEntryOffset', ctypes.c_uint32),
      ('Action', ctypes.c_uint32),
      ('FileNameLength', ctypes.c_uint32)
    ]

  # Windows API functions
  def CreateFile(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile):
    func = ctypes.windll.kernel32.CreateFileW
    func.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    func.restype = ctypes.c_void_p
    return func(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)

  def CreateEvent(lpEventAttributes, bManualReset, bInitialState, lpName):
    func = ctypes.windll.kernel32.CreateEventW
    func.argtypes = [ctypes.c_void_p, ctypes.c_bool, ctypes.c_bool, ctypes.c_wchar_p]
    func.restype = ctypes.c_void_p
    return func(lpEventAttributes, bManualReset, bInitialState, lpName)

  def ReadDirectoryChangesW(hDirectory, lpBuffer, nBufferLength, bWatchSubtree, dwNotifyFilter, lpBytesReturned, lpOverlapped, lpCompletionRoutine):
    func = ctypes.windll.kernel32.ReadDirectoryChangesW
    func.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_bool, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(OVERLAPPED), ctypes.c_void_p]
    func.restype = ctypes.c_bool
    return func(hDirectory, lpBuffer, nBufferLength, bWatchSubtree, dwNotifyFilter, lpBytesReturned, lpOverlapped, lpCompletionRoutine)

  def WaitForMultipleObjects(nCount, lpHandles, bWaitAll, dwMilliseconds):
    func = ctypes.windll.kernel32.WaitForMultipleObjects
    func.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p), ctypes.c_bool, ctypes.c_uint32]
    func.restype = ctypes.c_uint32
    return func(nCount, lpHandles, bWaitAll, dwMilliseconds)

  def GetOverlappedResult(hFile, lpOverlapped, lpNumberOfBytesTransferred, bWait):
    func = ctypes.windll.kernel32.GetOverlappedResult
    func.argtypes = [ctypes.c_void_p, ctypes.POINTER(OVERLAPPED), ctypes.POINTER(ctypes.c_uint32), ctypes.c_bool]
    func.restype = ctypes.c_bool
    return func(hFile, lpOverlapped, lpNumberOfBytesTransferred, bWait)

  def CloseHandle(hObject):
    func = ctypes.windll.kernel32.CloseHandle
    func.argtypes = [ctypes.c_void_p]
    func.restype = ctypes.c_bool
    return func(hObject)

  def parse_file_notify_information(buffer, bytes_returned):
    results = []
    offset = 0
    while offset + ctypes.sizeof(FILE_NOTIFY_INFORMATION) < bytes_returned:
      info = FILE_NOTIFY_INFORMATION.from_buffer(buffer, offset)
      if info.FileNameLength:
        first_char = ctypes.addressof(info) + ctypes.sizeof(FILE_NOTIFY_INFORMATION)
        filename = ctypes.wstring_at(first_char, info.FileNameLength // 2)
        results.append((info.Action, filename))
      if info.NextEntryOffset == 0:
        break
      offset += info.NextEntryOffset
    return results

  SetConsoleCtrlHandler(lambda x: semaphore.release(), True)

  def watch_thread():
    # Setup watches for all directories
    watches = []
    events = []
    buffers = []
    overlapped_structs = []

    try:
      for watch_dir in watch_dirs:
        # Create directory handle
        handle = CreateFile(
          watch_dir,
          GENERIC_READ,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
          None,
          OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
          None
        )
        
        if handle == INVALID_HANDLE_VALUE:
          raise ctypes.WinError()

        # Create event for overlapped I/O
        event = CreateEvent(None, True, False, None)
        if not event:
          raise ctypes.WinError()

        # Create buffer and overlapped structure
        buffer = ctypes.create_string_buffer(1024)
        overlapped = OVERLAPPED()
        overlapped.hEvent = event

        # Start async read
        bytes_returned = ctypes.c_uint32()
        success = ReadDirectoryChangesW(
          handle,
          buffer,
          1024,
          True,
          FILE_NOTIFY_CHANGE_LAST_WRITE,
          ctypes.byref(bytes_returned),
          ctypes.byref(overlapped),
          None
        )

        if not success and ctypes.GetLastError() != 997:  # ERROR_IO_PENDING
          raise ctypes.WinError()

        watches.append(handle)
        events.append(event)
        buffers.append(buffer)
        overlapped_structs.append(overlapped)

      # Wait for any event to be signaled
      if events:
        event_array = (ctypes.c_void_p * len(events))(*events)
        result = WaitForMultipleObjects(len(events), event_array, False, 0xFFFFFFFF)  # INFINITE
        
        if result >= WAIT_OBJECT_0 and result < WAIT_OBJECT_0 + len(events):
          # One of our events was signaled
          index = result - WAIT_OBJECT_0
          
          # Get the results
          bytes_returned = ctypes.c_uint32()
          if GetOverlappedResult(watches[index], ctypes.byref(overlapped_structs[index]), ctypes.byref(bytes_returned), False):
            results = parse_file_notify_information(buffers[index], bytes_returned.value)
            for action, filename in results:
              print(filename)

    finally:
      # Cleanup
      for handle in watches:
        CloseHandle(handle)
      for event in events:
        CloseHandle(event)

    semaphore.release()

  threading.Thread(target=watch_thread, daemon=True).start()
  semaphore.acquire()
