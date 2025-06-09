# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
import build
import extension_helper

util_macros = extension_helper.ExtensionHelper('util-macros', globals())
util_macros.FetchFromURL('https://www.x.org/archive/individual/util/util-macros-1.20.1.tar.xz')
util_macros.ConfigureWithAutotools(build.PREFIX / 'share' / 'pkgconfig' / 'xorg-macros.pc')

xorgproto = extension_helper.ExtensionHelper('xorgproto', globals())
xorgproto.FetchFromURL('https://xorg.freedesktop.org/archive/individual/proto/xorgproto-2024.1.tar.xz')
xorgproto.ConfigureDependsOn(util_macros)
xorgproto.ConfigureWithAutotools(build.PREFIX / 'share' / 'pkgconfig' / 'xproto.pc')

libXau = extension_helper.ExtensionHelper('libXau', globals())
libXau.FetchFromURL('https://www.x.org/pub/individual/lib/libXau-1.0.11.tar.xz')
libXau.ConfigureDependsOn(xorgproto)
libXau.ConfigureWithAutotools(build.PREFIX / 'lib64' / 'libXau.a')

libXdmcp = extension_helper.ExtensionHelper('libXdmcp', globals())
libXdmcp.FetchFromURL('https://www.x.org/archive/individual/lib/libXdmcp-1.1.5.tar.xz')
libXdmcp.ConfigureDependsOn(xorgproto)
libXdmcp.ConfigureWithAutotools(build.PREFIX / 'lib64' / 'libXdmcp.a')

xcb_proto = extension_helper.ExtensionHelper('xcb-proto', globals())
xcb_proto.FetchFromURL('https://xcb.freedesktop.org/dist/xcb-proto-1.17.0.tar.xz')
xcb_proto.ConfigureDependsOn(xorgproto)
xcb_proto.ConfigureWithAutotools(build.PREFIX / 'share' / 'pkgconfig' / 'xcb-proto.pc')

libX11 = extension_helper.ExtensionHelper('libX11', globals())
libX11.FetchFromURL('https://www.x.org/archive/individual/lib/libX11-1.8.tar.xz')
libX11.ConfigureDependsOn(xorgproto)
libX11.ConfigureWithAutotools(build.PREFIX / 'lib64' / 'libX11.a')

libXext = extension_helper.ExtensionHelper('libXext', globals())
libXext.FetchFromURL('https://www.x.org/archive/individual/lib/libXext-1.3.6.tar.xz')
libXext.ConfigureDependsOn(xorgproto)
libXext.ConfigureWithAutotools(build.PREFIX / 'lib64' / 'libXext.a')

libxcb = extension_helper.ExtensionHelper('libxcb', globals())
libxcb.FetchFromURL('https://xcb.freedesktop.org/dist/libxcb-1.17.0.tar.xz')
libxcb.ConfigureDependsOn(xcb_proto, libXau, libXdmcp, libXext, libX11)
libxcb.ConfigureWithAutotools(build.PREFIX / 'lib64' / 'libxcb.a', build.PREFIX / 'include' / 'xcb')
# '-l:libxcb-glx.a', '-l:libxcb-randr.a', '-l:libxcb-dri3.a', '-l:libX11.a', '-l:libX11-xcb.a', '-l:libXext.a', '-l:libXau.a', '-l:libXdmcp.a'
libxcb.AddLinkArgs('-l:libxcb.so',
                   '-Wl,--export-dynamic',
                   '-Wl,--whole-archive',
                   '-l:libxcb-xtest.a',
                   '-l:libxcb-xinput.a',
                   '-l:libxcb-shm.a',
                   '-l:libxcb-render.a',
                   '-Wl,--no-whole-archive',
                   '-Wl,--no-export-dynamic')
libxcb.InstallWhenIncluded(r'^xcb/.+\.h')

xcb_util_renderutil = extension_helper.ExtensionHelper('xcb-util-renderutil', globals())
xcb_util_renderutil.FetchFromURL('https://www.x.org/archive/individual/xcb/xcb-util-renderutil-0.3.10.tar.xz')
xcb_util_renderutil.ConfigureDependsOn(libxcb)
xcb_util_renderutil.ConfigureWithAutotools(build.PREFIX / 'lib64' / 'libxcb-render-util.a')

xcb_util = extension_helper.ExtensionHelper('xcb-util', globals())
xcb_util.FetchFromURL('https://www.x.org/archive/individual/xcb/xcb-util-0.4.1.tar.xz')
xcb_util.ConfigureDependsOn(xcb_proto, libxcb)
xcb_util.ConfigureWithAutotools(build.PREFIX / 'lib64' / 'libxcb-util.a')

xcb_util_image = extension_helper.ExtensionHelper('xcb-util-image', globals())
xcb_util_image.FetchFromURL('https://www.x.org/archive/individual/xcb/xcb-util-image-0.4.1.tar.xz')
xcb_util_image.ConfigureDependsOn(xcb_util)
xcb_util_image.PatchSources('''\
--- "image/xcb_bitops.h"
+++ "image/xcb_bitops.h"
@@ -207,6 +207,7 @@
       return XCB_IMAGE_ORDER_LSB_FIRST;
   }
   assert(0);
+  return XCB_IMAGE_ORDER_LSB_FIRST;
 }
 
 #endif /* __XCB_BITOPS_H__ */''')
xcb_util_image.ConfigureWithAutotools(build.PREFIX / 'lib64' / 'libxcb-image.a')

xcb_util_cursor = extension_helper.ExtensionHelper('xcb-util-cursor', globals())
xcb_util_cursor.FetchFromURL('https://xcb.freedesktop.org/dist/xcb-util-cursor-0.1.5.tar.xz')
xcb_util_cursor.ConfigureDependsOn(xcb_proto, xcb_util_image, xcb_util_renderutil)
xcb_util_cursor.ConfigureWithAutotools(build.PREFIX / 'lib64' / 'libxcb-cursor.a')
xcb_util_cursor.AddLinkArgs('-Wl,--export-dynamic',
                            '-Wl,--whole-archive',
                            '-l:libxcb-cursor.a',
                            '-l:libxcb-util.a',
                            '-l:libxcb-image.a',
                            '-l:libxcb-render-util.a',
                            '-Wl,--no-whole-archive',
                            '-Wl,--no-export-dynamic')
xcb_util_cursor.InstallWhenIncluded(r'^xcb/xcb_cursor\.h$')

xcb_util_errors = extension_helper.ExtensionHelper('xcb-util-errors', globals())
xcb_util_errors.FetchFromURL('https://www.x.org/archive/individual/xcb/xcb-util-errors-1.0.1.tar.gz')
xcb_util_errors.ConfigureDependsOn(xcb_proto, libxcb)
xcb_util_errors.ConfigureWithAutotools(build.PREFIX / 'lib64' / 'libxcb-errors.a')
xcb_util_errors.InstallWhenIncluded(r'^xcb/xcb_errors\.h$')
xcb_util_errors.AddLinkArgs('-Wl,--export-dynamic',
                            '-Wl,--whole-archive',
                            '-l:libxcb-errors.a',
                            '-Wl,--no-whole-archive',
                            '-Wl,--no-export-dynamic')
