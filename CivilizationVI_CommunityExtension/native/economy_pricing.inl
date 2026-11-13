// Generic read-only economic quotes for gameplay callers. CSC's transaction
// graph stays in Lua; CE only normalizes and discounts its declared streams.
// The target functions are identified in target_identity.h. Ghidra reference
// SHA324c51e9 / supported hotfix SHA092abfbb share these exact instructions.
// This include is inside DistrictScores' private namespace after the common
// pointer and HKS transport helpers. No worker thread calls into gameplay Lua.
using RegistryFn = const unsigned char* (__cdecl *)();
using CityFindFn = const unsigned char* (__cdecl *)(const void*, int);
using DefinitionsFn = const uintptr_t* (__cdecl *)();
using NormalizeYieldFn = int* (__cdecl *)(int*, int*, unsigned, const void*);
using TimedIncomeFn = int* (__cdecl *)(int*, int*, int*, int, int);
using AmenityIncomeFn = int* (__cdecl *)(int*, const void*, const void*, int, int*, int, int);
bool economy_pricing_ready{};

bool qualify_economy_pricing() {
    struct Prefix { uintptr_t rva; std::array<unsigned char, 16> bytes; };
    // Sixteen-byte entry identities supplement the full-file SHA guard already
    // required by Create. These are calls, not new hooks or writable patches.
    const Prefix expected[]{
        {NativePlayerRegistryRva, {0x48,0x8b,0x05,0xb9,0x75,0x88,0x00,0xc3,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}},
        {NativeCityFindRva, {0x83,0xfa,0xff,0x74,0x3d,0x4c,0x8b,0x91,0x98,0x00,0x00,0x00,0x48,0x8b,0x81,0xa0}},
        {NormalizeYieldIncomeRva, {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x57}},
        {DiscountTimedIncomeRva, {0x40,0x57,0x48,0x83,0xec,0x40,0x48,0x8b,0xf9,0x44,0x3b,0x4c,0x24,0x70,0x75,0x25}},
        {AmenityIncomeValueRva, {0x48,0x89,0x5c,0x24,0x10,0x44,0x89,0x4c,0x24,0x20,0x4c,0x89,0x44,0x24,0x18,0x48}},
    };
    for (const auto& item : expected) {
        const auto* target = Runtime::GetGameCoreGlobalAt<const unsigned char*>(item.rva);
        if (!readable(target, item.bytes.size(), false) || std::memcmp(target,item.bytes.data(),item.bytes.size()))
            return false;
    }
    return true;
}

bool quote_objects(int owner, int cityId, const unsigned char*& player, const unsigned char*& city) {
    if (!hooks_ready || !economy_pricing_ready || owner < 0 || owner >= MaxPlayers || cityId < 0)
        return false;
    const auto* registry = Runtime::GetGameCoreGlobalAt<RegistryFn>(NativePlayerRegistryRva)();
    if (!readable(registry, RegistryPlayerArrayOffset + sizeof(void*), false))
        return false;
    const auto* players = read_field<const unsigned char*>(registry, RegistryPlayerArrayOffset);
    if (!readable(players, MaxPlayers * sizeof(void*), false))
        return false;
    player = read_field<const unsigned char*>(players, owner * sizeof(void*));
    if (!readable(player, PlayerCitiesPointerOffset + sizeof(void*), false))
        return false;
    const auto* cities = read_field<const unsigned char*>(player, PlayerCitiesPointerOffset);
    if (!readable(cities, CityCollectionReadableBytes, false))
        return false;
    city = Runtime::GetGameCoreGlobalAt<CityFindFn>(NativeCityFindRva)(cities, cityId);
    return readable(city, CityOwnerOffset + sizeof(int), false) &&
        read_field<int>(city, CityOwnerOffset) == owner && read_field<int>(city, CityIdOffset) == cityId;
}

bool quote_times(int ready, int horizon, int discount) {
    // The initial public bound covers the existing50-turn district window.
    // Keep intermediate native fixed-point products bounded; this is not an
    // invitation to price an arbitrary thousand-turn income or encoding maximum.
    return ready >= 0 && ready < horizon && horizon <= 100 && discount > 0 && discount <= 64 * Q8Units;
}

int l_economy_status(lua_State* L) {
    char output[256];
    snprintf(output,sizeof output,
        "{\"abi\":1,\"finalReady\":%d,\"pricingReady\":%d,\"minFinalQ8\":%d,\"maxFinalQ8\":%d,\"maxHorizon\":100}",
        static_cast<int>(hooks_ready),static_cast<int>(economy_pricing_ready),MinimumScore,MaxDelta);
    push_json(L,output,std::strlen(output));
    return 1;
}

