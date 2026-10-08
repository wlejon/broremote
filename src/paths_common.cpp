#include "broremote/stream.h"

namespace broremote {

bool valid_socket_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64 || name.front() == '.') return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

}  // namespace broremote
