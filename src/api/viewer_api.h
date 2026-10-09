#pragma once
// The viewer half of bro.remote (viewer_api.cpp), as api.cpp mounts it.

#include "object_builder.h"

namespace broremote::api {

// Adds connect() to the realm's bro.remote.
void defineConnect(ObjectBuilder& remote);
// Host thread, once per frame: the sessions' state / config events.
void tickViewers();
// Closes every session (a new realm, or the host going away).
void closeAllViewers();

}  // namespace broremote::api
