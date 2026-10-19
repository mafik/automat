# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
import build
import build_variant
from extension_helper import ExtensionHelper
from sys import platform
import subprocess

skia = ExtensionHelper('Skia', globals())
skia.FetchFromGit('https://skia.googlesource.com/skia.git', 'chrome/m157')
skia.ConfigureWithGN(skia.build_dir / build.libname('skia'))
skia.ninja_target = 'all'
skia.ConfigureOptions(
  skia_use_vulkan='true',
  skia_use_vma='true',
  skia_use_system_expat='false',
  skia_use_system_icu='false',
  skia_use_system_libjpeg_turbo='false',
  skia_use_system_libpng='false',
  skia_use_system_libwebp='false',
  skia_use_system_zlib='false',
  skia_use_system_harfbuzz='false',
  skia_enable_ganesh='false',
  skia_enable_graphite='true',
  skia_use_partition_alloc='false') # TODO: try to enable (currently fails on windows)

if build.fast or build_variant.tsan:
  # SK_RELEASE:
  # - prevents debug assertions (e.g. SkCanvas rect checks) from aborting
  # - keeps Automat's Skia ABI consistent with this Skia
  # Keep frame pointers + symbols for readable stacks
  skia.ConfigureOptions(is_debug='false', is_official_build='true', extra_cflags='["-fno-omit-frame-pointer", "-g"]')
  build.compile_args += ['-DSK_RELEASE']
elif build.debug:
  skia.ConfigureOptions(is_debug='true', extra_cflags_cc='["-frtti"]')
  build.compile_args += ['-DSK_DEBUG']
elif build.release:
  skia.ConfigureOptions(is_debug='false', is_official_build='true')
  build.compile_args += ['-DSK_RELEASE']

if platform == 'win32':
  build.compile_args += ['-DNOMINMAX']
  # Prefer UTF-8 over UTF-16. This means no "UNICODE" define.
  # https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page
  # DO NOT ADD: build.compile_args += ('UNICODE')
  # <windows.h> has a side effect of defining ERROR macro.
  # Adding NOGDI prevents it from happening.
  build.compile_args += ['-DNOGDI']
  # Silence some MSCRT-specific deprecation warnings.
  build.compile_args += ['-D_CRT_SECURE_NO_WARNINGS']
  # No clue what it precisely does but many projects use it.
  build.compile_args += ['-DWIN32_LEAN_AND_MEAN']
  build.compile_args += ['-DVK_USE_PLATFORM_WIN32_KHR']
  # Set Windows version to Windows 10.
  build.compile_args += ['-D_WIN32_WINNT=0x0A00']
  build.compile_args += ['-DWINVER=0x0A00']
  skia.ConfigureOptions(clang_win='"C:\\Program Files\\LLVM"', clang_win_version='20')
  if build.debug:
    skia.ConfigureOptions(extra_cflags='["/MTd"]')
    # TODO: Enable SK_TRIVIAL_ABI by patching SkUserConfig.h
    # https://github.com/google/skia/blob/main/include/config/SkUserConfig.h
elif platform == 'linux':
  build.compile_args += ['-DVK_USE_PLATFORM_XCB_KHR']
  # Static linking to Vulkan on Linux seems to fail because the dynamically loaded libc / pthread is not properly initialized
  # by dlopen. It seems that glibc is closely coupled with ld.
  # Note that this problem might disappear when executed under debugger because they might be initialized when debugger ld runs.
  # Some discussion can be found here: https://www.openwall.com/lists/musl/2012/12/08/4
  # Most online discussions wrongly claim its nonsensical to call dlopen from a static binary because this loses the static linking benefits.
  # It is in fact useful, for example because it might allow Automat to bundle its dependencies but still use the system's vulkan drivers.
  # Anyway there is no clean, officially supported solution to dlopen from a static binary on Linux :(
  # A potential, slightly hacky workaround might be found here: https://github.com/pfalcon/foreign-dlopen/tree/master
  # For the time being we disable static linking on Linux and instead statically link everything except libm & libc (they seem to be coupled).
  build.compile_args = [x for x in build.compile_args if x != '-static']
  build.link_args = [x for x in build.link_args if x != '-static']
  build.link_args += ['-static-libstdc++', '-static-libgcc', '-ldl']
  skia.ConfigureOptions(skia_use_system_freetype2='false')

