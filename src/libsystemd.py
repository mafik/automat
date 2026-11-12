# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import sys
import build
import extension_helper
import fs_utils
import src

if sys.platform == 'linux':
    libucontext = src.load_extension('libucontext')
    hook = extension_helper.ExtensionHelper('libsystemd', globals())
    hook.FetchFromGit('https://github.com/systemd/systemd.git', 'v262.2')
    hook.DependsOn(libucontext.hook)
    # Can be removed once the libc shims of systemd make no dlsym lookups in static builds.
    hook.PatchSources('''--- src/libc/libc-shim.h
+++ src/libc/libc-shim.h
@@ -2,6 +2,7 @@
 #pragma once

 #include <dlfcn.h>
+#define dlsym(library, name) NULL
 #include <errno.h>
 #include <sys/syscall.h>
 #include <unistd.h>
''')
    hook.ConfigureOptions(**{
        'libc': 'musl',
        'static-libsystemd': 'true',
        'link-udev-shared': 'false',
        'link-executor-shared': 'false',
        'link-systemctl-shared': 'false',
        'link-networkd-shared': 'false',
        'link-timesyncd-shared': 'false',
        'link-journalctl-shared': 'false',
        'link-boot-shared': 'false',
        'link-portabled-shared': 'false',
        'mode': 'release',
        'utmp': 'false',
        'hibernate': 'false',
        'ldconfig': 'false',
        'resolve': 'false',
        'efi': 'false',
        'tpm': 'false',
        'environment-d': 'false',
        'binfmt': 'false',
        'repart': 'disabled',
        'sysupdate': 'disabled',
        'coredump': 'false',
        'pstore': 'false',
        'oomd': 'false',
        'logind': 'false',
        'hostnamed': 'false',
        'localed': 'false',
        'machined': 'false',
        'portabled': 'false',
        'sysext': 'false',
        'userdb': 'false',
        'homed': 'disabled',
        'networkd': 'false',
        'timedated': 'false',
        'timesyncd': 'false',
        'remote': 'disabled',
        'nss-myhostname': 'false',
        'nss-mymachines': 'disabled',
        'nss-resolve': 'disabled',
        'nss-systemd': 'false',
        'firstboot': 'false',
        'randomseed': 'false',
        'backlight': 'false',
        'vconsole': 'false',
        'quotacheck': 'false',
        'sysusers': 'false',
        'tmpfiles': 'false',
        'importd': 'disabled',
        'hwdb': 'false',
        'rfkill': 'false',
        'xdg-autostart': 'false',
        'man': 'disabled',
        'html': 'disabled',
        'translations': 'false',
        'tests': 'false',
        'seccomp': 'disabled',
        'selinux': 'disabled',
        'apparmor': 'disabled',
        'smack': 'false',
        'polkit': 'disabled',
        'ima': 'false',
        'acl': 'disabled',
        'audit': 'disabled',
        'blkid': 'disabled',
        'fdisk': 'disabled',
        'kmod': 'disabled',
        'pam': 'disabled',
        'passwdqc': 'disabled',
        'pwquality': 'disabled',
        'microhttpd': 'disabled',
        'libcryptsetup': 'disabled',
        'libcurl': 'disabled',
        'libidn2': 'disabled',
        'qrencode': 'disabled',
        'gcrypt': 'disabled',
        'gnutls': 'disabled',
        'openssl': 'disabled',
        'p11kit': 'disabled',
        'libfido2': 'disabled',
        'tpm2': 'disabled',
        'elfutils': 'disabled',
        'zlib': 'disabled',
        'bzip2': 'disabled',
        'xz': 'disabled',
        'lz4': 'disabled',
        'zstd': 'disabled',
        'xkbcommon': 'disabled',
        'pcre2': 'disabled',
        'glib': 'disabled',
        'dbus': 'disabled',
        'bootloader': 'disabled',
        'kernel-install': 'false',
        'ukify': 'disabled',
        'analyze': 'false',
    })
    hook.ConfigureWithMeson(build.PREFIX / 'lib64' / 'libsystemd.a',
                            install_tags='devel,libsystemd',
                            build_targets=('libsystemd', 'devel'))
    hook.AddLinkArgs('-l:libsystemd.a', '-lm')
    hook.InstallWhenIncluded(r'^systemd/.*')
