'''This module ensures that all of the necessary Windows dependencies are installed.'''
# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import os, shutil, fs_utils
from typing import Callable
from subprocess import run, check_output
from pathlib import Path

def WinRegGetSystemEnv(name):
  import winreg
  key = winreg.CreateKey(winreg.HKEY_LOCAL_MACHINE, r"System\CurrentControlSet\Control\Session Manager\Environment")
  return winreg.QueryValueEx(key, name)[0]

def WinRegGetUserEnv(name):
  import winreg
  key = winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Environment")
  return winreg.QueryValueEx(key, name)[0]

def WinRegSetUserEnv(name, value):
  import winreg
  key = winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Environment")
  winreg.SetValueEx(key, name, 0, winreg.REG_EXPAND_SZ, value)

def EnsureLLVMInPath():
  # Default installer doesn't add LLVM to PATH, so we need to do it manually.
  # We do this for the whole user (as opposed to the current process) to make
  # it more convenient for the user to use LLVM in other contexts.
  #
  # Note: It would be really nice if LLVM installer could take care of the
  # PATH update. See: https://github.com/llvm/llvm-project/issues/54724
  curr_path = WinRegGetUserEnv('PATH')
  sys_path = WinRegGetSystemEnv('PATH')
  llvm_path = 'C:\\Program Files\\LLVM\\bin'
  if llvm_path not in curr_path and llvm_path not in sys_path:
    print(f'Adding "{llvm_path}" to current user\'s PATH...')
    if not curr_path.endswith(';'):
      curr_path += ';'
    curr_path += llvm_path
    curr_path += ';'
    WinRegSetUserEnv('PATH', curr_path)

def RefreshPath():
  system_path = WinRegGetSystemEnv('PATH')
  user_path = WinRegGetUserEnv('PATH')
  os.environ['PATH'] = f'{system_path};{user_path}'


def SHA256(path):
  import hashlib
  return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def Download(name, url, output_path, expected_sha256):
  import urllib.request
  if os.path.exists(output_path):
    if SHA256(output_path) == expected_sha256:
      return
    print(f'{name} already exists but is corrupted. Downloading again...')
    os.unlink(output_path)
  print(f'Downloading {name} to {output_path}...')
  urllib.request.urlretrieve(url, output_path)
  if expected_sha256 and SHA256(output_path) != expected_sha256:
    raise Exception(f'{name} has a wrong SHA-256 hash.')


def InstallGit():
  fs_utils.build_dir.mkdir(parents=True, exist_ok=True)
  filename = fs_utils.build_dir / 'Git-2.49.0-64-bit.exe'
  Download('Git',
           'https://github.com/git-for-windows/git/releases/download/v2.49.0.windows.1/Git-2.49.0-64-bit.exe',
           filename,
           '726056328967f242fe6e9afbfe7823903a928aff577dcf6f517f2fb6da6ce83c')
  print('Installing Git...')
  run([filename,
       '/SILENT',
       '/NORESTART',
       '/NOCANCEL',
       '/SP-',
       '/CLOSEAPPLICATIONS',
       '/RESTARTAPPLICATIONS',
       '/COMPONENTS="icons,ext\\reg\\shellhere,assoc,assoc_sh"'])
  print('Git installed.')

def InstallCMake():
  fs_utils.build_dir.mkdir(parents=True, exist_ok=True)
  filename = fs_utils.build_dir / 'cmake-3.31.6-windows-x86_64.msi'
  Download('CMake',
           'https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-windows-x86_64.msi',
           filename,
           '4cc39d59426cbbeb33d0daff1350e39b0585a90e3c9883dbba70816b8d282811')
  print('Installing CMake...')
  run(['msiexec', '/i', filename, 'ALLUSERS=1', 'ADD_CMAKE_TO_PATH=System', '/passive'])
  print('CMake installed.')

def InstallClang():
  launcher = fs_utils.third_party_dir / 'WinElevator' / 'launcher.exe'
  fs_utils.build_dir.mkdir(parents=True, exist_ok=True)
  filename = fs_utils.build_dir / 'LLVM-20.1.1-win64.exe'
  Download('LLVM',
           'https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.1/LLVM-20.1.1-win64.exe',
           filename,
           '40fe7010123a501eb72d25a6e71ba536a10b5db47f6c19e3cf01f6157f7ff54a')
  print('Installing LLVM...')
  input('Press ENTER to continue... (you will be prompted for admin access)')
  run([launcher, filename, '/S'])
  print('LLVM installed.')

