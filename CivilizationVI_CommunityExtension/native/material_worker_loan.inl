// Included in WorkerDispatch to reuse the qualified priority pass and roster.
// A loan gives an existing supply-restoration job a chance to recruit a held
// worker. CE can expose the worker to the native chooser. It issues no orders.
namespace Loan {

// Windows XP2 Steam 26595377, SHA-256 4de40abf... (AI-N104).
// DoGameCoreActions records the donor after it has had its turn to act.
// RecruitUnits is witnessed at the ordinary Find Available Unit call.
// ReleaseUnits returns the native vector through its second argument.
constexpr uintptr_t TreeActionsRva = 0x540020, RecruitUnitsRva = 0x403170;
constexpr uintptr_t RecruitFromFindReturnRva = 0x55527f, ReleaseUnitsRva = 0x543870;
constexpr size_t TreeName = 0x268, TreeReleaseAllowed = 0x180;
constexpr size_t PlotFeature = 0x3c, PlotResource = 0x40, PlotImprovement = 0x44;
constexpr size_t EvaluatorTarget = 0x10;
constexpr size_t ClearFindNode = 16, ClearMoveNode = 19, ClearExecuteNode = 20;
constexpr unsigned DistrictNodes = 35;
constexpr int Ready = 0, Success = 1;
constexpr char DistrictTree[] = "Build City District";
constexpr char ImprovementTree[] = "Build City Improvement";

using Actions = void (*)(const Byte*, int);
using Recruit = void (*)(void*, int, int, const Byte*, int, bool);
using Release = void* (*)(const Byte*, void*, int, int, bool, int);
Actions originalActions{};
Recruit originalRecruit{};
Release originalRelease{};
bool ready{};
Receipt loans{};
uint64_t micros{};
// Reset with the policy packet. Multiple manager passes can share a packet.
std::array<bool, MaxPlayers> attempted{};

struct IdleHolder {
    const Byte* tree{};
    int controller{-1};
};
struct Pass {
    MaterialWorkerLoan offer{{0, 0, 0, 0, -1, 0, 0, 0}, -1};
    const Byte* collection{};
    int width{}, turn{};
    long epoch{};
    // Only during this player's priority-2 traversal. No pointers or
    // evaluated state survive it, including across loads and captures.
    mutable std::array<IdleHolder, 128> holders{};
    mutable size_t count{};
    mutable bool attempted{};
};
struct TreeContext { const Byte* tree; };
struct Request { int controller; };
thread_local const Pass* current{};
thread_local const TreeContext* activeTree{};
thread_local const Request* request{};

template<size_t N> bool named_tree(const Byte* tree, const char (&name)[N]) {
    const auto* text = read_field<const char*>(tree, TreeName);
    return readable(text, N, false) && std::memcmp(text, name, N) == 0;
}

Pass loan_pass(int player, int turn, const Byte* collection) {
    Pass pass{};
    if (!loansPublished.load(std::memory_order_relaxed)) return pass;
    std::shared_lock guard(policyMutex);
    if (!enabled || !policy.allowed[player] || attempted[player]) return pass;
    const auto begin = policy.materialLoans.begin();
    const auto end = begin + policy.materialLoanCount;
    const auto at = std::find_if(begin, end, [player](const auto& row) { return row.worker.player == player; });
    if (at != end) {
        pass.offer = *at;
        pass.collection = collection;
        pass.width = policy.width;
        pass.turn = turn;
        pass.epoch = generation;
    }
    return pass;
}

bool has_no_clearance(const Byte* plot, int artifact) {
    if (!readable(plot, PlotImprovement + sizeof(int16_t), false) ||
        read_field<int16_t>(plot, PlotFeature) != -1 ||
        read_field<int16_t>(plot, PlotImprovement) != -1)
        return false;
    const int resource = read_field<int16_t>(plot, PlotResource);
    return resource == -1 || (artifact >= 0 && resource == artifact);
}

#ifdef CE_AI_DISTRICT_SCORES_TEST
bool test_clearance_gate() {
    std::array<Byte, PlotImprovement + sizeof(int16_t)> plot{};
    const auto set = [&](size_t offset, int16_t value) {
        std::memcpy(plot.data() + offset, &value, sizeof value);
    };
    set(PlotFeature, -1);
    set(PlotResource, -1);
    set(PlotImprovement, -1);
    if (!has_no_clearance(plot.data(), -1)) return false;
    set(PlotResource, 47);
    if (has_no_clearance(plot.data(), -1) || !has_no_clearance(plot.data(), 47)) return false;
    set(PlotFeature, 2);
    if (has_no_clearance(plot.data(), 47)) return false;
    set(PlotFeature, -1);
    set(PlotImprovement, 1);
    return !has_no_clearance(plot.data(), 47);
}
#endif

void actions(const Byte* tree, int priority) {
    originalActions(tree, priority);
    const auto* pass = current;
    if (!pass || !activeTree || activeTree->tree != tree || pass->offer.worker.unit < 0 ||
        pass->attempted || priority != WorkerPriority || pass->count == pass->holders.size() ||
        !readable(tree, ImprovementTreePlotOffset + sizeof(const Byte*), false) ||
        read_field<unsigned>(tree, TreeNodeCount) != DistrictNodes || !named_tree(tree, DistrictTree))
        return;
    const auto* states = read_field<const Byte*>(tree, TreeRoot);
    if (!readable(states, (ClearExecuteNode + 1) * sizeof(int), false) ||
        read_field<int>(states, ClearFindNode * sizeof(int)) != Success ||
        read_field<int>(states, ClearMoveNode * sizeof(int)) != Ready ||
        read_field<int>(states, ClearExecuteNode * sizeof(int)) != Ready ||
        !has_no_clearance(read_field<const Byte*>(tree, ImprovementTreePlotOffset), pass->offer.artifact))
        return;
    pass->holders[pass->count++] = {tree, read_field<int>(tree, TreeControl)};
}

bool is_restoration(const Pass& pass, const Byte* tree, int controller, const Byte* evaluator) {
    if (!readable(tree, ImprovementTreePlotOffset + sizeof(const Byte*), false) ||
        read_field<int>(tree, TreeControl) != controller || !named_tree(tree, ImprovementTree) ||
        read_field<int>(tree, ImprovementTreeKindOffset) != pass.offer.worker.improvement ||
        !readable(evaluator, EvaluatorTarget + sizeof(const Byte*), false))
        return false;
    const auto& offer = pass.offer.worker;
    const auto* city = read_field<const Byte*>(tree, ImprovementTreeCityOffset);
    const auto* plot = read_field<const Byte*>(tree, ImprovementTreePlotOffset);
    const auto* target = read_field<const Byte*>(evaluator, EvaluatorTarget);
    const int x = offer.plot % pass.width, y = offer.plot / pass.width;
    return readable(city, CityOwnerOffset + sizeof(int), false) &&
        read_field<int>(city, CityOwnerOffset) == offer.player && read_field<int>(city, CityIdOffset) == offer.city &&
        readable(plot, PlotYOffset + sizeof(int16_t), false) && read_field<int16_t>(plot, PlotXOffset) == x &&
        read_field<int16_t>(plot, PlotYOffset) == y &&
        readable(target, 2 * sizeof(int), false) && read_field<int>(target, 0) == x && read_field<int>(target, sizeof(int)) == y;
}

void recruit(void* manager, int priority, int controller, const Byte* evaluator, int value, bool overrideRelease) {
    const auto* pass = current;
    if (pass && activeTree && pass->count && !pass->attempted && !overrideRelease && priority == WorkerPriority &&
        _ReturnAddress() == Runtime::GetGameCoreGlobalAt<void*>(RecruitFromFindReturnRva) &&
        is_restoration(*pass, activeTree->tree, controller, evaluator)) {
        const Request scopeInfo{controller};
        ScopedContext scope(request, scopeInfo);
        originalRecruit(manager, priority, controller, evaluator, value, overrideRelease);
        return;
    }
    originalRecruit(manager, priority, controller, evaluator, value, overrideRelease);
}

const Byte* available_worker(const Pass& pass, int controller) {
    const auto& offer = pass.offer.worker;
    const auto* unit = find_unit(pass.collection, offer);
    if (!unit || read_field<int>(unit, UnitControl) != controller ||
        read_field<int>(unit, UnitX) != offer.x || read_field<int>(unit, UnitY) != offer.y ||
        read_field<uint8_t>(unit, UnitProcessed) || read_field<uint8_t>(unit, UnitDeparting) ||
        read_field<int>(unit, UnitCharges) <= 0)
        return nullptr;

    // Never change release for a controller that also holds another unit.
    // Native membership normally agrees with the unit's control ID. Recheck
    // the returned vector before allowing the ordinary evaluator to see it.
    auto* node = read_field<const Byte*>(pass.collection, UnitHead);
    for (int count = 0; node && count < MaxScan; ++count) {
        if (!readable(node, NextListNode + sizeof(const Byte*), false)) return nullptr;
        const auto* candidate = read_field<const Byte*>(node, ListObjectOffset);
        if (!readable(candidate, UnitControl + sizeof(int), false)) return nullptr;
        if (candidate != unit && read_field<int>(candidate, UnitControl) == controller) return nullptr;
        node = read_field<const Byte*>(node, NextListNode);
    }
    return node ? nullptr : unit;
}

void* release(const Byte* tree, void* output, int priority, int value, bool overrideRelease, int receiver) {
    const auto* pass = current;
    if (!request || request->controller != receiver || !pass || pass->attempted || overrideRelease ||
        priority != WorkerPriority)
        return originalRelease(tree, output, priority, value, overrideRelease, receiver);
    const auto begun = Clock::now();
    const auto begin = pass->holders.begin(), end = begin + pass->count;
    const auto holder = std::find_if(begin, end, [tree](const auto& row) { return row.tree == tree; });
    if (holder == end || holder->controller == receiver || !read_field<uint8_t>(tree, TreeReleaseAllowed) ||
        read_field<int>(tree, TreeControl) != holder->controller || read_field<int>(tree, TreePriority) != priority ||
        !has_no_clearance(read_field<const Byte*>(tree, ImprovementTreePlotOffset), pass->offer.artifact) ||
        !available_worker(*pass, holder->controller))
        return originalRelease(tree, output, priority, value, overrideRelease, receiver);

    const int holderWhole = read_field<int>(tree, TreeValue) / Q8Units;
    auto* vector = static_cast<Byte*>(output);
    constexpr size_t VectorCount = 0x10;
    if (value < 0 || holderWhole < value || !readable(vector, VectorCount + sizeof(unsigned), true))
        return originalRelease(tree, output, priority, value, overrideRelease, receiver);
    bool accepted = false;
    {
        std::unique_lock guard(policyMutex);
        auto& used = attempted[pass->offer.worker.player];
        accepted = enabled && generation == pass->epoch && !used;
        if (accepted) used = true;
    }
    if (!accepted)
        return originalRelease(tree, output, priority, value, overrideRelease, receiver);
    // The caller owns this native vector. The baseline would provide zero
    // IDs here. Raise only this comparison and expose only the verified ID.
    // Capacity and the native buffer lifetime are unchanged by reducing size.
    pass->attempted = true;
    void* result = originalRelease(tree, output, priority, holderWhole + 1, overrideRelease, receiver);
    bool retained = false;
    if (readable(vector, VectorCount + sizeof(unsigned), true)) {
        const unsigned count = read_field<unsigned>(vector, VectorCount);
        auto* ids = read_field<int*>(vector, 0);
        if (count && count <= MaxScan && readable(ids, count * sizeof(int), true)) {
            retained = std::find(ids, ids + count, pass->offer.worker.unit) != ids + count;
            if (retained) ids[0] = pass->offer.worker.unit;
        }
        const unsigned size = retained ? 1 : 0;
        std::memcpy(vector + VectorCount, &size, sizeof size);
    }
    {
        std::unique_lock guard(policyMutex);
        if (enabled && generation == pass->epoch) {
            micros += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begun).count();
            if (retained) {
                const auto& offer = pass->offer.worker;
                loans.add({offer.player, offer.city, offer.plot, offer.improvement,
                    offer.unit, holder->controller, receiver, pass->turn});
            }
        }
    }
    return result;
}

