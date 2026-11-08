// Supplemental repair admission at the stock district nomination boundary.
// GameCore owns the new tree, controller, reservation, request and cleanup.
// This adapter stages identities only and supplies no score or priority delta.

constexpr uintptr_t CheckForDistrictRva = 0x3e0ee0;
constexpr uintptr_t LookupPlayerRva = 0x44f00;
constexpr uintptr_t EditMapRva = 0x44d40;
constexpr uintptr_t DistrictAtRva = 0x11e520;
constexpr uintptr_t CanZoneRva = 0x0f9d50;
constexpr uintptr_t FindBehaviorTreeRva = 0x564020;
constexpr uintptr_t CreateOneUseTreeRva = 0x563840;
constexpr uintptr_t DestroyTreeRva = 0x563960;
constexpr uintptr_t AddTreeControllerRva = 0x53feb0;
constexpr uintptr_t FindTreeControllerRva = 0x4019f0;
constexpr uintptr_t ConstructAnalysisRva = 0x48c8c0;
constexpr uintptr_t CopyTreeAnalysisRva = 0x0d6160;
constexpr uintptr_t FreeAnalysisBufferRva = 0x9908f0;
constexpr uintptr_t EmptyNativeStringRva = 0x993b30;
constexpr uintptr_t FormatNativeStringRva = 0x993930;
constexpr uintptr_t DestroyNativeStringRva = 0x993590;

constexpr size_t CityBuildPriorityOffset = 0x198, CityBuildDistrictTemplateOffset = 0x408;
constexpr size_t CityBuildDistrictSlotOffset = 0x430, DistrictSlotTreeIdOffset = 8;
constexpr size_t DistrictTemplateStringOffset = 0x10;
constexpr size_t CityDistrictsOffset = 0xb58, CityBuildQueueOffset = 0x10d0;
constexpr size_t DistrictTypeOffset = 0xf0, DistrictCompleteOffset = 0x3f8, DistrictPillagedOffset = 0x458;
constexpr size_t PlayerTreeManagerOffset = 0x6a8, PlayerControlManagerOffset = 0x6b0;
constexpr size_t MapPlotLookupVtableOffset = 0x50;
constexpr size_t ActiveTreeListOffset = 0x388, TreeListNextOffset = 0x10;
constexpr size_t TreeParentOffset = 0x120, TreeControllerIdOffset = 0x1b0, TreeIdOffset = 0x1c8;
constexpr size_t TreeControllerOffset = 0x218, ControllerBacklinkOffset = 0x10;
constexpr size_t TreeDisplayStringOffset = 0x250, TreeAnalysisOffset = 0x270;
constexpr size_t AutoVariableDataOffset = 0x10;
constexpr size_t AnalysisBonusOffset = 0x08, AnalysisBaseOffset = 0xc0;
constexpr size_t AnalysisDistrictOffset = 0x1c, AnalysisCityOffset = 0x70, AnalysisPlotOffset = 0x88;
constexpr size_t AnalysisVectorBufferOffset = 0x98, AnalysisSize = 0xe0;
constexpr int MaxActiveTreesToCheck = 2048;
constexpr char RepairTreeLabel[] = "CE Supplemental District Repair";

using CheckForDistrictFn = void (__cdecl *)(void*);
CheckForDistrictFn original_check_for_district{};
bool repair_ready{};
std::atomic<bool> repairs_published{};
long repair_considered{};
long repair_admitted{}, repair_duplicates{}, repair_rejected{}, repair_faults{};
std::array<int, 6> first_repair{};

struct RepairCoordinates { int x, y; };
enum class RepairResult { Rejected, Duplicate, Admitted, Fault };

auto repair_city_key(const RepairCandidate& entry) {
    return std::tie(entry.player, entry.city);
}

void finish_repairs(Policy& staged) {
    std::sort(staged.repairs.begin(), staged.repairs.begin() + staged.repairCount,
        [](const auto& a, const auto& b) { return repair_city_key(a) < repair_city_key(b); });
}