int l_yield_stream_value(lua_State* L) {
    const int owner=hks::checkinteger(L,1), cityId=hks::checkinteger(L,2);
    const int yield=hks::checkinteger(L,3), amount=hks::checkinteger(L,4);
    const int ready=hks::checkinteger(L,5), horizon=hks::checkinteger(L,6), discount=hks::checkinteger(L,7);
    const unsigned char *player{}, *city{};
    if (yield < 0 || yield >= 128 || amount < -128*Q8Units || amount > 128*Q8Units ||
        !quote_times(ready,horizon,discount) || !quote_objects(owner,cityId,player,city))
        return 0; // No Lua result means unavailable; never masquerade as zero value.
    const auto* definitions=Runtime::GetGameCoreGlobalAt<DefinitionsFn>(YieldDefinitionsRva)();
    if (!readable(definitions,2*sizeof(uintptr_t),false) || definitions[1]<definitions[0] ||
        static_cast<uintptr_t>(yield)>=(definitions[1]-definitions[0])/sizeof(void*))
        return 0;
    int input=amount, normalized{}, rate=discount, value{};
    const auto* returned=Runtime::GetGameCoreGlobalAt<NormalizeYieldFn>(NormalizeYieldIncomeRva)(
        &normalized,&input,static_cast<unsigned>(yield),player);
    if (returned!=&normalized)
        return 0;
    returned=Runtime::GetGameCoreGlobalAt<TimedIncomeFn>(DiscountTimedIncomeRva)(
        &value,&normalized,&rate,ready,horizon);
    if (returned!=&value || value < -MaxScore || value > MaxScore ||
        (amount>=0 && value<0) || (amount<=0 && value>0))
        return 0;
    hks::pushinteger(L,value);
    return 1;
}

// Same native helper's equal-time branch is a discounted fixed amount. This
// prices an actual land invoice once, rather than amortizing it as fake upkeep.
int l_yield_fixed_value(lua_State* L) {
    const int owner=hks::checkinteger(L,1),cityId=hks::checkinteger(L,2),yield=hks::checkinteger(L,3);
    const int amount=hks::checkinteger(L,4),at=hks::checkinteger(L,5),discount=hks::checkinteger(L,6);
    const unsigned char *player{}, *city{};
    if (yield<0 || yield>=128 || amount < -4096*Q8Units || amount > 4096*Q8Units ||
        at<0 || at>100 || discount<=0 || discount>64*Q8Units || !quote_objects(owner,cityId,player,city))
        return 0;
    const auto* definitions=Runtime::GetGameCoreGlobalAt<DefinitionsFn>(YieldDefinitionsRva)();
    if (!readable(definitions,2*sizeof(uintptr_t),false) || definitions[1]<definitions[0] ||
        static_cast<uintptr_t>(yield)>=(definitions[1]-definitions[0])/sizeof(void*))
        return 0;
    int input=amount, normalized{}, rate=discount,value{};
    if (Runtime::GetGameCoreGlobalAt<NormalizeYieldFn>(NormalizeYieldIncomeRva)(
        &normalized,&input,static_cast<unsigned>(yield),player)!=&normalized)
        return 0;
    if (Runtime::GetGameCoreGlobalAt<TimedIncomeFn>(DiscountTimedIncomeRva)(
        &value,&normalized,&rate,at,at)!=&value || value < -MaxScore || value > MaxScore ||
        (amount>=0 && value<0) || (amount<=0 && value>0))
        return 0;
    hks::pushinteger(L,value);
    return 1;
}

int l_amenity_stream_value(lua_State* L) {
    const int owner=hks::checkinteger(L,1), cityId=hks::checkinteger(L,2), amount=hks::checkinteger(L,3);
    const int ready=hks::checkinteger(L,4), horizon=hks::checkinteger(L,5), discount=hks::checkinteger(L,6);
    const unsigned char *player{}, *city{};
    if (amount < -8 || amount > 8 || !quote_times(ready,horizon,discount) ||
        !quote_objects(owner,cityId,player,city))
        return 0;
    int rate=discount,value{};
    const auto* returned=Runtime::GetGameCoreGlobalAt<AmenityIncomeFn>(AmenityIncomeValueRva)(
        &value,player,city,amount,&rate,ready,horizon);
    if (returned!=&value || value < -MaxScore || value > MaxScore)
        return 0;
    hks::pushinteger(L,value);
    return 1;
}
