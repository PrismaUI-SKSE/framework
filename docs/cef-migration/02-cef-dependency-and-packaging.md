# Step 2 - CEF Dependency And Packaging

## Goal

Add CEF to PrismaUI's build and distribution layout without changing runtime behavior yet.

## Edit Scope

- `CMakeLists.txt`
- new `cmake/cef.cmake`
- new CEF subprocess target source files
- `Utils::DllLoader` or replacement CEF loader
- distribution copy rules
- docs/readme updates after the build is stable

## Tasks

1. Vendor or reference the CEF binary package in `external/cef_binary`.
   - Mirror the working `C:\work\TestCef\cef_binary` layout initially.
   - Do not place generated CEF build output under source control.
2. Add a CMake module equivalent to the CEF sample:
   - set `CEF_ROOT`
   - add CEF module path
   - `find_package(CEF REQUIRED)`
   - add `libcef_dll_wrapper`
   - link `libcef_lib`
3. Add a dedicated subprocess executable:
   - suggested target: `PrismaUICefSubprocess`
   - package as `Data/PrismaUI/libs/PrismaUICefSubprocess.exe`
   - use the same `CefApp` implementation needed by the renderer process for JS bindings
4. Update the plugin target to link CEF:
   - `libcef_dll_wrapper`
   - `libcef_lib`
   - required Windows libs from the CEF sample
5. Replace Ultralight runtime copy rules with CEF copy rules:
   - CEF binary files to `Data/PrismaUI/libs`
   - CEF resources to `Data/PrismaUI/resources` or directly beside `libcef.dll`, based on what CEF accepts after testing
   - locales to `Data/PrismaUI/locales`
6. Add a CEF loader path:
   - set DLL directory or add `Data/PrismaUI/libs` with `AddDllDirectory`
   - load or allow delay-load of `libcef.dll`
   - make log messages say CEF, not Ultralight
7. Add logging for dependency and packaging checks:
   - resolved CEF root
   - `libcef.dll` path
   - subprocess executable path
   - resources and locales paths
   - missing file errors with absolute paths
8. Keep Ultralight build paths intact until CEF runtime is functional.

## Acceptance Criteria

- Release configure and build succeed.
- Distribution contains the plugin DLL, CEF DLLs, CEF resources, locales, and the subprocess exe.
- Plugin can load to the point of existing initialization without calling `CefInitialize`.
- Missing CEF files fail with clear logs naming the exact missing path.
- No files under `external/commonlibsse-ng` are touched.

## Risks

- CEF's subprocess path must be absolute or reliably resolved relative to `Data/PrismaUI/libs`.
- CEF may expect some files beside `libcef.dll`; verify packaging with a minimal `CefInitialize` smoke test.
- Runtime size increases significantly compared to Ultralight.
