# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

import re
import extension_helper

hook = extension_helper.ExtensionHelper('PipeWire', globals())

hook.FetchFromGit('https://gitlab.freedesktop.org/pipewire/pipewire.git', '1.2.5')

def patch_meson_build(marker):
  # replace "libpipewire = shared_library" with "libpipewire = library"
  meson_build = hook.checkout_dir / 'src' / 'pipewire' / 'meson.build'
  lines = open(meson_build).readlines()
  with open(meson_build, 'w') as f:
    for line in lines:
      f.write(re.sub(r'libpipewire = shared_library', 'libpipewire = library', line))
  marker.touch()

hook.PatchSources(patch_meson_build)
hook.ConfigureWithMeson('{PREFIX}/lib64/libpipewire-0.3.a')
hook.ConfigureOptions(**{
  'spa-plugins': 'disabled',
  'alsa': 'disabled',
  'pipewire-alsa': 'disabled',
  'systemd': 'disabled',
  'gstreamer': 'disabled',
  'tests': 'disabled',
  'libusb': 'disabled',
  'libpulse': 'disabled',
  'pw-cat': 'disabled',
  'sndfile': 'disabled',
  'dbus': 'disabled',
  'flatpak': 'disabled',
  'session-managers': '[]',
})
hook.InstallWhenIncluded(r'pipewire/pipewire\.h')
hook.AddCompileArg(lambda build_type: ['-D_REENTRANT', f'-I{build_type.PREFIX()}/include/pipewire-0.3', f'-I{build_type.PREFIX()}/include/spa-0.2'])
hook.AddLinkArgs('-ldl', '-lm', '-pthread', '-lpipewire-0.3')

