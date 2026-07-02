# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Warning: coded with a stochastic parrot

# Static GLib, installed into PREFIX so that dependent extensions (GStreamer)
# can resolve glib-2.0 & friends through pkg-config.

import os
import subprocess
import extension_helper
import build
import src

zlib_ext = src.load_extension('zlib')

hook = extension_helper.ExtensionHelper('GLib', globals())

hook.FetchFromGit('https://github.com/GNOME/glib.git', '2.84.4')
hook.ConfigureDependsOn(zlib_ext.hook)
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
    args = subprocess.check_output(['pkg-config', '--libs', '--static', *pkgs],
                                   env=env, text=True).split()
    return [arg for arg in args if arg != '-Wl,--export-dynamic']
  return resolve


hook.AddLinkArg(StaticLibs('gmodule-2.0', 'gobject-2.0'))
