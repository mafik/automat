# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Warning: coded with a stochastic parrot

# Vendors the Hyprland stack that Automat's Wayland compositor builds on:
# hyprutils (memory/signal/math primitives), hyprwayland-scanner (C++ protocol
# codegen) and aquamarine (backend & buffer abstractions). The system graphics
# libraries they sit on (libdrm, gbm, pixman, libseat, libinput, libdisplay-info)
# come from the distro.

import sys
import build
import extension_helper
import fs_utils
import make
import src
from functools import partial

if sys.platform == 'linux':

  # Server-side C++ protocol bindings for Automat's own compositor, generated
  # the same way Hyprland generates its: hyprwayland-scanner into one
  # .cpp/.hpp pair per protocol, each compiled as its own translation unit.
  PROTOCOLS = [
    ('/usr/share/wayland/wayland.xml', ['--wayland-enums']),
    ('/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml', []),
    ('/usr/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml', []),
    ('/usr/share/wayland-protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml', []),
  ]

  scanner_bin = build.PREFIX / 'bin' / 'hyprwayland-scanner'

  def hook_srcs(srcs: dict[str, 'src.File'], recipe: 'make.Recipe'):
    from pathlib import Path
    for xml_path, extra_args in PROTOCOLS:
      xml = Path(xml_path)
      cpp = fs_utils.generated_dir / (xml.stem + '.cpp')
      hpp = fs_utils.generated_dir / (xml.stem + '.hpp')
      # Each protocol must stay its own translation unit: the generated .cpp
      # re-includes its .hpp with `private` redefined.
      recipe.add_step(
          partial(make.Popen, [scanner_bin, *extra_args, xml, fs_utils.generated_dir]),
          [cpp, hpp],
          [scanner_bin, xml],
          desc=f'Generating {xml.stem} protocol',
          shortcut=f'protocol {xml.stem}')
      for path in (cpp, hpp):
        recipe.generated.add(str(path))
        srcs[str(path)] = src.File(path)

  def StaticizeCMake(hook):
    # hyprutils & aquamarine hardcode SHARED; Automat links statically.
    def Patch(token):
      path = hook.src_dir / 'CMakeLists.txt'
      path.write_text(path.read_text().replace(' SHARED ', ' STATIC '))
      token.touch()
    return Patch

  # System libwayland-server, linked whenever a TU includes <wayland-server.h>
  # (the generated protocol bindings do).
  wayland_server = extension_helper.ExtensionHelper('wayland-server', globals())
  wayland_server.SkipConfigure()
  wayland_server.InstallWhenIncluded(r'^wayland-server')
  wayland_server.AddLinkArgs('-lwayland-server')

  hyprutils = extension_helper.ExtensionHelper('hyprutils', globals())
  hyprutils.FetchFromGit('https://github.com/hyprwm/hyprutils.git', 'v0.13.1')
  hyprutils.PatchSources(StaticizeCMake(hyprutils))
  hyprutils.ConfigureWithCMake(build.PREFIX / 'lib64' / 'libhyprutils.a')
  hyprutils.InstallWhenIncluded(r'^hyprutils/.*')
  hyprutils.AddCompileArgs('-I/usr/include/pixman-1')
  hyprutils.AddLinkArgs('-l:libhyprutils.a', '-lpixman-1')

  hyprwayland_scanner = extension_helper.ExtensionHelper('hyprwayland-scanner', globals())
  hyprwayland_scanner.FetchFromGit('https://github.com/hyprwm/hyprwayland-scanner.git', 'v0.4.6')
  hyprwayland_scanner.ConfigureWithCMake(build.PREFIX / 'bin' / 'hyprwayland-scanner')

  aquamarine = extension_helper.ExtensionHelper('aquamarine', globals())
  aquamarine.FetchFromGit('https://github.com/hyprwm/aquamarine.git', 'v0.12.1')

  def PatchAquamarine(token):
    path = aquamarine.src_dir / 'CMakeLists.txt'
    text = path.read_text().replace(' SHARED ', ' STATIC ')
    # The custom commands expect the scanner on $PATH; use the BINDIR exported
    # by hyprwayland-scanner-config.cmake instead.
    text = text.replace('COMMAND hyprwayland-scanner', 'COMMAND ${BINDIR}/hyprwayland-scanner')
    path.write_text(text)
    token.touch()

  aquamarine.PatchSources(PatchAquamarine)
  aquamarine.ConfigureDependsOn(hyprutils, hyprwayland_scanner)
  # Debian's cmake/pkg-config don't search lib64 under a prefix; point them there.
  aquamarine.ConfigureOption('hyprwayland-scanner_DIR',
                             str(build.PREFIX / 'lib64' / 'cmake' / 'hyprwayland-scanner'))
  aquamarine.ConfigureEnvReplace('PKG_CONFIG_PATH', str(build.PREFIX / 'lib64' / 'pkgconfig'))
  aquamarine.ConfigureWithCMake(build.PREFIX / 'lib64' / 'libaquamarine.a')
  aquamarine.InstallWhenIncluded(r'^aquamarine/.*')
  aquamarine.AddCompileArgs('-I/usr/include/pixman-1', '-I/usr/include/libdrm')
  aquamarine.AddLinkArgs('-l:libaquamarine.a', '-l:libhyprutils.a', '-lseat', '-linput', '-ludev',
                         '-ldisplay-info', '-lpixman-1', '-ldrm', '-lgbm', '-lwayland-client',
                         '-lwayland-server', '-lEGL', '-lGLESv2')
