// Improvement NetValue has a normal Win64 entry. Plot and city are explicit
// arguments. The qualified row evaluator stores the returned int unchanged.
constexpr uintptr_t ImprovementNetValueRva = 0x52a8a0;
constexpr uintptr_t ImprovementRowCallRva = 0x3e2ce3;
constexpr uintptr_t ImprovementRowReturnRva = 0x3e2ce8;
constexpr uintptr_t ImprovementScoreFloorRva = 0xb66cb0;
constexpr unsigned char ImprovementNetPrefix[] = {
    0x44, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44,
    0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56,
};
using ImprovementNetFn = int* (__cdecl *)(int*, unsigned, const void*, int, int, void*, void*);
ImprovementNetFn original_improvement_net{};
bool builder_ready{};
long builder_changed{}, builder_bids{}, builder_faults{};
std::array<int, 6> first_builder_change{}, first_builder_bid{};

auto builder_site(const BuilderAllowance& entry) {
    return std::tie(entry.player, entry.plot, entry.city);
}

bool builder_order(const BuilderAllowance& a, const BuilderAllowance& b) {
    return std::tie(a.player, a.plot, a.city, a.improvement) <
        std::tie(b.player, b.plot, b.city, b.improvement);
}

auto supplier_at(const BuilderAllowance& key) {
    const auto first = policy.builders.begin();
    const auto end = first + policy.builderCount;
    const auto at = std::lower_bound(first, end, key, [](const auto& a, const auto& b) {
        return builder_site(a) < builder_site(b);
    });
    return at != end && builder_site(*at) == builder_site(key) ? at : end;
}

struct BuilderAdjustment {
    bool reject{};
    int deltaQ8{};
};

BuilderAdjustment builder_adjustment(const BuilderAllowance& key) {
    const auto end = policy.builders.begin() + policy.builderCount;
    const auto at = supplier_at(key);
    if (at == end)
        return {};
    const auto allowed = std::lower_bound(at, end, key, builder_order);
    if (allowed == end || builder_order(key, *allowed))
        return {true, 0};
    return {false, allowed->deltaQ8};
}

int adjusted_builder_score(int base, int floor, const BuilderAdjustment& adjustment) {
    if (adjustment.reject)
        return (std::min)(base, floor);
    // The row evaluator's eligibility tests have already passed. Preserve
    // zero, negative and sentinel values, and avoid signed overflow.
    if (base > 0 && base <= INT_MAX - adjustment.deltaQ8)
        return base + adjustment.deltaQ8;
    return base;
}

int* __cdecl hook_improvement_net(int* out, unsigned improvement, const void* discount,
    int delay, int horizon, void* plot, void* city) {
    ActiveCall activity;
    BuilderAllowance key{};
    long epoch{};
    BuilderAdjustment adjustment;
    if (enabled.load() && _ReturnAddress() ==
        Runtime::GetGameCoreGlobalAt<void*>(ImprovementRowReturnRva)) {
        std::shared_lock guard(policyMutex);
        if (enabled && !builder_faults && policy.builderCount && improvement <= INT_MAX &&
            readable(city, CityOwnerOffset + sizeof(int), false) &&
            readable(plot, PlotYOffset + sizeof(int16_t), false)) {
            const auto* c = static_cast<const unsigned char*>(city);
            const auto* p = static_cast<const unsigned char*>(plot);
            const int player = read_field<int>(c, CityOwnerOffset);
            const int x = read_field<int16_t>(p, PlotXOffset);
            const int y = read_field<int16_t>(p, PlotYOffset);
            if (player >= 0 && player < MaxPlayers && policy.allowed[player] &&
                x >= 0 && x < policy.width && y >= 0 && y < policy.height) {
                key = {player, read_field<int>(c, CityIdOffset), y * policy.width + x,
                    static_cast<int>(improvement)};
                adjustment = builder_adjustment(key);
                epoch = generation;
            }
        }
    }

    int* result = original_improvement_net(out, improvement, discount, delay, horizon, plot, city);
    if (adjustment.reject || adjustment.deltaQ8) {
        std::unique_lock guard(policyMutex);
        if (enabled && generation == epoch && !builder_faults) {
            const int floor = *Runtime::GetGameCoreGlobalAt<const int*>(ImprovementScoreFloorRva);
            if (result != out || !readable(out, sizeof(*out), true) || floor >= 0) {
                ++builder_faults;
            }
            else {
                const int next = adjusted_builder_score(*out, floor, adjustment);
                if (next != *out) {
                    auto& count = adjustment.reject ? builder_changed : builder_bids;
                    auto& first = adjustment.reject ? first_builder_change : first_builder_bid;
                    if (count == 0)
                        first = {key.player, key.city, key.plot, key.improvement, *out, next};
                    ++count;
                    *out = next;
                }
            }
        }
    }
    return result;
}

