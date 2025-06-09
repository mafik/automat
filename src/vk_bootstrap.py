# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
import extension_helper
import build
libname = build.libname('vk-bootstrap')

vulkan_headers = extension_helper.ExtensionHelper('vulkan-headers', globals())
vulkan_headers.FetchFromGit('https://github.com/KhronosGroup/Vulkan-Headers', 'vulkan-sdk-1.4.313.0')
vulkan_headers.ConfigureWithCMake(build.PREFIX / 'include' / 'vulkan' / 'vulkan.h')

vk_bootstrap = extension_helper.ExtensionHelper('vk-bootstrap', globals())
vk_bootstrap.FetchFromGit('https://github.com/charles-lunarg/vk-bootstrap', 'v1.3.290')
vk_bootstrap.ConfigureWithCMake(build.PREFIX / 'lib64' / libname)
vk_bootstrap.ConfigureOptions(VK_BOOTSTRAP_INSTALL='ON', VK_BOOTSTRAP_TEST='OFF')
vk_bootstrap.AddLinkArg('-lvk-bootstrap')
vk_bootstrap.ConfigureDependsOn(vulkan_headers)
vk_bootstrap.InstallWhenIncluded(r'^VkBootstrap\.h$')
