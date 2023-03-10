#include "win_main.h"

#include "VkTestUtils.h"
#include "backtrace.h"
#include "log.h"


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

vkb::Instance vkb_instance;

sk_sp<GrDirectContext> fContext;
sk_sp<const skgpu::VulkanInterface> fInterface;
int fWidth;
int fHeight;
double fRotationAngle = 0;

int fMSAASampleCount = 1;
bool fDisableVsync = false;
sk_sp<SkColorSpace> fColorSpace = nullptr;
SkSurfaceProps fSurfaceProps(0, kRGB_H_SkPixelGeometry);

int fSampleCount = 1;
int fStencilBits = 0;

VkInstance fInstance = VK_NULL_HANDLE;
VkPhysicalDevice fPhysicalDevice = VK_NULL_HANDLE;
VkDevice fDevice = VK_NULL_HANDLE;
VkDebugReportCallbackEXT fDebugCallback = VK_NULL_HANDLE;

VkSurfaceKHR fSurface;
VkSwapchainKHR fSwapchain;
uint32_t fGraphicsQueueIndex;
VkQueue fGraphicsQueue;
uint32_t fPresentQueueIndex;
VkQueue fPresentQueue;

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

GrContextOptions fGrContextOptions;

PFN_vkDestroySurfaceKHR fDestroySurfaceKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceSupportKHR fGetPhysicalDeviceSurfaceSupportKHR =
    nullptr;
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR
    fGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceFormatsKHR fGetPhysicalDeviceSurfaceFormatsKHR =
    nullptr;
PFN_vkGetPhysicalDeviceSurfacePresentModesKHR
    fGetPhysicalDeviceSurfacePresentModesKHR = nullptr;

PFN_vkCreateSwapchainKHR fCreateSwapchainKHR = nullptr;
PFN_vkDestroySwapchainKHR fDestroySwapchainKHR = nullptr;
PFN_vkGetSwapchainImagesKHR fGetSwapchainImagesKHR = nullptr;
PFN_vkAcquireNextImageKHR fAcquireNextImageKHR = nullptr;
PFN_vkQueuePresentKHR fQueuePresentKHR = nullptr;

PFN_vkDestroyInstance fDestroyInstance = nullptr;
PFN_vkDeviceWaitIdle fDeviceWaitIdle = nullptr;
PFN_vkDestroyDebugReportCallbackEXT fDestroyDebugReportCallbackEXT = nullptr;
PFN_vkQueueWaitIdle fQueueWaitIdle = nullptr;
PFN_vkDestroyDevice fDestroyDevice = nullptr;
PFN_vkGetDeviceQueue fGetDeviceQueue = nullptr;

GrVkBackendContext backendContext;
skgpu::VulkanExtensions extensions;

