#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include "GpuCompositor.h"

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

    constexpr wchar_t kAppClass[] = L"FocusGlass.HiddenHost";
    constexpr wchar_t kOverlayClass[] = L"FocusGlass.DesktopOverlay";
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
    constexpr int kMinBlurLevel = 1;
    constexpr int kMaxBlurLevel = 5;
    constexpr int kInitialBlurLevel = 1;

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

    class CaptureEngine : public std::enable_shared_from_this<CaptureEngine>
    {
    public:
        struct FrameSnapshot
        {
            wgc::Direct3D11CaptureFrame frame{ nullptr };
            winrt::com_ptr<ID3D11Texture2D> texture;
            UINT width{};
            UINT height{};

            ~FrameSnapshot()
            {
                if (frame)
                {
                    try { frame.Close(); }
                    catch (...) { }
                }
            }
        };

        explicit CaptureEngine(HWND notifyWindow) : m_notifyWindow(notifyWindow) { }

        void Start(const MonitorInfo& monitor)
        {
            m_monitor = monitor;
            m_stopping.store(false);
            m_frameMessagePending.store(false);

            if (!wgc::GraphicsCaptureSession::IsSupported())
                throw std::runtime_error("Windows Graphics Capture is not available on this PC.");

            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            D3D_FEATURE_LEVEL levels[] = {
                D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
            };
            D3D_FEATURE_LEVEL chosen{};
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                m_d3dDevice.put(), &chosen, m_d3dContext.put());
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
            if (auto session3 = m_session.try_as<wgc::IGraphicsCaptureSession3>())
                session3.IsBorderRequired(false);

            std::weak_ptr<CaptureEngine> weak = shared_from_this();
            m_frameToken = m_framePool.FrameArrived([weak](
                const wgc::Direct3D11CaptureFramePool& sender,
                const winrt::Windows::Foundation::IInspectable&)
            {
                if (auto self = weak.lock())
                    self->OnFrameArrived(sender);
            });

            m_session.StartCapture();
            Log(L"Windows Graphics Capture started; frames will stay on the GPU.");
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
            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                m_latestFrame.reset();
            }
            m_frameMessagePending.store(false);
            m_winrtDevice = nullptr;
            m_d3dContext = nullptr;
            m_d3dDevice = nullptr;
        }

        ID3D11Device* Device() const noexcept { return m_d3dDevice.get(); }
        ID3D11DeviceContext* Context() const noexcept { return m_d3dContext.get(); }

        bool GetLatestFrame(std::shared_ptr<FrameSnapshot>& frame)
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            frame = m_latestFrame;
            return frame && frame->texture.get() != nullptr;
        }

        void ClearFrameMessagePending() noexcept
        {
            m_frameMessagePending.store(false, std::memory_order_release);
        }

    private:
        void OnFrameArrived(const wgc::Direct3D11CaptureFramePool& sender)
        {
            if (m_stopping.load(std::memory_order_acquire))
                return;

            try
            {
                auto capturedFrame = sender.TryGetNextFrame();
                if (!capturedFrame)
                    return;

                auto snapshot = std::make_shared<FrameSnapshot>();
                snapshot->frame = capturedFrame;

                auto surface = capturedFrame.Surface();
                auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                winrt::check_hresult(access->GetInterface(
                    __uuidof(ID3D11Texture2D), snapshot->texture.put_void()));

                D3D11_TEXTURE2D_DESC desc{};
                snapshot->texture->GetDesc(&desc);
                if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
                    desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
                    throw std::runtime_error("Unexpected Windows Graphics Capture pixel format.");

                snapshot->width = desc.Width;
                snapshot->height = desc.Height;

                // Keep the WGC frame alive until CopyResource has consumed its texture.
                {
                    std::lock_guard<std::mutex> lock(m_frameMutex);
                    m_latestFrame = std::move(snapshot);
                }

                // Coalesce frame notifications instead of queuing stale frames behind the UI.
                if (!m_frameMessagePending.exchange(true, std::memory_order_acq_rel))
                {
                    if (!PostMessageW(m_notifyWindow, kNewFrameMessage, 0, 0))
                        m_frameMessagePending.store(false, std::memory_order_release);
                }
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
        MonitorInfo m_monitor{};
        std::atomic<bool> m_stopping{ false };
        std::atomic<bool> m_frameMessagePending{ false };
        winrt::com_ptr<ID3D11Device> m_d3dDevice;
        winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
        wd3d::IDirect3DDevice m_winrtDevice{ nullptr };
        wgc::GraphicsCaptureItem m_item{ nullptr };
        wgc::Direct3D11CaptureFramePool m_framePool{ nullptr };
        wgc::GraphicsCaptureSession m_session{ nullptr };
        winrt::event_token m_frameToken{};
        std::mutex m_frameMutex;
        std::shared_ptr<FrameSnapshot> m_latestFrame;
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

            m_capture = std::make_shared<CaptureEngine>(m_host);
            m_capture->Start(m_monitor);
            m_compositor = std::make_unique<focusglass::GpuCompositor>(
                m_capture->Device(), m_capture->Context(), m_overlay,
                m_monitor.width, m_monitor.height);
            AddTrayIcon();
            m_enabled = true;
            ShowOverlay();
            Log(L"FocusGlass v1.0.0 started. GPU Gaussian blur and DirectComposition are active; blur level defaults to 1/5.");

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
                if (self->m_capture) self->m_capture->ClearFrameMessagePending();
                self->RenderLatest();
                return 0;
            case kRefreshMaskMessage:
                self->RenderLatest();
                return 0;
            case kErrorMessage:
                self->HideOverlay();
                self->m_enabled = false;
                self->m_compositor.reset();
                if (self->m_capture) self->m_capture->Stop();
                self->m_capture.reset();
                self->UpdateTrayTip(L"FocusGlass - stopped after capture error");
                return 0;
            case WM_DISPLAYCHANGE:
                // v0.2 remains primary-monitor-only; restart after a display topology change.
                self->HideOverlay();
                self->m_enabled = false;
                self->m_compositor.reset();
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
            if (message == WM_NCHITTEST)
                return HTTRANSPARENT;
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

            // DirectComposition owns the visual surface; the HWND has no separate layered bitmap.
            // DirectComposition supports layered HWND targets. Layered + transparent
            // provides reliable click-through to background applications.
            const DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
            m_overlay = CreateWindowExW(exStyle, kOverlayClass, L"FocusGlass GPU backdrop", WS_POPUP,
                m_monitor.bounds.left, m_monitor.bounds.top, m_monitor.width, m_monitor.height,
                nullptr, nullptr, m_instance, nullptr);
            if (!m_overlay)
                throw std::runtime_error("Could not create the DirectComposition overlay window.");
            if (!SetLayeredWindowAttributes(m_overlay, 0, 255, LWA_ALPHA))
                throw std::runtime_error("Could not initialize the layered DirectComposition overlay.");
            g_overlayWindow = m_overlay;
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
                L"/5 blur (Ctrl+Alt+Up/Down)";
            UpdateTrayTip(status.c_str());

            const std::wstring message = L"Blur strength: " + std::to_wstring(next) + L"/5";
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
                m_compositor.reset();
                if (m_capture) m_capture->Stop();
                m_capture.reset();
                UpdateTrayTip(L"FocusGlass - paused (Ctrl+Alt+B to resume)");
                return;
            }

            try
            {
                m_capture = std::make_shared<CaptureEngine>(m_host);
                m_capture->Start(m_monitor);
                m_compositor = std::make_unique<focusglass::GpuCompositor>(
                    m_capture->Device(), m_capture->Context(), m_overlay,
                    m_monitor.width, m_monitor.height);
                m_enabled = true;
                ShowOverlay();
                UpdateTrayTip(L"FocusGlass - GPU blur active (Ctrl+Alt+B to pause)");
            }
            catch (const std::exception& e)
            {
                m_compositor.reset();
                if (m_capture) m_capture->Stop();
                m_capture.reset();
                m_enabled = false;
                HideOverlay();
                Log(std::wstring(L"Could not resume GPU blur: ") + winrt::to_hstring(e.what()).c_str());
                UpdateTrayTip(L"FocusGlass - could not resume; see log");
            }
            catch (...)
            {
                m_compositor.reset();
                if (m_capture) m_capture->Stop();
                m_capture.reset();
                m_enabled = false;
                HideOverlay();
                Log(L"Could not resume GPU blur due to an unknown error.");
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
            if (!m_enabled || !m_capture || !m_compositor || !m_overlay)
                return;

            std::shared_ptr<CaptureEngine::FrameSnapshot> frame;
            if (!m_capture->GetLatestFrame(frame) || !frame || frame->texture.get() == nullptr)
                return;

            HWND foreground = GetForegroundWindow();
            if (!foreground || foreground == m_host || foreground == m_overlay ||
                foreground == GetShellWindow())
            {
                HideOverlay();
                return;
            }

            RECT foregroundRect{};
            if (!GetVisibleFrameBounds(foreground, foregroundRect) ||
                foregroundRect.right <= m_monitor.bounds.left || foregroundRect.left >= m_monitor.bounds.right ||
                foregroundRect.bottom <= m_monitor.bounds.top || foregroundRect.top >= m_monitor.bounds.bottom)
            {
                // v0.2 remains primary-monitor-only. Never apply this overlay when focus is on another monitor.
                HideOverlay();
                return;
            }

            const auto clearRegions = BuildClearRegions(foreground, foregroundRect, m_monitor);
            try
            {
                // The retained WGC frame keeps the source texture valid until the GPU copy completes.
                m_compositor->Render(frame->texture.get(), m_monitor.bounds, clearRegions,
                    m_blurLevel->load(std::memory_order_relaxed));
                ShowWindow(m_overlay, SW_SHOWNOACTIVATE);
            }
            catch (const winrt::hresult_error& e)
            {
                Log(std::wstring(L"GPU compositor failed: ") + e.message().c_str());
                HideOverlay();
                m_enabled = false;
                m_compositor.reset();
                if (m_capture) m_capture->Stop();
                m_capture.reset();
                UpdateTrayTip(L"FocusGlass - stopped after GPU error; see log");
            }
            catch (const std::exception& e)
            {
                Log(std::wstring(L"GPU compositor failed: ") + winrt::to_hstring(e.what()).c_str());
                HideOverlay();
                m_enabled = false;
                m_compositor.reset();
                if (m_capture) m_capture->Stop();
                m_capture.reset();
                UpdateTrayTip(L"FocusGlass - stopped after GPU error; see log");
            }
        }

        void Shutdown() noexcept
        {
            if (m_shutdown) return;
            m_shutdown = true;
            m_compositor.reset();
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
        std::unique_ptr<focusglass::GpuCompositor> m_compositor;
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
