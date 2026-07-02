# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Warning: coded with a stochastic parrot

# Static GEGL. Upstream builds every operation as a loadable module that
# gegl_init scans for at runtime; here the operation sets Automat uses
# (operations/common plus operations/core) are compiled into one static
# archive, libgegl-common.a, whose generated entry point
# `gegl_module_register` registers them all. Passing it a null GTypeModule
# makes GLib register the operation types statically
# (see g_type_module_register_type in third_party/GLib/gobject/gtypemodule.c).
# library_gegl.cpp calls it right after gegl_init.

import extension_helper
import build
import src

glib = src.load_extension('glib')
json_glib = src.load_extension('json_glib')
babl = src.load_extension('babl')

hook = extension_helper.ExtensionHelper('GEGL', globals())

hook.FetchFromGit('https://gitlab.gnome.org/GNOME/gegl.git', 'GEGL_0_4_62')


def _replace(path, needle, replacement):
  text = path.read_text()
  if needle in text:
    path.write_text(text.replace(needle, replacement))
  elif replacement not in text:
    raise RuntimeError(f'GEGL patch no longer applies: {needle!r} not found in {path}')


# libjpeg, libpng and libnsgif are hard dependencies upstream, but every use
# sits behind .found() in subdirectories this build prunes (bin, external
# operations, tests). The pruned tree keeps only what libgegl and the static
# operation bundle need; gen-loader.py is re-anchored here because it lived in
# the pruned tools directory.
def _patch_prune_tree(marker):
  path = hook.src_dir / 'meson.build'
  _replace(path,
    "libjpeg   = dependency('libjpeg',     version: dep_ver.get('libjpeg'))",
    "libjpeg   = dependency('libjpeg',     version: dep_ver.get('libjpeg'), required: false)")
  _replace(path,
    "libpng    = dependency('libpng',      version: dep_ver.get('libpng'))",
    "libpng    = dependency('libpng',      version: dep_ver.get('libpng'), required: false)")
  _replace(path,
    """libnsgif = dependency('libnsgif',
  fallback: ['libnsgif', 'libnsgif'],
)""",
    """libnsgif = dependency('libnsgif',
  fallback: ['libnsgif', 'libnsgif'],
  required: false,
)""")
  _replace(path,
    """subdir('seamless-clone')
subdir('bin')
subdir('tools')
subdir('operations')
subdir('examples')
subdir('tests')
subdir('perf')
subdir('po')
subdir('docs')""",
    """gen_loader = find_program('tools' / 'gen-loader.py')
subdir('operations')""")
  marker.touch()
hook.PatchSources(_patch_prune_tree)


# The SIMD variants of gegl-algorithms.c are uninstalled static libraries;
# only link_whole folds their objects into the installed libgegl-0.4.a.
def _patch_link_whole_simd(marker):
  _replace(hook.src_dir / 'gegl' / 'meson.build',
    '  link_with: simd_extra,',
    '  link_whole: simd_extra,')
  marker.touch()
hook.PatchSources(_patch_link_whole_simd)


# Operations are linked statically, so loadable modules from GEGL_PATH or
# user directories must never enter this process: they would bring their own
# GLib, incompatible with the statically linked one.
def _patch_no_module_scanning(marker):
  _replace(hook.src_dir / 'gegl' / 'gegl-init.c',
    """gegl_get_default_module_paths(void)
{
  GSList *list = NULL;""",
    """gegl_get_default_module_paths(void)
{
  /* Operations are compiled in; never scan for loadable modules. */
  return NULL;

  GSList *list = NULL;""")
  marker.touch()
hook.PatchSources(_patch_no_module_scanning)


# OpenCL must never load, no matter what GEGL_USE_OPENCL says: its runtime
# historically collided with Automat's statically linked LLVM inside software
# Vulkan drivers (white-window hang at startup).
def _patch_opencl_off(marker):
  _replace(hook.src_dir / 'gegl' / 'opencl' / 'gegl-cl-init.c',
    """{
  cl_int err;

  if (cl_state.hard_disable)""",
    """{
  cl_int err;

  cl_state.hard_disable = TRUE;

  if (cl_state.hard_disable)""")
  marker.touch()
