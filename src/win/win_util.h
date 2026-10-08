#pragma once
// Small Win32 helpers shared by the Windows platform files.

#include <winsock2.h>
#include <windows.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace broremote::win {

inline std::wstring to_wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

inline std::string to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::string error_text(DWORD code) {
    wchar_t* msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::string s = n ? to_utf8(std::wstring_view(msg, n)) : std::string();
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s + " (" + std::to_string(code) + ")";
}

struct HandleCloser {
    void operator()(HANDLE h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

// The user SID of a process's token (TokenUser), as bytes; empty on failure.
inline std::vector<unsigned char> process_user_sid(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return {};
    UniqueHandle tk(token);
    DWORD len = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    if (len == 0) return {};
    std::vector<unsigned char> buf(len);
    if (!GetTokenInformation(token, TokenUser, buf.data(), len, &len)) return {};
    PSID sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
    DWORD sid_len = GetLengthSid(sid);
    const auto* p = static_cast<const unsigned char*>(sid);
    return std::vector<unsigned char>(p, p + sid_len);
}

inline std::vector<unsigned char> current_user_sid() { return process_user_sid(GetCurrentProcess()); }

}  // namespace broremote::win
