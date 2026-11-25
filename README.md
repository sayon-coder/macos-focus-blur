# FocusGlass v1.0.0 — Direct3D 11 GPU focus blur

FocusGlass is an early native Windows prototype for the macOS-style focus effect: the foreground application stays sharp while the rest of the primary display is blurred.

> **Product version:** v1.0.0. Bug fixes do not increment this version. **Validation:** The Windows x64 C++ build passes in GitHub Actions. GPU shaders compile dynamically at application startup; the latest source fixes a shader parser error, but successful GPU rendering and runtime stability still require verification on the target desktop.

## Current implementation

- Captures the primary monitor with Windows Graphics Capture and retains the captured Direct3D texture on the GPU.
- Runs a 3×3 downsample prefilter and separable horizontal/vertical Gaussian pixel-shader passes on a reduced-resolution texture (maximum blur width: 640 px).
- Uses DirectComposition with a premultiplied-alpha composition swap chain for the full-screen overlay.
- Composites the blurred backdrop, foreground cutout, exclusions and shadow entirely on the GPU; there is no CPU pixel readback or GDI full-screen bitmap.
- Clears only the foreground window and its owned popups, so unrelated topmost/background windows remain blurred to the screen edges.
- Uses a rounded-corner foreground cutout, preserves taskbars and the top 30 px safe area (for the custom menu bar), and adds a medium soft drop shadow around the active window.
- Uses a layered DirectComposition target with click-through window styles so background applications remain interactive.
- Excludes the overlay from screen capture. If this cannot be enabled, FocusGlass aborts rather than risking recursive capture.
- Provides a tray icon and global keyboard shortcuts.

## Controls

| Action | Shortcut |
|---|---|
| Pause / resume | Ctrl + Alt + B |
| Exit | Ctrl + Alt + Q |
| Increase blur strength | Ctrl + Alt + Up |
| Decrease blur strength | Ctrl + Alt + Down |
| Tray icon left-click | Pause / resume |
| Tray icon right-click | Open menu |

Blur strength has 5 levels (1–5). Level 1 is the default and provides the lightest blur; levels above 5 are not exposed because they made the backdrop unnecessarily dark. Each press adjusts one level; the new radius is applied on the next captured frame. A Windows notification reports the current level. Blur settings currently reset to level 1 when FocusGlass is restarted. You can also right-click the tray icon and choose Increase blur or Decrease blur.

FocusGlass enforces a **single running instance** using a named Windows mutex. Launching a second copy displays an “already running” message and exits rather than creating another capture overlay. Hotkey registration failures are logged with the Windows error code.

Logs are written to `%LOCALAPPDATA%\FocusGlass\FocusGlass.log`.

## Requirements

- Windows 10 version 2004 (build 19041) or later, or Windows 11.
- Visual Studio 2022 Build Tools with the **Desktop development with C++** workload.
- MSVC v143 x64/x86 build tools.
- Windows 10/11 SDK 10.0.20348.0 or newer (the build script selects the newest suitable SDK).
- C++/WinRT projection headers included with the Windows SDK. No separate NuGet restore is required.

The application is native C++/WinRT and does **not** require the .NET Desktop Runtime. Release builds statically link the MSVC C/C++ runtime (`/MT`), so the compiled Release executable does not require Visual Studio Build Tools or a separately installed Visual C++ Redistributable. Build Tools are required only to compile new versions, not to launch an already-built Release executable. Windows system libraries and the supported Windows version are still required. Static linking increases the executable size and means FocusGlass must be rebuilt and redistributed to pick up future MSVC runtime security fixes.

## Build

1. Clone this repository.
2. Open **Developer PowerShell for VS 2022**.
3. Change to the repository directory.
4. Run:

   ```powershell
   Set-ExecutionPolicy -Scope Process Bypass -Force
   .\build.ps1
   ```

5. The executable is generated at `artifacts\x64\Release\FocusGlass.exe`.

You can also open `FocusGlass.sln` and build the `Release | x64` configuration in Visual Studio.

## Build verification

The user-provided build log confirms a successful v0.1.1 Release x64 build using:

- Visual Studio 2022 Build Tools / MSBuild 17.14.60
- MSVC v143 toolset 14.44.35207
- Windows SDK 10.0.26100.0

Output: `artifacts\x64\Release\FocusGlass.exe`.

This verifies compilation of v0.1.1 only. The v0.1.4 mask correction, drop shadow, and static-runtime configuration must be rebuilt and tested on Windows before use.

## Known limitations

