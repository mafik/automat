#include "win_main.h"

#include "backtrace.h"
#include "log.h"

#include <tools/gpu/vk/GrVulkanDefines.h>

#include <VkBootstrap.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkFont.h>
#include <include/core/SkGraphics.h>
#include <include/core/SkSurface.h>
#include <include/effects/SkGradientShader.h>
#include <include/gpu/GrBackendSemaphore.h>
#include <include/gpu/GrDirectContext.h>
#include <include/gpu/vk/GrVkBackendContext.h>
#include <include/gpu/vk/VulkanExtensions.h>
#include <include/gpu/vk/VulkanTypes.h>
#include <src/gpu/ganesh/vk/GrVkUtil.h>
#include <src/gpu/vk/VulkanInterface.h>

#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <tchar.h>

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

namespace automaton {

static const wchar_t kWindowClass[] = L"Automaton";
static const wchar_t kWindowTitle[] = L"Automaton";

HINSTANCE GetInstance() {
  static HINSTANCE instance = GetModuleHandle(nullptr);
  return instance;
}

WNDCLASSEX &GetWindowClass() {
  static WNDCLASSEX wcex = []() {
    WNDCLASSEX wcex = {.cbSize = sizeof(WNDCLASSEX),
                       .style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC,
                       .lpfnWndProc = WndProc,
                       .cbClsExtra = 0,
                       .cbWndExtra = 0,
                       .hInstance = GetInstance(),
                       .hIcon = LoadIcon(GetInstance(), (LPCTSTR)IDI_WINLOGO),
                       .hCursor = LoadCursor(nullptr, IDC_ARROW),
                       .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1),
                       .lpszMenuName = nullptr,
                       .lpszClassName = kWindowClass,
                       .hIconSm =
                           LoadIcon(GetInstance(), (LPCTSTR)IDI_WINLOGO)};
    return wcex;
  }();
  return wcex;
}

HWND CreateAutomatonWindow() {
  return CreateWindowEx(WS_EX_OVERLAPPEDWINDOW, kWindowClass, kWindowTitle,
                        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800,
                        600, nullptr, nullptr, GetInstance(), nullptr);
}

// Initialized in VulkanInstance::Init
PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
PFN_vkGetDeviceProcAddr GetDeviceProcAddr;

// Initialized in InitVulkanFunctions
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR
    GetPhysicalDeviceSurfaceCapabilitiesKHR;
PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
PFN_vkGetPhysicalDeviceSurfacePresentModesKHR
    GetPhysicalDeviceSurfacePresentModesKHR;

PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
PFN_vkQueuePresentKHR QueuePresentKHR;

PFN_vkDeviceWaitIdle DeviceWaitIdle;
PFN_vkQueueWaitIdle QueueWaitIdle;
PFN_vkDestroyDevice DestroyDevice;
PFN_vkGetDeviceQueue GetDeviceQueue;

struct VulkanInstance : vkb::Instance {
  void Init() {
    if (instance != VK_NULL_HANDLE) {
      Destroy();
    }
    vkb::InstanceBuilder builder;
    builder.set_minimum_instance_version(instance_version);
    builder.require_api_version(api_version);
    for (auto &ext : extensions) {
      builder.enable_extension(ext);
    }
    auto result = builder.build();
    if (!result) {
      error = result.error().message();
    } else {
      (*(vkb::Instance *)this) = result.value();
    }
    GetInstanceProcAddr = fp_vkGetInstanceProcAddr;
    GetDeviceProcAddr = fp_vkGetDeviceProcAddr;
  }

  void Destroy() {
    vkb::destroy_instance(*this);
    debug_messenger = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
    error = "";
  }

  PFN_vkVoidFunction GetProc(const char *proc_name) {
    return GetInstanceProcAddr(instance, proc_name);
  }

  uint32_t instance_version = VK_MAKE_VERSION(1, 1, 0);
  uint32_t api_version = VK_API_VERSION_1_1;
  std::string error = "";
  std::vector<const char *> extensions = {"VK_KHR_win32_surface"};
};

VulkanInstance vulkan_instance;

