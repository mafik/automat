# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import sys
import build
import extension_helper
import fs_utils

if sys.platform == 'linux':
    hook = extension_helper.ExtensionHelper('libsystemd', globals())
    hook.FetchFromGit('https://github.com/systemd/systemd.git', 'v258.2')
    hook.ConfigureOptions(**{
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
        'libidn': 'disabled',
        'libiptc': 'disabled',
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
    # We only consume libsystemd.a. Don't let `ninja all` build every other systemd
    # target — on distros that ship libcrypt.a / libselinux.a without -fPIC (current
    # Debian/Ubuntu), linking those static archives into libsystemd-shared-258.so
    # aborts with R_X86_64_PC32 / R_X86_64_TPOFF32 "can not be used when making a
    # shared object" errors. The two meson aliases below are the minimum top-level
    # targets that cover every file tagged `devel` or `libsystemd`:
    #   * `libsystemd` — alias_target wrapping libsystemd.so + libsystemd.a
    #   * `devel`     — alias_target wrapping the .pc files for libsystemd, libudev, etc.
    hook.ConfigureWithMeson(build.PREFIX / 'lib64' / 'libsystemd.a',
                            install_tags='devel,libsystemd',
                            build_targets=['libsystemd', 'devel'])
    hook.AddLinkArgs('-l:libsystemd.a', '-lcap', '-lm')
    hook.InstallWhenIncluded(r'^systemd/.*')
