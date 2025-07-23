// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "vk.hh"

#include <VkBootstrap.h>
#include <include/core/SkAlphaType.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkColorType.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkSurface.h>
#include <include/gpu/GpuTypes.h>
#include <include/gpu/MutableTextureState.h>
#include <include/gpu/ganesh/GrRecordingContext.h>
#include <include/gpu/graphite/BackendSemaphore.h>
#include <include/gpu/graphite/BackendTexture.h>
#include <include/gpu/graphite/Context.h>
#include <include/gpu/graphite/ContextOptions.h>
#include <include/gpu/graphite/Recorder.h>
#include <include/gpu/graphite/Surface.h>
#include <include/gpu/graphite/vk/VulkanGraphiteTypes.h>
#include <include/gpu/graphite/vk/VulkanGraphiteUtils.h>
#include <include/gpu/vk/VulkanBackendContext.h>
#include <include/gpu/vk/VulkanExtensions.h>
#include <include/gpu/vk/VulkanMutableTextureState.h>
#include <include/gpu/vk/VulkanPreferredFeatures.h>
#include <src/gpu/graphite/vk/VulkanGraphiteUtils.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

#include "log.hh"
#include "root_widget.hh"
#include "status.hh"

#if defined(_WIN32)
#include "win32.hh"
#include "win32_window.hh"
#undef CreateSemaphore  // Windows is evil
#elif defined(__linux__)
#include "xcb.hh"
#include "xcb_window.hh"
#endif

#pragma comment(lib, "vk-bootstrap")

namespace graphite = skgpu::graphite;

namespace automat::vk {

// Initialized in Instance::Init
PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
PFN_vkGetDeviceProcAddr GetDeviceProcAddr;

// Initialized in InitFunctions
#define EACH_INSTANCE_PROC(X)                \
  X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
  X(GetPhysicalDeviceSurfaceFormatsKHR)      \
  X(GetPhysicalDeviceSurfacePresentModesKHR) \
  X(EnumerateDeviceExtensionProperties)      \
  X(GetPhysicalDeviceFeatures2)

#define EACH_DEVICE_PROC(X) \
  X(GetDeviceQueue)         \
  X(CreateSwapchainKHR)     \
  X(DestroySwapchainKHR)    \
  X(GetSwapchainImagesKHR)  \
  X(AcquireNextImageKHR)    \
  X(QueuePresentKHR)        \
  X(DeviceWaitIdle)         \
  X(QueueWaitIdle)          \
  X(DestroyDevice)          \
  X(CreateSemaphore)        \
  X(DestroySemaphore)       \
  X(CreateImage)            \
  X(DestroyImage)           \
  X(CreateCommandPool)      \
  X(DestroyCommandPool)     \
  X(AllocateCommandBuffers) \
  X(BeginCommandBuffer)     \
  X(EndCommandBuffer)       \
  X(CmdCopyImage)           \
  X(QueueSubmit2)           \
  X(BindImageMemory)        \
  X(CmdPipelineBarrier2)

#define VULKAN_DECLARE(F) PFN_vk##F vk##F;
EACH_INSTANCE_PROC(VULKAN_DECLARE)
EACH_DEVICE_PROC(VULKAN_DECLARE)
#undef VULKAN_DECLARE

constexpr auto kMinimumVulkanVersion = VK_API_VERSION_1_3;

#define CHECK_RESULT(call)                                               \
  if (auto res = call) {                                                 \
    AppendErrorMessage(status) += "Failure in " #call ": " + ToStr(res); \
    return;                                                              \
  }

skgpu::VulkanPreferredFeatures skia_features;

struct Instance : vkb::Instance {
  void Init(Status&);
  void Destroy();
  PFN_vkVoidFunction GetProc(const char* proc_name) {
    return GetInstanceProcAddr(instance, proc_name);
  }
  std::vector<const char*> extensions = {
#if defined(_WIN32)
      "VK_KHR_win32_surface"
#elif defined(__linux__)
      "VK_KHR_xcb_surface"
#endif
  };
} instance;

struct Surface {
  void Init(Status& status);
  void Destroy();
  operator VkSurfaceKHR() { return surface; }

  VkSurfaceKHR surface;
} surface;

struct PhysicalDevice : vkb::PhysicalDevice {
  void Init(Status& status);
  void Destroy();
} physical_device;

struct Device : vkb::Device {
  void Init(Status& status);
  void Destroy();
  PFN_vkVoidFunction GetProc(const char* proc_name) { return GetDeviceProcAddr(device, proc_name); }

