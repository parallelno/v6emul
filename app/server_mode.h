#pragma once

#include <cstdint>
#include "core/display.h"

namespace dev { class Hardware; }

int RunServerMode(dev::Hardware& hw, uint16_t port, dev::Display::ColorFormat colorFormat);
