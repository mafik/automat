# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Warning: coded with a stochastic parrot

import extension_helper
import build

hook = extension_helper.ExtensionHelper('Babl', globals())

hook.FetchFromGit('https://gitlab.gnome.org/GNOME/babl.git', 'BABL_0_1_118')

# Babl is linked statically, so its loadable extensions (SIMD fast paths that
# babl_init would dlopen from the compiled-in path) are not built at all; the
# reference conversions in the core library cover every format. Tests, tools
# and the CLI are dropped together with them. The def-file consistency check
# expects a shared library, and the wrap-build dependency must not reference
# the removed extensions directory.
def _patch_static_core_only(marker):
  path = hook.src_dir / 'meson.build'
  text = path.read_text()
  replacements = [
    ("""subdir('babl')
subdir('extensions')
subdir('tests')
subdir('tools')
if build_docs
  subdir('docs')
endif
subdir('bin')""",
     "subdir('babl')"),
    ("    build_by_default: true,\n", "    build_by_default: false,\n"),
    ("    'babl_path'   : babl_extensions_build_dir,\n",
     "    'babl_path'   : babl_library_build_dir,\n"),
  ]
  for needle, replacement in replacements:
    if needle in text:
      text = text.replace(needle, replacement)
    elif replacement not in text:
      raise RuntimeError(f'Babl patch no longer applies: {needle!r} not found in {path}')
  path.write_text(text)

  # An extension-free babl is the intended state here, not a broken install;
  # the warning would print on Automat's stderr at every first GEGL use.
  path = hook.src_dir / 'babl' / 'babl-extension.c'
  text = path.read_text()
  needle = 'if (babl_db_count (db) <= 1)'
  replacement = 'if (0 && babl_db_count (db) <= 1) /* Automat builds no extensions */'
  if needle in text:
    path.write_text(text.replace(needle, replacement))
  elif replacement not in text:
    raise RuntimeError(f'Babl patch no longer applies: {needle!r} not found in {path}')
  marker.touch()
hook.PatchSources(_patch_static_core_only)

hook.ConfigureWithMeson(build.PREFIX / 'include' / 'babl-0.1' / 'babl' / 'babl.h')
hook.ConfigureOptions(**{
  'with-docs': 'false',
  'enable-gir': 'false',
  'enable-vapi': 'false',
  'gi-docgen': 'disabled',
  'with-lcms': 'disabled',
})
hook.InstallWhenIncluded(r'babl/babl\.h')
hook.AddCompileArgs(f'-I{build.PREFIX / "include" / "babl-0.1"}')
hook.AddLinkArgs('-lbabl-0.1', '-lm', '-ldl', '-pthread')