  // Index within the physical device queue family list
  uint32_t graphics_queue_index;
  uint32_t present_queue_index;
  VkQueue graphics_queue = nullptr;
  VkQueue background_queue = nullptr;
  VkQueue present_queue = nullptr;
  skgpu::VulkanExtensions extensions;
  VkPhysicalDeviceFeatures2 features = {};
} device;

void Semaphore::Create(Status& status) {
  static const VkSemaphoreCreateInfo kVkSemaphoreCreateInfo = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0};
  if (vk_semaphore == VK_NULL_HANDLE) {
    CHECK_RESULT(vkCreateSemaphore(device, &kVkSemaphoreCreateInfo, nullptr, &vk_semaphore));
  }
}
void Semaphore::Destroy() {
  if (vk_semaphore != VK_NULL_HANDLE) {
    vkDestroySemaphore(device, vk_semaphore, nullptr);
    vk_semaphore = VK_NULL_HANDLE;
  }
}

Semaphore::Semaphore(Status& status) { Create(status); }
Semaphore& Semaphore::operator=(Semaphore&& other) {
  Destroy();
  vk_semaphore = other.vk_semaphore;
  other.vk_semaphore = VK_NULL_HANDLE;
  return *this;
}
Semaphore::~Semaphore() { Destroy(); }

Semaphore::operator graphite::BackendSemaphore() {
  return graphite::BackendSemaphores::MakeVulkan(vk_semaphore);
}

std::unique_ptr<graphite::Context> graphite_context, background_context;

std::unique_ptr<graphite::Recorder> graphite_recorder;

struct CommandPool {
  VkCommandPool vk_command_pool = VK_NULL_HANDLE;
  void Create(Status& status);
  void Destroy();
} command_pool;

struct Swapchain {
  void DestroyBuffers();
  void Create(int widthHint, int heightHint, Status& status);
  SkCanvas* AcquireCanvas();
  void Present();
  void WaitAndDestroy();
  operator VkSwapchainKHR() { return swapchain; }

  VkSwapchainKHR swapchain;

  std::vector<VkImage> images;

  struct Canvas {
    sk_sp<SkSurface> sk_surface;
    Semaphore sem_rendered;
  };
  std::vector<Canvas> canvases;

  uint32_t current_image_index = 0;
  Semaphore sem_acquired;  // signals image is available for use (vkAcquireNextImageKHR)

  // Skia milestone 137 includes a change that requires all window surfaces to support
  // VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT.
  // https://source.chromium.org/chromium/_/skia/skia/+/6627deb65939ee886c774d290d91269c6968eaf9
  // This feature is not supported by Windows Intel GPUs. As a workaround, we create a separate
  // target image that Skia can use as a render target and copy the results into the swapchain image
  // at present time.
  //
  // To see if Skia is fixed, configure it with:
  //
  // > bin\gn gen out/Static --args="skia_use_vulkan=true
  //   skia_enable_ganesh=true skia_enable_graphite=true skia_enable_tools=true"
  //
  // then build the viewer:
  //
  // > ninja -C out\Static viewer
  //
  // and run it (vulkan mode):
  //
  // > out\Static\viewer -b vk
  //
  // Finally, switch to the Graphite mode by typing '/' and changing the backend to 'graphite'. The
  // viewer should not crash with:
  //
  // [graphite] ** ERROR ** validate_backend_texture failed: backendTex.info =
  // Vulkan(viewFormat=BGRA8,flags=0x00000000,imageTiling=0,imageUsageFlags=0x00000017,sharingMode=0,aspectMask=1,bpp=4,sampleCount=1,mipmapped=0,protected=0);
  // colorType = 6
  struct OffscreenRenderingState {
    struct Backbuffer {
      graphite::BackendTexture texture;
      VkImage vk_image;
      // For each swapchain image, we pre-record a command buffer that copies this backbuffer into
      // that swapchain image.
      std::vector<VkCommandBuffer> vk_command_buffers;
      Semaphore sem_copied;
    } backbuffers[2];
    int current = 0;
  };
  Optional<OffscreenRenderingState> render_offscreen;
} swapchain;

