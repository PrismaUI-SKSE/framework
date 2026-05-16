# Step 3 - CEF Runtime Lifecycle

## Goal

Introduce a CEF runtime owner that initializes CEF once, shuts it down safely, and exposes a small backend API to the existing PrismaUI internals.

## Edit Scope

- new `src/PrismaUI/CefRuntime.*`
- new `src/PrismaUI/CefApp.*`
- `src/main.cpp`
- `src/PrismaUI/Core.*`
- loader utilities

## Tasks

1. Move CEF-specific state into `CefRuntime`:
   - `CefRefPtr<CefApp>`
   - main `CefRefPtr<CefBrowser>`
   - main OSR client
   - frame size
   - initialization flags
   - shutdown flags
2. Initialize CEF from PrismaUI core initialization:
   - `settings.no_sandbox = true`
   - `settings.windowless_rendering_enabled = true`
   - `settings.browser_subprocess_path = <Data/PrismaUI/libs/PrismaUICefSubprocess.exe>`
   - `settings.resources_dir_path`
   - `settings.locales_dir_path`
   - `settings.log_file = <Data/PrismaUI/logs/cef.log>` or existing build/runtime log location
   - local development switches equivalent to current behavior: disable web security, allow file access from files, allow universal access from files
3. Log every important CEF lifecycle step:
   - start and result of `CefInitialize`
   - chosen message loop mode
   - subprocess path and whether it exists
   - resources/locales/log paths
   - browser creation request
   - `OnAfterCreated`, `DoClose`, and `OnBeforeClose`
   - shell URL load start, finish, and failure
   - resize events and external begin-frame requests when debug logging is enabled
   - shutdown start, browser close request, browser closed, and `CefShutdown`
   - all CEF callback exceptions or unexpected null browser/frame states
4. Decide and test message loop mode:
   - Option A: `multi_threaded_message_loop = true`, use CEF's UI thread and `CefPostTask`.
   - Option B: explicit `CefDoMessageLoopWork()` from the present path.
   - Prefer Option A if it behaves correctly inside Skyrim because it avoids tying CEF UI work to the render hook.
5. Create one transparent OSR browser:
   - `CefWindowInfo::SetAsWindowless(hWnd)`
   - `shared_texture_enabled = true`
   - `external_begin_frame_enabled = true`
   - `browser_settings.windowless_frame_rate` above 30 FPS
   - transparent background color
6. Load an internal shell page from `Data/PrismaUI/shell/index.html` or a custom in-memory scheme.
7. Add lifecycle methods:
   - `Initialize(HWND, ID3D11Device*, ID3D11DeviceContext*, screen size)`
   - `Resize(width, height)`
   - `BeginFrame()`
   - `Shutdown()`
   - `PostToCefUi(std::function<void()>)`

## Acceptance Criteria

- CEF initializes once after PrismaUI first creates a view.
- The CEF browser is created only after HWND and D3D state are available.
- CEF shutdown closes the browser and waits for `OnBeforeClose` before `CefShutdown`.
- Repeated create/destroy of Prisma views does not reinitialize CEF unnecessarily.
- Logs can reconstruct the CEF lifecycle from initialization through shutdown.

## Risks

- CEF initialization inside a DLL loaded into Skyrim is more sensitive than the standalone sample. The subprocess executable is the main mitigation.
- Blocking CEF shutdown on the render thread can hang game exit. Use a bounded shutdown path with clear logging.
