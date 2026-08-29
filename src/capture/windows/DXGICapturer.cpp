#include "capture/windows/DXGICapturer.h"

#if !defined(_WIN32)
#error "DXGICapturer is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <cstring>
#include <thread>

#include "capture/VideoFrame.h"
#include "core/TimeBase.h"

namespace nr {

namespace {

// Converts an HWND to the integer id used by the UI.
int hwndToId(HWND hwnd) {
    return static_cast<int>(reinterpret_cast<intptr_t>(hwnd));
}

HWND idToHwnd(int id) {
    return reinterpret_cast<HWND>(static_cast<intptr_t>(id));
}

struct EnumWindowsState {
    std::vector<WindowInfo>* out;
};

BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* state = reinterpret_cast<EnumWindowsState*>(lParam);
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect) || rect.right <= rect.left ||
        rect.bottom <= rect.top) {
        return TRUE;
    }
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width < 40 || height < 40) {
        return TRUE;  // skip trivial/minimized windows
    }
    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, 512);
    if (title[0] == L'\0') {
        return TRUE;  // skip windows without a title (desktop, tooltips)
    }
    wchar_t className[128] = {};
    GetClassNameW(hwnd, className, 128);
    if (std::wcsstr(className, L"Progman") || std::wcsstr(className, L"WorkerW") ||
        std::wcsstr(className, L"Shell_TrayWnd")) {
        return TRUE;
    }

    WindowInfo info;
    info.id = hwndToId(hwnd);
    info.title = std::string(title, title + std::wcslen(title));
    info.application = std::string(className, className + std::wcslen(className));
    info.width = width;
    info.height = height;
    state->out->push_back(std::move(info));
    return TRUE;
}

} // namespace

