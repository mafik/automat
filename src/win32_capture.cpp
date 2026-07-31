// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "win32_capture.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <inspectable.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

#include <mutex>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")

#include "format.hpp"
#include "log.hpp"
#include "time.hpp"
#include "vk.hpp"
#include "win32.hpp"

namespace automat::win32_wm {

using Microsoft::WRL::ComPtr;
namespace capture = winrt::Windows::Graphics::Capture;
namespace direct3d = winrt::Windows::Graphics::DirectX::Direct3D11;
constexpr auto kPixelFormat = winrt::Windows::Graphics::DirectX::DirectXPixelFormat::
    B8G8R8A8UIntNormalized;

// The Direct3D device and every Skia import run under this. Frames arrive on
// system thread pool threads.
static std::mutex device_mutex;
static ComPtr<ID3D11Device> d3d_device;
static ComPtr<ID3D11DeviceContext> d3d_context;
static direct3d::IDirect3DDevice capture_device{nullptr};

static ComPtr<IDXGIAdapter1> AdapterVulkanUses() {
  uint8_t luid[8];
  if (!vk::AdapterLuid(luid)) return nullptr;
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc = {};
    if (FAILED(adapter->GetDesc1(&desc))) continue;
    if (memcmp(&desc.AdapterLuid, luid, sizeof(LUID)) == 0) return adapter;
  }
  return nullptr;
}

static bool EnsureDevice(Status& status) {
  if (d3d_device) return true;
  ComPtr<IDXGIAdapter1> adapter = AdapterVulkanUses();
  D3D_FEATURE_LEVEL level = {};
  HRESULT hr = D3D11CreateDevice(adapter.Get(),
                                 adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &d3d_device, &level, &d3d_context);
  if (FAILED(hr)) {
    AppendErrorMessage(status) += f("D3D11CreateDevice: {}", win32::ErrorStr(hr));
    return false;
  }
  ComPtr<ID3D10Multithread> multithread;
  if (SUCCEEDED(d3d_context.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
  ComPtr<IDXGIDevice> dxgi_device;
  winrt::com_ptr<::IInspectable> inspectable;
  if (FAILED(d3d_device.As(&dxgi_device)) ||
      FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), inspectable.put()))) {
    AppendErrorMessage(status) += "Could not hand the Direct3D device to the capture API.";
    d3d_device.Reset();
    d3d_context.Reset();
    return false;
  }
  capture_device = inspectable.as<direct3d::IDirect3DDevice>();
  return true;
}

void ReleaseCaptureDevice() {
  auto lock = std::lock_guard(device_mutex);
  capture_device = nullptr;
  d3d_context.Reset();
  d3d_device.Reset();
}

// A frame handler may be running on a pool thread while the board object drops
// its capture, so the session is kept alive through a shared pointer that the
// handler locks for the length of one frame.
struct Session {
  std::mutex mutex;
  std::weak_ptr<Session> self;
  std::function<void(sk_sp<SkImage>, SkISize)> on_frame;
  capture::GraphicsCaptureItem item{nullptr};
  capture::Direct3D11CaptureFramePool pool{nullptr};
  capture::GraphicsCaptureSession session{nullptr};
  winrt::event_token arrived_token{};
  bool closed = false;
  bool paused = false;

  ComPtr<ID3D11Texture2D> shared_texture;
  sk_sp<SkImage> image;
  SkISize size = {};
  SkISize pool_size = {};

  // A window that has already gone away makes these calls throw; the capture
  // is being torn down either way, so the failure is not worth reporting.
  void Stop() {
    try {
      if (pool) pool.FrameArrived(arrived_token);
      if (session) session.Close();
      if (pool) pool.Close();
    } catch (const winrt::hresult_error&) {
    }
    session = nullptr;
    pool = nullptr;
  }

  bool Open(Status& status) {
    try {
      auto wanted = size.isEmpty()
                        ? item.Size()
                        : winrt::Windows::Graphics::SizeInt32{size.width(), size.height()};
      pool_size = {wanted.Width, wanted.Height};
      pool = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(capture_device, kPixelFormat,
                                                                     2, wanted);
      std::weak_ptr<Session> weak = self;
      arrived_token = pool.FrameArrived([weak](auto&&, auto&&) {
        if (auto alive = weak.lock()) {
          try {
            alive->FrameArrived();
          } catch (const winrt::hresult_error& e) {
            ERROR_ONCE << "Window capture: " << win32::WideToUtf8(e.message());
          }
        }
      });
      session = pool.CreateCaptureSession(item);
      session.IsCursorCaptureEnabled(false);
      session.StartCapture();
      return true;
    } catch (const winrt::hresult_error& e) {
      AppendErrorMessage(status) += win32::WideToUtf8(e.message());
      Stop();
      return false;
    }
  }

  void Close() {
    auto lock = std::lock_guard(mutex);
    if (closed) return;
    closed = true;
    Stop();
    item = nullptr;
    auto device_lock = std::lock_guard(device_mutex);
    image.reset();
    shared_texture.Reset();
  }

