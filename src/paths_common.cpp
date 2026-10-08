// Where the server listens, and connecting to it (brolink's transport).
#include "broremote/stream.h"

#include <brolink/paths.h>

namespace broremote {

namespace {
constexpr std::string_view kAppName = "broremote";
}

bool valid_socket_name(std::string_view name) noexcept {
    return !name.empty() && name.front() != '.' && brolink::valid_name(name);
}

std::string socket_path(std::string_view name, std::string* err) {
    if (name.empty()) name = kDefaultSocketName;
    if (!valid_socket_name(name)) {
        if (err) *err = "invalid socket name '" + std::string(name) + "'";
        return {};
    }
    return brolink::local_address(kAppName, name, err);
}

std::unique_ptr<Stream> connect_local(std::string_view name, std::string* err, bool* not_running) {
    if (not_running) *not_running = false;
    const std::string address = socket_path(name, err);
    if (address.empty()) return nullptr;
    return brolink::connect_local(address, err, not_running);
}

}  // namespace broremote
