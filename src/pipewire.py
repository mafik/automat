# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

import re
import extension_helper

hook = extension_helper.ExtensionHelper('PipeWire', globals())

hook.FetchFromGit('https://gitlab.freedesktop.org/pipewire/pipewire.git', '1.2.5')

hook.ConfigureWithMeson('{PREFIX}/include/pipewire-0.3/pipewire/pipewire.h')
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
hook.AddCompileArg(lambda build_type: [f'-I{build_type.PREFIX()}/include/pipewire-0.3', f'-I{build_type.PREFIX()}/include/spa-0.2'])
hook.AddLinkArgs('-ldl', '-lm', '-lpipewire-0.3')