void destroyBuffers() {
  if (fBackbuffers) {
    for (uint32_t i = 0; i < fImageCount + 1; ++i) {
      fBackbuffers[i].fImageIndex = -1;
      GR_VK_CALL(
          fInterface,
          DestroySemaphore(fDevice, fBackbuffers[i].fRenderSemaphore, nullptr));
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
  if (VK_NULL_HANDLE != fDevice) {
    fQueueWaitIdle(fPresentQueue);
    fDeviceWaitIdle(fDevice);

    destroyBuffers();

    if (VK_NULL_HANDLE != fSwapchain) {
      fDestroySwapchainKHR(fDevice, fSwapchain, nullptr);
      fSwapchain = VK_NULL_HANDLE;
    }

    if (VK_NULL_HANDLE != fSurface) {
      fDestroySurfaceKHR(fInstance, fSurface, nullptr);
      fSurface = VK_NULL_HANDLE;
    }
  }

  SkASSERT(fContext->unique());
  fContext.reset();
  fInterface.reset();

  if (VK_NULL_HANDLE != fDevice) {
    fDestroyDevice(fDevice, nullptr);
    fDevice = VK_NULL_HANDLE;
  }

#if defined(SK_ENABLE_VK_LAYERS)
  if (fDebugCallback != VK_NULL_HANDLE) {
    fDestroyDebugReportCallbackEXT(fInstance, fDebugCallback, nullptr);
  }
#endif

  fPhysicalDevice = VK_NULL_HANDLE;

  if (VK_NULL_HANDLE != fInstance) {
    fDestroyInstance(fInstance, nullptr);
    fInstance = VK_NULL_HANDLE;
  }
}

bool createBuffers(VkFormat format, VkImageUsageFlags usageFlags,
                   SkColorType colorType, VkSharingMode sharingMode) {
  fGetSwapchainImagesKHR(fDevice, fSwapchain, &fImageCount, nullptr);
  SkASSERT(fImageCount);
  fImages = new VkImage[fImageCount];
  fGetSwapchainImagesKHR(fDevice, fSwapchain, &fImageCount, fImages);

  // set up initial image layouts and create surfaces
  fImageLayouts = new VkImageLayout[fImageCount];
  fSurfaces = new sk_sp<SkSurface>[fImageCount];
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
    info.fCurrentQueueFamily = fPresentQueueIndex;
    info.fSharingMode = sharingMode;

    if (usageFlags & VK_IMAGE_USAGE_SAMPLED_BIT) {
      GrBackendTexture backendTexture(fWidth, fHeight, info);
      fSurfaces[i] = SkSurface::MakeFromBackendTexture(
          fContext.get(), backendTexture, kTopLeft_GrSurfaceOrigin,
          fMSAASampleCount, colorType, fColorSpace, &fSurfaceProps);
    } else {
      if (fMSAASampleCount > 1) {
        return false;
      }
      GrBackendRenderTarget backendRT(fWidth, fHeight, fSampleCount, info);
      fSurfaces[i] = SkSurface::MakeFromBackendRenderTarget(
          fContext.get(), backendRT, kTopLeft_GrSurfaceOrigin, colorType,
          fColorSpace, &fSurfaceProps);
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
    SkDEBUGCODE(VkResult result =) GR_VK_CALL(
        fInterface, CreateSemaphore(fDevice, &semaphoreInfo, nullptr,
                                    &fBackbuffers[i].fRenderSemaphore));
    SkASSERT(result == VK_SUCCESS);
  }
  fCurrentBackbufferIndex = fImageCount;
  return true;
}

bool createSwapchain(int width, int height) {
  // check for capabilities
  VkSurfaceCapabilitiesKHR caps;
  if (fGetPhysicalDeviceSurfaceCapabilitiesKHR(fPhysicalDevice, fSurface,
                                               &caps)) {
    return false;
  }

  uint32_t surfaceFormatCount;
  if (fGetPhysicalDeviceSurfaceFormatsKHR(fPhysicalDevice, fSurface,
                                          &surfaceFormatCount, nullptr)) {
    return false;
  }

  VkSurfaceFormatKHR surfaceFormats[surfaceFormatCount];
  if (fGetPhysicalDeviceSurfaceFormatsKHR(
          fPhysicalDevice, fSurface, &surfaceFormatCount, surfaceFormats)) {
    return false;
  }

  uint32_t presentModeCount;
  if (fGetPhysicalDeviceSurfacePresentModesKHR(fPhysicalDevice, fSurface,
                                               &presentModeCount, nullptr)) {
    return false;
  }

  VkPresentModeKHR presentModes[presentModeCount];
  if (fGetPhysicalDeviceSurfacePresentModesKHR(
          fPhysicalDevice, fSurface, &presentModeCount, presentModes)) {
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
  swapchainCreateInfo.surface = fSurface;
  swapchainCreateInfo.minImageCount = imageCount;
  swapchainCreateInfo.imageFormat = surfaceFormat;
  swapchainCreateInfo.imageColorSpace = colorSpace;
  swapchainCreateInfo.imageExtent = extent;
  swapchainCreateInfo.imageArrayLayers = 1;
  swapchainCreateInfo.imageUsage = usageFlags;

  uint32_t queueFamilies[] = {fGraphicsQueueIndex, fPresentQueueIndex};
  if (fGraphicsQueueIndex != fPresentQueueIndex) {
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
  if (fCreateSwapchainKHR(fDevice, &swapchainCreateInfo, nullptr,
                          &fSwapchain)) {
    return false;
  }

  // destroy the old swapchain
  if (swapchainCreateInfo.oldSwapchain != VK_NULL_HANDLE) {
    fDeviceWaitIdle(fDevice);

    destroyBuffers();

    fDestroySwapchainKHR(fDevice, swapchainCreateInfo.oldSwapchain, nullptr);
  }

  if (!createBuffers(swapchainCreateInfo.imageFormat, usageFlags, colorType,
                     swapchainCreateInfo.imageSharingMode)) {
    fDeviceWaitIdle(fDevice);

    destroyBuffers();

    fDestroySwapchainKHR(fDevice, swapchainCreateInfo.oldSwapchain, nullptr);
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
  SkDEBUGCODE(VkResult result =)
      GR_VK_CALL(fInterface,
                 CreateSemaphore(fDevice, &semaphoreInfo, nullptr, &semaphore));
  SkASSERT(result == VK_SUCCESS);

  // acquire the image
  VkResult res =
      fAcquireNextImageKHR(fDevice, fSwapchain, UINT64_MAX, semaphore,
                           VK_NULL_HANDLE, &backbuffer->fImageIndex);
  if (VK_ERROR_SURFACE_LOST_KHR == res) {
    // need to figure out how to create a new vkSurface without the
    // platformData* maybe use attach somehow? but need a Window
    GR_VK_CALL(fInterface, DestroySemaphore(fDevice, semaphore, nullptr));
    return nullptr;
  }
  if (VK_ERROR_OUT_OF_DATE_KHR == res) {
    // tear swapchain down and try again
    if (!createSwapchain(-1, -1)) {
      GR_VK_CALL(fInterface, DestroySemaphore(fDevice, semaphore, nullptr));
      return nullptr;
    }
    backbuffer = getAvailableBackbuffer();

    // acquire the image
    res = fAcquireNextImageKHR(fDevice, fSwapchain, UINT64_MAX, semaphore,
                               VK_NULL_HANDLE, &backbuffer->fImageIndex);

    if (VK_SUCCESS != res) {
      GR_VK_CALL(fInterface, DestroySemaphore(fDevice, semaphore, nullptr));
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
                                          fPresentQueueIndex);
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

  fQueuePresentKHR(fPresentQueue, &presentInfo);
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

static wchar_t szWindowClass[] = L"Automaton";
static wchar_t kWindowTitle[] = L"Automaton";

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    PWSTR pCmdLine, int nCmdShow) {
  EnableBacktraceOnSIGSEGV();
  SetConsoleOutputCP(CP_UTF8); // utf-8
  SkGraphics::Init();
  WNDCLASSEX wcex = {.cbSize = sizeof(WNDCLASSEX),
                     .style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC,
                     .lpfnWndProc = WndProc,
                     .cbClsExtra = 0,
                     .cbWndExtra = 0,
                     .hInstance = hInstance,
                     .hIcon = LoadIcon(hInstance, (LPCTSTR)IDI_WINLOGO),
                     .hCursor = LoadCursor(nullptr, IDC_ARROW),
                     .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1),
                     .lpszMenuName = nullptr,
                     .lpszClassName = szWindowClass,
                     .hIconSm = LoadIcon(hInstance, (LPCTSTR)IDI_WINLOGO)};

  if (!RegisterClassEx(&wcex)) {
    FATAL() << "Call to RegisterClassEx failed!";
  }

  HWND hWnd =
      CreateWindowEx(WS_EX_OVERLAPPEDWINDOW, szWindowClass, L"Automaton",
                     WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800,
                     600, nullptr, nullptr, hInstance, nullptr);
  if (!hWnd) {
    FATAL() << "Call to CreateWindowEx failed!";
  }

  vkb::InstanceBuilder instance_builder;
  auto instance_builder_return =
      instance_builder.set_minimum_instance_version(1, 1, 0).build();
  if (!instance_builder_return) {
    FATAL() << "Failed to create Vulkan instance. Error: "
            << instance_builder_return.error().message();
  }
  vkb_instance = instance_builder_return.value();
  fInstance = vkb_instance.instance;

  PFN_vkGetInstanceProcAddr instProc = vkb_instance.fp_vkGetInstanceProcAddr;

  static PFN_vkCreateWin32SurfaceKHR createWin32SurfaceKHR = nullptr;
  if (!createWin32SurfaceKHR) {
    createWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)instProc(
        fInstance, "vkCreateWin32SurfaceKHR");
  }

  VkWin32SurfaceCreateInfoKHR surfaceCreateInfo = {
      .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
      .pNext = nullptr,
      .flags = 0,
      .hinstance = hInstance,
      .hwnd = hWnd};

  VkResult res =
      createWin32SurfaceKHR(fInstance, &surfaceCreateInfo, nullptr, &fSurface);
  if (VK_SUCCESS != res) {
    vkb::destroy_instance(vkb_instance);
    FATAL() << "Failure in createWin32SurfaceKHR.";
  }

  if (VK_NULL_HANDLE == fSurface) {
    vkb::destroy_instance(vkb_instance);
    FATAL() << "No surface after createWin32SurfaceKHR";
  }

  auto canPresent = [instProc](VkInstance instance, VkPhysicalDevice physDev,
                               uint32_t queueFamilyIndex) {
    static PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR
        getPhysicalDeviceWin32PresentationSupportKHR = nullptr;
    if (!getPhysicalDeviceWin32PresentationSupportKHR) {
      getPhysicalDeviceWin32PresentationSupportKHR =
          (PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR)instProc(
              instance, "vkGetPhysicalDeviceWin32PresentationSupportKHR");
    }

    VkBool32 check =
        getPhysicalDeviceWin32PresentationSupportKHR(physDev, queueFamilyIndex);
    return (VK_FALSE != check);
  };

  backendContext.fInstance = fInstance;

  vkb::PhysicalDeviceSelector phys_device_selector(vkb_instance);
  auto physical_device_selector_return =
      phys_device_selector.set_surface(fSurface)
          .add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
          .add_required_extension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)
          .prefer_gpu_device_type()
          .select();
  if (!physical_device_selector_return) {
    vkb::destroy_surface(vkb_instance, fSurface);
    FATAL() << "Failed to select Vulkan PhysicalDevice. Error: "
            << physical_device_selector_return.error().message();
  }
  auto phys_device = physical_device_selector_return.value();

  fPhysicalDevice = backendContext.fPhysicalDevice =
      phys_device.physical_device;

  vkb::DeviceBuilder device_builder{phys_device};
  auto dev_ret = device_builder.build();
  if (!dev_ret) {
    vkb::destroy_surface(vkb_instance, fSurface);
    FATAL() << "Failed to select Vulkan Device. Error: "
            << dev_ret.error().message();
  }
  vkb::Device vkb_device = dev_ret.value();

  fDevice = backendContext.fDevice = vkb_device.device;

  backendContext.fGetProc = [](const char *proc_name, VkInstance instance,
                               VkDevice device) {
    if (device != VK_NULL_HANDLE) {
      return vkb_instance.fp_vkGetDeviceProcAddr(device, proc_name);
    }
    return vkb_instance.fp_vkGetInstanceProcAddr(instance, proc_name);
  };

  auto queue_ret = vkb_device.get_queue(vkb::QueueType::graphics);
  if (!queue_ret) {
    vkb::destroy_device(vkb_device);
    vkb::destroy_surface(vkb_instance, fSurface);
    FATAL() << "Failed to get Vulkan Graphics Queue. Error: "
            << queue_ret.error().message();
  }
  fGraphicsQueue = backendContext.fQueue = queue_ret.value();

  auto queue_index_ret = vkb_device.get_queue_index(vkb::QueueType::graphics);
  if (!queue_index_ret) {
    vkb::destroy_device(vkb_device);
    vkb::destroy_surface(vkb_instance, fSurface);
    FATAL() << "Failed to get Vulkan Graphics Queue Index. Error: "
            << queue_index_ret.error().message();
  }
  fGraphicsQueueIndex = backendContext.fGraphicsQueueIndex =
      queue_index_ret.value();

  // TODO: set all fields of backendContext - same as CreateVkBackendContext
  // backendContext.fMaxAPIVersion = phys_device.

  PFN_vkGetPhysicalDeviceProperties localGetPhysicalDeviceProperties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
          backendContext.fGetProc("vkGetPhysicalDeviceProperties",
                                  backendContext.fInstance, VK_NULL_HANDLE));
  if (!localGetPhysicalDeviceProperties) {
    vkb::destroy_device(vkb_device);
    vkb::destroy_surface(vkb_instance, fSurface);
    return 4;
  }
  VkPhysicalDeviceProperties physDeviceProperties;
  localGetPhysicalDeviceProperties(backendContext.fPhysicalDevice,
                                   &physDeviceProperties);
  uint32_t physDevVersion = physDeviceProperties.apiVersion;

  fInterface.reset(new skgpu::VulkanInterface(
      backendContext.fGetProc, fInstance, fDevice,
      backendContext.fInstanceVersion, physDevVersion, &extensions));

#define GET_PROC(F)                                                            \
  f##F = (PFN_vk##F)backendContext.fGetProc("vk" #F, fInstance, VK_NULL_HANDLE)
#define GET_DEV_PROC(F)                                                        \
  f##F = (PFN_vk##F)backendContext.fGetProc("vk" #F, VK_NULL_HANDLE, fDevice)

  GET_PROC(DestroyInstance);
  if (fDebugCallback != VK_NULL_HANDLE) {
    GET_PROC(DestroyDebugReportCallbackEXT);
  }
  GET_PROC(DestroySurfaceKHR);
  GET_PROC(GetPhysicalDeviceSurfaceSupportKHR);
  GET_PROC(GetPhysicalDeviceSurfaceCapabilitiesKHR);
  GET_PROC(GetPhysicalDeviceSurfaceFormatsKHR);
  GET_PROC(GetPhysicalDeviceSurfacePresentModesKHR);
  GET_DEV_PROC(DeviceWaitIdle);
  GET_DEV_PROC(QueueWaitIdle);
  GET_DEV_PROC(DestroyDevice);
  GET_DEV_PROC(CreateSwapchainKHR);
  GET_DEV_PROC(DestroySwapchainKHR);
  GET_DEV_PROC(GetSwapchainImagesKHR);
  GET_DEV_PROC(AcquireNextImageKHR);
  GET_DEV_PROC(QueuePresentKHR);
  GET_DEV_PROC(GetDeviceQueue);

#undef GET_PROC
#undef GET_DEV_PROC

  fContext = GrDirectContext::MakeVulkan(backendContext, fGrContextOptions);

  VkBool32 supported;
  res = fGetPhysicalDeviceSurfaceSupportKHR(fPhysicalDevice, fPresentQueueIndex,
                                            fSurface, &supported);
  if (VK_SUCCESS != res) {
    destroyContext();
    return 6;
  }

  if (!createSwapchain(-1, -1)) {
    destroyContext();
    return 7;
  }

  // create presentQueue
  fGetDeviceQueue(fDevice, fPresentQueueIndex, 0, &fPresentQueue);

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

  vkb::destroy_instance(vkb_instance);
  return (int)msg.wParam;
}
