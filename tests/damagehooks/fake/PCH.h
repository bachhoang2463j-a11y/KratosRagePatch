#pragma once

// Mirrors src/PCH.h for the isolated damage-hook test build. The RE/REL/SKSE/spdlog headers
// resolve to the fakes next to this file; everything else is the real standard library.

#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