- Primary monitor only. A display-topology change hides the overlay and requires restart.
- CPU-based blur after Direct3D capture/readback; downsampled and capped at 15 FPS. Performance must be measured on real hardware.
- No settings window yet. Blur intensity is adjustable with the global shortcuts or tray menu; shadow radius/strength, brightness, top safe-area height, and frame cap remain source constants.
- Focus transitions are immediate; fades are not implemented.
- The foreground cutout is rectangular. Rounded corners, multi-monitor support, and per-app exclusions are future work.
- Protected video, secure desktop/UAC prompts, lock screen, and some elevated or exclusive-fullscreen apps may not capture normally.
- Windows may show a capture border; this unpackaged proof of concept does not yet request borderless capture permission.
- The blur is a three-pass box-blur approximation, not yet a Direct3D Gaussian shader.
- Continuously changing backgrounds may show motion lag due to the 15 FPS processing cap; the active foreground window itself remains live through the transparent opening.

## First-run safety

1. Save your work and close games or protected-video playback.
2. Run `artifacts\x64\Release\FocusGlass.exe`.
3. Test with ordinary windows such as File Explorer and Notepad.
4. Press **Ctrl + Alt + B** immediately if the overlay looks wrong.
5. Press **Ctrl + Alt + Q** to exit completely.

If the process becomes unresponsive, end `FocusGlass.exe` from Task Manager; its overlay disappears when the process exits.

## Technical references

- [Windows Graphics Capture overview](https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture)
- [Create a capture item for a monitor](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createformonitor)
- [Free-threaded capture frame pool](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded)
- [Exclude a window from capture](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity)
- [C++/WinRT SDK projection headers](https://learn.microsoft.com/en-us/windows/uwp/cpp-and-winrt-apis/intro-to-using-cpp-with-winrt)

## Roadmap

1. Validate no flicker, recursive capture, and correct foreground cutout on the target PC.
2. Measure CPU, GPU, memory, and latency on the GTX 1650 laptop.
3. Move blur passes to a Direct3D 11 shader if CPU usage or frame pacing requires it.
4. Add multi-monitor support, settings, exclusions, and installer packaging.
5. Evaluate a Windhawk Tool Mod wrapper only after the standalone engine is stable.


## v0.1.4 changes

- Background windows no longer get cut out just because they appear above the active window in global z-order.
- Only the foreground window, its owned popups, taskbars, and configured top safe area are kept clear.
- Added a medium, offset, two-scale Gaussian-style drop shadow over the blurred backdrop.
- The foreground cutout uses rounded corners.
- Release configuration uses the statically linked MSVC runtime (/MT) to make the executable portable without Visual Studio Build Tools or a separately installed VC++ Redistributable.
- No installer or auto-start registration is added in this version.


## v0.1.5 build fix

- Fixed MSVC compilation errors in the shadow distance calculations by explicitly converting Win32 `LONG` frame coordinates to `int` before calling `std::max<int>`.
- No rendering behaviour or shadow tuning was intentionally changed in this patch.
- The v0.1.5 changes need a fresh Windows Release build and runtime test.


## Windows CI build result

The v0.2.0 Release x64 native C++ build passed in GitHub Actions on Windows Server 2022 using Visual Studio 2022 Build Tools, MSVC v143 and Windows SDK 10.0.26100.0. The workflow uploaded `FocusGlass-v0.2.0-windows-x64` as an Actions artifact.

The CI build does not launch the GUI, compile the embedded HLSL at runtime, or test DirectComposition, the layered-window click-through behavior, GPU driver interaction, frame pacing or visual output. Those checks remain for the target Windows laptop.

## v0.2.0 GPU architecture and verification

The GPU renderer uses a small number of full-screen D3D11 passes:
1. GPU copy from the WGC frame texture into a compositor-owned shader-resource texture.
2. A 3×3 GPU prefilter/downsample into a texture no wider than 640 px.
3. A horizontal Gaussian shader pass.
4. A vertical Gaussian shader pass.
5. A final composite shader that outputs the blur plus premultiplied alpha for the active-window cutout, owned popups, taskbar/top-bar clear regions and medium shadow.
6. Presentation through an IDXGISwapChain1 composition swap chain hosted by DirectComposition.

Release builds remain /MT for the MSVC runtime, but the executable still depends on Windows graphics components including Direct3D 11, DXGI, DirectComposition, Windows Graphics Capture, and the system shader compiler component. Visual Studio Build Tools are required to compile, not to launch the Release EXE.

**Do not assume this version is faster or stable until the Windows CI build and the target-laptop tests pass.** The GTX 1650 supports D3D11, but actual frame pacing, CPU/GPU utilization, overlay hit-testing, transparency, and capture-exclusion behavior are runtime validation items.
