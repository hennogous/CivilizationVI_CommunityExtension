// Optional ownership-prerequisite acknowledgement. Gameplay Lua stages numeric
// player/city/district scopes in the existing publication. The executing native
// tree supplies its own city, target and contract. No pointer survives a callback;
// saved plans reconstruct these identities through their deserialized triggers.
// This neither buys land, supplies funds, changes a score nor issues a build.
//
// Evidence: XP2 reference SHA324c51e9 (text-identical Steam092abfbb), AI-N118,
// CSC AI-C161. A paid plan stayed accepted (3), then timed out because ownership
// made Economic eligibility false. The diagnostic tracked3->7 assignment released
// its ordinary request and foundation. Reload remains a live qualification gate.
constexpr uintptr_t OwnershipDecoratorRva = 0x55a030;
constexpr uintptr_t OwnershipPurchaseVtableRva = 0x9fa930;
constexpr uintptr_t OwnershipMapLookupRva = 0x52d00;
constexpr uintptr_t OwnershipAttributesRva = 0x5f090;
constexpr uintptr_t OwnershipPoolLookupRva = 0x4c210;
constexpr uintptr_t OwnershipTrackedAssignRva = 0x64a6d0;
constexpr size_t OwnershipContractStateVariable = 0xa0, OwnershipContractState = 0xb0;
constexpr size_t OwnershipContractId = 0x110, OwnershipContractControl = 0x128;
constexpr size_t OwnershipContractPlayer = 0x140, OwnershipContractXY = 0x178;
constexpr size_t OwnershipTreeDistrict = 0x29c, OwnershipTreeCity = 0x2f0, OwnershipTreePlot = 0x308;
constexpr size_t OwnershipAttributeCity = 0x0c, OwnershipAttributePlayer = 0x1c;
constexpr int OwnershipWaiting = 3, OwnershipComplete = 7, OwnershipMaxParents = 32;

// Win64 AL return; preserve both register and stack arguments. The native
// decorator alone advances its child and cleans up handles after this hook.
using OwnershipDecoratorFn = bool (__cdecl *)(void*, void*, bool, bool, bool, uint32_t*);
OwnershipDecoratorFn original_ownership_decorator{};
bool ownership_ready{};
std::atomic<bool> ownership_published{};
long ownership_faults{};
struct OwnershipXY { int x, y; };

auto ownership_key(const OwnershipScope& s) { return std::tie(s.player, s.city, s.district); }
template <typename Table>
auto find_ownership_scope(Table& table, const OwnershipScope& key) {
    return std::lower_bound(table.ownership.begin(), table.ownership.begin() + table.ownershipCount, key,
        [](const auto& a, const auto& b) { return ownership_key(a) < ownership_key(b); });
}
void finish_ownership_scopes(Policy& staged) {
    auto begin = staged.ownership.begin(), end = begin + staged.ownershipCount;
    std::sort(begin, end, [](const auto& a, const auto& b) { return ownership_key(a) < ownership_key(b); });
    staged.ownershipCount = static_cast<int>(std::unique(begin, end, [](const auto& a, const auto& b) {
        return ownership_key(a) == ownership_key(b);
    }) - begin);
}

const unsigned char* ownership_root(const unsigned char* tree) {
    std::array<const unsigned char*, OwnershipMaxParents> seen{};
    for (int n = 0; tree && n < OwnershipMaxParents; ++n) {
        if (!readable(tree, TreeIdOffset + sizeof(int), false) ||
            std::find(seen.begin(), seen.begin() + n, tree) != seen.begin() + n)
            return nullptr;
        seen[n] = tree;
        auto* parent = read_field<const unsigned char*>(tree, TreeParentOffset);
        if (!parent) return tree;
        tree = parent;
    }
    return nullptr;
}

