#pragma once
// The viewer's session is the library's (broremote/viewer.h); these are the
// names the SDL viewer knows it by.

#include "broremote/viewer.h"
#include "connect.h"

namespace broremote::view {

using Session = ViewerSession;
using SessionOptions = broremote::ViewerOptions;
using SessionState = ViewerState;
using SessionStatus = ViewerStatus;
using SessionStats = ViewerStats;
using FrameInfo = ViewerFrameInfo;
using AudioOptions = ViewerAudioOptions;

}  // namespace broremote::view
