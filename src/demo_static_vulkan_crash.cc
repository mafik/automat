#pragma maf main

#include <assert.h>
#include <dlfcn.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

#include <cmath>
#include <cstdio>

int main() {
  void* library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  PFN_vkGetInstanceProcAddr ptr_vkGetInstanceProcAddr =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));

  PFN_vkCreateInstance fp_vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
      ptr_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));

  VkApplicationInfo applicationInfo = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "Hello world app",
      .applicationVersion = 0,
      .pEngineName = "awesomeengine",
      .engineVersion = 0,
      .apiVersion = VK_API_VERSION_1_0,
  };

  VkInstanceCreateInfo createInfo = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .flags = 0,
      .pApplicationInfo = &applicationInfo,
      .enabledLayerCount = 0,
      .ppEnabledLayerNames = nullptr,
      .enabledExtensionCount = 0,
      .ppEnabledExtensionNames = nullptr,
  };

  VkInstance instance;
  if (fp_vkCreateInstance(&createInfo, NULL, &instance) != VK_SUCCESS) {
    printf("Failed to create instance.\n");
    return EXIT_FAILURE;
  }
  PFN_vkDestroyInstance fp_vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
      ptr_vkGetInstanceProcAddr(instance, "vkDestroyInstance"));
  fp_vkDestroyInstance(instance, NULL);

  return EXIT_SUCCESS;
}
