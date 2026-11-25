# FocusGlass v0.1.1 patch notes

This source update addresses the compiler diagnostics reported from the first Windows build of v0.1-fixed.

- Removed duplicate WIN32_LEAN_AND_MEAN and NOMINMAX definitions; the project file already defines them.
- Replaced ambiguous using namespace winrt / using namespace Windows imports with explicit C++/WinRT namespace aliases.
- Qualified C++/WinRT helper APIs (com_ptr, check_hresult, put_abi, guid_of, get_activation_factory, etc.).
- Explicitly global-qualified CreateDirect3D11DeviceFromDXGIDevice.
- Renamed the blur buffer variable small to blurPixels to avoid collision with the legacy Windows RPC small typedef.
- Corrected SetWindowDisplayAffinity to receive the class member m_overlay rather than the undeclared g_overlay.
- Removed a constant-condition warning for the fixed top safe-area region.

The user's Windows build log confirms that this v0.1.1 source compiled successfully in Release x64 with MSBuild 17.14.60, MSVC v143 14.44.35207, and Windows SDK 10.0.26100.0. Runtime behavior has not yet been tested.


# FocusGlass v0.1.2 — Adjustable blur controls

- Added ten runtime blur-strength levels.
- Added **Ctrl + Alt + Up** to increase blur.
- Added **Ctrl + Alt + Down** to decrease blur.
- Added Increase blur / Decrease blur entries to the notification-area tray menu.
- Added a Windows notification showing the selected blur strength.
- Level 4 maps to the previous radius-7 default, so the default appearance should remain close to v0.1.1.
- Blur strength changes are read by the capture callback on the next processed frame.
- The selected level currently resets to 4 when the app restarts; persistent settings and a graphical settings window remain future work.

The v0.1.2 source changes have not yet been compiled on Windows. The user must rebuild and test before treating the shortcuts as verified.


# FocusGlass v0.1.3 — Runtime diagnostics and single-instance guard

- Added a named Windows mutex to prevent multiple FocusGlass instances from creating competing capture sessions and overlay windows.
- A second launch now displays an explicit “already running” message and exits without starting another capture session.
- Hotkey registration errors now log the Win32 error code to help distinguish conflicts from other registration failures.
- Preserved the v0.1.2 blur strength controls and defaults.

The v0.1.3 change has not yet been compiled on Windows or runtime-tested.


# FocusGlass v0.1.4 — Foreground mask, shadow, and portable Release runtime

- Fixed edge leakage: the previous mask cleared every window enumerated before the foreground window in z-order. v0.1.4 clears only the foreground window and its owned popups, leaving unrelated background/topmost windows covered by blur.
- Uses visible DWM frame bounds and a rounded-corner transparent foreground cutout.
- Added a medium soft drop shadow over the blurred backdrop, offset slightly downward.
- Release now links the MSVC runtime statically (/MT). This removes the separate VC++ Redistributable dependency from the Release executable; Windows OS APIs remain external system dependencies.
- Debug configuration retains its developer runtime.
- No startup registration or installer has been added.

The user must rebuild v0.1.4 on Windows and test the new mask and shadow. This version has not yet been compiled or runtime-tested.


# FocusGlass v0.1.5 — MSVC LONG/int shadow compile fix

- Explicitly typed the shadow edge-distance calculations as `std::max<int>`.
- Cast Win32 RECT LONG coordinates to int at the arithmetic boundary to avoid mixed-type initializer-list deduction errors C2672/C2737 in MSVC.
- No intended change to shadow shape, blur intensity, or foreground mask.

Not yet compiled on Windows. Rebuild and report any remaining compiler diagnostics.
