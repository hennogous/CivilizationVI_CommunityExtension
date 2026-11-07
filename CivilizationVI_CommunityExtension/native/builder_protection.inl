// Improvement NetValue has a normal Win64 entry. Plot and city are explicit
// arguments. The qualified row evaluator stores the returned int unchanged.
constexpr uintptr_t ImprovementNetValueRva = 0x52a8c0;
constexpr uintptr_t ImprovementRowCallRva = 0x3e3423;
constexpr uintptr_t ImprovementRowReturnRva = 0x3e3428;
constexpr uintptr_t ImprovementScoreFloorRva = 0xb66cb0;
constexpr unsigned char ImprovementNetPrefix[] = {
    0x44, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44,
    0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56,
};
using ImprovementNetFn = int* (__cdecl *)(int*, unsigned, const void*, int, int, void*, void*);
ImprovementNetFn original_improvement_net{};
bool builder_ready{};
long builder_changed{}, builder_faults{};
std::array<int, 6> first_builder_change{};

auto builder_site(const BuilderAllowance& entry) {
    return std::tie(entry.player, entry.city, entry.plot);
}

bool builder_order(const BuilderAllowance& a, const BuilderAllowance& b) {
    return std::tie(a.player, a.city, a.plot, a.improvement) <
        std::tie(b.player, b.city, b.plot, b.improvement);
}

bool protected_improvement(const BuilderAllowance& key) {
    const auto first = policy.builders.begin();
    const auto end = first + policy.builderCount;
    const auto at = std::lower_bound(first, end, key, [](const auto& a, const auto& b) {
        return builder_site(a) < builder_site(b);
    });
    if (at == end || builder_site(*at) != builder_site(key))
        return false;
    const auto allowed = std::lower_bound(at, end, key, builder_order);
    return allowed == end || builder_order(key, *allowed);
}

int* __cdecl hook_improvement_net(int* out, unsigned improvement, const void* discount,
    int delay, int horizon, void* plot, void* city) {
    ActiveCall activity;
    BuilderAllowance key{};
    long epoch{};
    bool protect{};
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
                protect = protected_improvement(key);
                epoch = generation;
            }
        }
    }

    int* result = original_improvement_net(out, improvement, discount, delay, horizon, plot, city);
    if (protect) {
        std::unique_lock guard(policyMutex);
        if (enabled && generation == epoch && !builder_faults) {
            const int floor = *Runtime::GetGameCoreGlobalAt<const int*>(ImprovementScoreFloorRva);
            if (result != out || !readable(out, sizeof(*out), true) || floor >= 0) {
                ++builder_faults;
            }
            else if (*out > floor) {
                // This is the same sentinel the engine stores for an ineligible
                // improvement. The native ranking can still choose other rows.
                if (builder_changed == 0)
                    first_builder_change = {key.player, key.city, key.plot, key.improvement, *out, floor};
                ++builder_changed;
                *out = floor;
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
