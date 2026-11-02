// Platform side of the emoji module: draws one emoji with the system's colour emoji font.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "lunashare_emoji.h"

namespace lse {
bool BackendInit(const LseHost* host, std::string* why);
// RGBA, straight alpha, size x size, emoji centered and scaled to fill most of the square
bool BackendRender(const char* utf8, int size, std::vector<uint8_t>& rgba);
void Log(const std::string& msg);
}  // namespace lse
