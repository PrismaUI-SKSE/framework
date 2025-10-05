# Prisma UI

Skyrim Next-Gen Web UI Framework.

- **Docs and Guides: https://www.prismaui.dev**
- **Discord Community: https://discord.com/invite/QYztzZY8RG**

## Contributing Guide

- Feel free to contribute to this project.

## Development

#### Requirements

- [XMake](https://xmake.io) [2.8.2+]
- C++23 Compiler (MSVC, Clang-CL)

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
*   **Use the framework commercially** if your company's revenue or funding is **over US$100,000** (unless you purchase a Pro License from Ultralight, Inc.).
*   **Reverse-engineer** the included Ultralight SDK components.