int district_slot_id(const unsigned char* build) {
    const auto* slot = read_field<const unsigned char*>(build, CityBuildDistrictSlotOffset);
    return readable(slot, DistrictSlotTreeIdOffset + sizeof(int), false)
        ? read_field<int>(slot, DistrictSlotTreeIdOffset) : INT_MIN;
}

template <typename Fn>
Fn repair_native(uintptr_t rva) {
    return Runtime::GetGameCoreGlobalAt<Fn>(rva);
}

// The manager rebuilds this active list on load. Compare the current trigger
// rather than retaining object pointers or CE-only IDs across saves. The native
// label marks a supplemental tree even if the requested footprint changes.
RepairResult check_active_repairs(const unsigned char* manager, const unsigned char* city,
    const void* plot, int district) {
    if (!readable(manager, ActiveTreeListOffset + sizeof(void*), false))
        return RepairResult::Fault;
    auto* node = read_field<const unsigned char*>(manager, ActiveTreeListOffset);
    for (int remaining = MaxActiveTreesToCheck; node; --remaining) {
        if (!remaining || !readable(node, TreeListNextOffset + sizeof(void*), false))
            return RepairResult::Fault;
        const auto* tree = read_field<const unsigned char*>(node, 0);
        constexpr size_t dataOffset = TreeAnalysisOffset + AutoVariableDataOffset;
        if (!readable(tree, dataOffset + AnalysisPlotOffset + sizeof(void*), false))
            return RepairResult::Fault;
        const auto* analysis = tree + dataOffset;
        if (read_field<const unsigned char*>(analysis, AnalysisCityOffset) == city) {
            if (read_field<int>(analysis, AnalysisDistrictOffset) == district &&
                read_field<const void*>(analysis, AnalysisPlotOffset) == plot)
                return RepairResult::Duplicate;
            const auto* label = read_field<const char*>(tree, TreeDisplayStringOffset);
            if (readable(label, sizeof RepairTreeLabel, false) &&
                std::memcmp(label, RepairTreeLabel, sizeof RepairTreeLabel) == 0)
                return RepairResult::Duplicate;
        }
        node = read_field<const unsigned char*>(node, TreeListNextOffset);
    }
    return RepairResult::Admitted;
}

// Mirror CheckForDistrict's native stack lifetimes for the temporary value
// objects. The tree factory copies the two strings and the analysis separately.
struct RepairAnalysis {
    alignas(16) std::array<unsigned char, AnalysisSize> bytes{};
    RepairAnalysis() { repair_native<void* (*)(void*)>(ConstructAnalysisRva)(bytes.data()); }
    ~RepairAnalysis() {
        if (auto* buffer = read_field<void*>(bytes.data(), AnalysisVectorBufferOffset))
            repair_native<void (*)(void*)>(FreeAnalysisBufferRva)(buffer);
    }
    RepairAnalysis(const RepairAnalysis&) = delete;
    RepairAnalysis& operator=(const RepairAnalysis&) = delete;
    template <typename T>
    void set(size_t offset, T value) {
        std::memcpy(bytes.data() + offset, &value, sizeof value);
    }
};

struct RepairLabel {
    void* value;
    RepairLabel() : value(repair_native<void* (*)()>(EmptyNativeStringRva)()) {
        repair_native<void (*)(void**, const char*, ...)>(FormatNativeStringRva)(&value, RepairTreeLabel);
    }
    ~RepairLabel() { repair_native<void (*)(void**)>(DestroyNativeStringRva)(&value); }
    RepairLabel(const RepairLabel&) = delete;
    RepairLabel& operator=(const RepairLabel&) = delete;
};

