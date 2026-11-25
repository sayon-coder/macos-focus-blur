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
