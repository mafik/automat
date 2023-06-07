#include "vk.hh"

#if defined(_WIN32)
#include "win.hh"
#include "win_main.hh"
#elif defined(__linux__)
#include "linux_main.hh"
#endif

#include <VkBootstrap.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkColorType.h>
#include <include/core/SkSurface.h>
#include <include/gpu/GrBackendSemaphore.h>
#include <include/gpu/GrDirectContext.h>
#include <include/gpu/vk/GrVkBackendContext.h>
#include <include/gpu/vk/VulkanExtensions.h>
#include <src/gpu/ganesh/vk/GrVkUtil.h>
#include <vulkan/vulkan.h>

namespace automat::vk {

// Initialized in Instance::Init
PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
PFN_vkGetDeviceProcAddr GetDeviceProcAddr;

// Initialized in InitFunctions
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;

PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
PFN_vkQueuePresentKHR QueuePresentKHR;
PFN_vkDeviceWaitIdle DeviceWaitIdle;
PFN_vkQueueWaitIdle QueueWaitIdle;
PFN_vkDestroyDevice DestroyDevice;
PFN_vkCreateSemaphore CreateSemaphore;
PFN_vkDestroySemaphore DestroySemaphore;

struct Instance : vkb::Instance {
  void Init();
  void Destroy();
  PFN_vkVoidFunction GetProc(const char* proc_name) {
    return GetInstanceProcAddr(instance, proc_name);
  }

  uint32_t instance_version = VK_MAKE_VERSION(1, 1, 0);
  uint32_t api_version = VK_API_VERSION_1_1;
  std::string error = "";
  std::vector<const char*> extensions = {
#if defined(_WIN32)
    "VK_KHR_win32_surface"
#elif defined(__linux__)
    "VK_KHR_xcb_surface"
#endif
  };
} instance;

struct Surface {
  void Init();
  void Destroy();
  operator VkSurfaceKHR() { return surface; }

  VkSurfaceKHR surface;
  std::string error = "";
} surface;

struct PhysicalDevice : vkb::PhysicalDevice {
  void Init();
  void Destroy();

  std::vector<std::string> extensions_str;
  std::vector<const char*> extensions;
  std::string error = "";
} physical_device;

struct Device : vkb::Device {
  void Init();
  void Destroy();
  PFN_vkVoidFunction GetProc(const char* proc_name) { return GetDeviceProcAddr(device, proc_name); }

  uint32_t graphics_queue_index;
  VkQueue graphics_queue;
  uint32_t present_queue_index;
  VkQueue present_queue;
  skgpu::VulkanExtensions extensions;
  VkPhysicalDeviceFeatures2 features = {};
  std::string error = "";
} device;

sk_sp<GrDirectContext> gr_context;

struct Swapchain {
  struct BackbufferInfo {
    uint32_t image_index;          // image this is associated with
    VkSemaphore render_semaphore;  // we wait on this for rendering to be done
  };

  void DestroyBuffers();
  bool CreateBuffers(int width, int height, int sample_count, VkFormat format,
                     VkImageUsageFlags usageFlags, SkColorType colorType,
                     VkSharingMode sharingMode);
  bool Create(int widthHint, int heightHint);
  BackbufferInfo* GetAvailableBackbuffer();
  SkCanvas* GetBackbufferCanvas();
  void SwapBuffers();
  void WaitAndDestroy();
  operator VkSwapchainKHR() { return swapchain; }

  VkSwapchainKHR swapchain;
  uint32_t image_count;
  VkImage* images;  // images in the swapchain
  sk_sp<SkSurface>* surfaces;