// The production path resolves these qualified native leaves. Explicit typed
// dependencies also let isolated native fixtures exercise the *same* guards
// and setter path without calling addresses in an unloaded game image.
struct OwnershipNativeCalls {
    void* (*getMap)();
    const unsigned char* (*lookupPlot)(const void*, const OwnershipXY*);
    const unsigned char* (*attributes)(const void*);
    void (*assignTracked)(void*, const int*);
};
OwnershipNativeCalls ownership_native_calls() {
    return {Runtime::GetGameCoreGlobalAt<void* (*)()>(EditMapRva),
        Runtime::GetGameCoreGlobalAt<const unsigned char* (*)(const void*, const OwnershipXY*)>(OwnershipMapLookupRva),
        Runtime::GetGameCoreGlobalAt<const unsigned char* (*)(const void*)>(OwnershipAttributesRva),
        Runtime::GetGameCoreGlobalAt<void (*)(void*, const int*)>(OwnershipTrackedAssignRva)};
}
void acknowledge_owned_purchase(void* rawHandle, void* rawTree, const OwnershipNativeCalls& native) {
    if (!ownership_published.load(std::memory_order_relaxed) ||
        !readable(rawHandle, sizeof(void*), false)) return;
    auto* handle = static_cast<unsigned char*>(rawHandle);
    auto* contract = read_field<unsigned char*>(handle, 0);
    if (!readable(contract, OwnershipContractXY + sizeof(OwnershipXY), false) ||
        read_field<const void*>(contract, 0) != Runtime::GetGameCoreGlobalAt<void*>(OwnershipPurchaseVtableRva) ||
        read_field<int>(contract, OwnershipContractState) != OwnershipWaiting) return;
    auto* tree = static_cast<const unsigned char*>(rawTree);
    if (!readable(tree, OwnershipTreePlot + sizeof(void*), false)) return;
    auto* city = read_field<const unsigned char*>(tree, OwnershipTreeCity);
    auto* triggerPlot = read_field<const unsigned char*>(tree, OwnershipTreePlot);
    if (!readable(city, CityOwnerOffset + sizeof(int), false) || !triggerPlot) return;
    OwnershipScope key{read_field<int>(city, CityOwnerOffset), read_field<int>(city, CityIdOffset),
        read_field<int>(tree, OwnershipTreeDistrict)};
    long epoch; int width, height;
    {
        std::shared_lock guard(policyMutex);
        if (!enabled || ownership_faults || key.player < 0 || key.player >= MaxPlayers ||
            !policy.allowed[key.player]) return;
        auto found = find_ownership_scope(policy, key);
        if (found == policy.ownership.begin() + policy.ownershipCount || ownership_key(*found) != ownership_key(key)) return;
        epoch = generation; width = policy.width; height = policy.height;
    }
    const auto* root = ownership_root(tree);
    const auto xy = read_field<OwnershipXY>(contract, OwnershipContractXY);
    if (!root || read_field<int>(root, TreeIdOffset) < 0 || read_field<int>(root, TreeControllerIdOffset) < 0 ||
        read_field<int>(contract, OwnershipContractId) < 0 || read_field<int>(contract, OwnershipContractPlayer) != key.player ||
        read_field<int>(contract, OwnershipContractControl) != read_field<int>(root, TreeControllerIdOffset) ||
        xy.x < 0 || xy.y < 0 || xy.x >= width || xy.y >= height) return;
    // These are pure getters. EditPlot overloads would perform archive editing
    // and must not be substituted. Map/attributes are checked before reads.
    const auto map = native.getMap();
    if (!map) return;
    const auto plot = native.lookupPlot(map, &xy);
    if (!plot || plot != triggerPlot || !readable(plot, PlotYOffset + sizeof(int16_t), false) ||
        read_field<int16_t>(plot, PlotXOffset) != xy.x || read_field<int16_t>(plot, PlotYOffset) != xy.y) return;
    const auto attributes = native.attributes(plot);
    if (!readable(attributes, OwnershipAttributePlayer + sizeof(int8_t), false)) return;
    const bool owned = read_field<int8_t>(attributes, OwnershipAttributePlayer) == key.player &&
        read_field<int>(attributes, OwnershipAttributeCity) == key.city;
    std::unique_lock guard(policyMutex);
    if (!enabled || generation != epoch || ownership_faults) return;
    auto found = find_ownership_scope(policy, key);
    if (found == policy.ownership.begin() + policy.ownershipCount || ownership_key(*found) != ownership_key(key)) return;
    ++found->considered;
    found->lastContract = read_field<int>(contract, OwnershipContractId);
    found->lastPlot = xy.y * width + xy.x;
    found->lastBefore = read_field<int>(contract, OwnershipContractState);
    found->lastAfter = found->lastBefore;
    if (!owned) { ++found->rejected; return; }
    ++found->owned;
    if (found->lastBefore != OwnershipWaiting) return;
    // This short tracked assignment is the sole exception to keeping native
    // calls outside the policy lock. Its audited body only inserts archive
    // dirtiness and assigns the scalar: no AI/HKS callback or notification.
    // Holding the lock here makes publication retirement atomic with the write.
    // The original decorator and pure lookups always run outside the lock.
    const int complete = OwnershipComplete;
    native.assignTracked(contract + OwnershipContractStateVariable, &complete);
    found->lastAfter = read_field<int>(contract, OwnershipContractState);
    if (found->lastAfter == OwnershipComplete) ++found->acknowledged;
    else ++ownership_faults; // No further acknowledgements until context reset.
}

bool __cdecl hook_ownership_decorator(void* handle, void* tree, bool timeout, bool wait, bool flag5, uint32_t* result) {
    ActiveCall activity;
    acknowledge_owned_purchase(handle, tree, ownership_native_calls());
    return original_ownership_decorator(handle, tree, timeout, wait, flag5, result);
}

