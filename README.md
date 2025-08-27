# PrismaUI

Skyrim Next-Gen Web UI Framework. **Work In Progress Early Ready-To-Use Stage.**

- **Docs and Guides: https://www.prismaui.dev**
- **Discord Community: https://discord.com/invite/QYztzZY8RG**

## Contributing Guide

- Feel free to remake, improve or extend this project.

## Development

#### Requirements

- [XMake](https://xmake.io) [2.8.2+]
- C++23 Compiler (MSVC, Clang-CL)
- Windows 10/11
- DirectX 11

### Getting Started

```bat
git clone --recurse-submodules https://github.com/PrismaUI-SKSE/PrismaUI.git
```

### Build

To build the project, run the following command:

```bat
xmake build
```

> **_Note:_** *This will generate a `build/windows/` directory in the **project's root directory** with the build output.*d

### Project Generation for Visual Studio

If you want to generate a Visual Studio project, run the following command:

```bat
xmake project -k vsxmake
```

> **_Note:_** _This will generate a `vsxmakeXXXX/` directory in the **project's root directory** using the latest version of Visual Studio installed on the system._

### Upgrading Packages (Optional)

If you want to upgrade the project's dependencies, run the following commands:

```bat
xmake repo --update
xmake require --upgrade
```

## Dependencies / Acknowledgments

This plugin utilizes the **[Ultralight](https://ultralig.ht) SDK** for rendering web content.

The Ultralight SDK is provided under the **[Ultralight Free License Agreement](https://ultralig.ht/free-license/LICENSE.txt)**. The full terms of this license are available in the `NOTICES.txt` file located at the root of this repository.

## License

This project is licensed under the **Prisma UI Source-Available License v2**. Please see the [`LICENSE.md`](LICENSE.md) file for the full text.

### Summary

**You ARE allowed to:**
*   **Share and distribute the original mod files** with anyone.
*   Modify the mod for your own **private use**.
*   Fork the repository to submit Pull Requests with improvements.

**You ARE NOT allowed to:**
*   **Publicly release or distribute your own modified versions** of this mod.
*   Use the mod or its code for any commercial purpose.
