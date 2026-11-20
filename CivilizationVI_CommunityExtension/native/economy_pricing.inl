// Generic read-only economic quotes for gameplay callers. CSC's transaction
// graph stays in Lua; CE only normalizes and discounts its declared streams.
// target_identity.h identifies the current XP2 image and quote functions.
// The 17 November interval trace explains the original ABI. The Steam
// 26595377 relocation ledger records the addresses for this build.
// This include is inside DistrictScores' private namespace after the common
// pointer and HKS transport helpers. No worker thread calls into gameplay Lua.
using RegistryFn = const unsigned char* (__cdecl *)();
using CityFindFn = const unsigned char* (__cdecl *)(const void*, int);
using DefinitionsFn = const uintptr_t* (__cdecl *)();
using NormalizeYieldFn = int* (__cdecl *)(int*, int*, unsigned, const void*);
using TimedIncomeFn = int* (__cdecl *)(int*, int*, int*, int, int);
using AmenityIncomeFn = int* (__cdecl *)(int*, const void*, const void*, int, int*, int, int);
bool economy_pricing_ready{};
bool qualify_service_pricing();

bool qualify_economy_pricing() {
    struct Prefix { uintptr_t rva; std::array<unsigned char, 16> bytes; };
    // Sixteen-byte entry identities supplement the full-file SHA guard already
    // required by Create. These are calls, not new hooks or writable patches.
    const Prefix expected[]{
        {NativePlayerRegistryRva, {0x48, 0x8b, 0x05, 0x79, 0x83, 0x88, 0x00, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc}},
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
    // The bound covers ordinary native horizons through Marathon (300 turns).
    // Keep intermediate native fixed-point products bounded; this is not an
    // invitation to price an arbitrary thousand-turn income or encoding maximum.
    return ready >= 0 && ready < horizon && horizon <= 300 && discount > 0 && discount <= 64 * Q8Units;
}

// Read-only profiling clock. Relative steady-clock seconds measure the whole
// caller's synchronous policy pass, including native quote/UI calls. This is
// wall time, not game turns, CPU time or an economic discount parameter. A local
// origin preserves sub-millisecond precision over ordinary game sessions; no
// GameCore address, borrowed object or worker callback is involved.
int l_profile_clock(lua_State* L) {
    using Clock = std::chrono::steady_clock;
    static const auto origin = Clock::now();
    hks::pushnumber(L, std::chrono::duration<double>(Clock::now() - origin).count());
    return 1;
}

int l_economy_status(lua_State* L) {
    char output[256];
    snprintf(output,sizeof output,
        "{\"abi\":1,\"finalReady\":%d,\"pricingReady\":%d,\"minFinalQ8\":%d,\"maxFinalQ8\":%d,\"maxHorizon\":300,\"servicePricingReady\":%d}",
        static_cast<int>(hooks_ready),static_cast<int>(economy_pricing_ready),MinimumScore,MaxDelta,static_cast<int>(hooks_ready && economy_pricing_ready && qualify_service_pricing()));
    push_json(L,output,std::strlen(output));
    return 1;
}

// Native cumulative income returns the input at t=0, but a recurring interval
// needs a zero cumulative origin. Correct before range/sign validation: the raw
// native 0..1 difference can be negative even for positive income. Use a wide
// intermediate so adding the origin cannot wrap an invalid quote into range.
// Raw Stream bindings keep their original semantics for existing callers.
bool checked_economy_quote(int nativeValue, int origin, int amount, bool yieldSign, int& result) {
    const auto corrected = static_cast<int64_t>(nativeValue) + origin;
    if (corrected < -MaxScore || corrected > MaxScore ||
        (yieldSign && ((amount >= 0 && corrected < 0) || (amount <= 0 && corrected > 0))))
        return false;
    result = static_cast<int>(corrected);
    return true;
}

int yield_stream_value(lua_State* L, bool interval) {
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
    if (returned!=&value || !checked_economy_quote(value,
        interval && ready == 0 ? normalized : 0, amount, true, value))
        return 0;
    hks::pushinteger(L,value);
    return 1;
}

int l_yield_stream_value(lua_State* L) { return yield_stream_value(L, false); }
int l_yield_interval_value(lua_State* L) { return yield_stream_value(L, true); }

// Same native helper's equal-time branch is a discounted fixed amount. This
// prices an actual land invoice once, rather than amortizing it as fake upkeep.
int l_yield_fixed_value(lua_State* L) {
    const int owner=hks::checkinteger(L,1),cityId=hks::checkinteger(L,2),yield=hks::checkinteger(L,3);
    const int amount=hks::checkinteger(L,4),at=hks::checkinteger(L,5),discount=hks::checkinteger(L,6);
    const unsigned char *player{}, *city{};
    if (yield<0 || yield>=128 || amount < -4096*Q8Units || amount > 4096*Q8Units ||
        at<0 || at>300 || discount<=0 || discount>64*Q8Units || !quote_objects(owner,cityId,player,city))
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

int amenity_stream_value(lua_State* L, bool interval) {
    const int owner=hks::checkinteger(L,1), cityId=hks::checkinteger(L,2), amount=hks::checkinteger(L,3);
    const int ready=hks::checkinteger(L,4), horizon=hks::checkinteger(L,5), discount=hks::checkinteger(L,6);
    const unsigned char *player{}, *city{};
    if (amount < -8 || amount > 8 || !quote_times(ready,horizon,discount) ||
        !quote_objects(owner,cityId,player,city))
        return 0;
    int rate=discount,value{};
    const auto* returned=Runtime::GetGameCoreGlobalAt<AmenityIncomeFn>(AmenityIncomeValueRva)(
        &value,player,city,amount,&rate,ready,horizon);
    if (returned!=&value)
        return 0;
    int origin{};
    if (interval && ready == 0) {
        // Amenity utility is nonlinear around city happiness thresholds. The
        // equal-time branch quotes this same city/count's current scalar; it
        // must never be replaced with count times a one-amenity price. Reset
        // rate because the native ABI borrows a mutable fixed-point pointer.
        rate=discount;
        if (Runtime::GetGameCoreGlobalAt<AmenityIncomeFn>(AmenityIncomeValueRva)(
            &origin,player,city,amount,&rate,0,0)!=&origin)
            return 0;
    }
    // Native threshold utility supplies the sign; count alone cannot validate it.
    if (!checked_economy_quote(value,origin,amount,false,value))
        return 0;
    hks::pushinteger(L,value);
    return 1;
}

int l_amenity_stream_value(lua_State* L) { return amenity_stream_value(L, false); }
int l_amenity_interval_value(lua_State* L) { return amenity_stream_value(L, true); }

// Service quotes deliberately bypass the modifier attachment interpreter. Lua
// supplies eligibility/recipients; these calls only price an admitted effect.
// These entries use the current target_identity.h image qualification.
// The earlier pseudo-yield and Food-surplus traces establish the call shapes;
// the relocation ledger records their Steam 26595377 entry points.
namespace ServicePricingLayout {
constexpr uintptr_t NormalizePseudoYieldRva = 0x52af20;
constexpr uintptr_t PseudoYieldDefinitionsRva = 0x7d14c0;
constexpr uintptr_t FoodSurplusRva = 0x124060;
constexpr uintptr_t FoodYieldIndexRva = 0xb8f648;
constexpr size_t CityGrowthOffset = 0x970;
}
using NormalizePseudoFn = int* (__cdecl *)(int*, int*, unsigned, const void*, int);
using FoodSurplusFn = int* (__cdecl *)(const void*, int*);

bool qualify_service_pricing() {
    // Reuse CE's image/SHA qualification. Check every newly called entry too;
    // an unavailable optional Service consumer must not disable ordinary quotes.
    struct Prefix { uintptr_t rva; std::array<unsigned char,16> bytes; };
    const Prefix expected[]{
        {ServicePricingLayout::NormalizePseudoYieldRva,{0x48,0x89,0x6c,0x24,0x20,0x56,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x40,0x41,0x8b}},
        {ServicePricingLayout::PseudoYieldDefinitionsRva,{0x48, 0x83, 0xec, 0x28, 0xe8, 0x07, 0x39, 0x87, 0xff, 0x48, 0x05, 0x80, 0x1f, 0x00, 0x00, 0x48}},
        {ServicePricingLayout::FoodSurplusRva,{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x20,0x48}},
    };
    for (const auto& item : expected) {
        const auto* entry = Runtime::GetGameCoreGlobalAt<const unsigned char*>(item.rva);
        if (!readable(entry,item.bytes.size(),false) || std::memcmp(entry,item.bytes.data(),item.bytes.size())) return false;
    }
    return economy_pricing_ready;
}

int l_pseudo_yield_interval_value(lua_State* L) {
    // Same seven arguments and Q8 amount/result as CE_AIYieldIntervalValue.
    // The pseudo-yield ID comes from loaded definitions (GPP class/Tourism),
    // never a numeric enum baked into a mod. Player strategy/diplomacy applies
    // exactly once. The final -1 is native's unspecified diplomatic target.
    const int owner=hks::checkinteger(L,1),cityId=hks::checkinteger(L,2);
    const int kind=hks::checkinteger(L,3),amount=hks::checkinteger(L,4);
    const int ready=hks::checkinteger(L,5),horizon=hks::checkinteger(L,6),discount=hks::checkinteger(L,7);
    const unsigned char *player{},*city{};
    if (kind<0 || kind>=1024 || amount < -128*Q8Units || amount > 128*Q8Units ||
        !quote_times(ready,horizon,discount) || !quote_objects(owner,cityId,player,city) || !qualify_service_pricing()) return 0;
    const auto* definitions=Runtime::GetGameCoreGlobalAt<DefinitionsFn>(ServicePricingLayout::PseudoYieldDefinitionsRva)();
    if (!readable(definitions,2*sizeof(uintptr_t),false) || definitions[1]<definitions[0] ||
        static_cast<uintptr_t>(kind)>=(definitions[1]-definitions[0])/sizeof(void*)) return 0;
    int input=amount,normalized{},rate=discount,value{};
    if (Runtime::GetGameCoreGlobalAt<NormalizePseudoFn>(ServicePricingLayout::NormalizePseudoYieldRva)(
        &normalized,&input,static_cast<unsigned>(kind),player,-1)!=&normalized) return 0;
    if (Runtime::GetGameCoreGlobalAt<TimedIncomeFn>(DiscountTimedIncomeRva)(
        &value,&normalized,&rate,ready,horizon)!=&value ||
        !checked_economy_quote(value,ready==0?normalized:0,amount,false,value)) return 0;
    hks::pushinteger(L,value);return 1;
}

int l_growth_interval_value(lua_State* L) {
    // Q8 percentage POINTS (2560 means +10%), not an income amount. Match the
    // growth child46fac0's actual recipient Food-surplus/default-Food pricing;
    // the corrected recurring-interval origin is shared with other CE quotes.
    // This prices today's positive surplus, not a population-growth simulation.
    const int owner=hks::checkinteger(L,1),cityId=hks::checkinteger(L,2),percent=hks::checkinteger(L,3);
    const int ready=hks::checkinteger(L,4),horizon=hks::checkinteger(L,5),discount=hks::checkinteger(L,6);
    const unsigned char *player{},*city{};
    if (percent<0 || percent>100*Q8Units || !quote_times(ready,horizon,discount) ||
        !quote_objects(owner,cityId,player,city) || !qualify_service_pricing() ||
        !readable(city,ServicePricingLayout::CityGrowthOffset+sizeof(void*),false)) return 0;
    int surplus{};
    if (Runtime::GetGameCoreGlobalAt<FoodSurplusFn>(ServicePricingLayout::FoodSurplusRva)(
        city+ServicePricingLayout::CityGrowthOffset,&surplus)!=&surplus) return 0;
    const auto* food=Runtime::GetGameCoreGlobalAt<const int*>(ServicePricingLayout::FoodYieldIndexRva);
    if (!readable(food,sizeof(int),false) || *food<0 || *food>=128) return 0;
    const auto quantity=static_cast<int64_t>((std::max)(0,surplus))*percent/(100*Q8Units);
    if (quantity>128*Q8Units) return 0;
    int input=static_cast<int>(quantity),normalized{},rate=discount,value{};
    if (Runtime::GetGameCoreGlobalAt<NormalizeYieldFn>(NormalizeYieldIncomeRva)(
        &normalized,&input,static_cast<unsigned>(*food),nullptr)!=&normalized) return 0;
    if (Runtime::GetGameCoreGlobalAt<TimedIncomeFn>(DiscountTimedIncomeRva)(
        &value,&normalized,&rate,ready,horizon)!=&value ||
        !checked_economy_quote(value,ready==0?normalized:0,input,true,value)) return 0;
    hks::pushinteger(L,value);return 1;
}