  // Note that there are (image_count + 1) backbuffers. We create one additional
  // backbuffer structure, because we want to give the command buffers they
  // contain a chance to finish before we cycle back.
  BackbufferInfo* backbuffers;
  uint32_t current_backbuffer_index;
} swapchain;

void Instance::Init() {
  if (instance != VK_NULL_HANDLE) {
    Destroy();
  }
  vkb::InstanceBuilder builder;
  builder.set_minimum_instance_version(instance_version);
  builder.require_api_version(api_version);
  for (auto& ext : extensions) {
    builder.enable_extension(ext);
  }
  auto result = builder.build();
  if (!result) {
    error = result.error().message();
  } else {
    (*(vkb::Instance*)this) = result.value();
  }
  GetInstanceProcAddr = fp_vkGetInstanceProcAddr;
  GetDeviceProcAddr = fp_vkGetDeviceProcAddr;
}
void Instance::Destroy() {
  vkb::destroy_instance(*this);
  debug_messenger = VK_NULL_HANDLE;
  instance = VK_NULL_HANDLE;
  error = "";
}
PFN_vkVoidFunction GetProc(const char* proc_name, VkInstance vk_instance, VkDevice device) {
  if (device != VK_NULL_HANDLE) {
    return GetDeviceProcAddr(device, proc_name);
  }
  return GetInstanceProcAddr(vk_instance, proc_name);
};
void Surface::Init() {
  if (surface != VK_NULL_HANDLE) {
    error = "vk::Surface already initialized.";
    return;
  }

#if defined(_WIN32)

  PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR =
      (PFN_vkCreateWin32SurfaceKHR)instance.GetProc("vkCreateWin32SurfaceKHR");

  VkWin32SurfaceCreateInfoKHR surfaceCreateInfo = {
      .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
      .pNext = nullptr,
      .flags = 0,
      .hinstance = GetInstance(),
      .hwnd = main_window};

  VkResult res = vkCreateWin32SurfaceKHR(instance, &surfaceCreateInfo, nullptr, &surface);
  if (VK_SUCCESS != res) {
    error = "Failure in vkCreateWin32SurfaceKHR.";
    return;
  }

  if (VK_NULL_HANDLE == surface) {
    error = "No surface after vkCreateWin32SurfaceKHR";
    return;
  }

#elif defined(__linux__)

  PFN_vkCreateXcbSurfaceKHR vkCreateXcbSurfaceKHR =
      (PFN_vkCreateXcbSurfaceKHR)instance.GetProc("vkCreateXcbSurfaceKHR");

  VkXcbSurfaceCreateInfoKHR surfaceCreateInfo = {
      .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
      .pNext = nullptr,
      .flags = 0,
      .connection = connection,
      .window = xcb_window};

  VkResult res = vkCreateXcbSurfaceKHR(instance, &surfaceCreateInfo, nullptr, &surface);
  if (VK_SUCCESS != res) {
    error = "Failure in vkCreateXcbSurfaceKHR.";
    return;
  }

  if (VK_NULL_HANDLE == surface) {
    error = "No surface after vkCreateXcbSurfaceKHR";
    return;
  }

#endif
}
void Surface::Destroy() {
  vkb::destroy_surface(instance, surface);
  surface = VK_NULL_HANDLE;
  error = "";
}
void PhysicalDevice::Init() {
  if (physical_device != VK_NULL_HANDLE) {
    error = "vk::PhysicalDevice already initialized.";
    return;
  }
  auto result = vkb::PhysicalDeviceSelector(instance)
                    .set_surface(vk::surface)
                    .add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
                    .add_required_extension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)
                    .prefer_gpu_device_type()
                    .select();
  if (!result) {
    error = result.error().message();
    return;
  }
  *(vkb::PhysicalDevice*)this = result.value();
  extensions_str = get_extensions();
  for (auto& ext : extensions_str) {
    extensions.push_back(ext.c_str());
  }
}
void PhysicalDevice::Destroy() {
  physical_device = VK_NULL_HANDLE;
  extensions_str.clear();
  extensions.clear();
  error = "";
}
void Device::Init() {
  extensions.init(vk::GetProc, instance, vk::physical_device, instance.extensions.size(),
                  instance.extensions.data(), vk::physical_device.extensions.size(),
                  vk::physical_device.extensions.data());

  int api_version = vk::physical_device.properties.apiVersion;
  SkASSERT(api_version >= VK_MAKE_VERSION(1, 1, 0) ||
           extensions.hasExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, 1));

  features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features.pNext = nullptr;

  // Setup all extension feature structs we may want to use.
  void** tailPNext = &features.pNext;

  VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT* blend = nullptr;
  if (extensions.hasExtension(VK_EXT_BLEND_OPERATION_ADVANCED_EXTENSION_NAME, 2)) {
    blend = (VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT*)sk_malloc_throw(
        sizeof(VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT));
    blend->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT;
    blend->pNext = nullptr;
    *tailPNext = blend;
    tailPNext = &blend->pNext;
  }

  VkPhysicalDeviceSamplerYcbcrConversionFeatures* ycbcrFeature = nullptr;
  if (api_version >= VK_MAKE_VERSION(1, 1, 0) ||
      extensions.hasExtension(VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME, 1)) {
    ycbcrFeature = (VkPhysicalDeviceSamplerYcbcrConversionFeatures*)sk_malloc_throw(
        sizeof(VkPhysicalDeviceSamplerYcbcrConversionFeatures));
    ycbcrFeature->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    ycbcrFeature->pNext = nullptr;
    ycbcrFeature->samplerYcbcrConversion = VK_TRUE;
    *tailPNext = ycbcrFeature;
    tailPNext = &ycbcrFeature->pNext;
  }

  PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2 =
      (PFN_vkGetPhysicalDeviceFeatures2)instance.GetProc(api_version >= VK_MAKE_VERSION(1, 1, 0)
                                                             ? "vkGetPhysicalDeviceFeatures2"
                                                             : "vkGetPhysicalDeviceFeatures2KHR");
  vkGetPhysicalDeviceFeatures2(vk::physical_device, &features);

  // this looks like it would slow things down,
  // and we can't depend on it on all platforms
  features.features.robustBufferAccess = VK_FALSE;

  vkb::DeviceBuilder device_builder(vk::physical_device);

  // If we set the pNext of the VkDeviceCreateInfo to our
  // VkPhysicalDeviceFeatures2 struct, the device creation will use that
  // instead of the ppEnabledFeatures.
  device_builder.add_pNext(&features);
  auto dev_ret = device_builder.build();
  if (!dev_ret) {
    error = dev_ret.error().message();
    return;
  }
  *(vkb::Device*)this = dev_ret.value();

  auto gq_result = get_queue(vkb::QueueType::graphics);
  if (!gq_result) {
    error = gq_result.error().message();
    return;
  }
  graphics_queue = gq_result.value();

  auto gqi_result = get_queue_index(vkb::QueueType::graphics);
  if (!gqi_result) {
    error = gqi_result.error().message();
    return;
  }
  graphics_queue_index = gqi_result.value();

  auto pq_result = get_queue(vkb::QueueType::present);
  if (!pq_result) {
    error = pq_result.error().message();
    return;
  }
  present_queue = pq_result.value();

  auto pqi_result = get_queue_index(vkb::QueueType::present);
  if (!pqi_result) {
    error = pqi_result.error().message();
    return;
  }
  present_queue_index = pqi_result.value();
}
void Device::Destroy() {
  if (device != VK_NULL_HANDLE) {
    DestroyDevice(device, allocation_callbacks);
  }
  device = VK_NULL_HANDLE;
  extensions = {};
  features = {};
  error = "";
}
void InitGrContext() {
  GrVkBackendContext backend = {
      .fInstance = instance,
      .fPhysicalDevice = physical_device,
      .fDevice = device,
      .fQueue = device.graphics_queue,
      .fGraphicsQueueIndex = device.graphics_queue_index,
      .fMaxAPIVersion = instance.api_version,
      .fVkExtensions = &device.extensions,
      .fDeviceFeatures2 = &device.features,
      .fGetProc = GetProc,
  };

  GrContextOptions options = GrContextOptions();

  gr_context = GrDirectContext::MakeVulkan(backend, options);
}
void InitFunctions() {
#define INSTANCE_PROC(P) P = (PFN_vk##P)instance.GetProc("vk" #P)
#define DEVICE_PROC(P) P = (PFN_vk##P)device.GetProc("vk" #P)

  INSTANCE_PROC(GetPhysicalDeviceSurfaceCapabilitiesKHR);
  INSTANCE_PROC(GetPhysicalDeviceSurfaceFormatsKHR);
  INSTANCE_PROC(GetPhysicalDeviceSurfacePresentModesKHR);
  DEVICE_PROC(DeviceWaitIdle);
  DEVICE_PROC(QueueWaitIdle);
  DEVICE_PROC(DestroyDevice);
  DEVICE_PROC(CreateSwapchainKHR);
  DEVICE_PROC(DestroySwapchainKHR);
  DEVICE_PROC(GetSwapchainImagesKHR);
  DEVICE_PROC(AcquireNextImageKHR);
  DEVICE_PROC(QueuePresentKHR);
  DEVICE_PROC(CreateSemaphore);
  DEVICE_PROC(DestroySemaphore);

#undef INSTANCE_PROC
#undef DEVICE_PROC
}
void Swapchain::DestroyBuffers() {
  if (backbuffers) {
    for (uint32_t i = 0; i < image_count + 1; ++i) {
      backbuffers[i].image_index = -1;
      DestroySemaphore(device, backbuffers[i].render_semaphore, nullptr);
    }
  }

  delete[] backbuffers;
  backbuffers = nullptr;

  // Does this actually free the surfaces?
  delete[] surfaces;
  surfaces = nullptr;
  delete[] images;
  images = nullptr;
}
bool Swapchain::CreateBuffers(int width, int height, int sample_count, VkFormat format,
                              VkImageUsageFlags usageFlags, SkColorType colorType,
                              VkSharingMode sharingMode) {
  GetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
  SkASSERT(image_count);
  images = new VkImage[image_count];
  GetSwapchainImagesKHR(device, swapchain, &image_count, images);

  surfaces = new sk_sp<SkSurface>[image_count]();

  static SkSurfaceProps surface_props(0, kRGB_H_SkPixelGeometry);
  static sk_sp<SkColorSpace> color_space = SkColorSpace::MakeSRGB();

  for (uint32_t i = 0; i < image_count; ++i) {
    GrVkImageInfo info = {.fImage = images[i],
                          .fAlloc = skgpu::VulkanAlloc(),
                          .fImageTiling = VK_IMAGE_TILING_OPTIMAL,
                          .fImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .fFormat = format,
                          .fImageUsageFlags = usageFlags,
                          .fLevelCount = 1,
                          .fCurrentQueueFamily = device.present_queue_index,
                          .fSharingMode = sharingMode};

    if (usageFlags & VK_IMAGE_USAGE_SAMPLED_BIT) {
      GrBackendTexture backendTexture(width, height, info);
      surfaces[i] = SkSurface::MakeFromBackendTexture(gr_context.get(), backendTexture,
                                                      kTopLeft_GrSurfaceOrigin, cfg_MSAASampleCount,
                                                      colorType, color_space, &surface_props);
    } else {
      if (cfg_MSAASampleCount > 1) {
        return false;
      }
      GrBackendRenderTarget backendRT(width, height, sample_count, info);
      surfaces[i] = SkSurface::MakeFromBackendRenderTarget(gr_context.get(), backendRT,
                                                           kTopLeft_GrSurfaceOrigin, colorType,
                                                           color_space, &surface_props);
    }
    if (!surfaces[i]) {
      return false;
    }
  }

  // set up the backbuffers
  VkSemaphoreCreateInfo semaphoreInfo;
  memset(&semaphoreInfo, 0, sizeof(VkSemaphoreCreateInfo));
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  semaphoreInfo.pNext = nullptr;
  semaphoreInfo.flags = 0;

  backbuffers = new BackbufferInfo[image_count + 1];
  for (uint32_t i = 0; i < image_count + 1; ++i) {
    backbuffers[i].image_index = -1;
    SkDEBUGCODE(VkResult result =)
        CreateSemaphore(device, &semaphoreInfo, nullptr, &backbuffers[i].render_semaphore);
    SkASSERT(result == VK_SUCCESS);
  }
  current_backbuffer_index = image_count;
  return true;
}