  void Pause(bool wanted) {
    auto lock = std::lock_guard(mutex);
    if (closed || paused == wanted) return;
    paused = wanted;
    if (wanted) {
      Stop();
      return;
    }
    Status status;
    if (!Open(status)) {
      ERROR_ONCE << "Window capture: " << status.ToStr();
    }
  }

  bool Adopt(UINT width, UINT height) {
    auto device_lock = std::lock_guard(device_mutex);
    image.reset();
    shared_texture.Reset();
    size = {(int)width, (int)height};
    if (!d3d_device) return false;

    D3D11_TEXTURE2D_DESC desc = {
        .Width = width,
        .Height = height,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = {.Count = 1},
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
        .MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
    };
    if (HRESULT hr = d3d_device->CreateTexture2D(&desc, nullptr, &shared_texture); FAILED(hr)) {
      ERROR_ONCE << "Window capture: CreateTexture2D: " << win32::ErrorStr(hr);
      return false;
    }
    ComPtr<IDXGIResource1> resource;
    if (HRESULT hr = shared_texture.As(&resource); FAILED(hr)) {
      ERROR_ONCE << "Window capture: the texture cannot be shared: " << win32::ErrorStr(hr);
      return false;
    }
    HANDLE handle = nullptr;
    if (HRESULT hr = resource->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
        FAILED(hr)) {
      ERROR_ONCE << "Window capture: CreateSharedHandle: " << win32::ErrorStr(hr);
      return false;
    }
    Status status;
    image = vk::ImportSharedTexture(handle, (int)width, (int)height, status);
    CloseHandle(handle);
    if (!image) {
      ERROR_ONCE << "Window capture: " << status.ToStr();
      return false;
    }
    return true;
  }

  void FrameArrived() {
    sk_sp<SkImage> ready;
    SkISize ready_size;
    {
      auto lock = std::lock_guard(mutex);
      if (closed || !pool) return;
      auto frame = pool.TryGetNextFrame();
      if (!frame) return;
      auto surface = frame.Surface();
      auto access =
          surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
      ComPtr<ID3D11Texture2D> arrived;
      if (HRESULT hr = access->GetInterface(__uuidof(ID3D11Texture2D),
                                            (void**)arrived.GetAddressOf());
          FAILED(hr)) {
        ERROR_ONCE << "Window capture: the frame holds no texture: " << win32::ErrorStr(hr);
        return;
      }
      D3D11_TEXTURE2D_DESC desc = {};
      arrived->GetDesc(&desc);
      SkISize arrived_size = {(int)desc.Width, (int)desc.Height};
      if (size != arrived_size || !image) {
        if (!Adopt(desc.Width, desc.Height)) return;
      }
      if (pool_size != arrived_size) {
        pool_size = arrived_size;
        pool.Recreate(capture_device, kPixelFormat, 2,
                      {(int32_t)desc.Width, (int32_t)desc.Height});
      }
      {
        auto device_lock = std::lock_guard(device_mutex);
        if (!d3d_context) return;
        d3d_context->CopyResource(shared_texture.Get(), arrived.Get());
        d3d_context->Flush();
      }
      ready = image;
      ready_size = size;
    }
    if (on_frame) on_frame(std::move(ready), ready_size);
  }
};

struct SessionHandle : Capture {
  std::shared_ptr<Session> session;

  explicit SessionHandle(std::shared_ptr<Session> session) : session(std::move(session)) {}
  ~SessionHandle() override { session->Close(); }
  void Pause(bool wanted) override { session->Pause(wanted); }
};

std::unique_ptr<Capture> StartCapture(os::WindowHandle hwnd,
                                      std::function<void(sk_sp<SkImage>, SkISize)> on_frame,
                                      Status& status) {
  if (!capture::GraphicsCaptureSession::IsSupported()) {
    AppendErrorMessage(status) += "This system cannot capture windows.";
    return nullptr;
  }
  {
    auto lock = std::lock_guard(device_mutex);
    if (!EnsureDevice(status)) return nullptr;
  }
  auto session = std::make_shared<Session>();
  session->self = session;
  session->on_frame = std::move(on_frame);
  try {
    auto factory = winrt::get_activation_factory<capture::GraphicsCaptureItem>();
    auto interop = factory.as<IGraphicsCaptureItemInterop>();
    if (HRESULT hr = interop->CreateForWindow(hwnd, winrt::guid_of<capture::GraphicsCaptureItem>(),
                                              winrt::put_abi(session->item));
        FAILED(hr)) {
      AppendErrorMessage(status) += f("This window cannot be captured: {}", win32::ErrorStr(hr));
      return nullptr;
    }
  } catch (const winrt::hresult_error& e) {
    AppendErrorMessage(status) += f("Window capture: {}", win32::WideToUtf8(e.message()));
    return nullptr;
  }
  if (!session->Open(status)) return nullptr;
  return std::make_unique<SessionHandle>(std::move(session));
}

}  // namespace automat::win32_wm
