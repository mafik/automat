# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import os
import sys
import build
import extension_helper
import src

hook = extension_helper.ExtensionHelper('xkbcommon', globals())
hook.FetchFromURL('https://github.com/xkbcommon/libxkbcommon/archive/refs/tags/xkbcommon-1.8.1.tar.gz')
hook.ConfigureWithMeson(build.PREFIX / 'lib64' / 'libxkbcommon.a')
hook.ConfigureOption('enable-tools', 'false')
hook.ConfigureOption('enable-xkbregistry', 'false')
hook.InstallWhenIncluded(r'^xkbcommon/.*\.h$')

if sys.platform == 'linux':
  xcb = src.load_extension('xcb')
  # libxkbcommon mixes C & statically-built C++ dependencies.
  # This confuses Meson. Here we explicitly tell it to link in the C++ stdlib.
  #
  # TODO: Make the PKG_CONFIG_LIBDIR default for all extensions so they don't link against host's libs.
  hook.ConfigureEnvReplaces(
    LDFLAGS='-lstdc++',
    PKG_CONFIG_LIBDIR=':'.join(str(build.PREFIX / x / 'pkgconfig') for x in ('lib64', 'lib', 'share')),
  )
  hook.ConfigureOption('enable-x11', 'true')
  hook.ConfigureOption('xkb-config-root', '/usr/share/X11/xkb')
  hook.ConfigureDependsOn(xcb.libxcb)
  hook.AddLinkArgs('-l:libxkbcommon.a', '-l:libxkbcommon-x11.a', '-l:libxcb-xkb.a')
elif sys.platform == 'win32':
  # xkbcommon generates its keymap parser with bison at build time; winflexbison
  # provides a native bison that xkbcommon's meson finds under the name win_bison.
  winflexbison = extension_helper.ExtensionHelper('winflexbison', globals())
  winflexbison.FetchFromURL('https://github.com/lexxmark/winflexbison/releases/download/v2.5.25/win_flex_bison-2.5.25.zip')
  winflexbison.SkipConfigure()

  # Windows has no /usr/share/X11/xkb, so xkeyboard-config's keymap data is
  # shipped in our prefix. It's data-only: nothing links against it, and its
  # files carry no meson install tag (hence install_tags=None).
  xkeyboard_config = extension_helper.ExtensionHelper('xkeyboard_config', globals())
  xkeyboard_config.FetchFromURL('https://xorg.freedesktop.org/archive/individual/data/xkeyboard-config/xkeyboard-config-2.48.tar.xz')

  def DropPerlLstRules(marker):
    # The *.lst layout listings are generated with perl (xml2lst.pl), which Windows
    # lacks. xkbcommon never reads them - they only feed layout-picker UIs.
    path = xkeyboard_config.src_dir / 'rules' / 'meson.build'
    text = path.read_text()
    lst_block = '''    # Third: generate the evdev.lst and base.lst files
    lst_file = f'@ruleset@.lst'
    custom_target(lst_file,
                  build_by_default: true,
                  command: [xml2lst, ruleset_xml],
                  capture: true,
                  output: lst_file,
                  install: true,
                  install_dir: dir_xkb_rules)'''
    if lst_block not in text:
      raise ValueError(f'xkeyboard-config .lst patch no longer matches {path}')
    text = text.replace(lst_block, '')
    text = text.replace("xml2lst = find_program('xml2lst.pl')\n", '')
    path.write_text(text)
    marker.touch()
  xkeyboard_config.PatchSources(DropPerlLstRules)
  xkeyboard_config.ConfigureWithMeson(build.PREFIX / 'share' / 'X11' / 'xkb' / 'rules' / 'evdev',
                                      install_tags=None)
  hook.ConfigureDependsOn(xkeyboard_config)

  # There is no host X server to read the layout from, so enable-x11 is off. The
  # keymap data root points into our prefix, where xkeyboard-config gets installed.
  # Only the library is built: xkbcommon's default 'all' includes its test/bench
  # binaries, some of which don't link on Windows.
  hook.meson_build_targets = ['libxkbcommon.a']
  hook.ConfigureOption('enable-x11', 'false')
  hook.ConfigureOption('enable-wayland', 'false')
  hook.ConfigureOption('enable-bash-completion', 'false')
  hook.ConfigureOption('xkb-config-root', (build.PREFIX / 'share' / 'X11' / 'xkb').as_posix())
  hook.ConfigureEnvReplaces(PATH=str(winflexbison.checkout_dir) + os.pathsep + os.environ.get('PATH', ''))
  hook.ConfigureDependsOn(winflexbison.checkout_dir)
  hook.AddLinkArgs(str(build.PREFIX / 'lib64' / 'libxkbcommon.a'))