PFN_vkVoidFunction VulkanGetProc(const char *proc_name, VkInstance instance,
                                 VkDevice device) {
  if (device != VK_NULL_HANDLE) {
    return vulkan_instance.fp_vkGetDeviceProcAddr(device, proc_name);
  }
  return vulkan_instance.fp_vkGetInstanceProcAddr(instance, proc_name);
};

struct VulkanSurface {
  void Init(HWND hWnd) {
    if (surface != VK_NULL_HANDLE) {
      error = "VulkanSurface already initialized.";
      return;
    }
    PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR =
        (PFN_vkCreateWin32SurfaceKHR)vulkan_instance.GetProc(
            "vkCreateWin32SurfaceKHR");

    VkWin32SurfaceCreateInfoKHR surfaceCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .hinstance = GetInstance(),
        .hwnd = hWnd};

    VkResult res = vkCreateWin32SurfaceKHR(vulkan_instance, &surfaceCreateInfo,
                                           nullptr, &surface);
    if (VK_SUCCESS != res) {
      error = "Failure in createWin32SurfaceKHR.";
      return;
    }

    if (VK_NULL_HANDLE == surface) {
      error = "No surface after createWin32SurfaceKHR";
      return;
    }
  }

  void Destroy() {
    vkb::destroy_surface(vulkan_instance, surface);
    surface = VK_NULL_HANDLE;
    error = "";
  }

  operator VkSurfaceKHR() { return surface; }

  VkSurfaceKHR surface;
  std::string error = "";
};

VulkanSurface vulkan_surface;

struct VulkanPhysicalDevice : vkb::PhysicalDevice {
  void Init() {
    if (physical_device != VK_NULL_HANDLE) {
      error = "VulkanPhysicalDevice already initialized.";
      return;
    }
    auto result =
        vkb::PhysicalDeviceSelector(vulkan_instance)
            .set_surface(vulkan_surface)
            .add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
            .add_required_extension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)
            .prefer_gpu_device_type()
            .select();
    if (!result) {
      error = result.error().message();
      return;
    }
    *(vkb::PhysicalDevice *)this = result.value();
    extensions_str = get_extensions();
    for (auto &ext : extensions_str) {
      extensions.push_back(ext.c_str());
    }
  }

  void Destroy() {
    physical_device = VK_NULL_HANDLE;
    extensions_str.clear();
    extensions.clear();
    error = "";
  }

  std::vector<std::string> extensions_str;
  std::vector<const char *> extensions;
  std::string error = "";
};

VulkanPhysicalDevice vulkan_physical_device;

