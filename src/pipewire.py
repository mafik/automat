# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

import extension_helper
import build

hook = extension_helper.ExtensionHelper('PipeWire', globals())

hook.FetchFromGit('https://gitlab.freedesktop.org/pipewire/pipewire.git', '1.2.5')

hook.ConfigureWithMeson(build.PREFIX / 'include' / 'pipewire-0.3' / 'pipewire' / 'pipewire.h')
hook.ConfigureOptions(**{
  'spa-plugins': 'disabled',
  'alsa': 'disabled',
  'pipewire-alsa': 'disabled',
  'pipewire-jack': 'disabled',
  'systemd': 'disabled',
  'gstreamer': 'disabled',
  'tests': 'disabled',
  'libusb': 'disabled',
  'libpulse': 'disabled',
  'pw-cat': 'disabled',
  'sndfile': 'disabled',
  'dbus': 'disabled',
  'flatpak': 'disabled',
  'examples': 'disabled',
  'x11': 'disabled',
  'session-managers': '[]',
})
hook.InstallWhenIncluded(r'pipewire/pipewire\.h')
hook.AddCompileArgs(f'-I{build.PREFIX / "include" / "pipewire-0.3"}', f'-I{build.PREFIX / "include" / "spa-0.2"}')
hook.AddLinkArgs('-ldl', '-lm', f'-Wl,-rpath,{build.PREFIX / "lib64"}', '-lpipewire-0.3')
