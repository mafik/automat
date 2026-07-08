# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Warning: coded with a stochastic parrot

# Static JSON-GLib, installed into PREFIX. GEGL requires it.

import extension_helper
import build
import src

glib = src.load_extension('glib')

hook = extension_helper.ExtensionHelper('JsonGLib', globals())

hook.FetchFromGit('https://gitlab.gnome.org/GNOME/json-glib.git', '1.10.8')

hook.ConfigureDependsOn(glib.hook)
glib.UsePkgConfig(hook)

# GLib headers test __GNUC__ bare, which MSVC-target clang leaves undefined.
# Project args outrank -D c_args, so the flag has to go at the source.
hook.PatchSources('''--- meson.build
+++ meson.build
@@ -128,7 +128,6 @@
     '-Werror=sequence-point',
     '-Werror=trigraphs',
-    '-Werror=undef',
     '-Werror=write-strings',
   ]
 else
''')

hook.ConfigureWithMeson(
  build.PREFIX / 'include' / 'json-glib-1.0' / 'json-glib' / 'json-glib.h')
hook.ConfigureOptions(**{
  'introspection': 'disabled',
  'documentation': 'disabled',
  'man': 'false',
  'tests': 'false',
  'conformance': 'false',
  'installed_tests': 'false',
  'nls': 'disabled',
  'wrap_mode': 'nofallback',
})
hook.InstallWhenIncluded(r'json-glib/json-glib\.h')
hook.AddCompileArgs(f'-I{build.PREFIX / "include" / "json-glib-1.0"}')
hook.AddLinkArg(glib.StaticLibs('json-glib-1.0'))