RepairResult admit_repair(unsigned char* build, unsigned char* city, const RepairCandidate& entry,
    int width, int stockId, std::array<int, 6>& receipt) {
    const RepairCoordinates xy{entry.plot % width, entry.plot / width};
    auto* district = repair_native<unsigned char* (*)(void*, const RepairCoordinates*)>(DistrictAtRva)(
        city + CityDistrictsOffset, &xy);
    if (!readable(district, DistrictPillagedOffset + 1, false) ||
        read_field<int>(district, DistrictTypeOffset) != entry.district ||
        !read_field<uint8_t>(district, DistrictCompleteOffset) || !read_field<uint8_t>(district, DistrictPillagedOffset))
        return RepairResult::Rejected;
    // These are the same arguments used by CityBuild::BuildDistrict to
    // establish the exact footprint's zoning/repair legality.
    if (!repair_native<bool (*)(void*, int, bool, const RepairCoordinates*, void*)>(CanZoneRva)(
        city + CityBuildQueueOffset, entry.district, false, &xy, nullptr))
        return RepairResult::Rejected;

    auto* player = repair_native<unsigned char* (*)(int)>(LookupPlayerRva)(entry.player);
    if (!readable(player, PlayerControlManagerOffset + sizeof(void*), false))
        return RepairResult::Fault;
    auto* manager = read_field<unsigned char*>(player, PlayerTreeManagerOffset);
    auto* control = read_field<unsigned char*>(player, PlayerControlManagerOffset);
    if (!readable(manager, ActiveTreeListOffset + sizeof(void*), false) || !control)
        return RepairResult::Fault;
    if (!repair_native<void* (*)(void*, int)>(FindBehaviorTreeRva)(manager, stockId))
        return RepairResult::Rejected;

    auto* map = repair_native<unsigned char* (*)()>(EditMapRva)();
    if (!readable(map, sizeof(void*), false))
        return RepairResult::Fault;
    const auto* table = read_field<const unsigned char*>(map, 0);
    if (!readable(table, MapPlotLookupVtableOffset + sizeof(void*), false))
        return RepairResult::Fault;
    using PlotLookupFn = void* (*)(void*, const RepairCoordinates*);
    const auto lookup = read_field<PlotLookupFn>(table, MapPlotLookupVtableOffset);
    const auto address = reinterpret_cast<uintptr_t>(lookup);
    if (address < Runtime::GameCoreAddress || address - Runtime::GameCoreAddress >= GameCoreImageSize)
        return RepairResult::Fault;
    auto* plot = lookup(map, &xy);
    if (!plot)
        return RepairResult::Rejected;

    const auto activeCheck = check_active_repairs(manager, city, plot, entry.district);
    if (activeCheck != RepairResult::Admitted)
        return activeCheck;
    int nativeQ8{};
    if (original_present(build, &nativeQ8, entry.district, plot) != &nativeQ8 ||
        nativeQ8 < Q8Units || nativeQ8 > MaxScore)
        return RepairResult::Rejected;

    const auto* definition = read_field<const unsigned char*>(build, CityBuildDistrictTemplateOffset);
    if (!readable(definition, DistrictTemplateStringOffset + sizeof(void*), false))
        return RepairResult::Fault;
    RepairAnalysis analysis;
    analysis.set(AnalysisDistrictOffset, entry.district);
    analysis.set(AnalysisCityOffset, city);
    analysis.set(AnalysisPlotOffset, plot);
    RepairLabel label;
    auto* tree = repair_native<unsigned char* (*)(void*, const void*, const void*)>(CreateOneUseTreeRva)(
        manager, definition + DistrictTemplateStringOffset, &label.value);
    if (!tree)
        return RepairResult::Rejected;

    if (!readable(tree, TreeAnalysisOffset + AutoVariableDataOffset + AnalysisSize, false))
        return RepairResult::Fault;
    const int treeId = read_field<int>(tree, TreeIdOffset);
    if (treeId < 0 || treeId == stockId)
        return RepairResult::Fault;
    const auto cancel = [&] {
        repair_native<void (*)(void*, int, int)>(DestroyTreeRva)(manager, treeId, -1);
        return RepairResult::Fault;
    };
    if (read_field<void*>(tree, TreeParentOffset))
        return cancel();
    repair_native<void (*)(void*, void*, int)>(AddTreeControllerRva)(
        tree, player, read_field<int>(build, CityBuildPriorityOffset));
    const int controllerId = read_field<int>(tree, TreeControllerIdOffset);
    const auto* owner = repair_native<const unsigned char* (*)(void*, int)>(FindTreeControllerRva)(control, controllerId);
    if (!readable(owner, ControllerBacklinkOffset + sizeof(void*), false) ||
        read_field<void*>(owner, ControllerBacklinkOffset) != tree + TreeControllerOffset)
        return cancel();
    repair_native<void (*)(void*, const void*, uint8_t)>(CopyTreeAnalysisRva)(
        tree + TreeAnalysisOffset, analysis.bytes.data(), static_cast<uint8_t>(xy.x));
    // The native decoder sees only this exact action and its current plot
    // value. A mismatched transfer is retired before the manager can run it.
    const auto* copied = tree + TreeAnalysisOffset + AutoVariableDataOffset;
    if (read_field<int>(copied, AnalysisDistrictOffset) != entry.district ||
        read_field<void*>(copied, AnalysisCityOffset) != city ||
        read_field<void*>(copied, AnalysisPlotOffset) != plot ||
        read_field<int>(copied, AnalysisBaseOffset) || read_field<int>(copied, AnalysisBonusOffset) ||
        district_slot_id(build) != stockId)
        return cancel();
    receipt = {entry.player, entry.city, entry.plot, treeId, controllerId, nativeQ8};
    return RepairResult::Admitted;
}