def AliasPython3():
  python_path = Path(shutil.which('python'))
  python3_path = python_path.with_stem('python3')
  launcher = fs_utils.third_party_dir / 'WinElevator' / 'launcher.exe'
  print('Aliasing python to python3...')
  input('Press ENTER to continue... (you will be prompted for admin access)')
  args = [launcher, 'python', '-c', f'import shutil; shutil.copy2(r"{python_path}", r"{python3_path}")']
  run(args)
  print('python3 alias created.')

command_to_installer = {
  'git': InstallGit,
  'cmake': InstallCMake,
  'clang': InstallClang,
  'python3': AliasPython3,
}

def CheckCommand(command: str, installer: Callable[[], None]):
  while not shutil.which(command):
    installer()
    RefreshPath()

msvc_tools_dir = 'C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\BuildTools\\VC\\Tools\\MSVC\\14.29.30133\\'

windows_sdk_dir = 'C:\\Program Files (x86)\\Windows Kits\\10\\'
windows_sdk_version = '10.0.26100.0'
windows_sdk_include_dir = windows_sdk_dir + 'include\\' + windows_sdk_version + '\\'
windows_sdk_lib_dir = windows_sdk_dir + 'lib\\' + windows_sdk_version + '\\'

def EnsureVisualStudioBuildToolsInstalled():
  if os.path.exists(msvc_tools_dir) and os.path.exists(windows_sdk_include_dir) and os.path.exists(windows_sdk_lib_dir):
    return
  url = 'https://aka.ms/vs/16/release/vs_buildtools.exe'
  fs_utils.build_dir.mkdir(parents=True, exist_ok=True)
  filename = fs_utils.build_dir / 'vs_BuildTools.exe'
  # Microsoft seems to be changing the installer hash every now and then.
  # 2024-10: 6d2322a49b1666a95d3ba7fa4947b8b689e8c378296a99b78e01d945040d45f9
  # 2025-06-06: 1d9a29c9b731030bc077f384ad2d7580747906576d1d0d2bc6b33bf6fcb483bc
  # As we don't really care about reproducibility ATM, we can accept Microsoft's decision and
  # use whatever they serve.
  Download('Visual Studio 2019 Build Tools',
           url,
           filename, expected_sha256=None)
  print('Installing Visual Studio 2019 Build Tools...')
  run([filename,
       '--passive', # show progress window during install
       '--add', 'Microsoft.VisualStudio.Workload.VCTools', # very likely needed
       '--add', 'Microsoft.VisualStudio.Workload.MSBuildTools', # unknown if needed
       '--add', 'Microsoft.VisualStudio.Component.Windows10SDK', # very likely needed
       '--includeRecommended', # unknown if needed
       '--norestart',
       '--wait'])
  
def SetupEnvironment():
  # See: https://clang.llvm.org/docs/UsersManual.html#windows-system-headers-and-library-lookup
  # Environment variables obtained from:
  # C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat amd64 10.0.26100.0 -vcvars_ver=14.29
  # and then running `set`.

  os.environ['INCLUDE'] = ';'.join(
    [msvc_tools_dir + 'include',
     windows_sdk_include_dir + 'ucrt',
     windows_sdk_include_dir + 'shared',
     windows_sdk_include_dir + 'um',
     windows_sdk_include_dir + 'winrt',
     windows_sdk_include_dir + 'cppwinrt'])
  
  os.environ['LIB'] = ';'.join(
    [msvc_tools_dir + 'lib\\x64',
     windows_sdk_lib_dir + 'ucrt\\x64',
     windows_sdk_lib_dir + 'um\\x64'])
  
  os.environ['VCToolsInstallDir'] = msvc_tools_dir


def check_and_install():
  EnsureLLVMInPath()
  RefreshPath()
  for command, installer in command_to_installer.items():
    CheckCommand(command, installer)

  EnsureVisualStudioBuildToolsInstalled()
  SetupEnvironment()
