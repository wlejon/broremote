// Where the server's socket lives on POSIX: a private directory under
// $XDG_RUNTIME_DIR (or /tmp when that is unset).
#include "broremote/stream.h"
#include "posix_util.h"

#include <sys/stat.h>
#include <sys/un.h>

#include <cstdlib>

namespace broremote {

namespace {

// Create `dir` 0700 if missing; refuse one that is not a private directory of ours.
bool ensure_private_dir(const std::string& dir, std::string* err) {
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        if (err) *err = "cannot create " + dir + ": " + posix::errno_text(errno);
        return false;
    }
    struct stat st {};
    if (::lstat(dir.c_str(), &st) != 0) {
        if (err) *err = "cannot stat " + dir + ": " + posix::errno_text(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != ::getuid()) {
        if (err) *err = dir + " is not a directory owned by this user; refusing it";
        return false;
    }
    if ((st.st_mode & 077) != 0) {
        if (err) *err = dir + " is accessible to other users; refusing it";
        return false;
    }
    return true;
}

std::string base_dir(std::string* err) {
    if (const char* x = std::getenv("XDG_RUNTIME_DIR"); x && *x) {
        struct stat st {};
        if (::stat(x, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::getuid()) {
            std::string dir = std::string(x) + "/broremote";
            if (ensure_private_dir(dir, err)) return dir;
            return {};
        }
    }
    std::string tmp = "/tmp";
    if (const char* t = std::getenv("TMPDIR"); t && *t) tmp = t;
    while (tmp.size() > 1 && tmp.back() == '/') tmp.pop_back();
    std::string dir = tmp + "/broremote-" + std::to_string(::getuid());
    if (!ensure_private_dir(dir, err)) return {};
    return dir;
}

}  // namespace

std::string socket_path(std::string_view name, std::string* err) {
    if (name.empty()) name = kDefaultSocketName;
    if (!valid_socket_name(name)) {
        if (err) *err = "invalid socket name '" + std::string(name) + "'";
        return {};
    }
    std::string dir = base_dir(err);
    if (dir.empty()) return {};
    std::string path = dir + "/" + std::string(name) + ".sock";
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        if (err) *err = "socket path too long: " + path;
        return {};
    }
    return path;
}

}  // namespace broremote
