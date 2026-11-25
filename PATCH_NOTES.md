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