void Instance::Init(Status& status) {
  if (instance != VK_NULL_HANDLE) {
    Destroy();
  }

  auto system_info = vkb::SystemInfo::get_system_info().value();
  skia_features.init(kMinimumVulkanVersion);
  skia_features.addToInstanceExtensions(system_info.available_extensions.data(),
                                        system_info.available_extensions.size(), extensions);

  vkb::InstanceBuilder builder;
  builder.request_validation_layers();
  builder.require_api_version(kMinimumVulkanVersion);
  builder.enable_extensions(extensions);
  auto result = builder.build();
  if (!result) {
    AppendErrorMessage(status) += result.error().message();
    return;
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
}
PFN_vkVoidFunction GetProc(const char* proc_name, VkInstance vk_instance, VkDevice device) {
  if (device != VK_NULL_HANDLE) {
    return GetDeviceProcAddr(device, proc_name);
  }
  return GetInstanceProcAddr(vk_instance, proc_name);
};
void Surface::Init(Status& status) {
  if (surface != VK_NULL_HANDLE) {
    AppendErrorMessage(status) += "vk::Surface already initialized";
    return;
  }

#if defined(_WIN32)

  PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR =
      (PFN_vkCreateWin32SurfaceKHR)instance.GetProc("vkCreateWin32SurfaceKHR");

  auto win32_window = dynamic_cast<Win32Window*>(automat::gui::root_widget->window.get());

  VkWin32SurfaceCreateInfoKHR surfaceCreateInfo = {
      .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
      .pNext = nullptr,
      .flags = 0,
      .hinstance = win32::GetInstance(),
      .hwnd = win32_window->hwnd};

  VkResult res = vkCreateWin32SurfaceKHR(instance, &surfaceCreateInfo, nullptr, &surface);
  if (VK_SUCCESS != res) {
    AppendErrorMessage(status) += "Failure in vkCreateWin32SurfaceKHR";
    return;
  }

  if (VK_NULL_HANDLE == surface) {
    AppendErrorMessage(status) += "No surface after vkCreateWin32SurfaceKHR";
    return;
  }

#elif defined(__linux__)

  PFN_vkCreateXcbSurfaceKHR vkCreateXcbSurfaceKHR =
      (PFN_vkCreateXcbSurfaceKHR)instance.GetProc("vkCreateXcbSurfaceKHR");

  auto xcb_window =
      dynamic_cast<xcb::XCBWindow*>(automat::gui::root_widget->window.get())->xcb_window;

  VkXcbSurfaceCreateInfoKHR surfaceCreateInfo = {
      .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
      .pNext = nullptr,
      .flags = 0,
      .connection = xcb::connection,
      .window = xcb_window};

  VkResult res = vkCreateXcbSurfaceKHR(instance, &surfaceCreateInfo, nullptr, &surface);
  if (VK_SUCCESS != res) {
    AppendErrorMessage(status) += "Failure in vkCreateXcbSurfaceKHR.";
    return;
  }

  if (VK_NULL_HANDLE == surface) {
    AppendErrorMessage(status) += "No surface after vkCreateXcbSurfaceKHR";
    return;
  }

#endif
}
void Surface::Destroy() {
  vkb::destroy_surface(instance, surface);
  surface = VK_NULL_HANDLE;
}
void PhysicalDevice::Init(Status& status) {
  if (physical_device != VK_NULL_HANDLE) {
    AppendErrorMessage(status) += "vk::PhysicalDevice already initialized";
    return;
  }
  auto result = vkb::PhysicalDeviceSelector(instance)
                    .set_surface(vk::surface)
                    .add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
                    .prefer_gpu_device_type()
                    .select();
  if (!result) {
    AppendErrorMessage(status) += result.error().message();
    return;
  }
  *(vkb::PhysicalDevice*)this = std::move(result.value());
}
void PhysicalDevice::Destroy() { physical_device = VK_NULL_HANDLE; }

void Device::Init(Status& status) {
  uint32_t extension_count = 0;
  vkEnumerateDeviceExtensionProperties(vk::physical_device, nullptr, &extension_count, nullptr);
  std::vector<VkExtensionProperties> device_extensions(extension_count);
  vkEnumerateDeviceExtensionProperties(vk::physical_device, nullptr, &extension_count,
                                       device_extensions.data());
  features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features.pNext = nullptr;

  skia_features.addFeaturesToQuery(device_extensions.data(), extension_count, features);

  vkGetPhysicalDeviceFeatures2(vk::physical_device, &features);

  std::vector<const char*> app_extensions;
  skia_features.addFeaturesToEnable(app_extensions, features);
  vk::physical_device.enable_extensions_if_present(app_extensions);

  vkb::DeviceBuilder device_builder(vk::physical_device);

  std::vector<vkb::CustomQueueDescription> queue_descriptions;

  auto queue_families = vk::physical_device.get_queue_families();
  graphics_queue_index = -1;
  present_queue_index = -1;
  int graphics_queue_count = 0;
  for (uint32_t i = 0; i < (uint32_t)queue_families.size(); i++) {
    if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      graphics_queue_index = i;
      // TODO: check for presentation support
      present_queue_index = i;
      graphics_queue_count = queue_families[i].queueCount;
      auto priorities = std::vector<float>(queue_families[i].queueCount, 1.0f);
      if (queue_families[i].queueCount > 1) {
        priorities.back() = 0.0f;
      }
      queue_descriptions.push_back(vkb::CustomQueueDescription(i, priorities));
    }
  }

  device_builder.custom_queue_setup(queue_descriptions);

  // If we set the pNext of the VkDeviceCreateInfo to our
  // VkPhysicalDeviceFeatures2 struct, the device creation will use that
  // instead of the ppEnabledFeatures.
  device_builder.add_pNext(&features);
  auto dev_ret = device_builder.build();
  if (!dev_ret) {
    AppendErrorMessage(status) += dev_ret.error().message();
    return;
  }
  *(vkb::Device*)this = dev_ret.value();

  std::vector<std::string> extensions_str = this->physical_device.get_extensions();
  std::vector<const char*> extensions_cstr;
  for (auto& ext : extensions_str) {
    extensions_cstr.push_back(ext.c_str());
  }

  extensions.init(vk::GetProc, instance, vk::physical_device, instance.extensions.size(),
                  instance.extensions.data(), extensions_cstr.size(), extensions_cstr.data());

  vkGetDeviceQueue = (PFN_vkGetDeviceQueue)GetProc("vkGetDeviceQueue");
  vkGetDeviceQueue(device, graphics_queue_index, 0, &graphics_queue);
  vkGetDeviceQueue(device, graphics_queue_index, graphics_queue_count - 1, &background_queue);
  vkGetDeviceQueue(device, present_queue_index, 0, &present_queue);
}
void Device::Destroy() {
  if (device != VK_NULL_HANDLE) {
    vkDestroyDevice(device, allocation_callbacks);
  }
  device = VK_NULL_HANDLE;
  extensions = {};
  features = {};
}
void InitGrContext() {
  skgpu::VulkanBackendContext foreground_backend = {
      .fInstance = instance,
      .fPhysicalDevice = physical_device,
      .fDevice = device,
      .fQueue = device.graphics_queue,
      .fGraphicsQueueIndex = device.graphics_queue_index,
      .fMaxAPIVersion = kMinimumVulkanVersion,
      .fVkExtensions = &device.extensions,
      .fDeviceFeatures2 = &device.features,
      .fGetProc = GetProc,
  };
  skgpu::VulkanBackendContext background_backend = {
      .fInstance = instance,
      .fPhysicalDevice = physical_device,
      .fDevice = device,
      .fQueue = device.background_queue,
      .fGraphicsQueueIndex = device.graphics_queue_index,
      .fMaxAPIVersion = kMinimumVulkanVersion,
      .fVkExtensions = &device.extensions,
      .fDeviceFeatures2 = &device.features,
      .fGetProc = GetProc,
  };

  graphite::ContextOptions options{};

  graphite_context = graphite::ContextFactory::MakeVulkan(foreground_backend, options);
  background_context = graphite::ContextFactory::MakeVulkan(background_backend, options);
  graphite_recorder = graphite_context->makeRecorder();
}
static void InitInstanceFunctions() {
#define GET_INSTANCE_PROC(P) vk##P = (PFN_vk##P)instance.GetProc("vk" #P);
  EACH_INSTANCE_PROC(GET_INSTANCE_PROC);
#undef GET_INSTANCE_PROC
}

