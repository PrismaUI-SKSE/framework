# Prisma UI

Skyrim Next-Gen Web UI Framework.

- **Docs and Guides: https://www.prismaui.dev**
- **Discord Community: https://discord.com/invite/QYztzZY8RG**

## Contributing Guide

- Use `dev` branch for your pull requests.
- Feel free to contribute to this project.

## Development

### Requirements

- [CMake](https://cmake.org/) 4.1+
- [Ninja](https://ninja-build.org/) (recommended build system)
- [vcpkg](https://vcpkg.io/) with `VCPKG_ROOT` environment variable set
- Visual Studio 2022 with C++23 support
- C++23 Compiler (MSVC)
- [Chromium Embedded Framework (CEF) binary distribution](https://cef-builds.spotifycdn.com/index.html) 147.0.14+g76d2442+chromium-147.0.7727.138 for Windows x64
  - place `cef_binary_147.0.14+g76d2442+chromium-147.0.7727.138_windows64.tar.bz2` in the `external` folder, or configure `PRISMAUI_CEF_ARCHIVE`.
### Getting Started

```bat
git clone --recurse-submodules https://github.com/PrismaUI-SKSE/PrismaUI.git
cd PrismaUI
```

### Build with CMake

#### Quick Build (Recommended)

Use the helper script to build with optimal settings:

```powershell
# Release build (default)
.\BuildRelease.ps1

# Debug build
.\BuildRelease.ps1 -preset debug

# Customize thread count
.\BuildRelease.ps1 -preset release -threads 4
```

> **_Note:_** The script automatically launches the VS Developer Shell and configures the build environment.

#### Manual Build

If you prefer manual CMake commands:

```bat
# Configure (from VS Developer Command Prompt)
cmake -S . --preset=release

# Build
cmake --build --preset=release --parallel 8
```

Available presets: `debug`, `release`

### Build Output

- **DLL Output**: `build/release/bin/PrismaUI.dll`
- **Distribution Package**: `dist/PrismaUI_<version>/` (created automatically after build)

### Public API ABI Break: DevTools

The CEF migration intentionally breaks the old Ultralight-inspector ABI. Mods must recompile against the current `src/PrismaUI_API.h`.

- Supported `InterfaceVersion` values are now `V1 = 4`, `V2 = 5`, and `V3 = 6`.
- Legacy binary requests using numeric interface values `0`, `1`, or `2` are rejected with `nullptr` instead of receiving a mismatched vtable.
- `CreateInspectorView`, `SetInspectorVisibility`, `IsInspectorVisible`, and `SetInspectorBounds` were removed.
- Use `OpenDevTools()`, `CloseDevTools()`, and `IsDevToolsOpen()` to inspect the single PrismaUI CEF shell browser. Individual Prisma views appear as iframes named `prisma-view-<PrismaView>`.

### Upgrading Packages (Optional)

**vcpkg:**
```bat
vcpkg upgrade
```

## Dependencies / Acknowledgments

This plugin uses **[Chromium Embedded Framework (CEF)](https://bitbucket.org/chromiumembedded/cef)** for embedded web rendering.

CEF is BSD-licensed and includes Chromium components with additional third-party notices. The redistributed notice text is in `NOTICES.txt`.

## License

This project is licensed under the **Prisma UI License**. Please see the [`LICENSE.md`](LICENSE.md) file for the full text.

### Summary

This license is designed to keep the framework free for community and small commercial projects, encourage contributions, and give the author full control over public versions of the code.

✔️ **You ARE allowed to:**
*   **Use** the framework in your non-commercial or small commercial project.
*   **Use it commercially** if your company's total annual revenue and total funding are **under US$100,000**.
*   **Share and distribute** the original, official framework files with anyone.
*   **Modify** the framework for your own **private use**.
*   **Fork the repository** for the sole purpose of submitting improvements back to the official project via a Pull Request.

❌ **You ARE NOT allowed to:**
*   **Publicly release or distribute your own modified versions** of this framework without the author's explicit written permission.
*   **Use the framework commercially** if your company's revenue or funding is **over US$100,000** without the author's explicit written permission.
*   **Reverse-engineer** included binary components except where allowed by their upstream licenses.

## Contributors

- [StarkMP](https://github.com/StarkMP)
- [langfod](https://github.com/langfod)