struct VulkanDevice : vkb::Device {
  void Init() {
    extensions.init(VulkanGetProc, vulkan_instance, vulkan_physical_device,
                    vulkan_instance.extensions.size(),
                    vulkan_instance.extensions.data(),
                    vulkan_physical_device.extensions.size(),
                    vulkan_physical_device.extensions.data());

    int api_version = vulkan_physical_device.properties.apiVersion;
    SkASSERT(api_version >= VK_MAKE_VERSION(1, 1, 0) ||
             extensions.hasExtension(
                 VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, 1));

    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = nullptr;

    // Setup all extension feature structs we may want to use.
    void **tailPNext = &features.pNext;

    VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT *blend = nullptr;
    if (extensions.hasExtension(VK_EXT_BLEND_OPERATION_ADVANCED_EXTENSION_NAME,
                                2)) {
      blend =
          (VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT *)sk_malloc_throw(
              sizeof(VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT));
      blend->sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT;
      blend->pNext = nullptr;
      *tailPNext = blend;
      tailPNext = &blend->pNext;
    }

    VkPhysicalDeviceSamplerYcbcrConversionFeatures *ycbcrFeature = nullptr;
    if (api_version >= VK_MAKE_VERSION(1, 1, 0) ||
        extensions.hasExtension(VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
                                1)) {
      ycbcrFeature =
          (VkPhysicalDeviceSamplerYcbcrConversionFeatures *)sk_malloc_throw(
              sizeof(VkPhysicalDeviceSamplerYcbcrConversionFeatures));
      ycbcrFeature->sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
      ycbcrFeature->pNext = nullptr;
      ycbcrFeature->samplerYcbcrConversion = VK_TRUE;
      *tailPNext = ycbcrFeature;
      tailPNext = &ycbcrFeature->pNext;
    }

    PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2 =
        (PFN_vkGetPhysicalDeviceFeatures2)vulkan_instance.GetProc(
            api_version >= VK_MAKE_VERSION(1, 1, 0)
                ? "vkGetPhysicalDeviceFeatures2"
                : "vkGetPhysicalDeviceFeatures2KHR");
    vkGetPhysicalDeviceFeatures2(vulkan_physical_device, &features);

    // this looks like it would slow things down,
    // and we can't depend on it on all platforms
    features.features.robustBufferAccess = VK_FALSE;

    vkb::DeviceBuilder device_builder(vulkan_physical_device);

    // If we set the pNext of the VkDeviceCreateInfo to our
    // VkPhysicalDeviceFeatures2 struct, the device creation will use that
    // instead of the ppEnabledFeatures.
    device_builder.add_pNext(&features);
    auto dev_ret = device_builder.build();
    if (!dev_ret) {
      error = dev_ret.error().message();
      return;
    }
    *(vkb::Device *)this = dev_ret.value();

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

  void Destroy() {
    if (device != VK_NULL_HANDLE) {
      vkb::destroy_device(*this);
    }
    device = VK_NULL_HANDLE;
    extensions = {};
    features = {};
    error = "";
  }

  PFN_vkVoidFunction GetProc(const char *proc_name) {
    return GetDeviceProcAddr(device, proc_name);
  }

  uint32_t graphics_queue_index;
  VkQueue graphics_queue;
  uint32_t present_queue_index;
  VkQueue present_queue;
  skgpu::VulkanExtensions extensions;
  VkPhysicalDeviceFeatures2 features = {};
  std::string error = "";
};

VulkanDevice vulkan_device;

sk_sp<const skgpu::VulkanInterface> skia_vulkan_interface;

void InitSkiaVulkanInterface() {
  skia_vulkan_interface.reset(new skgpu::VulkanInterface(
      VulkanGetProc, vulkan_instance, vulkan_device,
      vulkan_instance.instance_version,
      vulkan_physical_device.properties.apiVersion, &vulkan_device.extensions));
}

sk_sp<GrDirectContext> gr_context;

void InitGrContext() {
  GrVkBackendContext grVkBackendContext = {
      .fInstance = vulkan_instance,
      .fPhysicalDevice = vulkan_physical_device,
      .fDevice = vulkan_device,
      .fQueue = vulkan_device.graphics_queue,
      .fGraphicsQueueIndex = vulkan_device.graphics_queue_index,
      .fMaxAPIVersion = vulkan_instance.api_version,
      .fVkExtensions = &vulkan_device.extensions,
      .fDeviceFeatures2 = &vulkan_device.features,
      .fGetProc = VulkanGetProc,
  };

  GrContextOptions grContextOptions = GrContextOptions();

  gr_context =
      GrDirectContext::MakeVulkan(grVkBackendContext, grContextOptions);
}

void InitVulkanFunctions() {
#define INSTANCE_PROC(P) P = (PFN_vk##P)vulkan_instance.GetProc("vk" #P)
#define DEVICE_PROC(P) P = (PFN_vk##P)vulkan_device.GetProc("vk" #P)

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
  DEVICE_PROC(GetDeviceQueue);

#undef INSTANCE_PROC
#undef DEVICE_PROC
}

} // namespace automaton

using namespace automaton;

int fWidth;
int fHeight;
double fRotationAngle = 0;

int fMSAASampleCount = 1;
bool fDisableVsync = true;
SkSurfaceProps fSurfaceProps(0, kRGB_H_SkPixelGeometry);

int fSampleCount = 1;
int fStencilBits = 0;

VkSwapchainKHR fSwapchain;

uint32_t fImageCount;
VkImage *fImages; // images in the swapchain
VkImageLayout
    *fImageLayouts; // layouts of these images when not color attachment
sk_sp<SkSurface>
    *fSurfaces; // surfaces client renders to (may not be based on rts)

struct BackbufferInfo {
  uint32_t fImageIndex;         // image this is associated with
  VkSemaphore fRenderSemaphore; // we wait on this for rendering to be done
};

BackbufferInfo *fBackbuffers;
uint32_t fCurrentBackbufferIndex;