struct DXGICapturer::Impl {
    VideoConfig videoConfig;
    VideoFrameCallback onFrame;
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};
    std::thread captureThread;

    // DXGI state.
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    int desktopWidth = 0;
    int desktopHeight = 0;
    bool useDxgi = false;

    // GDI state.
    HDC memDC = nullptr;
    HBITMAP bitmap = nullptr;
    BITMAPINFO bmi{};
    void* gdiBits = nullptr;

    int64_t startUs = 0;

    void cleanupDxgi() {
        duplication.Reset();
        staging.Reset();
        context.Reset();
        device.Reset();
        useDxgi = false;
    }

    void cleanupGdi() {
        if (bitmap) {
            DeleteObject(bitmap);
            bitmap = nullptr;
        }
        if (memDC) {
            DeleteDC(memDC);
            memDC = nullptr;
        }
        gdiBits = nullptr;
    }

    bool setupDxgi() {
        cleanupDxgi();
        const D3D_DRIVER_TYPE driverTypes[] = {
            D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
        for (const D3D_DRIVER_TYPE type : driverTypes) {
            HRESULT hr = D3D11CreateDevice(
                nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
            if (SUCCEEDED(hr)) {
                break;
            }
        }
        if (!device || !context) {
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        if (FAILED(device.As(&dxgiDevice)) ||
            FAILED(dxgiDevice->GetAdapter(&adapter)) ||
            FAILED(adapter->EnumOutputs(0, &output))) {
            return false;
        }
        Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
        if (FAILED(output.As(&output1))) {
            return false;
        }
        if (FAILED(output1->DuplicateOutput(device.Get(), &duplication))) {
            return false;
        }

        DXGI_OUTDUPL_DESC desc{};
        duplication->GetDesc(&desc);
        desktopWidth = static_cast<int>(desc.ModeDesc.Width);
        desktopHeight = static_cast<int>(desc.ModeDesc.Height);
        if (desktopWidth <= 0 || desktopHeight <= 0) {
            return false;
        }

        D3D11_TEXTURE2D_DESC stagingDesc{};
        stagingDesc.Width = desktopWidth;
        stagingDesc.Height = desktopHeight;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
            return false;
        }
        useDxgi = true;
        return true;
    }

    bool setupGdi() {
        cleanupGdi();
        const int width = GetSystemMetrics(SM_CXSCREEN);
        const int height = GetSystemMetrics(SM_CYSCREEN);
        if (width <= 0 || height <= 0) {
            return false;
        }
        HDC screenDC = GetDC(nullptr);
        if (!screenDC) {
            return false;
        }
        memDC = CreateCompatibleDC(screenDC);
        if (!memDC) {
            ReleaseDC(nullptr, screenDC);
            return false;
        }
        std::memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;  // top-down rows
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &gdiBits,
                                  nullptr, 0);
        ReleaseDC(nullptr, screenDC);
        if (!bitmap || !gdiBits) {
            cleanupGdi();
            return false;
        }
        SelectObject(memDC, bitmap);
        desktopWidth = width;
        desktopHeight = height;
        return true;
    }

    bool grabRegion(const RECT& rect, VideoFrame& frame) {
        const int w = rect.right - rect.left;
        const int h = rect.bottom - rect.top;
        if (w <= 0 || h <= 0) {
            return false;
        }
        frame.width = w;
        frame.height = h;
        frame.bgra.resize(static_cast<size_t>(w) * h * 4);
        frame.stride = w * 4;

        if (useDxgi) {
            // Read the full desktop staging texture, then slice the region.
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                return false;
            }
            const auto* src = static_cast<const uint8_t*>(mapped.pData);
            const int rowPitch = static_cast<int>(mapped.RowPitch);
            for (int y = 0; y < h; ++y) {
                const int srcY = rect.top + y;
                if (srcY < 0 || srcY >= desktopHeight) {
                    continue;
                }
                const int srcX = rect.left;
                const int copyW = std::min(w, desktopWidth - srcX);
                if (copyW > 0) {
                    std::memcpy(
                        frame.bgra.data() + static_cast<size_t>(y) * frame.stride,
                        src + static_cast<size_t>(srcY) * rowPitch +
                            static_cast<size_t>(srcX) * 4,
                        static_cast<size_t>(copyW) * 4);
                }
            }
            context->Unmap(staging.Get(), 0);
            return true;
        }

        HDC screenDC = GetDC(nullptr);
        if (!screenDC) {
            return false;
        }
        const BOOL ok = BitBlt(memDC, 0, 0, w, h, screenDC, rect.left, rect.top,
                               SRCCOPY | CAPTUREBLT);
        ReleaseDC(nullptr, screenDC);
        if (!ok) {
            return false;
        }
        std::memcpy(frame.bgra.data(), gdiBits, static_cast<size_t>(w) * h * 4);
        return true;
    }

    void captureLoop() {
        startUs = TimeBase::now().count();
        const int fps = videoConfig.fps > 0 ? videoConfig.fps : 60;
        const auto frameInterval = std::chrono::microseconds(1'000'000 / fps);
        auto next = std::chrono::steady_clock::now();

        while (running.load()) {
            next += frameInterval;

            if (!paused.load() && onFrame) {
                RECT rect{};
                if (videoConfig.mode == CaptureMode::Window && videoConfig.windowId > 0) {
                    HWND hwnd = idToHwnd(videoConfig.windowId);
                    if (!IsWindow(hwnd) || !GetWindowRect(hwnd, &rect)) {
                        std::this_thread::sleep_until(next);
                        continue;
                    }
                } else if (videoConfig.mode == CaptureMode::Region &&
                           videoConfig.region.valid()) {
                    rect = {videoConfig.region.x, videoConfig.region.y,
                            videoConfig.region.x + videoConfig.region.width,
                            videoConfig.region.y + videoConfig.region.height};
                } else {
                    rect = {0, 0, desktopWidth, desktopHeight};
                }

                if (useDxgi) {
                    DXGI_OUTDUPL_FRAME_INFO info{};
                    Microsoft::WRL::ComPtr<IDXGIResource> resource;
                    const HRESULT hr =
                        duplication->AcquireNextFrame(100, &info, &resource);
                    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                        std::this_thread::sleep_until(next);
                        continue;
                    }
                    if (FAILED(hr)) {
                        // Duplication lost (desktop switch/lock). Recreate it.
                        duplication.Reset();
                        setupDxgi();
                        std::this_thread::sleep_until(next);
                        continue;
                    }
                    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
                    if (SUCCEEDED(resource.As(&texture))) {
                        context->CopyResource(staging.Get(), texture.Get());
                        VideoFrame frame;
                        frame.ptsUs = TimeBase::now().count() - startUs;
                        frame.durationUs = frameInterval.count();
                        if (grabRegion(rect, frame)) {
                            onFrame(std::move(frame));
                        }
                    }
                    duplication->ReleaseFrame();
                } else {
                    VideoFrame frame;
                    frame.ptsUs = TimeBase::now().count() - startUs;
                    frame.durationUs = frameInterval.count();
                    if (grabRegion(rect, frame)) {
                        onFrame(std::move(frame));
                    }
                }
            }

            const auto now = std::chrono::steady_clock::now();
            if (next > now) {
                std::this_thread::sleep_until(next);
            }
        }
    }
};

DXGICapturer::DXGICapturer() : impl_(std::make_unique<Impl>()) {}

DXGICapturer::~DXGICapturer() {
    stop();
}

bool DXGICapturer::start(const VideoConfig& videoConfig,
                         const AudioConfig& /*audioConfig*/,
                         VideoFrameCallback onFrame,
                         AudioFrameCallback /*onAudio*/) {
    if (!impl_) {
        return false;
    }
    stop();
    impl_->videoConfig = videoConfig;
    impl_->onFrame = std::move(onFrame);
    impl_->paused.store(false);

    // Prefer DXGI duplication; fall back to GDI (remote sessions, VMs).
    if (!impl_->setupDxgi() && !impl_->setupGdi()) {
        return false;
    }
    impl_->running.store(true);
    impl_->captureThread = std::thread([this] { impl_->captureLoop(); });
    return true;
}

void DXGICapturer::stop() {
    if (!impl_) {
        return;
    }
    impl_->running.store(false);
    if (impl_->captureThread.joinable()) {
        impl_->captureThread.join();
    }
    impl_->cleanupDxgi();
    impl_->cleanupGdi();
    impl_->onFrame = nullptr;
}

void DXGICapturer::setPaused(bool paused) {
    if (impl_) {
        impl_->paused.store(paused);
    }
}

std::vector<WindowInfo> DXGICapturer::listWindows() {
    std::vector<WindowInfo> windows;
    EnumWindowsState state{&windows};
    EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&state));
    return windows;
}

double DXGICapturer::displayScale() {
    return 1.0;
}

} // namespace nr
