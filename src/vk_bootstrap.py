# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
import extension_helper

hook = extension_helper.ExtensionHelper('vk-bootstrap', globals())
hook.FetchFromGit('https://github.com/charles-lunarg/vk-bootstrap', 'v1.3.290')
hook.ConfigureWithCMake('{PREFIX}/lib64/libvk-bootstrap.a')
hook.AddLinkArg('-l:libvk-bootstrap.a')
hook.InstallWhenIncluded(r'^VkBootstrap\.h$')