void Install(const Byte* base) {
    using Runtime::DescribeHook;
    using Runtime::GetGameCoreGlobalAt;
    if (!prefix(base, TreeActionsRva, std::to_array<Byte>({0x40,0x53,0x48,0x83,0xec,0x50,0x48,0x8b,0xd9,0x3b,0x91,0xe0,0x01,0x00,0x00})) ||
        !prefix(base, RecruitUnitsRva, std::to_array<Byte>({0x4d,0x85,0xc9,0x0f,0x84,0x51,0x06,0x00,0x00,0x48,0x8b,0xc4})) ||
        !prefix(base, RecruitFromFindReturnRva - 5, std::to_array<Byte>({0xe8,0xf1,0xde,0xea,0xff})) ||
        !prefix(base, ReleaseUnitsRva, std::to_array<Byte>({0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x55,0x57,0x41,0x54,0x41})))
        return;
    const Runtime::HookSpec hooks[] = {
        DescribeHook("Record idle district worker after actions", GetGameCoreGlobalAt<Actions>(TreeActionsRva), &actions, &originalActions),
        DescribeHook("Material restoration recruitment", GetGameCoreGlobalAt<Recruit>(RecruitUnitsRva), &recruit, &originalRecruit),
        DescribeHook("Release idle district worker to native chooser", GetGameCoreGlobalAt<Release>(ReleaseUnitsRva), &release, &originalRelease),
    };
    ready = Runtime::InstallHookGroup(hooks);
}