bool install_ownership_hook(const unsigned char* base) {
    // Parent feature qualifies the full image. Independently qualify every new
    // call/dispatch layout; a mismatch disables only this optional capability.
    struct Prefix { uintptr_t rva; const char* bytes; size_t length; };
    const Prefix prefixes[] = {
        {OwnershipDecoratorRva, "\x48\x89\x5c\x24\x18\x55\x57\x41\x56\x48\x83\xec\x20", 13},
        {OwnershipMapLookupRva, "\x44\x8b\x02\x41\x81\xf8\xf1\xd8\xff\xff\x0f\x84\x92\x00\x00\x00", 16},
        {OwnershipAttributesRva, "\x0f\xb7\x49\x4e\xe9\x77\xd1\xfe\xff", 9},
        {OwnershipPoolLookupRva, "\x3b\x0d\x82\x52\xb1\x00\x48\x8b\x15\x6b\x52\xb1\x00", 13},
        {OwnershipTrackedAssignRva, "\x48\x89\x5c\x24\x18\x57\x48\x83\xec\x30\x48\x8b\x41\x08", 14},
        {EditMapRva, "\x48\x8b\x05\x21\x5d\xb4\x00\x48", 8},
    };
    for (const auto& prefix : prefixes)
        if (std::memcmp(base + prefix.rva, prefix.bytes, prefix.length) != 0) return false;
    const Runtime::HookSpec hooks[] = {
        Runtime::DescribeHook("Owned plot purchase prerequisite", Runtime::GetGameCoreGlobalAt<OwnershipDecoratorFn>(OwnershipDecoratorRva),
            &hook_ownership_decorator, &original_ownership_decorator),
    };
    return Runtime::InstallHookGroup(hooks);
}

int l_ownership_stage(lua_State* L) {
    const OwnershipScope entry{hks::checkinteger(L, 1), hks::checkinteger(L, 2), hks::checkinteger(L, 3)};
    bool ok;
    {
        std::unique_lock guard(policyMutex);
        ok = ownership_ready && !ownership_faults && staging_valid && staging.ownershipCount < MaxEntries &&
            entry.player >= 0 && entry.player < MaxPlayers && staging.allowed[entry.player] && entry.city >= 0 &&
            entry.district > 0 && entry.district < staging.districts;
        if (ok) staging.ownership[staging.ownershipCount++] = entry;
        else staging_valid = false;
    }
    return integer_result(L, ok);
}
int l_ownership_status(lua_State* L) {
    char output[256]; int length;
    {
        std::shared_lock guard(policyMutex);
        long considered = 0, acknowledged = 0;
        for (int i = 0; i < policy.ownershipCount; ++i) {
            considered += policy.ownership[i].considered;
            acknowledged += policy.ownership[i].acknowledged;
        }
        length = snprintf(output, sizeof output,
            "{\"ready\":%d,\"scopes\":%d,\"capacity\":%d,\"considered\":%ld,\"acknowledged\":%ld,\"faults\":%ld}",
            static_cast<int>(ownership_ready && !ownership_faults), policy.ownershipCount, MaxEntries, considered, acknowledged, ownership_faults);
    }
    if (length > 0 && length < static_cast<int>(sizeof output)) push_json(L, output, static_cast<size_t>(length));
    else push_json(L, "{}", 2);
    return 1;
}
int l_ownership_receipt(lua_State* L) {
    const int version = hks::checkinteger(L, 1);
    const OwnershipScope key{hks::checkinteger(L, 2), hks::checkinteger(L, 3), hks::checkinteger(L, 4)};
    OwnershipScope snapshot{}; bool staged = false;
    {
        std::shared_lock guard(policyMutex);
        if (version == policy.version) {
            auto found = find_ownership_scope(policy, key);
            staged = found != policy.ownership.begin() + policy.ownershipCount && ownership_key(*found) == ownership_key(key);
            if (staged) snapshot = *found;
        }
    }
    char output[384];
    const int length = snprintf(output, sizeof output,
        "{\"staged\":%d,\"considered\":%ld,\"owned\":%ld,\"acknowledged\":%ld,\"rejected\":%ld,\"contract\":%d,\"plot\":%d,\"before\":%d,\"after\":%d}",
        static_cast<int>(staged), snapshot.considered, snapshot.owned, snapshot.acknowledged, snapshot.rejected,
        snapshot.lastContract, snapshot.lastPlot, snapshot.lastBefore, snapshot.lastAfter);
    if (length > 0 && length < static_cast<int>(sizeof output)) push_json(L, output, static_cast<size_t>(length));
    else push_json(L, "{}", 2);
    return 1;
}
