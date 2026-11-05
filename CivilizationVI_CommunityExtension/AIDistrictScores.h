#pragma once
#include "HavokScript.h"

namespace AI::DistrictScores {
    // Run once after Runtime/HKS initialization, before CE's registration hooks.
    // An unsupported image or incomplete hook group leaves the API unavailable.
    bool Create();

    // Register on CE's gameplay Lua thread. Workers never retain this state.
    void RegisterLua(hks::lua_State* L);

    // Retire policy before context creation/destruction. Hooks stay process-owned.
    void Reset();

#ifdef CE_AI_DISTRICT_SCORES_TEST
    int SelfTest();
#endif
}
