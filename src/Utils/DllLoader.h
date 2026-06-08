#pragma once

#include <Windows.h>

#include <array>
#include <filesystem>
#include <mutex>
#include <string>

// Note: This header depends on 'logger' namespace from PCH.h (SKSE::log)

namespace PrismaUI::Utils {
    // Common base path for PrismaUI data files
    inline std::filesystem::path GetBasePath() { return std::filesystem::current_path() / "Data" / "PrismaUI"; }

    class DllLoader {
    public:
        static DllLoader& GetSingleton() {
            static DllLoader instance;
            return instance;
        }

        bool LoadCefLibraries() {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (m_cefLoaded) {
                return true;
            }

            const auto libsPath = GetBasePath() / "libs";
            logger::info("Preparing CEF library directory: {}", libsPath.string());

            if (!std::filesystem::is_directory(libsPath)) {
                logger::error("CEF libs path does not exist or is not a directory: {}", libsPath.string());
                return false;
            }

            const std::array requiredFiles = {L"libcef.dll", L"chrome_elf.dll", L"PrismaUICefSubprocess.exe",
                                              L"icudtl.dat"};
            for (const auto* requiredFile : requiredFiles) {
                const auto requiredPath = libsPath / requiredFile;
                logger::info("CEF dependency path: {}", requiredPath.string());
                if (!std::filesystem::is_regular_file(requiredPath)) {
                    logger::error("Required CEF runtime file is missing: {}", requiredPath.string());
                    return false;
                }
            }

            const auto localesPath = libsPath / "locales";
            logger::info("CEF locales path: {}", localesPath.string());
            if (!std::filesystem::is_directory(localesPath)) {
                logger::error("Required CEF locales directory is missing: {}", localesPath.string());
                return false;
            }

            m_cefDllDirectoryCookie = AddDllDirectory(libsPath.wstring().c_str());
            if (!m_cefDllDirectoryCookie) {
                const DWORD error = GetLastError();
                logger::error("AddDllDirectory failed for CEF path '{}'. Error: {}", libsPath.string(), error);
                return false;
            }

            const auto libcefPath = libsPath / "libcef.dll";
            m_cefModule = LoadLibraryExW(
                libcefPath.wstring().c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
            if (!m_cefModule) {
                const DWORD error = GetLastError();
                logger::error("Failed to load CEF DLL: {} (Error: {})", libcefPath.string(), error);
                RemoveDllDirectory(m_cefDllDirectoryCookie);
                m_cefDllDirectoryCookie = nullptr;
                return false;
            }

            m_cefLoaded = true;
            logger::info("Loaded CEF DLL: {}", libcefPath.string());
            return true;
        }

        // Unload all loaded CEF DLL state.
        void UnloadAll() {
            std::lock_guard<std::mutex> lock(m_mutex);
            UnloadAllInternal();
        }

        bool IsCefLoaded() const { return m_cefLoaded; }

    private:
        DllLoader() = default;
        ~DllLoader() { UnloadAllInternal(); }

        DllLoader(const DllLoader&) = delete;
        DllLoader& operator=(const DllLoader&) = delete;

        void UnloadCefInternal() {
            if (m_cefModule) {
                FreeLibrary(m_cefModule);
                m_cefModule = nullptr;
            }

            if (m_cefDllDirectoryCookie) {
                RemoveDllDirectory(m_cefDllDirectoryCookie);
                m_cefDllDirectoryCookie = nullptr;
            }

            m_cefLoaded = false;
        }

        void UnloadAllInternal() { UnloadCefInternal(); }

        HMODULE m_cefModule = nullptr;
        DLL_DIRECTORY_COOKIE m_cefDllDirectoryCookie = nullptr;
        bool m_cefLoaded = false;
        mutable std::mutex m_mutex;
    };
}
