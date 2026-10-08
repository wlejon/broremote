// Where the server's socket lives on Windows: %LOCALAPPDATA%\broremote, inside
// the user's profile (whose ACL already keeps other users out). Windows
// hosting is for tests and development; the real host is bro on Linux.
#include "broremote/stream.h"
#include "win_util.h"

#include <cstdlib>

namespace broremote {

std::string socket_path(std::string_view name, std::string* err) {
    if (name.empty()) name = kDefaultSocketName;
    if (!valid_socket_name(name)) {
        if (err) *err = "invalid socket name '" + std::string(name) + "'";
        return {};
    }
    std::wstring base;
    if (const wchar_t* l = _wgetenv(L"LOCALAPPDATA"); l && *l) base = l;
    else if (const wchar_t* t = _wgetenv(L"TEMP"); t && *t) base = t;
    if (base.empty()) {
        if (err) *err = "neither LOCALAPPDATA nor TEMP is set";
        return {};
    }
    std::wstring dir = base + L"\\broremote";
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        if (err) *err = "cannot create " + win::to_utf8(dir) + ": " + win::error_text(GetLastError());
        return {};
    }
    std::string path = win::to_utf8(dir) + "\\" + std::string(name) + ".sock";
    if (path.size() >= 108) {  // sockaddr_un::sun_path
        if (err) *err = "socket path too long: " + path;
        return {};
    }
    return path;
}

}  // namespace broremote
