import autotools, re, build, src

def hook_recipe(recipe):
  autotools.register_package(recipe, 'https://www.x.org/archive/individual/util/util-macros-1.20.1.tar.xz', [], ['{PREFIX}/share/pkgconfig/xorg-macros.pc'])
  autotools.register_package(recipe, 'https://xorg.freedesktop.org/archive/individual/proto/xorgproto-2024.1.tar.xz', ['{PREFIX}/share/pkgconfig/xorg-macros.pc'], ['{PREFIX}/include/X11'])
  autotools.register_package(recipe, 'https://www.x.org/pub/individual/lib/libXau-1.0.11.tar.xz', ['{PREFIX}/include/X11'], ['{PREFIX}/lib/libXau.a'])
  autotools.register_package(recipe, 'https://xcb.freedesktop.org/dist/xcb-proto-1.17.0.tar.xz', [], ['{PREFIX}/share/pkgconfig/xcb-proto.pc'])
  autotools.register_package(recipe, 'https://xcb.freedesktop.org/dist/libxcb-1.17.0.tar.xz', ['{PREFIX}/share/pkgconfig/xcb-proto.pc', '{PREFIX}/lib/libXau.a'], ['{PREFIX}/lib/libxcb.a', '{PREFIX}/include/xcb'])

xcb_libs = set(['xcb', 'xcb-xinput', 'xcb-xtest'])

# Binaries that should link to XCB
xcb_bins = set()

def hook_srcs(srcs : dict[str, src.File], recipe):
  for src in srcs.values():
    if xcb_libs.intersection(src.comment_libs):
      src.link_args[''] += ['-lXau']


def hook_plan(srcs, objs : list[build.ObjectFile], bins, recipe):
  for obj in objs:
    if any(re.match(r'xcb/.*', inc) for inc in obj.source.system_includes):
      obj.deps.add(str(obj.build_type.PREFIX() / 'include' / 'xcb'))

  for bin in bins:
    for obj in bin.objects:
      if xcb_libs.intersection(obj.source.comment_libs):
        xcb_bins.add(bin)


def hook_final(srcs, objs, bins, recipe):
  for step in recipe.steps:
    needs_xcb = False
    build_type = None
    for bin in xcb_bins:
      if str(bin.path) in step.outputs:
        needs_xcb = True
        build_type = bin.build_type
        break
    if needs_xcb:
      step.inputs.add(str(build_type.PREFIX() / 'lib' / 'libxcb.a'))