bool Swapchain::Create(int widthHint, int heightHint) {
  // check for capabilities
  VkSurfaceCapabilitiesKHR caps;
  if (GetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &caps)) {
    return false;
  }

  uint32_t surfaceFormatCount;
  if (GetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &surfaceFormatCount, nullptr)) {
    return false;
  }

  VkSurfaceFormatKHR surfaceFormats[surfaceFormatCount];
  if (GetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &surfaceFormatCount,
                                         surfaceFormats)) {
    return false;
  }

  uint32_t presentModeCount;
  if (GetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &presentModeCount,
                                              nullptr)) {
    return false;
  }

  VkPresentModeKHR presentModes[presentModeCount];
  if (GetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &presentModeCount,
                                              presentModes)) {
    return false;
  }

  VkExtent2D extent = caps.currentExtent;
  // use the hints
  if (extent.width == (uint32_t)-1) {
    extent.width = widthHint;
    extent.height = heightHint;
  }

  // clamp width; to protect us from broken hints
  if (extent.width < caps.minImageExtent.width) {
    extent.width = caps.minImageExtent.width;
  } else if (extent.width > caps.maxImageExtent.width) {
    extent.width = caps.maxImageExtent.width;
  }
  // clamp height
  if (extent.height < caps.minImageExtent.height) {
    extent.height = caps.minImageExtent.height;
  } else if (extent.height > caps.maxImageExtent.height) {
    extent.height = caps.maxImageExtent.height;
  }

  int width = (int)extent.width;
  int height = (int)extent.height;

  uint32_t imageCount = caps.minImageCount;
  if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
    // Application must settle for fewer images than desired:
    imageCount = caps.maxImageCount;
  }

  VkImageUsageFlags usageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  SkASSERT((caps.supportedUsageFlags & usageFlags) == usageFlags);
  if (caps.supportedUsageFlags & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) {
    usageFlags |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
  }
  if (caps.supportedUsageFlags & VK_IMAGE_USAGE_SAMPLED_BIT) {
    usageFlags |= VK_IMAGE_USAGE_SAMPLED_BIT;
  }
  SkASSERT(caps.supportedTransforms & caps.currentTransform);
  SkASSERT(caps.supportedCompositeAlpha &
           (VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR));
  VkCompositeAlphaFlagBitsKHR composite_alpha =
      (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
          ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
          : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

  // Pick our surface format.
  VkFormat surfaceFormat = VK_FORMAT_UNDEFINED;
  VkColorSpaceKHR colorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
  for (uint32_t i = 0; i < surfaceFormatCount; ++i) {
    VkFormat localFormat = surfaceFormats[i].format;
    if (GrVkFormatIsSupported(localFormat)) {
      surfaceFormat = localFormat;
      colorSpace = surfaceFormats[i].colorSpace;
      break;
    }
  }
  int sample_count = std::max(1, cfg_MSAASampleCount);

  if (VK_FORMAT_UNDEFINED == surfaceFormat) {
    return false;
  }

  SkColorType colorType;
  switch (surfaceFormat) {
    case VK_FORMAT_R8G8B8A8_UNORM:  // fall through
    case VK_FORMAT_R8G8B8A8_SRGB:
      colorType = kRGBA_8888_SkColorType;
      break;
    case VK_FORMAT_B8G8R8A8_UNORM:  // fall through
      colorType = kBGRA_8888_SkColorType;
      break;
    default:
      return false;
  }

  // If mailbox mode is available, use it, as it is the lowest-latency non-
  // tearing mode. If not, fall back to FIFO which is always available.
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
  bool hasImmediate = false;
  for (uint32_t i = 0; i < presentModeCount; ++i) {
    // use mailbox
    if (VK_PRESENT_MODE_MAILBOX_KHR == presentModes[i]) {
      mode = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    if (VK_PRESENT_MODE_IMMEDIATE_KHR == presentModes[i]) {
      hasImmediate = true;
    }
  }
  if (cfg_DisableVsync && hasImmediate) {
    mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
  }

  VkSwapchainCreateInfoKHR swapchainCreateInfo;
  memset(&swapchainCreateInfo, 0, sizeof(VkSwapchainCreateInfoKHR));
  swapchainCreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  swapchainCreateInfo.surface = surface;
  swapchainCreateInfo.minImageCount = imageCount;
  swapchainCreateInfo.imageFormat = surfaceFormat;
  swapchainCreateInfo.imageColorSpace = colorSpace;
  swapchainCreateInfo.imageExtent = extent;
  swapchainCreateInfo.imageArrayLayers = 1;
  swapchainCreateInfo.imageUsage = usageFlags;

  uint32_t queueFamilies[] = {device.graphics_queue_index, device.present_queue_index};
  if (device.graphics_queue_index != device.present_queue_index) {
    swapchainCreateInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    swapchainCreateInfo.queueFamilyIndexCount = 2;
    swapchainCreateInfo.pQueueFamilyIndices = queueFamilies;
  } else {
    swapchainCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainCreateInfo.queueFamilyIndexCount = 0;
    swapchainCreateInfo.pQueueFamilyIndices = nullptr;
  }

  swapchainCreateInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  swapchainCreateInfo.compositeAlpha = composite_alpha;
  swapchainCreateInfo.presentMode = mode;
  swapchainCreateInfo.clipped = true;
  swapchainCreateInfo.oldSwapchain = swapchain;
  if (CreateSwapchainKHR(device, &swapchainCreateInfo, nullptr, &swapchain)) {
    return false;
  }

  // destroy the old swapchain
  if (swapchainCreateInfo.oldSwapchain != VK_NULL_HANDLE) {
    DeviceWaitIdle(device);

    DestroyBuffers();

    DestroySwapchainKHR(device, swapchainCreateInfo.oldSwapchain, nullptr);
  }

  if (!CreateBuffers(width, height, sample_count, swapchainCreateInfo.imageFormat, usageFlags,
                     colorType, swapchainCreateInfo.imageSharingMode)) {
    DeviceWaitIdle(device);

    DestroyBuffers();

    DestroySwapchainKHR(device, swapchainCreateInfo.oldSwapchain, nullptr);
  }

  return true;
}

Swapchain::BackbufferInfo* Swapchain::GetAvailableBackbuffer() {
  SkASSERT(backbuffers);

  ++current_backbuffer_index;
  if (current_backbuffer_index > image_count) {
    current_backbuffer_index = 0;
  }

  BackbufferInfo* backbuffer = backbuffers + current_backbuffer_index;
  return backbuffer;
}

SkCanvas* Swapchain::GetBackbufferCanvas() {
  BackbufferInfo* backbuffer = GetAvailableBackbuffer();
  SkASSERT(backbuffer);

  // semaphores should be in unsignaled state
  VkSemaphoreCreateInfo semaphoreInfo;
  memset(&semaphoreInfo, 0, sizeof(VkSemaphoreCreateInfo));
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  semaphoreInfo.pNext = nullptr;
  semaphoreInfo.flags = 0;
  VkSemaphore semaphore;
  SkDEBUGCODE(VkResult result =) CreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore);
  SkASSERT(result == VK_SUCCESS);

  VkResult res = AcquireNextImageKHR(device, swapchain, UINT64_MAX, semaphore, VK_NULL_HANDLE,
                                     &backbuffer->image_index);
  if (VK_ERROR_SURFACE_LOST_KHR == res) {
    // need to figure out how to create a new vkSurface without the
    // platformData* maybe use attach somehow? but need a Window
    DestroySemaphore(device, semaphore, nullptr);
    return nullptr;
  }
  if (VK_ERROR_OUT_OF_DATE_KHR == res) {
    // tear swapchain down and try again
    if (!Create(-1, -1)) {
      DestroySemaphore(device, semaphore, nullptr);
      return nullptr;
    }
    backbuffer = GetAvailableBackbuffer();

    res = AcquireNextImageKHR(device, swapchain, UINT64_MAX, semaphore, VK_NULL_HANDLE,
                              &backbuffer->image_index);

    if (VK_SUCCESS != res) {
      DestroySemaphore(device, semaphore, nullptr);
      return nullptr;
    }
  }

  SkSurface* surface = surfaces[backbuffer->image_index].get();

  GrBackendSemaphore beSemaphore;
  beSemaphore.initVulkan(semaphore);

  surface->wait(1, &beSemaphore);

  return surface->getCanvas();
}