void hook_check_for_district(void* object) {
    auto* build = static_cast<unsigned char*>(object);
    RepairCandidate candidate{};
    unsigned char* city = nullptr;
    long epoch{};
    int width{};
    if (repairs_published.load(std::memory_order_relaxed) && enabled &&
        readable(build, CityBuildDistrictSlotOffset + sizeof(void*), false)) {
        city = read_field<unsigned char*>(build, CityBuildCityOffset);
        if (readable(city, CityOwnerOffset + sizeof(int), false)) {
            const int player = read_field<int>(city, CityOwnerOffset);
            const RepairCandidate key{player, read_field<int>(city, CityIdOffset), 0, 0};
            std::unique_lock guard(policyMutex);
            if (enabled && repair_ready && !repair_faults && player >= 0 && player < MaxPlayers && policy.allowed[player]) {
                const auto end = policy.repairs.begin() + policy.repairCount;
                const auto at = std::lower_bound(policy.repairs.begin(), end, key,
                    [](const auto& a, const auto& b) { return repair_city_key(a) < repair_city_key(b); });
                if (at != end && repair_city_key(*at) == repair_city_key(key)) {
                    ++repair_considered;
                    if (district_slot_id(build) == -1) {
                        candidate = *at;
                        epoch = generation;
                        width = policy.width;
                    }
                }
            }
        }
    }

    // Keep the stock nomination in charge of its slot. This first capability
    // runs only beside a newly selected stock tree, the verified native phase.
    original_check_for_district(object);
    if (!width)
        return;
    const int stockId = district_slot_id(build);
    if (stockId < 0)
        return;
    {
        std::shared_lock guard(policyMutex);
        if (!enabled || generation != epoch || repair_faults)
            return;
    }
    std::array<int, 6> receipt{};
    const auto result = admit_repair(build, city, candidate, width, stockId, receipt);
    std::unique_lock guard(policyMutex);
    if (generation != epoch)
        return;
    switch (result) {
    case RepairResult::Admitted:
        if (repair_admitted++ == 0) first_repair = receipt;
        break;
    case RepairResult::Duplicate: ++repair_duplicates; break;
    case RepairResult::Rejected: ++repair_rejected; break;
    case RepairResult::Fault: ++repair_faults; break;
    }
}