static void InitDeviceFunctions() {
#define GET_DEVICE_PROC(P) vk##P = (PFN_vk##P)device.GetProc("vk" #P);
  EACH_DEVICE_PROC(GET_DEVICE_PROC);
#undef GET_DEVICE_PROC
}

void CommandPool::Create(Status& status) {
  const VkCommandPoolCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .queueFamilyIndex = device.graphics_queue_index,
  };
  CHECK_RESULT(vkCreateCommandPool(device, &create_info, nullptr, &vk_command_pool));
}

void CommandPool::Destroy() {
  if (VK_NULL_HANDLE != vk_command_pool) {
    vkDestroyCommandPool(device, vk_command_pool, nullptr);
    vk_command_pool = VK_NULL_HANDLE;
  }
}

void Swapchain::DestroyBuffers() {
  canvases.clear();
  images.clear();
}

Str ToStr(VkResult res) {
  switch (res) {
    case VK_SUCCESS:
      return "VK_SUCCESS";
    case VK_NOT_READY:
      return "VK_NOT_READY";
    case VK_TIMEOUT:
      return "VK_TIMEOUT";
    case VK_EVENT_SET:
      return "VK_EVENT_SET";
    case VK_EVENT_RESET:
      return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
      return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
      return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
      return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
      return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
      return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
      return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
      return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
      return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
      return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
      return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
      return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:
      return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN:
      return "VK_ERROR_UNKNOWN";
    case VK_ERROR_OUT_OF_POOL_MEMORY:
      return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE:
      return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_FRAGMENTATION:
      return "VK_ERROR_FRAGMENTATION";
    case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS:
      return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
    case VK_PIPELINE_COMPILE_REQUIRED:
      return "VK_PIPELINE_COMPILE_REQUIRED";
    case VK_ERROR_NOT_PERMITTED:
      return "VK_ERROR_NOT_PERMITTED";
    case VK_ERROR_SURFACE_LOST_KHR:
      return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
      return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR:
      return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:
      return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:
      return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
    case VK_ERROR_VALIDATION_FAILED_EXT:
      return "VK_ERROR_VALIDATION_FAILED_EXT";
    case VK_ERROR_INVALID_SHADER_NV:
      return "VK_ERROR_INVALID_SHADER_NV";
    case VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR:
      return "VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR:
      return "VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR:
      return "VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR:
      return "VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR:
      return "VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR";
    case VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR:
      return "VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR";
    case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT:
      return "VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT";
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
      return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
    case VK_THREAD_IDLE_KHR:
      return "VK_THREAD_IDLE_KHR";
    case VK_THREAD_DONE_KHR:
      return "VK_THREAD_DONE_KHR";
    case VK_OPERATION_DEFERRED_KHR:
      return "VK_OPERATION_DEFERRED_KHR";
    case VK_OPERATION_NOT_DEFERRED_KHR:
      return "VK_OPERATION_NOT_DEFERRED_KHR";
    case VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR:
      return "VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR";
    case VK_ERROR_COMPRESSION_EXHAUSTED_EXT:
      return "VK_ERROR_COMPRESSION_EXHAUSTED_EXT";
    case VK_INCOMPATIBLE_SHADER_BINARY_EXT:
      return "VK_INCOMPATIBLE_SHADER_BINARY_EXT";
    case VK_PIPELINE_BINARY_MISSING_KHR:
      return "VK_PIPELINE_BINARY_MISSING_KHR";
    case VK_ERROR_NOT_ENOUGH_SPACE_KHR:
      return "VK_ERROR_NOT_ENOUGH_SPACE_KHR";
    default:
      return "Unknown error";
  }
}

