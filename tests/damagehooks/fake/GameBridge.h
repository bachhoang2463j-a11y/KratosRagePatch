#pragma once

#include <RE/Skyrim.h>

namespace kr {

// Test stand-in for the real GameBridge, limited to the one entry point the damage hook
// calls. The real class is expected to expose the same signature:
//   void SubmitPlayerDamage(RE::FormID targetID, RE::FormID baseID, float healthLost);
class GameBridge
{
public:
    static GameBridge& Get();

    void SubmitPlayerDamage(RE::FormID a_targetID, RE::FormID a_baseID, float a_healthLost);
};

}