void Swapchain::SwapBuffers() {
  BackbufferInfo& backbuffer = backbuffers[current_backbuffer_index];
  SkSurface* surface = surfaces[backbuffer.image_index].get();

  GrBackendSemaphore beSemaphore;
  beSemaphore.initVulkan(backbuffer.render_semaphore);

  GrFlushInfo info = {.fNumSemaphores = 1, .fSignalSemaphores = &beSemaphore};
  skgpu::MutableTextureState presentState(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                          device.present_queue_index);
  surface->flush(info, &presentState);
  surface->recordingContext()->asDirectContext()->submit();

  const VkPresentInfoKHR presentInfo = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                        nullptr,
                                        1,
                                        &backbuffer.render_semaphore,
                                        1,
                                        &swapchain,
                                        &backbuffer.image_index,
                                        nullptr};

  QueuePresentKHR(device.present_queue, &presentInfo);
}
void Swapchain::WaitAndDestroy() {
  if (VK_NULL_HANDLE != swapchain) {
    QueueWaitIdle(device.present_queue);
    DeviceWaitIdle(device);
    DestroyBuffers();
    DestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
  }
}
std::string Init() {
  instance.Init();
  if (!instance.error.empty()) {
    return "Failed to create Vulkan instance: " + instance.error;
  }

  surface.Init();
  if (!surface.error.empty()) {
    return "Failed to create Vulkan surface: " + surface.error;
  }

  physical_device.Init();
  if (!physical_device.error.empty()) {
    return "Failed to create Vulkan physical device: " + physical_device.error;
  }

  device.Init();
  if (!device.error.empty()) {
    return "Failed to create Vulkan device: " + device.error;
  }

  InitGrContext();
  InitFunctions();

  if (!swapchain.Create(-1, -1)) {
    return "Failed to create Vulkan swapchain.";
  }
  return "";
}
void Destroy() {
  if (VK_NULL_HANDLE != device) {
    swapchain.WaitAndDestroy();
    surface.Destroy();
  }

  SkASSERT(gr_context->unique());
  gr_context.reset();

  device.Destroy();
  physical_device.Destroy();
  instance.Destroy();
}

std::string Resize(int width_hint, int height_hint) {
  if (!vk::swapchain.Create(width_hint, height_hint)) {
    return "Couldn't create swapchain";
  }
  return "";
}

SkCanvas* GetBackbufferCanvas() { return swapchain.GetBackbufferCanvas(); }

void Present() { swapchain.SwapBuffers(); }

}  // namespace automat::vk