bool install_repair_hook(const unsigned char* base) {
    // The parent feature already verifies the original file hash, timestamp
    // and mapped PE image. These extra entry prefixes guard this capability
    // independently, so a failure leaves the other policies available.
    struct EntryPrefix { uintptr_t rva; std::array<unsigned char, 8> bytes; };
    const EntryPrefix prefixes[] = {
        {CheckForDistrictRva, {0x40,0x55,0x57,0x48,0x8d,0x6c,0x24,0xd8}},
        {LookupPlayerRva, {0x8b,0xd1,0x48,0x8b,0x0d,0x5f,0x5b,0xb4}},
        {EditMapRva, {0x48,0x8b,0x05,0x21,0x5d,0xb4,0x00,0x48}},
        {DistrictAtRva, {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c}},
        {CanZoneRva, {0x44,0x88,0x44,0x24,0x18,0x48,0x89,0x4c}},
        {FindBehaviorTreeRva, {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83}},
        {CreateOneUseTreeRva, {0x40,0x53,0x48,0x83,0xec,0x30,0x45,0x33}},
        {DestroyTreeRva, {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83}},
        {AddTreeControllerRva, {0x48,0x89,0x5c,0x24,0x10,0x44,0x89,0x44}},
        {FindTreeControllerRva, {0x44,0x8b,0xc2,0x83,0xfa,0xff,0x74,0x45}},
        {ConstructAnalysisRva, {0x83,0xc8,0xff,0x33,0xd2,0x89,0x01,0x89}},
        {CopyTreeAnalysisRva, {0x48,0x89,0x5c,0x24,0x18,0x57,0x48,0x83}},
        {FreeAnalysisBufferRva, {0x48,0xff,0x25,0x89,0x6d,0x02,0x00,0xcc}},
        {EmptyNativeStringRva, {0x48,0x83,0xec,0x28,0x65,0x48,0x8b,0x04}},
        {FormatNativeStringRva, {0x48,0x89,0x54,0x24,0x10,0x4c,0x89,0x44}},
        {DestroyNativeStringRva, {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b}},
    };
    for (const auto& prefix : prefixes)
        if (std::memcmp(base + prefix.rva, prefix.bytes.data(), prefix.bytes.size()) != 0)
            return false;
    const Runtime::HookSpec hooks[] = {
        Runtime::DescribeHook("CheckForDistrict", repair_native<CheckForDistrictFn>(CheckForDistrictRva),
            &hook_check_for_district, &original_check_for_district),
    };
    return Runtime::InstallHookGroup(hooks);
}

int l_repair_stage(lua_State* L) {
    const RepairCandidate entry{hks::checkinteger(L, 1), hks::checkinteger(L, 2),
        hks::checkinteger(L, 3), hks::checkinteger(L, 4)};
    bool ok;
    {
        std::unique_lock guard(policyMutex);
        const auto end = staging.repairs.begin() + staging.repairCount;
        ok = repair_ready && !repair_faults && staging_valid && staging.repairCount < MaxRepairCandidates &&
            entry.player >= 0 && entry.player < MaxPlayers && staging.allowed[entry.player] &&
            entry.city >= 0 && entry.district > 0 && entry.district < staging.districts &&
            entry.plot >= 0 && entry.plot < staging.width * staging.height &&
            std::none_of(staging.repairs.begin(), end, [&](const auto& item) {
                return repair_city_key(item) == repair_city_key(entry);
            });
        if (ok)
            staging.repairs[staging.repairCount++] = entry;
    }
    return integer_result(L, ok);
}

int l_repair_status(lua_State* L) {
    char output[320];
    int length;
    {
        std::shared_lock guard(policyMutex);
        const auto& a = first_repair;
        length = snprintf(output, sizeof output,
            "{\"ready\":%d,\"entries\":%d,\"maxCandidates\":%d,\"considered\":%ld,\"admitted\":%ld,\"duplicates\":%ld,"
            "\"rejected\":%ld,\"faults\":%ld,\"first\":[%d,%d,%d,%d,%d,%d]}",
            static_cast<int>(repair_ready), policy.repairCount, MaxRepairCandidates, repair_considered, repair_admitted,
            repair_duplicates, repair_rejected, repair_faults, a[0], a[1], a[2], a[3], a[4], a[5]);
    }
    if (length > 0 && length < static_cast<int>(sizeof output))
        push_json(L, output, static_cast<size_t>(length));
    else
        push_json(L, "{}", 2);
    return 1;
}