bool install_builder_hook(const unsigned char* base) {
    const auto* call = base + ImprovementRowCallRva;
    if (*call != X64CallRel32 || read_field<int32_t>(call, 1) !=
        static_cast<int32_t>(ImprovementNetValueRva - ImprovementRowReturnRva) ||
        std::memcmp(base + ImprovementNetValueRva, ImprovementNetPrefix, sizeof ImprovementNetPrefix))
        return false;
    using namespace Runtime;
    const HookSpec hooks[] = {
        DescribeHook("Improvement NetValue", GetGameCoreGlobalAt<ImprovementNetFn>(ImprovementNetValueRva),
            &hook_improvement_net, &original_improvement_net),
    };
    return InstallHookGroup(hooks);
}

// The tile-bearing building caller writes to the plot's Wonder score. The
// other caller (3e3edd) passes (-9999,-9999) for ordinary buildings/repairs.
constexpr uintptr_t BuildingNetValueRva = 0x52a030;
constexpr uintptr_t WonderRowCallRva = 0x3e3a5a;
constexpr uintptr_t WonderRowReturnRva = 0x3e3a5f;
// Pinned XP2 hotfix ordinary city-building forecast caller. Unlike the
// Wonder path it supplies sentinel coordinates. Observation never edits it.
constexpr uintptr_t OrdinaryBuildingForecastCallRva = 0x3e379d;
constexpr uintptr_t OrdinaryBuildingForecastReturnRva = OrdinaryBuildingForecastCallRva + 5;
bool ordinary_forecast_ready{};
constexpr unsigned char BuildingNetPrefix[] = {
    0x4c, 0x89, 0x44, 0x24, 0x18, 0x89, 0x54, 0x24,
    0x10, 0x55, 0x53, 0x57, 0x41, 0x54, 0x41, 0x55,
};
using BuildingNetFn = int* (__cdecl *)(int*, unsigned, const void*, int, int, void*, void*, const int*);
BuildingNetFn original_building_net{};
bool wonder_ready{};
long wonder_changed{}, wonder_faults{};
long wonder_matched{}, wonder_boosted{};
std::array<int, 6> first_wonder_change{}, first_wonder_offer{};

auto wonder_offer_key(const WonderPlacementOffer& offer) {
    return std::tie(offer.player, offer.city, offer.building, offer.plot);
}

int wonder_placement_offer(const WonderPlacementOffer& key) {
    const auto end = policy.wonderOffers.begin() + policy.wonderOfferCount;
    const auto found = std::lower_bound(policy.wonderOffers.begin(), end, key,
        [](const auto& a, const auto& b) { return wonder_offer_key(a) < wonder_offer_key(b); });
    return found != end && wonder_offer_key(*found) == wonder_offer_key(key) ? found->deltaQ8 : 0;
}

int adjust_wonder_score(int nativeQ8, int requestedQ8) {
    // Let a working CSC chain compete with middling alternatives. One
    // twelfth of the legal row and an absolute 24 utility bound the swing.
    // Preserve all nonpositive/sentinel scores and leave headroom for the sum.
    if (nativeQ8 <= 0 || nativeQ8 > MaxScore || requestedQ8 <= 0)
        return nativeQ8;
    return nativeQ8 + (std::min)({requestedQ8, MaxWonderOfferQ8, nativeQ8 / 12});
}