void destroyBuffers() {
  if (fBackbuffers) {
    for (uint32_t i = 0; i < fImageCount + 1; ++i) {
      fBackbuffers[i].fImageIndex = -1;
      GR_VK_CALL(skia_vulkan_interface,
                 DestroySemaphore(vulkan_device,
                                  fBackbuffers[i].fRenderSemaphore, nullptr));
    }
  }

  delete[] fBackbuffers;
  fBackbuffers = nullptr;

  // Does this actually free the surfaces?
  delete[] fSurfaces;
  fSurfaces = nullptr;
  delete[] fImageLayouts;
  fImageLayouts = nullptr;
  delete[] fImages;
  fImages = nullptr;
}

void destroyContext() {
  if (VK_NULL_HANDLE != vulkan_device) {
    QueueWaitIdle(vulkan_device.present_queue);
    DeviceWaitIdle(vulkan_device);

    destroyBuffers();

    if (VK_NULL_HANDLE != fSwapchain) {
      DestroySwapchainKHR(vulkan_device, fSwapchain, nullptr);
      fSwapchain = VK_NULL_HANDLE;
    }

    vulkan_surface.Destroy();
  }

  SkASSERT(gr_context->unique());
  gr_context.reset();
  skia_vulkan_interface.reset();

  vulkan_device.Destroy();

  vulkan_physical_device.Destroy();
  vulkan_instance.Destroy();
}

bool createBuffers(VkFormat format, VkImageUsageFlags usageFlags,
                   SkColorType colorType, VkSharingMode sharingMode) {
  GetSwapchainImagesKHR(vulkan_device, fSwapchain, &fImageCount, nullptr);
  SkASSERT(fImageCount);
  fImages = new VkImage[fImageCount];
  GetSwapchainImagesKHR(vulkan_device, fSwapchain, &fImageCount, fImages);

  // set up initial image layouts and create surfaces
  fImageLayouts = new VkImageLayout[fImageCount];
  fSurfaces = new sk_sp<SkSurface>[fImageCount]();
  for (uint32_t i = 0; i < fImageCount; ++i) {
    fImageLayouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;

    GrVkImageInfo info;
    info.fImage = fImages[i];
    info.fAlloc = skgpu::VulkanAlloc();
    info.fImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.fImageTiling = VK_IMAGE_TILING_OPTIMAL;
    info.fFormat = format;
    info.fImageUsageFlags = usageFlags;
    info.fLevelCount = 1;
    info.fCurrentQueueFamily = vulkan_device.present_queue_index;
    info.fSharingMode = sharingMode;

    sk_sp<SkColorSpace> colorSpace = SkColorSpace::MakeSRGB();

    if (usageFlags & VK_IMAGE_USAGE_SAMPLED_BIT) {
      GrBackendTexture backendTexture(fWidth, fHeight, info);
      fSurfaces[i] = SkSurface::MakeFromBackendTexture(
          gr_context.get(), backendTexture, kTopLeft_GrSurfaceOrigin,
          fMSAASampleCount, colorType, colorSpace, &fSurfaceProps);
    } else {
      if (fMSAASampleCount > 1) {
        return false;
      }
      GrBackendRenderTarget backendRT(fWidth, fHeight, fSampleCount, info);
      fSurfaces[i] = SkSurface::MakeFromBackendRenderTarget(
          gr_context.get(), backendRT, kTopLeft_GrSurfaceOrigin, colorType,
          colorSpace, &fSurfaceProps);
    }
    if (!fSurfaces[i]) {
      return false;
    }
  }

  // set up the backbuffers
  VkSemaphoreCreateInfo semaphoreInfo;
  memset(&semaphoreInfo, 0, sizeof(VkSemaphoreCreateInfo));
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  semaphoreInfo.pNext = nullptr;
  semaphoreInfo.flags = 0;

  // we create one additional backbuffer structure here, because we want to
  // give the command buffers they contain a chance to finish before we cycle
  // back
  fBackbuffers = new BackbufferInfo[fImageCount + 1];
  for (uint32_t i = 0; i < fImageCount + 1; ++i) {
    fBackbuffers[i].fImageIndex = -1;
    SkDEBUGCODE(VkResult result =)
        GR_VK_CALL(skia_vulkan_interface,
                   CreateSemaphore(vulkan_device, &semaphoreInfo, nullptr,
                                   &fBackbuffers[i].fRenderSemaphore));
    SkASSERT(result == VK_SUCCESS);
  }
  fCurrentBackbufferIndex = fImageCount;
  return true;
}