struct StageAccess {
  VkPipelineStageFlags2 stage;
  VkAccessFlags2 access;

  StageAccess(VkImageLayout layout) {
    switch (layout) {
      case VK_IMAGE_LAYOUT_UNDEFINED:
        stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        access = VK_ACCESS_2_NONE;
        break;
      case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        break;
      case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        stage = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;
      case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT;
        access = VK_ACCESS_2_SHADER_READ_BIT;
        break;
      case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        access = VK_ACCESS_2_TRANSFER_READ_BIT;
        break;
      case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        break;
      case VK_IMAGE_LAYOUT_GENERAL:
        stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT |
                 VK_ACCESS_2_TRANSFER_WRITE_BIT;
        break;
      case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        access = VK_ACCESS_2_NONE | VK_ACCESS_2_SHADER_WRITE_BIT;
        break;
      default:
        ERROR_ONCE << "Unsupported image layout transition: " << layout;
        stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        break;
    }
  }
};

static void ImageMemBarrier(VkCommandBuffer cmd_buffer, VkImage image, VkFormat format,
                            VkImageLayout old_layout, VkImageLayout new_layout) {
  StageAccess src = StageAccess(old_layout);
  StageAccess dst = StageAccess(new_layout);

  const VkImageMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = src.stage,
      .srcAccessMask = src.access,
      .dstStageMask = dst.stage,
      .dstAccessMask = dst.access,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .baseMipLevel = 0,
              .levelCount = 1,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
  };

  const VkDependencyInfo depInfo{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &barrier,
  };

  vkCmdPipelineBarrier2(cmd_buffer, &depInfo);
}

