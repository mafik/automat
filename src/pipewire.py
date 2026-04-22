# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

import extension_helper
import build

hook = extension_helper.ExtensionHelper('PipeWire', globals())

hook.FetchFromGit('https://gitlab.freedesktop.org/pipewire/pipewire.git', '1.4.11')

# Upstream bug (present at least through 1.6.3): src/modules/meson.build unconditionally
# references `plugin_dependencies`, which is only defined inside spa/plugins/filter-graph/
# meson.build. When we build with `spa-plugins=disabled` (below), that subdir is skipped
# and meson aborts with "Unknown variable 'plugin_dependencies'" while configuring the
# echo-cancel module. Since we don't need the echo-cancel module at all, strip the stray
# reference so configure succeeds. See https://gitlab.freedesktop.org/pipewire/pipewire/
# -/blob/1.4.11/src/modules/meson.build#L101
def _patch_echo_cancel_plugin_dependencies(marker):
  path = hook.src_dir / 'src' / 'modules' / 'meson.build'
  text = path.read_text()
  needle = 'dependencies : [mathlib, dl_lib, pipewire_dep, plugin_dependencies],'
  replacement = 'dependencies : [mathlib, dl_lib, pipewire_dep],'
  if needle in text:
    path.write_text(text.replace(needle, replacement))
  elif replacement not in text:
    raise RuntimeError(
      f'PipeWire patch no longer applies: neither needle nor replacement found in {path}')
  marker.touch()
hook.PatchSources(_patch_echo_cancel_plugin_dependencies)

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
  'selinux': 'disabled',
  'gsettings': 'disabled',
  'avahi': 'disabled',
  'raop': 'disabled',
  'snap': 'disabled',
  'session-managers': '[]',
})
hook.InstallWhenIncluded(r'pipewire/pipewire\.h')
hook.AddCompileArgs(f'-I{build.PREFIX / "include" / "pipewire-0.3"}', f'-I{build.PREFIX / "include" / "spa-0.2"}')
hook.AddLinkArgs('-ldl', '-lm', '-lpipewire-0.3')
