#pragma once

// The editor's status line: a message shown for a few seconds.

#include "EditorState.hpp"

#include <cstdio>
#include <string>

namespace quadmapper {

inline void postStatus(EditorState& es, const std::string& message) {
    snprintf(es.statusMessage, sizeof(es.statusMessage), "%s", message.c_str());
    es.statusTimer = 4.0f;
}

}  // namespace quadmapper