void Swapchain::Create(int widthHint, int heightHint, Status& status) {
  sem_acquired.Create(status);
  if (!OK(status)) {
    return;
  }

  // check for capabilities
  VkSurfaceCapabilitiesKHR caps;
  CHECK_RESULT(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &caps));

  uint32_t surfaceFormatCount;
  CHECK_RESULT(
      vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &surfaceFormatCount, nullptr));

  VkSurfaceFormatKHR surfaceFormats[surfaceFormatCount];
  CHECK_RESULT(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &surfaceFormatCount,
                                                    surfaceFormats));

  uint32_t presentModeCount;
  CHECK_RESULT(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface,
                                                         &presentModeCount, nullptr));

  VkPresentModeKHR presentModes[presentModeCount];
  CHECK_RESULT(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface,
                                                         &presentModeCount, presentModes));

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

  uint32_t image_count = caps.minImageCount;
  if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
    // Application must settle for fewer images than desired:
    image_count = caps.maxImageCount;
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

  VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  SkAlphaType alpha_type = kUnknown_SkAlphaType;
  {  // Decide how our window will be composited with others
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) {
      composite_alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
      alpha_type = kPremul_SkAlphaType;
    } else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) {
      composite_alpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
      alpha_type = kUnpremul_SkAlphaType;
    } else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
      composite_alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
      alpha_type = kUnpremul_SkAlphaType;  // typically this should be the case
    } else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
      composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
      alpha_type = kOpaque_SkAlphaType;
    } else {
      ERROR_ONCE << "Window surface does not define any valid supportedCompositeAlpha value";
    }
  }

  int sample_count = std::max(1, cfg_MSAASampleCount);

  // Pick our surface format.
  VkFormat surfaceFormat = VK_FORMAT_UNDEFINED;
  VkColorSpaceKHR colorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
  for (uint32_t i = 0; i < surfaceFormatCount; ++i) {
    VkFormat format = surfaceFormats[i].format;
    surfaceFormat = format;
    colorSpace = surfaceFormats[i].colorSpace;
    break;
  }

  if (VK_FORMAT_UNDEFINED == surfaceFormat) {
    AppendErrorMessage(status) += "No supported surface format found";
    return;
  }

  SkColorType colorType;
  switch (surfaceFormat) {
    case VK_FORMAT_R8G8B8A8_UNORM:  // fall through
    case VK_FORMAT_R8G8B8A8_SRGB:
      colorType = kRGBA_8888_SkColorType;
      break;
    case VK_FORMAT_B8G8R8A8_UNORM:  // fall through
    case VK_FORMAT_B8G8R8A8_SRGB:
      colorType = kBGRA_8888_SkColorType;
      break;
    default:
      AppendErrorMessage(status) += "Unsupported surface format";
      return;
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
  swapchainCreateInfo.minImageCount = image_count;
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
  CHECK_RESULT(vkCreateSwapchainKHR(device, &swapchainCreateInfo, nullptr, &swapchain));

  // destroy the old swapchain
  if (swapchainCreateInfo.oldSwapchain != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(device);
    DestroyBuffers();
    vkDestroySwapchainKHR(device, swapchainCreateInfo.oldSwapchain, nullptr);
    swapchainCreateInfo.oldSwapchain = VK_NULL_HANDLE;
  }

  images.resize(image_count);
  vkGetSwapchainImagesKHR(device, swapchain, &image_count, images.data());

  static SkSurfaceProps surface_props(0, kRGB_H_SkPixelGeometry);
  static sk_sp<SkColorSpace> color_space = SkColorSpace::MakeSRGB();

  SkISize dimensions = {(int)extent.width, (int)extent.height};
  auto texture_info = graphite::VulkanTextureInfo(
      sample_count, skgpu::Mipmapped::kNo, 0, surfaceFormat, VK_IMAGE_TILING_OPTIMAL, usageFlags,
      swapchainCreateInfo.imageSharingMode, VK_IMAGE_ASPECT_COLOR_BIT,
      skgpu::VulkanYcbcrConversionInfo());

  std::vector<graphite::BackendTexture> canvas_textures;

  if (texture_info.fImageUsageFlags & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) {
    render_offscreen.reset();
    for (VkImage image : images) {
      auto backend_texture = graphite::BackendTextures::MakeVulkan(
          dimensions, texture_info, VK_IMAGE_LAYOUT_UNDEFINED, device.graphics_queue_index, image,
          skgpu::VulkanAlloc());
      if (!backend_texture.isValid()) {
        AppendErrorMessage(status) += "Failed wrapping swapchain image as graphite::BackendTexture";
        return;
      }
      canvas_textures.push_back(std::move(backend_texture));
    }
  } else {
    render_offscreen.emplace();
    auto surface = SkSurfaces::RenderTarget(
        graphite_recorder.get(), SkImageInfo::Make(dimensions, colorType, alpha_type, color_space));
    texture_info.fImageUsageFlags |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;

    const VkCommandBufferAllocateInfo command_buffer_allocate_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = command_pool.vk_command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = image_count,
    };

    auto texture_info2 = graphite::TextureInfos::MakeVulkan(texture_info);

    for (int i = 0; i < std::size(render_offscreen->backbuffers); ++i) {
      auto& backbuffer = render_offscreen->backbuffers[i];
      backbuffer.texture = graphite_recorder->createBackendTexture(dimensions, texture_info2);
      if (!backbuffer.texture.isValid()) {
        AppendErrorMessage(status) += "Failed creating backend texture for offscreen rendering";
        return;
      }
      backbuffer.vk_image = graphite::BackendTextures::GetVkImage(backbuffer.texture);

      backbuffer.vk_command_buffers.resize(image_count);

      CHECK_RESULT(vkAllocateCommandBuffers(device, &command_buffer_allocate_info,
                                            backbuffer.vk_command_buffers.data()));

      backbuffer.sem_copied.Create(status);
      if (!OK(status)) {
        AppendErrorMessage(status) += "Failed to create semaphore for offscreen rendering";
        return;
      }

      const VkImageCopy image_copy = {
          .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = 1},
          .srcOffset = {0, 0, 0},
          .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = 1},
          .dstOffset = {0, 0, 0},
          .extent = {.width = extent.width, .height = extent.height, .depth = 1}};

      for (int j = 0; j < image_count; ++j) {
        const static VkCommandBufferBeginInfo kCommandBufferBeginInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = 0,
            .pInheritanceInfo = nullptr};
        auto& cmd_buffer = backbuffer.vk_command_buffers[j];
        CHECK_RESULT(vkBeginCommandBuffer(cmd_buffer, &kCommandBufferBeginInfo));

        ImageMemBarrier(cmd_buffer, images[j], surfaceFormat, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        vkCmdCopyImage(cmd_buffer, backbuffer.vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       images[j], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_copy);

        ImageMemBarrier(cmd_buffer, images[j], surfaceFormat, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        CHECK_RESULT(vkEndCommandBuffer(cmd_buffer));
      }
      canvas_textures.push_back(backbuffer.texture);
    }
    render_offscreen->current = 0;
  }

  for (auto& backend_texture : canvas_textures) {
    auto sk_surface = SkSurfaces::WrapBackendTexture(graphite_recorder.get(), backend_texture,
                                                     colorType, color_space, &surface_props,
                                                     nullptr, nullptr, "backend_texture");
    if (!sk_surface) {
      AppendErrorMessage(status) += "SkSurfaces::WrapBackendTexture failed";
      return;
    }
    canvases.push_back({.sk_surface = std::move(sk_surface), .sem_rendered = Semaphore(status)});
  }

  if (!OK(status)) {
    vkDeviceWaitIdle(device);
    DestroyBuffers();
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
    return;
  }
}