build.compile_args += ['-I', skia.src_dir]
build.compile_args += ['-DSK_GRAPHITE']
build.compile_args += ['-DSK_VULKAN']
build.compile_args += ['-DSK_USE_VMA']
build.compile_args += ['-DSK_SHAPER_HARFBUZZ_AVAILABLE']

skia.InstallWhenIncluded(r'(include|src)/.*Sk.*\.h')
skia.AddLinkArgs('-L', skia.build_dir)

def skia_git_sync_with_deps(marker):
  subprocess.run(['python', 'tools/git-sync-deps'], cwd=skia.src_dir, check=True)
  marker.touch()
skia.PatchSources(skia_git_sync_with_deps)

# Remove when Skia Graphite binds image and buffer memory through VMA instead of raw
# vkBind*Memory (not fixed as of chrome/m157).
skia.PatchSources('''--- include/gpu/vk/VulkanMemoryAllocator.h
+++ include/gpu/vk/VulkanMemoryAllocator.h
@@ -64,6 +64,10 @@
                                           uint32_t allocationPropertyFlags,
                                           skgpu::VulkanBackendMemory* memory) = 0;
 
+    virtual VkResult bindImageMemory(VkImage image, const skgpu::VulkanBackendMemory&) = 0;
+
+    virtual VkResult bindBufferMemory(VkBuffer buffer, const skgpu::VulkanBackendMemory&) = 0;
+
     // Fills out the passed in skgpu::VulkanAlloc struct for the passed in
     // skgpu::VulkanBackendMemory.
     virtual void getAllocInfo(const skgpu::VulkanBackendMemory&, skgpu::VulkanAlloc*) const = 0;
--- src/gpu/graphite/vk/VulkanBuffer.cpp
+++ src/gpu/graphite/vk/VulkanBuffer.cpp
@@ -161,11 +161,8 @@
     }
 
     // Bind buffer
-    VULKAN_CALL_RESULT(
-            sharedContext,
-            result,
-            BindBufferMemory(sharedContext->device(), buffer, alloc.fMemory, alloc.fOffset));
-    if (result != VK_SUCCESS) {
+    result = allocator->bindBufferMemory(buffer, alloc.fBackendMemory);
+    if (!sharedContext->checkVkResult(result)) {
         skgpu::VulkanMemory::FreeBufferMemory(allocator, alloc);
         VULKAN_CALL(sharedContext->interface(),
                     DestroyBuffer(sharedContext->device(),
--- src/gpu/graphite/vk/VulkanTexture.cpp
+++ src/gpu/graphite/vk/VulkanTexture.cpp
@@ -142,12 +142,8 @@
         }
     }
 
-    VULKAN_CALL_RESULT(
-            sharedContext,
-            result,
-            BindImageMemory(
-                    device, image, outInfo->fMemoryAlloc.fMemory, outInfo->fMemoryAlloc.fOffset));
-    if (result != VK_SUCCESS) {
+    result = allocator->bindImageMemory(image, outInfo->fMemoryAlloc.fBackendMemory);
+    if (!sharedContext->checkVkResult(result)) {
         skgpu::VulkanMemory::FreeImageMemory(allocator, outInfo->fMemoryAlloc);
         VULKAN_CALL(sharedContext->interface(), DestroyImage(device, image, nullptr));
         return false;
--- src/gpu/vk/vulkanmemoryallocator/VulkanAMDMemoryAllocator.cpp
+++ src/gpu/vk/vulkanmemoryallocator/VulkanAMDMemoryAllocator.cpp
@@ -211,6 +211,20 @@
     return result;
 }
 
+VkResult VulkanAMDMemoryAllocator::bindImageMemory(VkImage image,
+                                                   const VulkanBackendMemory& memoryHandle) {
+    TRACE_EVENT0("skia.gpu", TRACE_FUNC);
+    const VmaAllocation allocation = (VmaAllocation)memoryHandle;
+    return vmaBindImageMemory(fAllocator, allocation, image);
+}
+
+VkResult VulkanAMDMemoryAllocator::bindBufferMemory(VkBuffer buffer,
+                                                    const VulkanBackendMemory& memoryHandle) {
+    TRACE_EVENT0("skia.gpu", TRACE_FUNC);
+    const VmaAllocation allocation = (VmaAllocation)memoryHandle;
+    return vmaBindBufferMemory(fAllocator, allocation, buffer);
+}
+
 void VulkanAMDMemoryAllocator::freeMemory(const VulkanBackendMemory& memoryHandle) {
     TRACE_EVENT0("skia.gpu", TRACE_FUNC);
     const VmaAllocation allocation = (VmaAllocation)memoryHandle;
--- src/gpu/vk/vulkanmemoryallocator/VulkanAMDMemoryAllocator.h
+++ src/gpu/vk/vulkanmemoryallocator/VulkanAMDMemoryAllocator.h
@@ -44,6 +44,10 @@
                                   uint32_t allocationPropertyFlags,
                                   skgpu::VulkanBackendMemory*) override;
 
+    VkResult bindImageMemory(VkImage image, const VulkanBackendMemory&) override;
+
+    VkResult bindBufferMemory(VkBuffer buffer, const VulkanBackendMemory&) override;
+
     void freeMemory(const VulkanBackendMemory&) override;
 
     void getAllocInfo(const VulkanBackendMemory&, VulkanAlloc*) const override;
--- tools/gpu/vk/VkTestMemoryAllocator.cpp
+++ tools/gpu/vk/VkTestMemoryAllocator.cpp
@@ -196,6 +196,20 @@
     return result;
 }
 
+VkResult VkTestMemoryAllocator::bindImageMemory(VkImage image,
+                                                const skgpu::VulkanBackendMemory& memoryHandle) {
+    TRACE_EVENT0("skia.gpu", TRACE_FUNC);
+    const VmaAllocation allocation = (VmaAllocation)memoryHandle;
+    return vmaBindImageMemory(fAllocator, allocation, image);
+}
+
+VkResult VkTestMemoryAllocator::bindBufferMemory(VkBuffer buffer,
+                                                 const skgpu::VulkanBackendMemory& memoryHandle) {
+    TRACE_EVENT0("skia.gpu", TRACE_FUNC);
+    const VmaAllocation allocation = (VmaAllocation)memoryHandle;
+    return vmaBindBufferMemory(fAllocator, allocation, buffer);
+}
+
 void VkTestMemoryAllocator::freeMemory(const skgpu::VulkanBackendMemory& memoryHandle) {
     TRACE_EVENT0("skia.gpu", TRACE_FUNC);
     const VmaAllocation allocation = (VmaAllocation)memoryHandle;
--- tools/gpu/vk/VkTestMemoryAllocator.h
+++ tools/gpu/vk/VkTestMemoryAllocator.h
@@ -50,6 +50,10 @@
                                   uint32_t allocationPropertyFlags,
                                   skgpu::VulkanBackendMemory*) override;
 
+    VkResult bindImageMemory(VkImage image, const skgpu::VulkanBackendMemory&) override;
+
+    VkResult bindBufferMemory(VkBuffer buffer, const skgpu::VulkanBackendMemory&) override;
+
     void freeMemory(const skgpu::VulkanBackendMemory&) override;
 
     void getAllocInfo(const skgpu::VulkanBackendMemory&, skgpu::VulkanAlloc*) const override;
''')

# Remove when Skia's vkCreateRenderPass2 path sets the sam rasterization order subpass flag as
# its pipelines use (not fixed as of chrome/m157).
skia.PatchSources('''--- src/gpu/graphite/vk/VulkanRenderPass.cpp
+++ src/gpu/graphite/vk/VulkanRenderPass.cpp
@@ -422,6 +422,10 @@
         mainSubpassDesc.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
         populate_main_subpass_desc(
                 context->vulkanCaps(), mainSubpassDesc, colorRef, resolveRef, depthStencilRef);
+        if (context->vulkanCaps().supportsRasterizationOrderColorAttachmentAccess()) {
+            mainSubpassDesc.flags =
+                    VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT;
+        }
 
         // Enable multisampled render to single-sampled with the render pass's sample count.
         VkMultisampledRenderToSingleSampledInfoEXT msrtss = {};
''')
