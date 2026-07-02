# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Warning: coded with a stochastic parrot

# Static GStreamer built from the monorepo: core plus the few plugins Automat
# uses, merged into libgstreamer-full-1.0.a. The core is compiled with
# GST_FULL_STATIC_COMPILATION, so gst_init registers the built-in plugins
# itself - no extra registration call is needed in Automat.

import extension_helper
import build
import src

glib = src.load_extension('glib')

hook = extension_helper.ExtensionHelper('GStreamer', globals())

hook.FetchFromGit('https://github.com/GStreamer/gstreamer.git', '1.26.2')
hook.DependsOn(glib.hook)
hook.ConfigureWithMeson(
  build.PREFIX / 'include' / 'gstreamer-1.0' / 'gst' / 'gst.h',
  build.PREFIX / 'lib64' / 'libgstreamer-full-1.0.a',
  build.PREFIX / 'lib64' / 'pkgconfig' / 'gstreamer-full-1.0.pc')
hook.ConfigureOptions(**{
  'auto_features': 'disabled',
  'gst-full': 'enabled',
  'gst-full-target-type': 'static_library',
  'gst-full-libraries': 'gstreamer-app-1.0,gstreamer-video-1.0',
  # '*' registers every plugin built below; explicit lists must use library
  # file names (libgstX.a), which is more brittle than restricting the build.
  'gst-full-plugins': '*',
  'base': 'enabled',
  'good': 'enabled',
  'ugly': 'disabled',
  'bad': 'disabled',
  'libav': 'disabled',
  'devtools': 'disabled',
  'ges': 'disabled',
  'rtsp_server': 'disabled',
  'gst-examples': 'disabled',
  'python': 'disabled',
  'tls': 'disabled',
  'libnice': 'disabled',
  'gtk': 'disabled',
  'qt5': 'disabled',
  'qt6': 'disabled',
  'webrtc': 'disabled',
  'orc': 'disabled',
  'tools': 'disabled',
  'examples': 'disabled',
  'tests': 'disabled',
  'benchmarks': 'disabled',
  'introspection': 'disabled',
  'nls': 'disabled',
  'doc': 'disabled',
  'glib_debug': 'disabled',
  # Pipelines are built programmatically; skipping the string parser also
  # drops the flex/bison build dependency.
  'gstreamer:gst_parse': 'false',
  # Every plugin is compiled in, so the on-disk registry cache is useless.
  # Worse, a stale ~/.cache/gstreamer-1.0/ cache written by a shared
  # GStreamer masks the static features with file-backed ones.
  'gstreamer:registry': 'false',
  # The plugins Automat needs (auto_features=disabled turns off the rest;
  # coreelements is unconditional):
  'gst-plugins-base:app': 'enabled',                # appsrc, appsink
  'gst-plugins-base:videotestsrc': 'enabled',
  'gst-plugins-base:videoconvertscale': 'enabled',  # videoconvert, videoscale
  'gst-plugins-base:typefind': 'enabled',           # typefindfunctions
  'gst-plugins-base:audiotestsrc': 'enabled',
  'gst-plugins-base:audioconvert': 'enabled',
  'gst-plugins-good:videofilter': 'enabled',        # videoflip
  'gst-plugins-good:level': 'enabled',              # audio level analyzer
})
hook.InstallWhenIncluded(r'gst/')
hook.AddCompileArgs(f'-I{build.PREFIX / "include" / "gstreamer-1.0"}',
                    '-DGST_STATIC_COMPILATION',
                    *glib.hook.compile_args)
# The per-plugin .pc files land in a subdirectory that pkg-config does not
# search by default.
hook.AddLinkArg(glib.StaticLibs(
    'gstreamer-full-1.0',
    pc_dirs=(build.PREFIX / 'lib64' / 'gstreamer-1.0' / 'pkgconfig',)))