int stage_offer(lua_State* L) {
    const MaterialWorkerLoan offer{{hks::checkinteger(L, 1), hks::checkinteger(L, 2), hks::checkinteger(L, 3),
        hks::checkinteger(L, 4), hks::checkinteger(L, 5), hks::checkinteger(L, 6),
        hks::checkinteger(L, 7), hks::checkinteger(L, 8)}, hks::checkinteger(L, 9)};
    const auto& worker = offer.worker;
    std::unique_lock guard(policyMutex);
    const auto allowed = staging.builders.begin(), end = allowed + staging.builderCount;
    const auto loans = staging.materialLoans.begin(), loanEnd = loans + staging.materialLoanCount;
    const bool ok = staging_valid && ready && valid_player(worker.player) && staging.allowed[worker.player] &&
        worker.city >= 0 && worker.plot >= 0 && worker.plot < staging.width * staging.height &&
        worker.improvement >= 0 && worker.improvement < 4096 && worker.unit >= 0 &&
        worker.unitType >= 0 && worker.unitType < 4096 && offer.artifact >= -1 && offer.artifact < 4096 &&
        worker.x >= 0 && worker.x < staging.width && worker.y >= 0 && worker.y < staging.height &&
        staging.materialLoanCount < static_cast<int>(staging.materialLoans.size()) &&
        std::none_of(loans, loanEnd, [&](const auto& row) { return row.worker.player == worker.player; }) &&
        std::any_of(allowed, end, [&](const auto& row) {
            return row.player == worker.player && row.city == worker.city && row.plot == worker.plot &&
                row.improvement == worker.improvement && row.deltaQ8 > 0;
        });
    if (ok) staging.materialLoans[staging.materialLoanCount++] = offer;
    else staging_valid = false;
    guard.unlock();
    return integer_result(L, ok);
}

} // namespace Loan