SkCanvas* Swapchain::AcquireCanvas() {
  VkResult res = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, sem_acquired, VK_NULL_HANDLE,
                                       &current_image_index);
  if (VK_ERROR_SURFACE_LOST_KHR == res) {
    // need to figure out how to create a new vkSurface without the
    // platformData* maybe use attach somehow? but need a Window
    return nullptr;
  }
  if (VK_ERROR_OUT_OF_DATE_KHR == res) {
    // tear swapchain down and try again
    Status status;
    Create(-1, -1, status);
    if (!OK(status)) {
      return nullptr;
    }

    res = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, sem_acquired, VK_NULL_HANDLE,
                                &current_image_index);

    if (VK_SUCCESS != res) {
      return nullptr;
    }
  }

  int current_backbuffer_index = render_offscreen ? render_offscreen->current : current_image_index;

  return canvases[current_backbuffer_index].sk_surface->getCanvas();
}

void Swapchain::Present() {
  const int current_backbuffer_index =
      render_offscreen ? render_offscreen->current : current_image_index;
  auto& backbuffer = canvases[current_backbuffer_index];

  graphite::BackendSemaphore sk_sem_acquired = sem_acquired;
  graphite::BackendSemaphore sk_sem_rendered = backbuffer.sem_rendered;
  VkSemaphore* present_semaphore = &backbuffer.sem_rendered.vk_semaphore;

  if (auto recording = graphite_recorder->snap()) {
    graphite::InsertRecordingInfo insert_recording_info;
    insert_recording_info.fRecording = recording.get();

    skgpu::MutableTextureState target_texture_state;
    if (render_offscreen) {
      insert_recording_info.fWaitSemaphores = nullptr;
      insert_recording_info.fNumWaitSemaphores = 0;
      target_texture_state = skgpu::MutableTextureStates::MakeVulkan(
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, device.graphics_queue_index);
    } else {
      insert_recording_info.fWaitSemaphores = &sk_sem_acquired;
      insert_recording_info.fNumWaitSemaphores = 1;
      target_texture_state = skgpu::MutableTextureStates::MakeVulkan(
          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, device.present_queue_index);
    }
    insert_recording_info.fSignalSemaphores = &sk_sem_rendered;
    insert_recording_info.fNumSignalSemaphores = 1;
    insert_recording_info.fTargetTextureState = &target_texture_state;
    insert_recording_info.fTargetSurface = backbuffer.sk_surface.get();

    insert_recording_info.fFinishedContext = nullptr;
    insert_recording_info.fFinishedProc = nullptr;

    graphite_context->insertRecording(insert_recording_info);
    graphite_context->submit();
  }

  if (render_offscreen) {
    const VkSemaphoreSubmitInfo wait_semaphores[2] = {
        VkSemaphoreSubmitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                              .semaphore = sem_acquired},
        VkSemaphoreSubmitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                              .semaphore = backbuffer.sem_rendered},
    };
    auto& backbuffer = render_offscreen->backbuffers[render_offscreen->current];
    const VkCommandBufferSubmitInfo command_buffer_submit_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = backbuffer.vk_command_buffers[current_image_index],
    };
    const VkSemaphoreSubmitInfo signal_semaphore_submit_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = backbuffer.sem_copied,
    };
    const VkSubmitInfo2 submit_info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                                       .pNext = nullptr,
                                       .flags = 0,
                                       .waitSemaphoreInfoCount = 2,
                                       .pWaitSemaphoreInfos = wait_semaphores,
                                       .commandBufferInfoCount = 1,
                                       .pCommandBufferInfos = &command_buffer_submit_info,
                                       .signalSemaphoreInfoCount = 1,
                                       .pSignalSemaphoreInfos = &signal_semaphore_submit_info};

    vkQueueSubmit2(device.graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
    present_semaphore = &backbuffer.sem_copied.vk_semaphore;
  }

  const VkPresentInfoKHR presentInfo = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                        nullptr,
                                        1,
                                        present_semaphore,
                                        1,
                                        &swapchain,
                                        &current_image_index,
                                        nullptr};

  vkQueuePresentKHR(device.present_queue, &presentInfo);

  if (render_offscreen) {
    ++render_offscreen->current;
    if (render_offscreen->current >= canvases.size()) {
      render_offscreen->current = 0;
    }
  }
}
void Swapchain::WaitAndDestroy() {
  if (VK_NULL_HANDLE != swapchain) {
    vkQueueWaitIdle(device.present_queue);
    vkDeviceWaitIdle(device);
    DestroyBuffers();
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
  }
}