int* __cdecl hook_building_net(int* out, unsigned building, const void* discount,
    int delay, int horizon, void* playerObject, void* city, const int* coords) {
    ActiveCall activity;
    WonderPlacementOffer key{};
    long epoch{};
    bool protect{};
    int requestedQ8{};
    const auto* caller = _ReturnAddress();
    if (ordinary_forecast_ready &&
        caller == Runtime::GetGameCoreGlobalAt<void*>(OrdinaryBuildingForecastReturnRva))
        observe_nomination_forecast(city, building);
    if (enabled.load() && caller ==
        Runtime::GetGameCoreGlobalAt<void*>(WonderRowReturnRva)) {
        std::shared_lock guard(policyMutex);
        if (enabled && (policy.protectWonders || policy.wonderOfferCount > 0) &&
            !wonder_faults && building <= INT_MAX &&
            readable(city, CityOwnerOffset + sizeof(int), false) && readable(coords, 2 * sizeof(int), false)) {
            const auto* c = static_cast<const unsigned char*>(city);
            const int player = read_field<int>(c, CityOwnerOffset);
            const int x = coords[0], y = coords[1];
            if (player >= 0 && player < MaxPlayers && policy.allowed[player] &&
                x >= 0 && x < policy.width && y >= 0 && y < policy.height) {
                key = {player, read_field<int>(c, CityIdOffset), static_cast<int>(building),
                    y * policy.width + x, 0};
                const BuilderAllowance supplier{player, key.city, key.plot, 0};
                protect = policy.protectWonders &&
                    supplier_at(supplier) != policy.builders.begin() + policy.builderCount;
                requestedQ8 = wonder_placement_offer(key);
                epoch = generation;
            }
        }
    }

    int* result = original_building_net(out, building, discount, delay, horizon, playerObject, city, coords);
    if (protect || requestedQ8 > 0) {
        std::unique_lock guard(policyMutex);
        // The original function can run across a commit or disable. Only the
        // still-current packet can edit its returned score or count a receipt.
        if (enabled && generation == epoch && !wonder_faults) {
            const int floor = *Runtime::GetGameCoreGlobalAt<const int*>(ImprovementScoreFloorRva);
            if (result != out || !readable(out, sizeof(*out), true) || (protect && floor >= 0)) {
                ++wonder_faults;
            } else {
                if (requestedQ8 > 0)
                    ++wonder_matched;
                const int before = *out;
                if (protect) {
                    if (before > floor) {
                        if (wonder_changed == 0)
                            first_wonder_change = {key.player, key.city, key.plot, key.building, before, floor};
                        ++wonder_changed;
                        *out = floor;
                    }
                } else {
                    const int after = adjust_wonder_score(before, requestedQ8);
                    if (after != before) {
                        if (wonder_boosted == 0)
                            first_wonder_offer = {key.player, key.city, key.plot, key.building, before, after};
                        ++wonder_boosted;
                        *out = after;
                    }
                }
            }
        }
    }
    return result;
}

bool install_wonder_hook(const unsigned char* base) {
    const auto* call = base + WonderRowCallRva;
    if (*call != X64CallRel32 || read_field<int32_t>(call, 1) !=
        static_cast<int32_t>(BuildingNetValueRva - WonderRowReturnRva) ||
        std::memcmp(base + BuildingNetValueRva, BuildingNetPrefix, sizeof BuildingNetPrefix))
        return false;
    const auto* forecastCall = base + OrdinaryBuildingForecastCallRva;
    ordinary_forecast_ready = *forecastCall == X64CallRel32 &&
        read_field<int32_t>(forecastCall, 1) == static_cast<int32_t>(
            BuildingNetValueRva - OrdinaryBuildingForecastReturnRva);
    using namespace Runtime;
    const HookSpec hooks[] = {
        DescribeHook("Wonder plot NetValue", GetGameCoreGlobalAt<BuildingNetFn>(BuildingNetValueRva),
            &hook_building_net, &original_building_net),
    };
    return InstallHookGroup(hooks);
}
