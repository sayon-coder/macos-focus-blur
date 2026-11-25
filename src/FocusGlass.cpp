#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
    namespace wgc = winrt::Windows::Graphics::Capture;
    namespace wdx = winrt::Windows::Graphics::DirectX;
    namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

    constexpr wchar_t kAppClass[] = L"FocusGlass.HiddenHost.v0.1.3";
    constexpr wchar_t kOverlayClass[] = L"FocusGlass.DesktopOverlay.v0.1.3";
    constexpr UINT kTrayMessage = WM_APP + 10;
    constexpr UINT kNewFrameMessage = WM_APP + 11;
    constexpr UINT kRefreshMaskMessage = WM_APP + 12;
    constexpr UINT kErrorMessage = WM_APP + 13;
    constexpr UINT kHotkeyToggle = 1;
    constexpr UINT kHotkeyExit = 2;
    constexpr UINT kHotkeyIncreaseBlur = 3;
    constexpr UINT kHotkeyDecreaseBlur = 4;
    constexpr UINT kMenuToggle = 1001;
    constexpr UINT kMenuExit = 1002;
    constexpr UINT kMenuIncreaseBlur = 1003;
    constexpr UINT kMenuDecreaseBlur = 1004;
    constexpr UINT kWdaExcludeFromCapture = 0x00000011;
    constexpr int kTopSafeAreaPx = 30;       // Keeps the user's macOS-style top bar clear.
    constexpr int kMaxBlurWidth = 640;       // Blur is calculated at reduced resolution.
    constexpr int kMinBlurLevel = 1;
    constexpr int kMaxBlurLevel = 10;
    constexpr int kInitialBlurLevel = 4;     // Preserves the original radius-7 default.
    constexpr int kWindowShadowExtent = 36;
    constexpr int kWindowShadowVerticalOffset = 6;
    constexpr double kBackdropBrightness = 0.93;
    constexpr unsigned kMaxProcessedFps = 15;

    HWND g_hostWindow = nullptr;
    HWND g_overlayWindow = nullptr;
    constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\FocusGlass.SingleInstance.v1";

    std::filesystem::path LogPath()
    {
        wchar_t* localAppData = nullptr;
        size_t length = 0;
        std::filesystem::path result = L"FocusGlass.log";
        if (_wdupenv_s(&localAppData, &length, L"LOCALAPPDATA") == 0 && localAppData)
        {
            result = std::filesystem::path(localAppData) / L"FocusGlass" / L"FocusGlass.log";
            free(localAppData);
        }
        return result;
    }

    void Log(const std::wstring& message)
    {
        try
        {
            const auto path = LogPath();
            std::filesystem::create_directories(path.parent_path());
            std::wofstream stream(path, std::ios::app);
            if (stream)
            {
                SYSTEMTIME time{};
                GetLocalTime(&time);
                stream << L"[" << time.wYear << L"-" << time.wMonth << L"-" << time.wDay
                    << L" " << time.wHour << L":" << time.wMinute << L":" << time.wSecond
                    << L"] " << message << L"\n";
            }
        }
        catch (...) { }
    }

    bool RegisterAppHotKey(HWND window, UINT id, UINT modifiers, UINT virtualKey,
        const wchar_t* description)
    {
        if (RegisterHotKey(window, id, modifiers, virtualKey))
            return true;

        const DWORD error = GetLastError();
        Log(std::wstring(L"Could not register ") + description +
            L" (Win32 error " + std::to_wstring(error) + L").");
        return false;
    }

    struct Pixel
    {
        uint8_t b, g, r, a;
    };

    int BlurLevelToRadius(int level) noexcept
    {
        // Level 1 is subtle; level 10 is strong. Level 4 preserves the original radius 7.
        return 1 + (std::clamp(level, kMinBlurLevel, kMaxBlurLevel) - 1) * 2;
    }

    struct MonitorInfo
    {
        HMONITOR handle{};
        RECT bounds{};
        int width{};
        int height{};
    };

    MonitorInfo GetPrimaryMonitor()
    {
        MonitorInfo result{};
        result.handle = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{ sizeof(info) };
        if (!result.handle || !GetMonitorInfoW(result.handle, &info))
            throw std::runtime_error("Could not read primary monitor information.");
        result.bounds = info.rcMonitor;
        result.width = result.bounds.right - result.bounds.left;
        result.height = result.bounds.bottom - result.bounds.top;
        if (result.width <= 0 || result.height <= 0)
            throw std::runtime_error("Primary monitor has an invalid size.");
        return result;
    }

    void BoxBlurHorizontal(std::vector<Pixel>& pixels, int width, int height, int radius)
    {
        std::vector<Pixel> output(pixels.size());
        const int diameter = radius * 2 + 1;
        for (int y = 0; y < height; ++y)
        {
            const size_t row = static_cast<size_t>(y) * width;
            uint32_t sb = 0, sg = 0, sr = 0;
            for (int k = -radius; k <= radius; ++k)
            {
                const int x = std::clamp(k, 0, width - 1);
                const auto& p = pixels[row + x];
                sb += p.b; sg += p.g; sr += p.r;
            }
            for (int x = 0; x < width; ++x)
            {
                output[row + x] = Pixel{
                    static_cast<uint8_t>(sb / diameter),
                    static_cast<uint8_t>(sg / diameter),
                    static_cast<uint8_t>(sr / diameter), 255 };
                const int removeX = std::clamp(x - radius, 0, width - 1);
                const int addX = std::clamp(x + radius + 1, 0, width - 1);
                const auto& remove = pixels[row + removeX];
                const auto& add = pixels[row + addX];
                sb = sb - remove.b + add.b;
                sg = sg - remove.g + add.g;
                sr = sr - remove.r + add.r;
            }
        }
        pixels.swap(output);
    }

    void BoxBlurVertical(std::vector<Pixel>& pixels, int width, int height, int radius)
    {
        std::vector<Pixel> output(pixels.size());
        const int diameter = radius * 2 + 1;
        for (int x = 0; x < width; ++x)
        {
            uint32_t sb = 0, sg = 0, sr = 0;
            for (int k = -radius; k <= radius; ++k)
            {
                const int y = std::clamp(k, 0, height - 1);
                const auto& p = pixels[static_cast<size_t>(y) * width + x];
                sb += p.b; sg += p.g; sr += p.r;
            }
            for (int y = 0; y < height; ++y)
            {
                output[static_cast<size_t>(y) * width + x] = Pixel{
                    static_cast<uint8_t>(sb / diameter),
                    static_cast<uint8_t>(sg / diameter),
                    static_cast<uint8_t>(sr / diameter), 255 };
                const int removeY = std::clamp(y - radius, 0, height - 1);
                const int addY = std::clamp(y + radius + 1, 0, height - 1);
                const auto& remove = pixels[static_cast<size_t>(removeY) * width + x];
                const auto& add = pixels[static_cast<size_t>(addY) * width + x];
                sb = sb - remove.b + add.b;
                sg = sg - remove.g + add.g;
                sr = sr - remove.r + add.r;
            }
        }
        pixels.swap(output);
    }

    std::vector<Pixel> MakeBlurredPreview(const uint8_t* source, UINT sourcePitch,
        int sourceWidth, int sourceHeight, int blurRadius, int& smallWidth, int& smallHeight)
    {
        const double scale = std::min(1.0, static_cast<double>(kMaxBlurWidth) / sourceWidth);
        smallWidth = std::max(1, static_cast<int>(sourceWidth * scale));
        smallHeight = std::max(1, static_cast<int>(sourceHeight * scale));
        std::vector<Pixel> blurPixels(static_cast<size_t>(smallWidth) * smallHeight);

        // Area-average the source while downscaling to avoid aliasing before blur.
        for (int y = 0; y < smallHeight; ++y)
        {
            const int y0 = y * sourceHeight / smallHeight;
            const int y1 = std::max(y0 + 1, (y + 1) * sourceHeight / smallHeight);
            for (int x = 0; x < smallWidth; ++x)
            {
                const int x0 = x * sourceWidth / smallWidth;
                const int x1 = std::max(x0 + 1, (x + 1) * sourceWidth / smallWidth);
                uint64_t b = 0, g = 0, r = 0, count = 0;
                for (int sy = y0; sy < std::min(y1, sourceHeight); ++sy)
                {
                    const auto* row = source + static_cast<size_t>(sy) * sourcePitch;
                    for (int sx = x0; sx < std::min(x1, sourceWidth); ++sx)
                    {
                        const auto* p = row + static_cast<size_t>(sx) * 4;
                        b += p[0]; g += p[1]; r += p[2]; ++count;
                    }
                }
                blurPixels[static_cast<size_t>(y) * smallWidth + x] = Pixel{
                    static_cast<uint8_t>(b / std::max<uint64_t>(1, count)),
                    static_cast<uint8_t>(g / std::max<uint64_t>(1, count)),
                    static_cast<uint8_t>(r / std::max<uint64_t>(1, count)), 255 };
            }
        }

        // Three horizontal/vertical box passes approximate a Gaussian kernel.
        // Blur radius is read from the live user setting for every captured frame.
        const int radius = std::clamp(blurRadius, 1, 19);
        for (int pass = 0; pass < 3; ++pass)
        {
            BoxBlurHorizontal(blurPixels, smallWidth, smallHeight, radius);
            BoxBlurVertical(blurPixels, smallWidth, smallHeight, radius);
        }
        return blurPixels;
    }

    class CaptureEngine : public std::enable_shared_from_this<CaptureEngine>
    {
    public:
        explicit CaptureEngine(HWND notifyWindow, std::shared_ptr<std::atomic<int>> blurLevel)
            : m_notifyWindow(notifyWindow), m_blurLevel(std::move(blurLevel)) { }

        void Start(const MonitorInfo& monitor)
        {
            m_monitor = monitor;
            if (!wgc::GraphicsCaptureSession::IsSupported())
                throw std::runtime_error("Windows Graphics Capture is not available on this PC.");

            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
            D3D_FEATURE_LEVEL chosen{};
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                m_d3dDevice.put(), &chosen, m_d3dContext.put());
            if (FAILED(hr))
            {
                Log(L"Hardware D3D11 device creation failed; trying WARP fallback.");
                hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                    flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                    m_d3dDevice.put(), &chosen, m_d3dContext.put());
            }
            winrt::check_hresult(hr);

            winrt::com_ptr<IDXGIDevice> dxgiDevice;
            winrt::check_hresult(m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.put_void()));
            winrt::check_hresult(::CreateDirect3D11DeviceFromDXGIDevice(
                dxgiDevice.get(), reinterpret_cast<::IInspectable**>(winrt::put_abi(m_winrtDevice))));

            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
            winrt::check_hresult(interop->CreateForMonitor(monitor.handle,
                winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(m_item)));

            const auto size = m_item.Size();
            m_framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                m_winrtDevice, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
            m_session = m_framePool.CreateCaptureSession(m_item);
            m_session.IsCursorCaptureEnabled(false);
            // Available on newer Windows builds; avoids the capture border where supported.
            if (auto session3 = m_session.try_as<wgc::IGraphicsCaptureSession3>())
                session3.IsBorderRequired(false);

            std::weak_ptr<CaptureEngine> weak = shared_from_this();
            m_frameToken = m_framePool.FrameArrived([weak](const wgc::Direct3D11CaptureFramePool& sender,
                const winrt::Windows::Foundation::IInspectable&)
            {
                if (auto self = weak.lock()) self->OnFrameArrived(sender);
            });
            m_session.StartCapture();
            Log(L"Capture session started for primary monitor.");
        }

        void Stop() noexcept
        {
            m_stopping.store(true);
            try
            {
                if (m_framePool)
                    m_framePool.FrameArrived(m_frameToken);
                if (m_session) m_session.Close();
                if (m_framePool) m_framePool.Close();
            }
            catch (...) { }
            m_session = nullptr;
            m_framePool = nullptr;
        }

        bool GetLatest(std::vector<Pixel>& pixels, int& width, int& height)
        {
            std::lock_guard<std::mutex> lock(m_previewMutex);
            if (m_latestPreview.empty()) return false;
            pixels = m_latestPreview;
            width = m_previewWidth;
            height = m_previewHeight;
            return true;
        }

    private:
        void OnFrameArrived(const wgc::Direct3D11CaptureFramePool& sender)
        {
            if (m_stopping.load()) return;
            try
            {
                auto frame = sender.TryGetNextFrame();
                if (!frame) return;

                const auto now = std::chrono::steady_clock::now();
                if (m_lastProcessed.time_since_epoch().count() != 0 &&
                    now - m_lastProcessed < std::chrono::milliseconds(1000 / kMaxProcessedFps))
                    return;
                m_lastProcessed = now;

                auto surface = frame.Surface();
                auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                winrt::com_ptr<ID3D11Texture2D> texture;
                winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));

                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
                    throw std::runtime_error("Unexpected Windows Graphics Capture pixel format.");

                if (!m_staging || m_stagingWidth != static_cast<int>(desc.Width) ||
                    m_stagingHeight != static_cast<int>(desc.Height) || m_stagingFormat != desc.Format)
                {
                    m_staging = nullptr;
                    D3D11_TEXTURE2D_DESC stagingDesc = desc;
                    stagingDesc.Usage = D3D11_USAGE_STAGING;
                    stagingDesc.BindFlags = 0;
                    stagingDesc.MiscFlags = 0;
                    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    stagingDesc.ArraySize = 1;
                    stagingDesc.MipLevels = 1;
                    stagingDesc.SampleDesc.Count = 1;
                    stagingDesc.SampleDesc.Quality = 0;
                    winrt::check_hresult(m_d3dDevice->CreateTexture2D(&stagingDesc, nullptr, m_staging.put()));
                    m_stagingWidth = static_cast<int>(desc.Width);
                    m_stagingHeight = static_cast<int>(desc.Height);
                    m_stagingFormat = desc.Format;
                }

                m_d3dContext->CopyResource(m_staging.get(), texture.get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                winrt::check_hresult(m_d3dContext->Map(m_staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
                int smallWidth = 0, smallHeight = 0;
                const int blurRadius = BlurLevelToRadius(m_blurLevel->load(std::memory_order_relaxed));
                auto preview = MakeBlurredPreview(static_cast<const uint8_t*>(mapped.pData),
                    mapped.RowPitch, m_stagingWidth, m_stagingHeight, blurRadius, smallWidth, smallHeight);
                m_d3dContext->Unmap(m_staging.get(), 0);

                {
                    std::lock_guard<std::mutex> lock(m_previewMutex);
                    m_latestPreview.swap(preview);
                    m_previewWidth = smallWidth;
                    m_previewHeight = smallHeight;
                }
                PostMessageW(m_notifyWindow, kNewFrameMessage, 0, 0);
            }
            catch (const winrt::hresult_error& e)
            {
                Log(std::wstring(L"Capture callback failed: ") + e.message().c_str());
                PostMessageW(m_notifyWindow, kErrorMessage, 0, 0);
            }
            catch (const std::exception& e)
            {
                Log(std::wstring(L"Capture callback failed: ") + winrt::to_hstring(e.what()).c_str());
                PostMessageW(m_notifyWindow, kErrorMessage, 0, 0);
            }
            catch (...)
            {
                Log(L"Capture callback failed with an unknown error.");
                PostMessageW(m_notifyWindow, kErrorMessage, 0, 0);
            }
        }

        HWND m_notifyWindow{};
        std::shared_ptr<std::atomic<int>> m_blurLevel;
        MonitorInfo m_monitor{};
        std::atomic<bool> m_stopping{ false };
        std::chrono::steady_clock::time_point m_lastProcessed{};
        winrt::com_ptr<ID3D11Device> m_d3dDevice;
        winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
        winrt::com_ptr<ID3D11Texture2D> m_staging;
        int m_stagingWidth{};
        int m_stagingHeight{};
        DXGI_FORMAT m_stagingFormat{ DXGI_FORMAT_UNKNOWN };
        wd3d::IDirect3DDevice m_winrtDevice{ nullptr };
        wgc::GraphicsCaptureItem m_item{ nullptr };
        wgc::Direct3D11CaptureFramePool m_framePool{ nullptr };
        wgc::GraphicsCaptureSession m_session{ nullptr };
        winrt::event_token m_frameToken{};
        std::mutex m_previewMutex;
        std::vector<Pixel> m_latestPreview;
        int m_previewWidth{};
        int m_previewHeight{};
    };


    bool GetVisibleFrameBounds(HWND hwnd, RECT& rect)
    {
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))))
            return true;
        return GetWindowRect(hwnd, &rect) != FALSE;
    }

    bool IsVisibleOwnedPopup(HWND candidate, HWND foreground)
    {
        if (candidate == foreground || !IsWindowVisible(candidate) || IsIconic(candidate))
            return false;

        // Keep only popups owned by the foreground window (or one of its owned
        // popups) clear. Unrelated topmost/background windows stay behind the blur.
        HWND owner = GetWindow(candidate, GW_OWNER);
        for (int depth = 0; owner && depth < 16; ++depth)
        {
            if (owner == foreground) return true;
            owner = GetWindow(owner, GW_OWNER);
        }
        return false;
    }

    struct OwnedPopupContext
    {
        HWND foreground{};
        std::vector<RECT>* regions{};
    };

    BOOL CALLBACK EnumOwnedPopups(HWND hwnd, LPARAM parameter)
    {
        auto* context = reinterpret_cast<OwnedPopupContext*>(parameter);
        if (!IsVisibleOwnedPopup(hwnd, context->foreground))
            return TRUE;

        RECT rect{};
        if (GetVisibleFrameBounds(hwnd, rect))
            context->regions->push_back(rect);
        return TRUE;
    }

    std::vector<RECT> BuildClearRegions(HWND foreground, const RECT& foregroundRect,
        const MonitorInfo& monitor)
    {
        std::vector<RECT> regions;

        // Do NOT punch holes for every window above the foreground in z-order.
        // That was exposing unrelated background windows at the foreground edges.
        regions.push_back(foregroundRect);

        OwnedPopupContext popupContext{ foreground, &regions };
        EnumWindows(EnumOwnedPopups, reinterpret_cast<LPARAM>(&popupContext));

        // Keep the Windows taskbar and secondary taskbars crisp.
        struct TaskbarContext { std::vector<RECT>* regions; } taskbarContext{ &regions };
        EnumWindows([](HWND hwnd, LPARAM param) -> BOOL
        {
            wchar_t className[128]{};
            GetClassNameW(hwnd, className, ARRAYSIZE(className));
            if (wcscmp(className, L"Shell_TrayWnd") == 0 || wcscmp(className, L"Shell_SecondaryTrayWnd") == 0)
            {
                RECT r{};
                if (GetWindowRect(hwnd, &r))
                    reinterpret_cast<TaskbarContext*>(param)->regions->push_back(r);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&taskbarContext));

        // Preserve the user's custom macOS-style menu bar near the top edge.
        regions.push_back(RECT{ monitor.bounds.left, monitor.bounds.top,
            monitor.bounds.right, std::min(monitor.bounds.bottom, monitor.bounds.top + kTopSafeAreaPx) });
        return regions;
    }

    void ApplyWindowDropShadow(Pixel* output, int width, int height,
        const RECT& monitorRect, const RECT& windowRect)
    {
        // Precomputed tight + wide Gaussian falloff gives a medium, soft shadow.
        static const std::array<uint8_t, kWindowShadowExtent + 1> opacity = []
        {
            std::array<uint8_t, kWindowShadowExtent + 1> values{};
            for (int d = 0; d <= kWindowShadowExtent; ++d)
            {
                const double tight = 0.20 * std::exp(-0.5 * (d * d) / (6.5 * 6.5));
                const double wide = 0.15 * std::exp(-0.5 * (d * d) / (17.0 * 17.0));
                values[static_cast<size_t>(d)] = static_cast<uint8_t>(
                    std::clamp((tight + wide) * 255.0, 0.0, 255.0));
            }
            return values;
        }();

        RECT clipped = windowRect;
        clipped.left = std::max(clipped.left, monitorRect.left);
        clipped.top = std::max(clipped.top, monitorRect.top);
        clipped.right = std::min(clipped.right, monitorRect.right);
        clipped.bottom = std::min(clipped.bottom, monitorRect.bottom);
        if (clipped.right <= clipped.left || clipped.bottom <= clipped.top) return;

        RECT shadowShape = clipped;
        OffsetRect(&shadowShape, 0, kWindowShadowVerticalOffset);

        const int left = std::max(monitorRect.left, shadowShape.left - kWindowShadowExtent) - monitorRect.left;
        const int top = std::max(monitorRect.top, shadowShape.top - kWindowShadowExtent) - monitorRect.top;
        const int right = std::min(monitorRect.right, shadowShape.right + kWindowShadowExtent) - monitorRect.left;
        const int bottom = std::min(monitorRect.bottom, shadowShape.bottom + kWindowShadowExtent) - monitorRect.top;

        const int frameLeft = clipped.left - monitorRect.left;
        const int frameTop = clipped.top - monitorRect.top;
        const int frameRight = clipped.right - monitorRect.left;
        const int frameBottom = clipped.bottom - monitorRect.top;

        for (int y = std::max(0, top); y < std::min(height, bottom); ++y)
        {
            auto* row = output + static_cast<size_t>(y) * width;
            const int screenY = monitorRect.top + y;
            const int dy = std::max<int>({
                static_cast<int>(shadowShape.top) - screenY,
                0,
                screenY - (static_cast<int>(shadowShape.bottom) - 1)
            });
            for (int x = std::max(0, left); x < std::min(width, right); ++x)
            {
                if (x >= frameLeft && x < frameRight && y >= frameTop && y < frameBottom)
                    continue;

                const int screenX = monitorRect.left + x;
                const int dx = std::max<int>({
                    static_cast<int>(shadowShape.left) - screenX,
                    0,
                    screenX - (static_cast<int>(shadowShape.right) - 1)
                });
                const int distance = static_cast<int>(std::sqrt(static_cast<double>(dx * dx + dy * dy)));
                if (distance > kWindowShadowExtent) continue;

                const unsigned shade = opacity[static_cast<size_t>(distance)];
                if (shade == 0) continue;
                const unsigned keep = 255 - shade;
                auto& pixel = row[x];
                pixel.b = static_cast<uint8_t>((pixel.b * keep) / 255);
                pixel.g = static_cast<uint8_t>((pixel.g * keep) / 255);
                pixel.r = static_cast<uint8_t>((pixel.r * keep) / 255);
            }
        }
    }

    void ClearAlphaInRect(Pixel* output, int width, int height, const RECT& monitorRect, RECT rect)
    {
        rect.left = std::max(rect.left, monitorRect.left);
        rect.top = std::max(rect.top, monitorRect.top);
        rect.right = std::min(rect.right, monitorRect.right);
        rect.bottom = std::min(rect.bottom, monitorRect.bottom);
        if (rect.right <= rect.left || rect.bottom <= rect.top) return;
        const int x0 = rect.left - monitorRect.left;
        const int x1 = rect.right - monitorRect.left;
        const int y0 = rect.top - monitorRect.top;
        const int y1 = rect.bottom - monitorRect.top;
        for (int y = y0; y < y1 && y < height; ++y)
        {
            auto* row = output + static_cast<size_t>(y) * width;
            for (int x = x0; x < x1 && x < width; ++x)
            {
                row[x].b = row[x].g = row[x].r = 0;
                row[x].a = 0;
            }
        }
    }

    void ClearAlphaInRoundedRect(Pixel* output, int width, int height,
        const RECT& monitorRect, RECT rect, int cornerRadius)
    {
        rect.left = std::max(rect.left, monitorRect.left);
        rect.top = std::max(rect.top, monitorRect.top);
        rect.right = std::min(rect.right, monitorRect.right);
        rect.bottom = std::min(rect.bottom, monitorRect.bottom);
        if (rect.right <= rect.left || rect.bottom <= rect.top) return;

        const int x0 = rect.left - monitorRect.left;
        const int x1 = rect.right - monitorRect.left;
        const int y0 = rect.top - monitorRect.top;
        const int y1 = rect.bottom - monitorRect.top;
        const int radius = std::clamp(cornerRadius, 0, std::min(x1 - x0, y1 - y0) / 2);
        const int radiusSquared = radius * radius;

        for (int y = y0; y < y1 && y < height; ++y)
        {
            auto* row = output + static_cast<size_t>(y) * width;
            for (int x = x0; x < x1 && x < width; ++x)
            {
                int cornerDx = 0;
                int cornerDy = 0;
                if (radius > 0)
                {
                    if (x < x0 + radius) cornerDx = x0 + radius - 1 - x;
                    else if (x >= x1 - radius) cornerDx = x - (x1 - radius);
                    if (y < y0 + radius) cornerDy = y0 + radius - 1 - y;
                    else if (y >= y1 - radius) cornerDy = y - (y1 - radius);
                }
                if (cornerDx > 0 && cornerDy > 0 &&
                    cornerDx * cornerDx + cornerDy * cornerDy > radiusSquared)
                    continue;

                row[x].b = row[x].g = row[x].r = 0;
                row[x].a = 0;
            }
        }
    }

    class App
    {
    public:
        ~App() { Shutdown(); }

        int Run(HINSTANCE instance)
        {
            m_instance = instance;
            m_monitor = GetPrimaryMonitor();
            RegisterHostWindow();
            RegisterOverlayWindow();

            // A capture-excluded overlay is mandatory. If Windows refuses this, do not show it.
            if (!SetWindowDisplayAffinity(m_overlay, kWdaExcludeFromCapture))
                throw std::runtime_error("Windows could not exclude the overlay from screen capture. The effect was not started.");

            RegisterAppHotKey(m_host, kHotkeyToggle, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'B',
                L"Ctrl+Alt+B pause/resume hotkey");
            RegisterAppHotKey(m_host, kHotkeyExit, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q',
                L"Ctrl+Alt+Q exit hotkey");
            RegisterAppHotKey(m_host, kHotkeyIncreaseBlur, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_UP,
                L"Ctrl+Alt+Up blur-increase hotkey");
            RegisterAppHotKey(m_host, kHotkeyDecreaseBlur, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_DOWN,
                L"Ctrl+Alt+Down blur-decrease hotkey");

            m_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            m_locationHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

            m_capture = std::make_shared<CaptureEngine>(m_host, m_blurLevel);
            m_capture->Start(m_monitor);
            AddTrayIcon();
            m_enabled = true;
            ShowOverlay();
            Log(L"FocusGlass v0.1.3 started. A single-instance guard is active; blur level defaults to 4/10. Blur level defaults to 4/10; Ctrl+Alt+Up/Down adjusts it. Ctrl+Alt+B toggles; Ctrl+Alt+Q exits.");

            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Shutdown();
            return static_cast<int>(message.wParam);
        }

    private:
        static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
            LONG idObject, LONG idChild, DWORD, DWORD)
        {
            if (!g_hostWindow) return;
            if (event == EVENT_SYSTEM_FOREGROUND ||
                (event == EVENT_OBJECT_LOCATIONCHANGE && idObject == OBJID_WINDOW && idChild == CHILDID_SELF && hwnd == GetForegroundWindow()))
                PostMessageW(g_hostWindow, kRefreshMaskMessage, 0, 0);
        }

        static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (message == WM_NCCREATE)
            {
                auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
                self = static_cast<App*>(create->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);

            switch (message)
            {
            case WM_HOTKEY:
                if (wParam == kHotkeyToggle) self->Toggle();
                else if (wParam == kHotkeyExit) { self->m_exitRequested = true; DestroyWindow(hwnd); }
                else if (wParam == kHotkeyIncreaseBlur) self->AdjustBlur(+1);
                else if (wParam == kHotkeyDecreaseBlur) self->AdjustBlur(-1);
                return 0;
            case WM_COMMAND:
                if (LOWORD(wParam) == kMenuToggle) self->Toggle();
                else if (LOWORD(wParam) == kMenuExit) { self->m_exitRequested = true; DestroyWindow(hwnd); }
                else if (LOWORD(wParam) == kMenuIncreaseBlur) self->AdjustBlur(+1);
                else if (LOWORD(wParam) == kMenuDecreaseBlur) self->AdjustBlur(-1);
                return 0;
            case kTrayMessage:
                if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) self->ShowTrayMenu();
                else if (lParam == WM_LBUTTONUP) self->Toggle();
                return 0;
            case kNewFrameMessage:
            case kRefreshMaskMessage:
                self->RenderLatest();
                return 0;
            case kErrorMessage:
                self->HideOverlay();
                self->m_enabled = false;
                if (self->m_capture) self->m_capture->Stop();
                self->m_capture.reset();
                self->UpdateTrayTip(L"FocusGlass - stopped after capture error");
                return 0;
            case WM_DISPLAYCHANGE:
                // v0.1 deliberately stays on the primary monitor; restart is safer after topology changes.
                self->HideOverlay();
                self->m_enabled = false;
                if (self->m_capture) self->m_capture->Stop();
                self->m_capture.reset();
                Log(L"Display configuration changed. Restart FocusGlass to reinitialize capture safely.");
                self->UpdateTrayTip(L"FocusGlass - restart required after display change");
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        void RegisterHostWindow()
        {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc = HostWndProc;
            wc.hInstance = m_instance;
            wc.lpszClassName = kAppClass;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassExW(&wc);
            m_host = CreateWindowExW(0, kAppClass, L"FocusGlass host", WS_OVERLAPPED,
                0, 0, 0, 0, nullptr, nullptr, m_instance, this);
            if (!m_host) throw std::runtime_error("Could not create FocusGlass host window.");
            g_hostWindow = m_host;
        }

        void RegisterOverlayWindow()
        {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc = OverlayWndProc;
            wc.hInstance = m_instance;
            wc.lpszClassName = kOverlayClass;
            RegisterClassExW(&wc);

            const DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
            m_overlay = CreateWindowExW(exStyle, kOverlayClass, L"FocusGlass backdrop", WS_POPUP,
                m_monitor.bounds.left, m_monitor.bounds.top, m_monitor.width, m_monitor.height,
                nullptr, nullptr, m_instance, nullptr);
            if (!m_overlay) throw std::runtime_error("Could not create the desktop overlay window.");
            g_overlayWindow = m_overlay;

            m_memoryDc = CreateCompatibleDC(nullptr);
            if (!m_memoryDc) throw std::runtime_error("Could not create overlay drawing context.");
            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bmiHeader.biWidth = m_monitor.width;
            bitmapInfo.bmiHeader.biHeight = -m_monitor.height;
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;
            m_bitmap = CreateDIBSection(m_memoryDc, &bitmapInfo, DIB_RGB_COLORS,
                reinterpret_cast<void**>(&m_bitmapBits), nullptr, 0);
            if (!m_bitmap || !m_bitmapBits) throw std::runtime_error("Could not allocate overlay bitmap.");
            m_oldBitmap = SelectObject(m_memoryDc, m_bitmap);
            std::memset(m_bitmapBits, 0, static_cast<size_t>(m_monitor.width) * m_monitor.height * 4);
        }

        void AddTrayIcon()
        {
            m_tray = {};
            m_tray.cbSize = sizeof(m_tray);
            m_tray.hWnd = m_host;
            m_tray.uID = 1;
            m_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
            m_tray.uCallbackMessage = kTrayMessage;
            m_tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
            wcscpy_s(m_tray.szTip, L"FocusGlass - active (Ctrl+Alt+B to pause)");
            Shell_NotifyIconW(NIM_ADD, &m_tray);
        }

        void UpdateTrayTip(const wchar_t* tip)
        {
            if (!m_tray.hWnd) return;
            m_tray.uFlags = NIF_TIP;
            wcsncpy_s(m_tray.szTip, tip, _TRUNCATE);
            Shell_NotifyIconW(NIM_MODIFY, &m_tray);
        }

        void ShowTrayMenu()
        {
            POINT point{};
            GetCursorPos(&point);
            HMENU menu = CreatePopupMenu();
            if (!menu) return;
            const int level = m_blurLevel->load(std::memory_order_relaxed);
            const UINT increaseFlags = MF_STRING | (level >= kMaxBlurLevel ? MF_GRAYED : 0);
            const UINT decreaseFlags = MF_STRING | (level <= kMinBlurLevel ? MF_GRAYED : 0);
            AppendMenuW(menu, increaseFlags, kMenuIncreaseBlur, L"Increase blur  (Ctrl+Alt+Up)");
            AppendMenuW(menu, decreaseFlags, kMenuDecreaseBlur, L"Decrease blur  (Ctrl+Alt+Down)");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kMenuToggle, m_enabled ? L"Pause focus blur" : L"Resume focus blur");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit FocusGlass");
            HideOverlay();
            SetForegroundWindow(m_host);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, point.x, point.y, 0, m_host, nullptr);
            DestroyMenu(menu);
            if (m_enabled && !m_exitRequested) ShowOverlay();
        }

        void AdjustBlur(int delta)
        {
            const int current = m_blurLevel->load(std::memory_order_relaxed);
            const int next = std::clamp(current + delta, kMinBlurLevel, kMaxBlurLevel);
            if (next == current)
            {
                ShowBlurNotification(current >= kMaxBlurLevel
                    ? L"Maximum blur strength reached."
                    : L"Minimum blur strength reached.");
                return;
            }

            m_blurLevel->store(next, std::memory_order_relaxed);
            const std::wstring status = L"FocusGlass - " + std::to_wstring(next) +
                L"/10 blur (Ctrl+Alt+Up/Down)";
            UpdateTrayTip(status.c_str());

            const std::wstring message = L"Blur strength: " + std::to_wstring(next) + L"/10";
            ShowBlurNotification(message.c_str());
            Log(L"Blur strength changed to " + std::to_wstring(next) + L"/10.");
        }

        void ShowBlurNotification(const wchar_t* message)
        {
            if (!m_tray.hWnd) return;
            NOTIFYICONDATAW notification = m_tray;
            notification.uFlags = NIF_INFO;
            wcscpy_s(notification.szInfoTitle, L"FocusGlass");
            wcsncpy_s(notification.szInfo, message, _TRUNCATE);
            notification.dwInfoFlags = NIIF_INFO;
            notification.uTimeout = 1500;
            Shell_NotifyIconW(NIM_MODIFY, &notification);
        }

        void Toggle()
        {
            if (m_enabled)
            {
                m_enabled = false;
                HideOverlay();
                if (m_capture) m_capture->Stop();
                m_capture.reset();
                UpdateTrayTip(L"FocusGlass - paused (Ctrl+Alt+B to resume)");
                return;
            }

            try
            {
                m_capture = std::make_shared<CaptureEngine>(m_host, m_blurLevel);
                m_capture->Start(m_monitor);
                m_enabled = true;
                ShowOverlay();
                UpdateTrayTip(L"FocusGlass - active (Ctrl+Alt+B to pause)");
            }
            catch (const std::exception& e)
            {
                if (m_capture) m_capture->Stop();
                m_capture.reset();
                m_enabled = false;
                HideOverlay();
                Log(std::wstring(L"Could not resume capture: ") + winrt::to_hstring(e.what()).c_str());
                UpdateTrayTip(L"FocusGlass - could not resume; see log");
            }
            catch (...)
            {
                if (m_capture) m_capture->Stop();
                m_capture.reset();
                m_enabled = false;
                HideOverlay();
                Log(L"Could not resume capture due to an unknown error.");
                UpdateTrayTip(L"FocusGlass - could not resume; see log");
            }
        }

        void ShowOverlay()
        {
            if (!m_overlay) return;
            ShowWindow(m_overlay, SW_SHOWNOACTIVATE);
            SetWindowPos(m_overlay, HWND_TOPMOST, m_monitor.bounds.left, m_monitor.bounds.top,
                m_monitor.width, m_monitor.height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            RenderLatest();
        }

        void HideOverlay()
        {
            if (m_overlay) ShowWindow(m_overlay, SW_HIDE);
        }

        void RenderLatest()
        {
            if (!m_enabled || !m_capture || !m_overlay || !m_bitmapBits) return;
            std::vector<Pixel> preview;
            int previewWidth = 0, previewHeight = 0;
            if (!m_capture->GetLatest(preview, previewWidth, previewHeight)) return;

            auto* output = static_cast<Pixel*>(m_bitmapBits);
            const int width = m_monitor.width;
            const int height = m_monitor.height;

            // Bilinear upsample from the reduced-resolution blur buffer.
            for (int y = 0; y < height; ++y)
            {
                const double fy = (static_cast<double>(y) * (previewHeight - 1)) / std::max(1, height - 1);
                const int y0 = static_cast<int>(fy);
                const int y1 = std::min(y0 + 1, previewHeight - 1);
                const double wy = fy - y0;
                for (int x = 0; x < width; ++x)
                {
                    const double fx = (static_cast<double>(x) * (previewWidth - 1)) / std::max(1, width - 1);
                    const int x0 = static_cast<int>(fx);
                    const int x1 = std::min(x0 + 1, previewWidth - 1);
                    const double wx = fx - x0;
                    const Pixel& p00 = preview[static_cast<size_t>(y0) * previewWidth + x0];
                    const Pixel& p10 = preview[static_cast<size_t>(y0) * previewWidth + x1];
                    const Pixel& p01 = preview[static_cast<size_t>(y1) * previewWidth + x0];
                    const Pixel& p11 = preview[static_cast<size_t>(y1) * previewWidth + x1];
                    auto interpolate = [&](uint8_t Pixel::* channel) -> uint8_t
                    {
                        const double top = p00.*channel * (1.0 - wx) + p10.*channel * wx;
                        const double bottom = p01.*channel * (1.0 - wx) + p11.*channel * wx;
                        return static_cast<uint8_t>(std::clamp((top * (1.0 - wy) + bottom * wy) * kBackdropBrightness, 0.0, 255.0));
                    };
                    output[static_cast<size_t>(y) * width + x] = Pixel{
                        interpolate(&Pixel::b), interpolate(&Pixel::g), interpolate(&Pixel::r), 255 };
                }
            }

            HWND foreground = GetForegroundWindow();
            if (!foreground || foreground == m_host || foreground == m_overlay || foreground == GetShellWindow())
            {
                HideOverlay();
                return;
            }
            RECT foregroundRect{};
            if (!GetVisibleFrameBounds(foreground, foregroundRect) ||
                foregroundRect.right <= m_monitor.bounds.left || foregroundRect.left >= m_monitor.bounds.right ||
                foregroundRect.bottom <= m_monitor.bounds.top || foregroundRect.top >= m_monitor.bounds.bottom)
            {
                // v0.1 captures only the primary monitor; do not blur it when focus is on another display.
                HideOverlay();
                return;
            }

            // Draw a medium soft shadow over the blurred backdrop, then cut a rounded
            // transparent opening so the active foreground window remains sharp.
            ApplyWindowDropShadow(output, width, height, m_monitor.bounds, foregroundRect);

            auto regions = BuildClearRegions(foreground, foregroundRect, m_monitor);
            if (!regions.empty())
                ClearAlphaInRoundedRect(output, width, height, m_monitor.bounds, regions.front(), 10);
            for (size_t i = 1; i < regions.size(); ++i)
                ClearAlphaInRect(output, width, height, m_monitor.bounds, regions[i]);

            POINT destination{ m_monitor.bounds.left, m_monitor.bounds.top };
            POINT source{ 0, 0 };
            SIZE size{ width, height };
            BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            HDC screen = GetDC(nullptr);
            const BOOL updated = UpdateLayeredWindow(m_overlay, screen, &destination, &size,
                m_memoryDc, &source, 0, &blend, ULW_ALPHA);
            ReleaseDC(nullptr, screen);
            if (!updated)
            {
                Log(L"UpdateLayeredWindow failed; hiding overlay. Error=" + std::to_wstring(GetLastError()));
                HideOverlay();
                m_enabled = false;
                if (m_capture) m_capture->Stop();
                m_capture.reset();
                UpdateTrayTip(L"FocusGlass - paused after overlay error");
            }
            else
            {
                ShowWindow(m_overlay, SW_SHOWNOACTIVATE);
            }
        }

        void Shutdown() noexcept
        {
            if (m_shutdown) return;
            m_shutdown = true;
            if (m_capture) m_capture->Stop();
            m_capture.reset();
            HideOverlay();
            if (m_foregroundHook) UnhookWinEvent(m_foregroundHook);
            if (m_locationHook) UnhookWinEvent(m_locationHook);
            UnregisterHotKey(m_host, kHotkeyToggle);
            UnregisterHotKey(m_host, kHotkeyExit);
            UnregisterHotKey(m_host, kHotkeyIncreaseBlur);
            UnregisterHotKey(m_host, kHotkeyDecreaseBlur);
            if (m_tray.hWnd) Shell_NotifyIconW(NIM_DELETE, &m_tray);
            if (m_memoryDc && m_oldBitmap) SelectObject(m_memoryDc, m_oldBitmap);
            if (m_bitmap) DeleteObject(m_bitmap);
            if (m_memoryDc) DeleteDC(m_memoryDc);
            if (m_overlay) DestroyWindow(m_overlay);
            if (m_host) DestroyWindow(m_host);
            g_overlayWindow = nullptr;
            g_hostWindow = nullptr;
            Log(L"FocusGlass shut down cleanly.");
        }

        HINSTANCE m_instance{};
        HWND m_host{};
        HWND m_overlay{};
        MonitorInfo m_monitor{};
        HWINEVENTHOOK m_foregroundHook{};
        HWINEVENTHOOK m_locationHook{};
        NOTIFYICONDATAW m_tray{};
        HDC m_memoryDc{};
        HBITMAP m_bitmap{};
        HGDIOBJ m_oldBitmap{};
        void* m_bitmapBits{};
        bool m_enabled{ false };
        bool m_shutdown{ false };
        bool m_exitRequested{ false };
        std::shared_ptr<std::atomic<int>> m_blurLevel{
            std::make_shared<std::atomic<int>>(kInitialBlurLevel) };
        std::shared_ptr<CaptureEngine> m_capture;
    };
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    // Multiple copies create competing capture sessions and layered overlays.
    // Enforce a single process-wide instance before creating windows or hotkeys.
    SetLastError(ERROR_SUCCESS);
    HANDLE singletonMutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName);
    if (!singletonMutex)
    {
        const DWORD error = GetLastError();
        const std::wstring message = L"Could not create the FocusGlass single-instance guard. Win32 error " +
            std::to_wstring(error) + L".";
        MessageBoxW(nullptr, message.c_str(), L"FocusGlass startup error", MB_ICONERROR | MB_OK);
        return 1;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(singletonMutex);
        MessageBoxW(nullptr,
            L"FocusGlass is already running. Use the existing instance. If it is unresponsive, close all FocusGlass.exe processes in Task Manager and launch it again.",
            L"FocusGlass is already running", MB_ICONINFORMATION | MB_OK);
        return 0;
    }

    const auto closeMutex = [](HANDLE handle) { if (handle) CloseHandle(handle); };
    std::unique_ptr<void, decltype(closeMutex)> singletonGuard(singletonMutex, closeMutex);

    try
    {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        App app;
        return app.Run(instance);
    }
    catch (const winrt::hresult_error& e)
    {
        Log(std::wstring(L"Startup failed: ") + e.message().c_str());
        MessageBoxW(nullptr, e.message().c_str(), L"FocusGlass startup error", MB_ICONERROR | MB_OK);
        return 1;
    }
    catch (const std::exception& e)
    {
        const auto message = winrt::to_hstring(e.what());
        Log(std::wstring(L"Startup failed: ") + message.c_str());
        MessageBoxW(nullptr, message.c_str(), L"FocusGlass startup error", MB_ICONERROR | MB_OK);
        return 1;
    }
    catch (...)
    {
        Log(L"Startup failed with an unknown error.");
        MessageBoxW(nullptr, L"FocusGlass could not start. See the log in %LOCALAPPDATA%\\FocusGlass.",
            L"FocusGlass startup error", MB_ICONERROR | MB_OK);
        return 1;
    }
}
