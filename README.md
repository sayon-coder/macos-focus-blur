# FocusGlass v0.1.3 — Windows focus-blur proof of concept

FocusGlass is an early native Windows prototype for the macOS-style focus effect: the foreground application stays sharp while the rest of the primary display is blurred.

> **Status:** Source-code proof of concept. v0.1.1 has successfully compiled on the target Windows PC in Release x64. Runtime blur behaviour has not yet been validated; treat this build as experimental.

## Current implementation

- Captures the primary monitor with Windows Graphics Capture.
- Copies frames through Direct3D 11, then creates a reduced-resolution blur buffer (maximum width: 640 px).
- Applies three horizontal/vertical box-blur passes on the CPU, capped at approximately 15 processed frames per second.
- Displays the blurred image through a click-through layered overlay.
- Clears only the foreground window and its owned popups, so unrelated topmost/background windows remain blurred to the screen edges.
- Uses a rounded-corner foreground cutout, preserves taskbars and the top 30 px safe area (for the custom menu bar), and adds a medium soft drop shadow around the active window.
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

Blur strength has 10 levels. Level 4 is the default and preserves the original v0.1.1 blur radius. Each press adjusts one level; the new radius is applied on the next captured frame. A Windows notification reports the current level. Blur settings currently reset to level 4 when FocusGlass is restarted. You can also right-click the tray icon and choose Increase blur or Decrease blur.

FocusGlass enforces a **single running instance** using a named Windows mutex. Launching a second copy displays an “already running” message and exits rather than creating another capture overlay. Hotkey registration failures are logged with the Windows error code.

Logs are written to `%LOCALAPPDATA%\FocusGlass\FocusGlass.log`.

## Requirements

- Windows 10 version 2004 (build 19041) or later, or Windows 11.
- Visual Studio 2022 Build Tools with the **Desktop development with C++** workload.
- MSVC v143 x64/x86 build tools.
- Windows 10/11 SDK 10.0.20348.0 or newer (the build script selects the newest suitable SDK).
- C++/WinRT projection headers included with the Windows SDK. No separate NuGet restore is required.

The application is native C++/WinRT and does **not** require the .NET Desktop Runtime. Release builds statically link the MSVC C/C++ runtime (`/MT`), so the compiled Release executable does not require Visual Studio Build Tools or a separately installed Visual C++ Redistributable. Build Tools are required only to compile new versions. Windows system libraries and the supported Windows version are still required.

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