bool createSwapchain(int width, int height) {
  // check for capabilities
  VkSurfaceCapabilitiesKHR caps;
  if (GetPhysicalDeviceSurfaceCapabilitiesKHR(vulkan_physical_device,
                                              vulkan_surface, &caps)) {
    return false;
  }

  uint32_t surfaceFormatCount;
  if (GetPhysicalDeviceSurfaceFormatsKHR(vulkan_physical_device, vulkan_surface,
                                         &surfaceFormatCount, nullptr)) {
    return false;
  }

  VkSurfaceFormatKHR surfaceFormats[surfaceFormatCount];
  if (GetPhysicalDeviceSurfaceFormatsKHR(vulkan_physical_device, vulkan_surface,
                                         &surfaceFormatCount, surfaceFormats)) {
    return false;
  }

  uint32_t presentModeCount;
  if (GetPhysicalDeviceSurfacePresentModesKHR(
          vulkan_physical_device, vulkan_surface, &presentModeCount, nullptr)) {
    return false;
  }

  VkPresentModeKHR presentModes[presentModeCount];
  if (GetPhysicalDeviceSurfacePresentModesKHR(vulkan_physical_device,
                                              vulkan_surface, &presentModeCount,
                                              presentModes)) {
    return false;
  }

  VkExtent2D extent = caps.currentExtent;
  // use the hints
  if (extent.width == (uint32_t)-1) {
    extent.width = width;
    extent.height = height;
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

  fWidth = (int)extent.width;
  fHeight = (int)extent.height;

  uint32_t imageCount = caps.minImageCount + 2;
  if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
    // Application must settle for fewer images than desired:
    imageCount = caps.maxImageCount;
  }

  VkImageUsageFlags usageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  SkASSERT((caps.supportedUsageFlags & usageFlags) == usageFlags);
  if (caps.supportedUsageFlags & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) {
    usageFlags |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
  }
  if (caps.supportedUsageFlags & VK_IMAGE_USAGE_SAMPLED_BIT) {
    usageFlags |= VK_IMAGE_USAGE_SAMPLED_BIT;
  }
  SkASSERT(caps.supportedTransforms & caps.currentTransform);
  SkASSERT(caps.supportedCompositeAlpha & (VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR |
                                           VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR));
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
  fSampleCount = std::max(1, fMSAASampleCount);
  fStencilBits = 8;

  if (VK_FORMAT_UNDEFINED == surfaceFormat) {
    return false;
  }

  SkColorType colorType;
  switch (surfaceFormat) {
  case VK_FORMAT_R8G8B8A8_UNORM: // fall through
  case VK_FORMAT_R8G8B8A8_SRGB:
    colorType = kRGBA_8888_SkColorType;
    break;
  case VK_FORMAT_B8G8R8A8_UNORM: // fall through
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
  if (fDisableVsync && hasImmediate) {
    mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
  }

  VkSwapchainCreateInfoKHR swapchainCreateInfo;
  memset(&swapchainCreateInfo, 0, sizeof(VkSwapchainCreateInfoKHR));
  swapchainCreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  swapchainCreateInfo.surface = vulkan_surface;
  swapchainCreateInfo.minImageCount = imageCount;
  swapchainCreateInfo.imageFormat = surfaceFormat;
  swapchainCreateInfo.imageColorSpace = colorSpace;
  swapchainCreateInfo.imageExtent = extent;
  swapchainCreateInfo.imageArrayLayers = 1;
  swapchainCreateInfo.imageUsage = usageFlags;

  uint32_t queueFamilies[] = {vulkan_device.graphics_queue_index,
                              vulkan_device.present_queue_index};
  if (vulkan_device.graphics_queue_index != vulkan_device.present_queue_index) {
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
  swapchainCreateInfo.oldSwapchain = fSwapchain;
  if (CreateSwapchainKHR(vulkan_device, &swapchainCreateInfo, nullptr,
                         &fSwapchain)) {
    return false;
  }

  // destroy the old swapchain
  if (swapchainCreateInfo.oldSwapchain != VK_NULL_HANDLE) {
    DeviceWaitIdle(vulkan_device);

    destroyBuffers();

    DestroySwapchainKHR(vulkan_device, swapchainCreateInfo.oldSwapchain,
                        nullptr);
  }

  if (!createBuffers(swapchainCreateInfo.imageFormat, usageFlags, colorType,
                     swapchainCreateInfo.imageSharingMode)) {
    DeviceWaitIdle(vulkan_device);

    destroyBuffers();

    DestroySwapchainKHR(vulkan_device, swapchainCreateInfo.oldSwapchain,
                        nullptr);
  }

  return true;
}

void OnResize(int w, int h) {
  if (!createSwapchain(w, h)) {
    MessageBox(nullptr, L"createSwapchain failed!", L"ALERT", 0);
  }
}

BackbufferInfo *getAvailableBackbuffer() {
  SkASSERT(fBackbuffers);

  ++fCurrentBackbufferIndex;
  if (fCurrentBackbufferIndex > fImageCount) {
    fCurrentBackbufferIndex = 0;
  }

  BackbufferInfo *backbuffer = fBackbuffers + fCurrentBackbufferIndex;
  return backbuffer;
}

sk_sp<SkSurface> getBackbufferSurface() {
  BackbufferInfo *backbuffer = getAvailableBackbuffer();
  SkASSERT(backbuffer);

  // semaphores should be in unsignaled state
  VkSemaphoreCreateInfo semaphoreInfo;
  memset(&semaphoreInfo, 0, sizeof(VkSemaphoreCreateInfo));
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  semaphoreInfo.pNext = nullptr;
  semaphoreInfo.flags = 0;
  VkSemaphore semaphore;
  SkDEBUGCODE(VkResult result =) GR_VK_CALL(
      skia_vulkan_interface,
      CreateSemaphore(vulkan_device, &semaphoreInfo, nullptr, &semaphore));
  SkASSERT(result == VK_SUCCESS);

  // acquire the image
  VkResult res =
      AcquireNextImageKHR(vulkan_device, fSwapchain, UINT64_MAX, semaphore,
                          VK_NULL_HANDLE, &backbuffer->fImageIndex);
  if (VK_ERROR_SURFACE_LOST_KHR == res) {
    // need to figure out how to create a new vkSurface without the
    // platformData* maybe use attach somehow? but need a Window
    GR_VK_CALL(skia_vulkan_interface,
               DestroySemaphore(vulkan_device, semaphore, nullptr));
    return nullptr;
  }
  if (VK_ERROR_OUT_OF_DATE_KHR == res) {
    // tear swapchain down and try again
    if (!createSwapchain(-1, -1)) {
      GR_VK_CALL(skia_vulkan_interface,
                 DestroySemaphore(vulkan_device, semaphore, nullptr));
      return nullptr;
    }
    backbuffer = getAvailableBackbuffer();

    // acquire the image
    res = AcquireNextImageKHR(vulkan_device, fSwapchain, UINT64_MAX, semaphore,
                              VK_NULL_HANDLE, &backbuffer->fImageIndex);

    if (VK_SUCCESS != res) {
      GR_VK_CALL(skia_vulkan_interface,
                 DestroySemaphore(vulkan_device, semaphore, nullptr));
      return nullptr;
    }
  }

  SkSurface *surface = fSurfaces[backbuffer->fImageIndex].get();

  GrBackendSemaphore beSemaphore;
  beSemaphore.initVulkan(semaphore);

  surface->wait(1, &beSemaphore);

  return sk_ref_sp(surface);
}

void swapBuffers() {

  BackbufferInfo *backbuffer = fBackbuffers + fCurrentBackbufferIndex;
  SkSurface *surface = fSurfaces[backbuffer->fImageIndex].get();

  GrBackendSemaphore beSemaphore;
  beSemaphore.initVulkan(backbuffer->fRenderSemaphore);

  GrFlushInfo info;
  info.fNumSemaphores = 1;
  info.fSignalSemaphores = &beSemaphore;
  skgpu::MutableTextureState presentState(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                          vulkan_device.present_queue_index);
  surface->flush(info, &presentState);
  surface->recordingContext()->asDirectContext()->submit();

  // Submit present operation to present queue
  const VkPresentInfoKHR presentInfo = {
      VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, // sType
      nullptr,                            // pNext
      1,                                  // waitSemaphoreCount
      &backbuffer->fRenderSemaphore,      // pWaitSemaphores
      1,                                  // swapchainCount
      &fSwapchain,                        // pSwapchains
      &backbuffer->fImageIndex,           // pImageIndices
      nullptr                             // pResults
  };

  QueuePresentKHR(vulkan_device.present_queue, &presentInfo);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
  case WM_SIZE:
    OnResize(LOWORD(lParam), HIWORD(lParam));
    break;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);

    sk_sp<SkSurface> backbuffer = getBackbufferSurface();

    auto canvas = backbuffer->getCanvas();

    // Clear background
    canvas->clear(SK_ColorWHITE);

    SkPaint paint;
    paint.setColor(SK_ColorRED);

    // Draw a rectangle with red paint
    SkRect rect = SkRect::MakeXYWH(10, 10, 128, 128);
    canvas->drawRect(rect, paint);

    // Set up a linear gradient and draw a circle
    {
      SkPoint linearPoints[] = {{0, 0}, {300, 300}};
      SkColor linearColors[] = {SK_ColorGREEN, SK_ColorBLACK};
      paint.setShader(SkGradientShader::MakeLinear(
          linearPoints, linearColors, nullptr, 2, SkTileMode::kMirror));
      paint.setAntiAlias(true);

      canvas->drawCircle(200, 200, 64, paint);

      // Detach shader
      paint.setShader(nullptr);
    }

    // Draw a message with a nice black paint
    SkFont font;
    font.setSubpixel(true);
    font.setSize(20);
    paint.setColor(SK_ColorBLACK);

    canvas->save();
    static const char message[] = "Hello World ";

    // Translate and rotate
    canvas->translate(300, 300);
    fRotationAngle += 0.2f;
    if (fRotationAngle > 360) {
      fRotationAngle -= 360;
    }
    canvas->rotate(fRotationAngle);

    // Draw the text
    canvas->drawSimpleText(message, strlen(message), SkTextEncoding::kUTF8, 0,
                           0, font, paint);

    canvas->restore();

    backbuffer->flushAndSubmit();
    swapBuffers();

    EndPaint(hWnd, &ps);
    break;
  }
  case WM_DESTROY:
    PostQuitMessage(0);
    break;
  default:
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
    break;
  }
  return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    PWSTR pCmdLine, int nCmdShow) {
  EnableBacktraceOnSIGSEGV();
  SetConsoleOutputCP(CP_UTF8); // utf-8
  SkGraphics::Init();

  if (!RegisterClassEx(&GetWindowClass())) {
    FATAL() << "Failed to register window class.";
  }

  HWND hWnd = CreateAutomatonWindow();
  if (!hWnd) {
    FATAL() << "Failed to create main window.";
  }

  vulkan_instance.Init();
  if (!vulkan_instance.error.empty()) {
    FATAL() << "Failed to create Vulkan instance: " << vulkan_instance.error;
  }

  vulkan_surface.Init(hWnd);
  if (!vulkan_surface.error.empty()) {
    FATAL() << "Failed to create Vulkan surface: " << vulkan_surface.error;
  }

  vulkan_physical_device.Init();
  if (!vulkan_physical_device.error.empty()) {
    FATAL() << "Failed to create Vulkan physical device: "
            << vulkan_physical_device.error;
  }

  vulkan_device.Init();
  if (!vulkan_device.error.empty()) {
    FATAL() << "Failed to create Vulkan device: " << vulkan_device.error;
  }

  InitSkiaVulkanInterface();
  InitGrContext();
  InitVulkanFunctions();

  if (!createSwapchain(-1, -1)) {
    destroyContext();
    return 7;
  }

  ShowWindow(hWnd, nCmdShow);
  UpdateWindow(hWnd);

  RECT rect;
  GetClientRect(hWnd, &rect);
  OnResize(rect.right - rect.left, rect.bottom - rect.top);

  MSG msg = {};
  while (GetMessage(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  destroyContext();
  return (int)msg.wParam;
}
