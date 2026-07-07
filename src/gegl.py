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


# proxy-libintl declares no wide-character gettext variants, and the pruned
# tree ships no translation catalogs in the first place.
def _patch_narrow_bindtextdomain(marker):
  _replace(hook.src_dir / 'gegl' / 'gegl-init.c',
    'wbindtextdomain (GETTEXT_PACKAGE, dir_name_utf16);',
    'bindtextdomain (GETTEXT_PACKAGE, localedir);')
  marker.touch()
hook.PatchSources(_patch_narrow_bindtextdomain)


# The sources assume a mingw runtime on Windows: <unistd.h> is included
# unconditionally and a few POSIX calls appear in the buffer code. This shim
# supplies the MSVC equivalents; only the win32 c_args below put it on the
# include path, so other platforms never see it.
def _patch_msvc_compat(marker):
  compat = hook.src_dir / 'msvc-compat'
  compat.mkdir(exist_ok=True)
  (compat / 'unistd.h').write_text('''#pragma once
#include <io.h>
#include <process.h>
#include <direct.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* rpcndr.h claims this as a macro; ctx.h uses it as an identifier. near & far
   must stay defined: winsock headers still spell out FAR. */
#undef small

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif

#ifndef _PID_T_DEFINED
#define _PID_T_DEFINED
typedef int pid_t;
#endif

#define fsync _commit
#define ftruncate _chsize_s

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

static inline int usleep(unsigned int usec)
{
  Sleep((usec + 999) / 1000);
  return 0;
}
''')
  (compat / 'strings.h').write_text('''#pragma once
#include <string.h>

#define strcasecmp _stricmp
#define strncasecmp _strnicmp
''')
  # UCRT ships exp2f; the op's _MSC_VER fallback macro is malformed anyway.
  _replace(hook.src_dir / 'operations' / 'common' / 'exposure.c',
    """#ifdef _MSC_VER
#define exp2f (b) ((gfloat) pow (2.0, b))
#endif

""", '')
  # clang's MSVC mode gives C `inline` MSVC semantics and emits no external
  # definition, which the cross-TU callers need.
  _replace(hook.src_dir / 'gegl' / 'gegl-parallel.c',
    """inline gint
gegl_parallel_distribute_get_optimal_n_threads (gdouble n_elements,""",
    """gint
gegl_parallel_distribute_get_optimal_n_threads (gdouble n_elements,""")
  (compat / 'sys').mkdir(exist_ok=True)
  (compat / 'sys' / 'time.h').write_text('''#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>  /* struct timeval */
#include <stdint.h>

/* rpcndr.h claims this as a macro; ctx.h uses it as an identifier. */
#undef small

static inline int gettimeofday(struct timeval *tv, void *tz)
{
  uint64_t t;
  FILETIME ft;
  (void)tz;
  GetSystemTimePreciseAsFileTime(&ft);
  t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  t -= 116444736000000000ULL;  /* 1601 -> 1970 */
  tv->tv_sec = (long)(t / 10000000ULL);
  tv->tv_usec = (long)((t % 10000000ULL) / 10);
  return 0;
}
''')
  marker.touch()
hook.PatchSources(_patch_msvc_compat)


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


# One static operation bundle. The core, transform and generated (the SVG
# compositing family) operations join the common source list: operations
# reference each other by name across directories (meta-operations build
# gegl:scale-ratio and svg blend children), so an incomplete bundle leaves
# passthrough nops behind and their property bindings fail. Excluded stay:
# json.c (its hand-written registration dereferences the GTypeModule and only
# scans now-empty module paths), common-cxx (its C++ operations each carry
# their own module entry points, which collide when folded into one library),
# common-gpl3+ (GPL3 operations in an MIT binary), external (system library
# dependencies), seamless-clone and workshop. The x86-64-v2/v3 duplicates of
# the bundle stay loadable-module territory and are dropped.
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
  folded = [
    '../core/cache.c', '../core/cast-format.c', '../core/cast-space.c',
    '../core/clone.c', '../core/convert-format.c', '../core/convert-space.c',
    '../core/crop.c', '../core/load.c', '../core/nop.c',
  ]
  for sibling in ('transform', 'generated'):
    sibling_dir = hook.src_dir / 'operations' / sibling
    names = sorted(f.name for f in sibling_dir.iterdir() if f.suffix == '.c')
    folded += [f'../{sibling}/{name}' for name in names]
  folded_lines = ''.join(f"  '{f}',\n" for f in folded)

  # The transform family registers through its own module singleton:
  # module.c's entry points are renamed (only dlopen would call them by the
  # generic names), the generated registrar calls the renamed register first
  # (it stores the GTypeModule the per-operation registrations need), and the
  # loader scan skips transform files so they are not registered twice.
  module_c = hook.src_dir / 'operations' / 'transform' / 'module.c'
  _replace(module_c, 'gegl_module_query', 'transform_module_query')
  _replace(module_c, 'gegl_module_register', 'transform_module_register')
  gen_loader = hook.src_dir / 'tools' / 'gen-loader.py'
  _replace(gen_loader,
    "for file_path in sys.argv[1:]:\n",
    "for file_path in sys.argv[1:]:\n"
    "  if '/transform/' in file_path:\n"
    "    continue\n")
  _replace(gen_loader,
    'print(f"void gegl_op_{a}_register_type(GTypeModule *module);")',
    'print(f"void gegl_op_{a}_register_type(GTypeModule *module);")\n'
    'print("gboolean transform_module_register (GTypeModule *module);")')
  _replace(gen_loader,
    'for a in operation_names:\n  print(f"  gegl_op_{a}_register_type(module);")',
    'print("  transform_module_register (module);")\n'
    'for a in operation_names:\n  print(f"  gegl_op_{a}_register_type(module);")')
  _replace(path,
    """  'weighted-blend.c',
  'write-buffer.c',
)""",
    f"""  'weighted-blend.c',
  'write-buffer.c',
{folded_lines})""")
  _replace(path,
    "gegl_common = shared_library('gegl-common',",
    "gegl_common = static_library('gegl-common',")
  # Each operation includes its own .c file by bare name (gegl-op.h's
  # GEGL_OP_C_FILE); the folded core sources need their directory searched.
  _replace(path,
    "  include_directories: [ rootInclude, geglInclude, ],\n  dependencies: [",
    "  include_directories: [ rootInclude, geglInclude, include_directories('../core'),"
    " include_directories('../transform'), include_directories('../generated'), ],\n"
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
glib.UsePkgConfig(hook)
hook.ConfigureWithMeson(build.PREFIX / 'include' / 'gegl-0.4' / 'gegl.h')
if build.platform == 'win32':
  # M_PI & friends hide behind _USE_MATH_DEFINES; the S_I?USR permission bits
  # take their MSVC spellings, mirroring mingw's sys/stat.h.
  hook.ConfigureOption('c_args', ' '.join([
    '-D_USE_MATH_DEFINES',
    '-DS_IRUSR=_S_IREAD',
    '-DS_IWUSR=_S_IWRITE',
    '-DS_IXUSR=_S_IEXEC',
    f'-I{hook.src_dir.as_posix()}/msvc-compat',
  ]))
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
  common = (str(build.PREFIX / 'lib64' / 'libgegl-common.a')
            if build.platform == 'win32' else '-lgegl-common')
  return [common] + gegl_libs()


hook.AddLinkArg(_link_args)