bool initialized = false;
void Init(Status& status) {
  initialized = true;
  instance.Init(status);
  if (!OK(status)) {
    AppendErrorMessage(status) += "Failed to create Vulkan instance";
    return;
  }

  InitInstanceFunctions();

  surface.Init(status);
  if (!OK(status)) {
    AppendErrorMessage(status) += "Failed to create Vulkan surface";
    return;
  }

  physical_device.Init(status);
  if (!OK(status)) {
    AppendErrorMessage(status) += "Failed to create Vulkan physical device";
    return;
  }

  device.Init(status);
  if (!OK(status)) {
    AppendErrorMessage(status) += "Failed to create Vulkan device";
    return;
  }

  InitDeviceFunctions();
  InitGrContext();

  command_pool.Create(status);
  if (!OK(status)) {
    AppendErrorMessage(status) += "Failed to create Vulkan command pool";
    return;
  }

  swapchain.Create(-1, -1, status);
  if (!OK(status)) {
    AppendErrorMessage(status) += "Failed to create Vulkan swapchain";
    return;
  }
}
void Destroy() {
  if (VK_NULL_HANDLE != device) {
    swapchain.WaitAndDestroy();
    surface.Destroy();
    command_pool.Destroy();
  }

  graphite_recorder.reset();
  graphite_context.reset();

  device.Destroy();
  physical_device.Destroy();
  instance.Destroy();
}

void Resize(int width_hint, int height_hint, Status& status) {
  if (!initialized) {
    AppendErrorMessage(status) += "vk::Resize called before initialization";
    return;
  }
  swapchain.Create(width_hint, height_hint, status);
  if (!OK(status)) {
    AppendErrorMessage(status) += "Couldn't create swapchain";
    return;
  }
}

SkCanvas* AcquireCanvas() { return swapchain.AcquireCanvas(); }

void Present() { swapchain.Present(); }

}  // namespace automat::vk