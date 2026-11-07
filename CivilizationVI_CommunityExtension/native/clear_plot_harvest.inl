// ClearPlotNode::Update uses the normal Win64 member-call ABI: node,
// player, tree. The qualified XP2 routine returns a state in EAX.
// AI-N097 and AI-C074 identify the ready/failed paths and the exact live
// fixture. This adapter runs the original worker selection and cleanup.
constexpr uintptr_t ClearPlotUpdateRva = 0x55eaf0;
constexpr uintptr_t ClearPlotReadyWorkerStoreRva = 0x55ed6d;
constexpr uintptr_t PlayerIsHumanRva = 0x2f9820;
constexpr size_t ClearTargetBindings = 0x20;
constexpr size_t ClearTargetIndex = 0x2c;
constexpr size_t ClearHarvestBindings = 0x30;
constexpr size_t ClearHarvestIndex = 0x3c;
constexpr size_t ClearSelectedWorker = 0x40;
constexpr size_t PlayerInstanceId = 0xd8;
constexpr size_t BindingCountOffset = 0x10;
constexpr size_t ClearTargetStride = 0xc;
constexpr size_t ClearHarvestStride = 8;
constexpr unsigned MaxClearBindings = 4096;
constexpr int ClearReady = 1;
constexpr int ClearFailed = 3;
constexpr int NoSelectedWorker = -1;

using ClearPlotUpdateFn = int (__cdecl *)(unsigned char*, const unsigned char*, void*);
ClearPlotUpdateFn original_clear_plot_update{};
bool harvest_ready{};
long harvest_rejected{}, harvest_faults{};
std::array<int, 3> first_harvest_rejection{};

const unsigned char* clear_binding(const unsigned char* node, size_t vectorOffset,
    size_t indexOffset, size_t stride) {
    const auto* vector = read_field<const unsigned char*>(node, vectorOffset);
    const auto index = read_field<unsigned>(node, indexOffset);
    if (index >= MaxClearBindings || !readable(vector, BindingCountOffset + sizeof(unsigned), false) ||
        index >= read_field<unsigned>(vector, BindingCountOffset))
        return nullptr;
    const auto* data = read_field<const unsigned char*>(vector, 0);
    if (!readable(data, (static_cast<size_t>(index) + 1) * stride, false))
        return nullptr;
    return data + index * stride;
}

bool protects_harvest(int player, int plot) {
    // Allowances sort by (player, plot, city, improvement). Clear Plot
    // has a player and target but no city. Match any protected site for
    // that owner without scanning the publication or depending on city IDs.
    const auto key = std::tie(player, plot);
    const auto first = policy.builders.begin();
    const auto end = first + policy.builderCount;
    const auto at = std::lower_bound(first, end, key, [](const auto& entry, const auto& site) {
        return std::tie(entry.player, entry.plot) < site;
    });
    return at != end && std::tie(at->player, at->plot) == key;
}

int apply_harvest_protection(unsigned char* node, int player, int nativeState) {
    if (nativeState != ClearReady || !readable(node, ClearSelectedWorker + sizeof(int), false))
        return nativeState;

    const auto* target = clear_binding(node, ClearTargetBindings, ClearTargetIndex, ClearTargetStride);
    if (!target)
        return nativeState;
    const int x = read_field<int>(target, 0);
    const int y = read_field<int>(target, sizeof(int));
    if (x < 0 || x >= policy.width || y < 0 || y >= policy.height)
        return nativeState;
    const int plot = y * policy.width + x;
    if (!protects_harvest(player, plot))
        return nativeState;

    // A normal improvement or repair may visit this node. Leave its
    // outcome alone unless the tree bound the harvest argument to true.
    const auto* harvest = clear_binding(node, ClearHarvestBindings, ClearHarvestIndex, ClearHarvestStride);
    if (!harvest || !*harvest)
        return nativeState;
    auto* worker = node + ClearSelectedWorker;
    const int id = read_field<int>(node, ClearSelectedWorker);
    if (id < 0 || !readable(worker, sizeof(id), true)) {
        ++harvest_faults;
        return nativeState;
    }

    // Match the engine failure contract. The original has already freed
    // its temporary worker list. The behavior tree owns release and retry.
    std::memcpy(worker, &NoSelectedWorker, sizeof(NoSelectedWorker));
    if (harvest_rejected++ == 0)
        first_harvest_rejection = {player, plot, id};
    return ClearFailed;
}

int __cdecl hook_clear_plot_update(unsigned char* node, const unsigned char* player, void* tree) {
    ActiveCall activity;
    int id = -1;
    long epoch{};
    if (enabled.load()) {
        std::shared_lock guard(policyMutex);
        if (enabled && policy.protectHarvests && !harvest_faults &&
            readable(player, PlayerInstanceId + sizeof(int), false)) {
            const int candidate = read_field<int>(player, PlayerInstanceId);
            if (candidate >= 0 && candidate < MaxPlayers && policy.allowed[candidate]) {
                id = candidate;
                epoch = generation;
            }
        }
    }
    const int result = original_clear_plot_update(node, player, tree);
    if (id >= 0 && result == ClearReady) {
        std::unique_lock guard(policyMutex);
        if (enabled && generation == epoch && !harvest_faults)
            return apply_harvest_protection(node, id, result);
    }
    return result;
}

bool install_harvest_hook(const unsigned char* base) {
    constexpr unsigned char prefix[] = {0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74,
        0x24, 0x18, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41};
    constexpr unsigned char workerStore[] = {0x41, 0x89, 0x47, 0x40};
    // IsHuman loads the same player ID at +0xd8 before consulting config.
    constexpr unsigned char playerIdRead[] = {0x48, 0x83, 0xec, 0x28, 0x8b, 0x91, 0xd8, 0x00, 0x00, 0x00};
    if (std::memcmp(base + ClearPlotUpdateRva, prefix, sizeof prefix) ||
        std::memcmp(base + ClearPlotReadyWorkerStoreRva, workerStore, sizeof workerStore) ||
        std::memcmp(base + PlayerIsHumanRva, playerIdRead, sizeof playerIdRead))
        return false;
    using namespace Runtime;
    const HookSpec hooks[] = {
        DescribeHook("Clear Plot harvest readiness", GetGameCoreGlobalAt<ClearPlotUpdateFn>(ClearPlotUpdateRva),
            &hook_clear_plot_update, &original_clear_plot_update),
    };
    return InstallHookGroup(hooks);
}
