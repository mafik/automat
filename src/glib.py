# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Warning: coded with a stochastic parrot

# Static GLib, installed into PREFIX so that dependent extensions (GStreamer)
# can resolve glib-2.0 & friends through pkg-config.

import os
import subprocess
from pathlib import Path
import extension_helper
import build
import src

zlib_ext = src.load_extension('zlib')

hook = extension_helper.ExtensionHelper('GLib', globals())

if build.platform == 'win32':
  # meson resolves the zlib dependency (and every GLib consumer resolves GLib)
  # through pkg-config, which Windows lacks; ezwinports ships a native build.
  pkg_config = extension_helper.ExtensionHelper('pkg_config', globals())
  pkg_config.FetchFromURL('https://downloads.sourceforge.net/project/ezwinports/pkg-config-0.28-w32-bin.zip')
  pkg_config.SkipConfigure()
  PKG_CONFIG_EXE = pkg_config.checkout_dir / 'bin' / 'pkg-config.exe'


def UsePkgConfig(other_hook):
  '''Points a meson build at the fetched pkg-config; a no-op outside Windows.'''
  if build.platform == 'win32':
    other_hook.ConfigureDependsOn(pkg_config.checkout_dir)
    other_hook.ConfigureEnvReplace('PKG_CONFIG', str(PKG_CONFIG_EXE))


hook.FetchFromGit('https://github.com/GNOME/glib.git', '2.84.4')

# GNU-syntax clang with MSVC headers slips through GLib's msvc/clang-cl
# conditions: the ssize_t probe then reads the absent unistd.h and the bundled
# dirent shim is skipped. Key on the platform / the missing header instead.
hook.PatchSources('''--- meson.build
+++ meson.build
@@ -1613,7 +1613,7 @@
 endif
 sizet_size = cc.sizeof('size_t')
-if cc.get_id() == 'msvc' or cc.get_id() == 'clang-cl'
+if host_system == 'windows'
   ssizet_size = cc.sizeof('SSIZE_T', prefix : '#include <BaseTsd.h>')
 else
   ssizet_size = cc.sizeof('ssize_t', prefix : '#include <unistd.h>')
 endif
--- glib/meson.build
+++ glib/meson.build
@@ -365,5 +365,5 @@
   glib_sources += files('gwin32.c', 'gspawn-win32.c', 'giowin32.c')
   platform_deps = [winsock2, cc.find_library('winmm')]
-  if cc.get_id() == 'msvc' or cc.get_id() == 'clang-cl'
+  if not cc.has_header('dirent.h')
     glib_sources += files('dirent/wdirent.c')
   endif
''')

hook.ConfigureDependsOn(zlib_ext.hook)
UsePkgConfig(hook)
if build.platform == 'win32':
  # Windows-target clang folds girepository/cmph's debug macros into
  # -Werror=format=2; the Linux build of the same sources does not warn.
  hook.ConfigureOption('c_args', '-Wno-error=format-nonliteral')
# bin-devel carries glib-mkenums & glib-genmarshal, which GStreamer's build runs.
hook.ConfigureWithMeson(
  build.PREFIX / 'include' / 'glib-2.0' / 'glib.h',
  build.PREFIX / 'lib64' / 'libglib-2.0.a',
  install_tags='devel,runtime,bin-devel')
hook.ConfigureOptions(**{
  'introspection': 'disabled',
  'documentation': 'false',
  'man-pages': 'disabled',
  'tests': 'false',
  'nls': 'disabled',
  'selinux': 'disabled',
  'libmount': 'disabled',
  'libelf': 'disabled',
  'xattr': 'false',
  'dtrace': 'disabled',
  'systemtap': 'disabled',
  'sysprof': 'disabled',
  'glib_debug': 'disabled',
})
hook.InstallWhenIncluded(r'g(lib|io|object|module)[-./]')
hook.AddCompileArgs(f'-I{build.PREFIX / "include" / "glib-2.0"}',
                    f'-I{build.PREFIX / "lib64" / "glib-2.0" / "include"}')


def StaticLibs(*pkgs, pc_dirs=()):
  '''Zero-arg callable resolving link args at link time from .pc files installed into PREFIX.

  `pc_dirs` are extra pkg-config search directories (beyond PREFIX/lib64/pkgconfig).

  gmodule-2.0.pc injects -Wl,--export-dynamic so loadable modules can resolve symbols
  from the executable. Nothing here loads modules, and the flag would put the whole
  statically linked binary (GLib, LLVM) into the dynamic symbol table, where dlopen'd
  libraries - software Vulkan drivers carry their own LLVM - bind against it and hang
  the renderer at startup. It is stripped for every consumer of this helper.'''
  def resolve():
    for pkg in pkgs:
      if not (build.PREFIX / 'lib64' / 'pkgconfig' / f'{pkg}.pc').exists():
        raise RuntimeError(f'{pkg}.pc is not installed in {build.PREFIX} - '
                           'pkg-config would silently fall back to the system copy')
    env = os.environ.copy()
    paths = [str(d) for d in pc_dirs]
    if env.get('PKG_CONFIG_PATH'):
      paths.append(env['PKG_CONFIG_PATH'])
    env['PKG_CONFIG_PATH'] = os.pathsep.join(paths)
    exe = str(PKG_CONFIG_EXE) if build.platform == 'win32' else 'pkg-config'
    args = subprocess.check_output([exe, '--libs', '--static', *pkgs],
                                   env=env, text=True).split()
    args = [arg for arg in args if arg != '-Wl,--export-dynamic']
    if build.platform == 'win32':
      # lld-link resolves -lfoo as foo.lib and never finds meson's libfoo.a;
      # name the archives directly. System import libs stay -l and resolve
      # through the LIB environment.
      lib_dirs = [Path(arg[2:]) for arg in args if arg.startswith('-L')]
      def Materialize(arg):
        if arg.startswith('-l'):
          for lib_dir in lib_dirs:
            for candidate in (lib_dir / f'lib{arg[2:]}.a', lib_dir / f'{arg[2:]}.lib'):
              if candidate.exists():
                return str(candidate)
        return arg
      args = [Materialize(arg) for arg in args]
    return args
  return resolve


hook.AddLinkArg(StaticLibs('gmodule-2.0', 'gobject-2.0'))