hook.PatchSources(_patch_opencl_off)


# One static operation bundle. The core operations join the common source
# list, except json.c: its hand-written registration dereferences the
# GTypeModule and only scans (now empty) module paths. The x86-64-v2/v3
# duplicates of the bundle stay loadable-module territory and are dropped.
def _patch_static_operations(marker):
  _replace(hook.src_dir / 'operations' / 'meson.build',
    """subdir('common-gpl3+')
subdir('common-cxx')
subdir('common')
subdir('core')
subdir('external')
subdir('generated')
subdir('json')
subdir('seamless-clone')
subdir('transform')
if get_option('workshop')
  subdir('workshop')
endif""",
    "subdir('common')")
  path = hook.src_dir / 'operations' / 'common' / 'meson.build'
  _replace(path,
    """  'weighted-blend.c',
  'write-buffer.c',
)""",
    """  'weighted-blend.c',
  'write-buffer.c',
  '../core/cache.c',
  '../core/cast-format.c',
  '../core/cast-space.c',
  '../core/clone.c',
  '../core/convert-format.c',
  '../core/convert-space.c',
  '../core/crop.c',
  '../core/load.c',
  '../core/nop.c',
)""")
  _replace(path,
    "gegl_common = shared_library('gegl-common',",
    "gegl_common = static_library('gegl-common',")
  # Each operation includes its own .c file by bare name (gegl-op.h's
  # GEGL_OP_C_FILE); the folded core sources need their directory searched.
  _replace(path,
    "  include_directories: [ rootInclude, geglInclude, ],\n  dependencies: [",
    "  include_directories: [ rootInclude, geglInclude, include_directories('../core'), ],\n"
    '  dependencies: [')
  _replace(path,
    """  name_prefix: '',
  install: true,
  install_dir: get_option('libdir') / api_name,
)""",
    """  install: true,
)""")
  _replace(path, "if host_cpu_family == 'x86_64'", 'if false')
  _replace(path, "elif host_cpu_family == 'arm'", 'elif false')
  marker.touch()
hook.PatchSources(_patch_static_operations)


hook.ConfigureDependsOn(babl.hook, json_glib.hook, glib.hook)
hook.ConfigureWithMeson(build.PREFIX / 'include' / 'gegl-0.4' / 'gegl.h')
hook.ConfigureOptions(**{
  'docs': 'false',
  'gtk-doc': 'false',
  'gi-docgen': 'disabled',
  'workshop': 'false',
  'introspection': 'false',
  'vapigen': 'disabled',
  'gdk-pixbuf': 'disabled',
  'gexiv2': 'disabled',
  'graphviz': 'disabled',
  'jasper': 'disabled',
  'lcms': 'disabled',
  'lensfun': 'disabled',
  'libav': 'disabled',
  'libraw': 'disabled',
  'librsvg': 'disabled',
  'libspiro': 'disabled',
  'libtiff': 'disabled',
  'libv4l': 'disabled',
  'libv4l2': 'disabled',
  'lua': 'disabled',
  'maxflow': 'disabled',
  'mrg': 'disabled',
  'openexr': 'disabled',
  'openmp': 'disabled',
  'cairo': 'disabled',
  'pango': 'disabled',
  'pangocairo': 'disabled',
  'poppler': 'disabled',
  'pygobject': 'disabled',
  'sdl1': 'disabled',
  'sdl2': 'disabled',
  'umfpack': 'disabled',
  'webp': 'disabled',
  'wrap_mode': 'nofallback',
})
hook.InstallWhenIncluded(r'gegl\.h')
hook.AddCompileArgs(
  f'-I{build.PREFIX / "include" / "gegl-0.4"}',
  f'-I{build.PREFIX / "include" / "babl-0.1"}',
  f'-I{build.PREFIX / "include" / "glib-2.0"}',
  f'-I{build.PREFIX / "lib64" / "glib-2.0" / "include"}',
)
gegl_libs = glib.StaticLibs('gegl-0.4')


def _link_args():
  return ['-lgegl-common'] + gegl_libs()


hook.AddLinkArg(_link_args)
