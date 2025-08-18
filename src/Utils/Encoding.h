#pragma once

#include <string>
#include <vector>
#include <windows.h>

bool isValidUTF8(const char* str)
{
    if (!str) {
        return true;
    }
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str, -1, NULL, 0);
    return len != 0;
}

std::string convertFromANSIToUTF8(const char* str)
{
    if (!str) {
        return "";
    }

    int wide_char_len = MultiByteToWideChar(CP_ACP, 0, str, -1, NULL, 0);
    if (wide_char_len == 0) {
        return str;
    }
    std::vector<wchar_t> wide_char_buffer(wide_char_len);
    MultiByteToWideChar(CP_ACP, 0, str, -1, wide_char_buffer.data(), wide_char_len);

    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide_char_buffer.data(), -1, NULL, 0, NULL, NULL);
    if (utf8_len == 0) {
        return str;
    }
    std::vector<char> utf8_buffer(utf8_len);
    WideCharToMultiByte(CP_UTF8, 0, wide_char_buffer.data(), -1, utf8_buffer.data(), utf8_len, NULL, NULL);

    return std::string(utf8_buffer.data());
}