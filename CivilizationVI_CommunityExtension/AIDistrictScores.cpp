/* CE district-site score API. Read native/README.md for publication and paging.
 *
 * There are two execution domains: gameplay Lua stages a complete numeric policy;
 * native AI jobs copy an admitted entry, run the original game routine, and apply
 * at most one adjustment if the publication generation still matches. AI hooks
 * never invoke Lua/gameplay APIs. The engine owns every object/output pointer;
 * CE owns only the fixed-size policy, samples and trace journal below.
 *
 * This module is compiled into CE. Its integration points must be called by CE's
 * process-once initializer and context lifecycle, not by an addon/DLL loader. */
#include "AIDistrictScores.h"
#include "Runtime.h"
#include "native/target_identity.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bcrypt.h>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <climits>
#include <tuple>
#include <intrin.h>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace AI::DistrictScores {
namespace {

using hks::lua_State;
using namespace GameCoreLayout;

constexpr int MaxEntries = 4096;
constexpr int MaxRepairCandidates = 256;
constexpr int MaxSupplierExclusions = 256;
constexpr int MaxPlayers = 64;
constexpr int MaxMapDimension = 256;
constexpr int MaxSamples = 64;
constexpr int TraceCapacity = 16384;
constexpr int TracePageSize = 64;
constexpr int Q8Units = 256;
constexpr int MaxBuilderBidQ8 = 64 * Q8Units;
constexpr int MaxNominationBidQ8 = 512 * Q8Units;
constexpr int InvalidScore = -9999 * Q8Units;
constexpr int MinimumScore = -4096 * Q8Units;
constexpr int MinDelta = -1024 * Q8Units;
constexpr int MaxDelta = 4096 * Q8Units;
constexpr int MaxScore = (std::numeric_limits<int>::max)() / 2;

struct Entry {
    int player, city, district, plot, delta;
    // A positive expected score marks a late entry. Early entries have no
    // expected aggregate and still flow through the original NetValue hook.
    int expectedBefore{}, expectedFinal{};
};
struct Sample {
    Entry key;
    int base, result;
};
// One or more compatible improvement IDs at a protected supplier site.
struct BuilderAllowance {
    int player, city, plot, improvement;
    int deltaQ8{};
};
struct BuildingNomination {
    int player, city, building, deltaQ8;
    int forecasts{}, comparisons{}, writes{};
};
struct RepairCandidate {
    int player, city, district, plot;
};
struct SupplierExclusion {
    int player, city, plot;
};
struct Policy {
    int version{}, width{}, height{}, districts{}, count{};
    bool hasLateEntries{};
    std::array<bool, MaxPlayers> allowed{};
    std::array<Entry, MaxEntries> entries{};
    int builderCount{};
    bool protectWonders{};
    bool protectHarvests{};
    std::array<BuilderAllowance, MaxEntries> builders{};
    int nominationCount{};
    std::array<BuildingNomination, MaxEntries> nominations{};
    int repairCount{};
    std::array<RepairCandidate, MaxRepairCandidates> repairs{};
    int exclusionCount{};
    std::array<SupplierExclusion, MaxSupplierExclusions> exclusions{};
};

void clear_policy(Policy& value) {
    // Old entries remain unreachable once count is zero. Avoid materializing
    // the entire fixed table as a temporary on the gameplay Lua stack.
    value.version = value.width = value.height = value.districts = value.count = value.builderCount = 0;
    value.nominationCount = value.repairCount = value.exclusionCount = 0;
    value.hasLateEntries = false;
    value.protectWonders = false;
    value.protectHarvests = false;
    value.allowed.fill(false);
}

// Gameplay Lua publishes a complete policy under this lock. A hook copies its
// entry before running the original function so no policy pointer crosses that
// call. HKS can raise errors by longjmp and is never called with a lock held.
std::shared_mutex policyMutex;
Policy staging, policy;
std::atomic<bool> enabled{};
std::atomic<long> calls{}, active{}, excludedSupplierRows{};
long generation{}, matched{}, changed{}, faults{};
bool staging_valid{}, hooks_ready{};
int sample_count{};
std::array<Sample, MaxSamples> samples{};

enum TraceKind { Net, Cache, Best, PresentValue, Late };
enum TraceReason { Applied = 0, ProtectedScore = 2, InvalidArithmetic, WriteBudget, LateInputMismatch, LateFinalMismatch };

struct TraceRecord {
    TraceKind kind;
    int generation, version, player, city, district, plot;
    int base, result, reason, eligible;
};
struct Observation {
    bool accepted{};
    bool hasLateEntries{};
    long epoch{};
    Entry key{};
};
std::array<TraceRecord, TraceCapacity> trace_records{};
int trace_count{};

// An original game routine may reenter another hook. Only the activity counter
// spans the call. Context reset deliberately leaves old active frames counted.
struct ActiveCall {
    ActiveCall() { ++active; }
    ~ActiveCall() { --active; }
    ActiveCall(const ActiveCall&) = delete;
    ActiveCall& operator=(const ActiveCall&) = delete;
};

using CacheFn = void (__cdecl *)(void*);
using BestFn = int (__cdecl *)(void*, int*);
using PresentFn = int* (__cdecl *)(void*, int*, int, void*);
using NetFn = int* (__cdecl *)(int*, unsigned, int*, int, int, void*, void*);

CacheFn original_cache{};
BestFn original_best{};
PresentFn original_present{};
NetFn original_net{};

// MASM forwards caller R12 and its return address as two extra arguments.
// NetValue itself receives its original seven arguments and returns out.
extern "C" int* AIDistrictScoreNetEntry(int*, unsigned, int*, int, int, void*, void*);

/* The qualified HKS formatter truncates a payload at 511 bytes; no free
 * lua_pushlstring export was found. Our JSON has fixed ASCII keys and integers,
 * no embedded NULs. A 240-byte literal fragment stays well below that limit.
 * Incremental concat needs only two temporary stack values, independent of JSON
 * length. HKS may raise an error here, so all callers release the policy lock first.
 * Always use "%s": the JSON must never become a printf formatting program. */
void push_json(lua_State* L, const char* text, size_t length) {
    char fragment[241];
    size_t offset = 0;
    hks::checkstack(L, 2, "CE district-score JSON");
    do {
        size_t count = length - offset;
        if (count > 240)
            count = 240;
        memcpy(fragment, text + offset, count);
        fragment[count] = 0;
        hks::pushfstring(L, "%s", fragment);
        if (offset)
            hks::concat(L, 2);
        offset += count;
    } while (offset < length);
}

template <typename Value>
Value read_field(const unsigned char* object, size_t offset) {
    Value value;
    std::memcpy(&value, object + offset, sizeof value);
    return value;
}

/* Validate the whole borrowed span within one committed, accessible region.
 * Rejecting a cross-region span is conservative. VirtualQuery is not a lifetime
 * lock: it cannot prevent unrelated engine code from freeing memory afterward.
 * Correct native-call ownership and the pinned-layout contract remain required. */
int readable(const void* p, size_t bytes, bool write) {
    MEMORY_BASIC_INFORMATION m;
    const auto start = reinterpret_cast<uintptr_t>(p);
    const auto end = start + bytes;
    if (!p || !bytes || end < start || VirtualQuery(p, &m, sizeof m) != sizeof m ||
        m.State != MEM_COMMIT || (m.Protect&PAGE_GUARD) || end > reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize)
        return 0;
    const DWORD mode = m.Protect & 0xff;
    return mode == PAGE_READWRITE || mode == PAGE_WRITECOPY || mode == PAGE_EXECUTE_READWRITE ||
        mode == PAGE_EXECUTE_WRITECOPY || (!write && (mode == PAGE_READONLY || mode == PAGE_EXECUTE_READ));
}

/* Default to the original value on every rejection. Use int64 for addition,
 * preserve native zero/invalid results, and never raise a low score while trying
 * to penalize it. INT_MAX/2 is conservative headroom for the known later 1.5
 * district preference factor, not a proof about every downstream computation. */
int adjusted(int base, int delta, int* result) {
    int64_t sum = static_cast<int64_t>(base) + delta;
    *result = base;
    if (!base || base <= InvalidScore || (delta < 0 && base <= MinimumScore))
        return 0;
    if (delta < 0 && sum < MinimumScore)
        sum = MinimumScore;
    /* Retain room for the native positive district preference factor. */
    if (sum > MaxScore || sum < INT_MIN || sum == InvalidScore)
        return 0;
    *result = static_cast<int>(sum);
    return 1;
}

/* Called only while the exclusive policy lock is held. Diagnostic exhaustion
 * disables further incentives rather than losing the relationship between a
 * native write and its evidence. Published policy is never changed by a hook. */
void native_fault(void) {
    faults++;
    enabled = false;
}
int trace_room(void) {
    if (trace_count >= TraceCapacity) {
        native_fault();
        return 0;
    }
    return 1;
}
void record_trace(TraceKind kind, Entry key, int district, int plot, int base, int result, int reason, int eligible) {
    if (!trace_room())
        return;
    trace_records[trace_count++] = TraceRecord{kind, static_cast<int>(generation), policy.version,
    key.player, key.city, district, plot, base, result, reason, eligible};
}
// Called with the exclusive lock and a validated score pointer. Record every
// attempted write before allowing the next change. A late result must remain a
// positive district opportunity within the qualified selection window.
void apply_score(TraceKind kind, const Entry& entry, int* score) {
    if (!trace_room())
        return;
    const int base = *score;
    int result = base;
    TraceReason reason = Applied;
    if (!adjusted(base, entry.delta, &result)) {
        reason = (!base || base <= InvalidScore || (entry.delta < 0 && base <= MinimumScore))
            ? ProtectedScore : InvalidArithmetic;
    }
    else if (kind == Late && result <= 0) {
        reason = InvalidArithmetic;
        result = base;
    }
    else if (changed >= MaxEntries) {
        native_fault();
        reason = WriteBudget;
        result = base;
    }
    else {
        *score = result;
        ++changed;
        if (sample_count < MaxSamples)
            samples[sample_count++] = Sample{entry, base, result};
    }
    record_trace(kind, entry, entry.district, entry.plot, base, result, reason, 1);
}

/* Both lookup helpers require the caller's shared/exclusive policy lock.
 * Returned addresses may be used only while that lock is held; hook admissions
 * copy the Entry so no policy-table pointer crosses an original game call. */
Entry* find_pair(int player, int city, int district, int plot) {
    if (player < 0 || player >= MaxPlayers || !policy.allowed[player])
        return nullptr;
    for (int i = 0; i < policy.count; i++) {
        Entry* e = &policy.entries[i];
        if (e->player == player && e->city == city && e->district == district && e->plot == plot)
            return e;
    }
    return nullptr;
}
Entry* find_city(int player, int city) {
    if (player < 0 || player >= MaxPlayers || !policy.allowed[player])
        return nullptr;
    for (int i = 0; i < policy.count; i++) {
        Entry* e = &policy.entries[i];
        if (e->player == player && e->city == city)
            return e;
    }
    return nullptr;
}
/* Admit read-only tracing only for a city represented in the current policy.
 * Borrow the City through CityBuild and recheck the owner and local city ID. */
Observation observe_enter(void* object, bool includeExclusions = false) {
    Observation o;
    ++calls;
    std::shared_lock guard(policyMutex);
    if (enabled && readable(object, CityBuildRowCountOffset + sizeof(unsigned), 0)) {
        unsigned char* city = read_field<unsigned char*>(static_cast<unsigned char*>(object), CityBuildCityOffset);
        if (readable(city, CityOwnerOffset + sizeof(int), 0)) {
            Entry* e = find_city(read_field<int>(city, CityOwnerOffset), read_field<int>(city, CityIdOffset));
            if (e) {
                o.accepted = true;
                o.hasLateEntries = policy.hasLateEntries;
                o.epoch = generation;
                o.key = *e;
            } else if (includeExclusions) {
                // An exclusion is independent of scoring. A city may still
                // compare cached candidates without a score entry this turn.
                const int player = read_field<int>(city, CityOwnerOffset);
                const int cityId = read_field<int>(city, CityIdOffset);
                const auto end = policy.exclusions.begin() + policy.exclusionCount;
                const auto site = std::find_if(policy.exclusions.begin(), end, [&](const auto& exclusion) {
                    return exclusion.player == player && exclusion.city == cityId;
                });
                if (site != end) {
                    o.accepted = true;
                    o.epoch = generation;
                    o.key = {player, cityId, 0, site->plot, 0};
                }
            }
        }
    }
    return o;
}
/* Called with the exclusive policy lock AFTER the original routine returned.
 * Recheck the city identity and bounded row/score spans. A publication generation
 * protects policy data, while the engine still owns the cache's lifetime.
 *
 * kind 1 records published pairs in the completed cache. kind 2 records the
 * actual winning pair even if it is another district in the scoped city. This
 * is a return-boundary observation, not an atomic whole-analysis snapshot. */
void observe_cache(void* object, Observation o, TraceKind kind, int district, int plot) {
    if (!enabled || generation != o.epoch)
        return;
    auto* b = static_cast<unsigned char*>(object);
    if (!readable(b, CityBuildRowCountOffset + sizeof(unsigned), 0)) {
        native_fault();
        return;
    }
    unsigned char* city = read_field<unsigned char*>(b, CityBuildCityOffset);
    if (!readable(city, CityOwnerOffset + sizeof(int), 0) || read_field<int>(city, CityOwnerOffset) != o.key.player || read_field<int>(city, CityIdOffset) != o.key.city) {
        native_fault();
        return;
    }
    unsigned n = read_field<unsigned>(b, CityBuildRowCountOffset);
    unsigned char* rows = read_field<unsigned char*>(b, CityBuildRowsOffset);
    if (n > MaxCityPlotRows || (n && !readable(rows, static_cast<size_t>(n) * PlotAnalysisRowSize, 0))) {
        native_fault();
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        unsigned char* row = rows + static_cast<size_t>(i) * PlotAnalysisRowSize;
        int x = read_field<int>(row, RowXOffset), y = read_field<int>(row, RowYOffset);
        if (x < 0 || y < 0 || x >= policy.width || y >= policy.height)
            continue;
        int index = y*policy.width + x;
        if (kind == Best && index != plot)
            continue;
        int* scores = read_field<int*>(row, RowDistrictScoresOffset);
        if (!readable(scores, static_cast<size_t>(policy.districts) * sizeof(int), 0)) {
            native_fault();
            return;
        }
        int eligible = !!(row[RowDistrictEligibleOffset] && (row[RowOwnedOffset] || row[RowFrontierOffset]));
        if (kind == Best) {
            record_trace(kind, o.key, district, plot, scores[district], scores[district], 0, eligible);
            return;
        }
        for (int j = 0; j < policy.count; j++) {
            Entry e = policy.entries[j];
            if (e.player == o.key.player && e.city == o.key.city && e.plot == index) {
                record_trace(kind, e, e.district, index, scores[e.district], scores[e.district], 0, eligible);
                if (!enabled)
                    return;
            }
        }
    }
}
// The row's native score pointer must alias the writable aggregation matrix.
// CE borrows those pointers only during one original call and revalidates
// matrix bounds, ownership, coordinates and the reservation at both edges.
struct DistrictMatrix {
    unsigned char* rows{};
    int* scores{};
    unsigned rowCount{};
    bool operator==(const DistrictMatrix&) const = default;
};

DistrictMatrix read_matrix(const void* object, const Entry& cityKey) {
    if (!readable(object, DistrictMatrixLengthOffset + sizeof(unsigned), false))
        return {};
    const auto* bytes = static_cast<const unsigned char*>(object);
    const auto* city = read_field<const unsigned char*>(bytes, CityBuildCityOffset);
    if (!readable(city, CityOwnerOffset + sizeof(int), false) ||
        read_field<int>(city, CityOwnerOffset) != cityKey.player ||
        read_field<int>(city, CityIdOffset) != cityKey.city)
        return {};

    const auto count = read_field<unsigned>(bytes, CityBuildRowCountOffset);
    if (count < 2 || count > MaxCityPlotRows)
        return {};
    auto* rows = read_field<unsigned char*>(bytes, CityBuildRowsOffset);
    auto* scores = read_field<int*>(bytes, DistrictMatrixDataOffset);
    const auto length = read_field<unsigned>(bytes, DistrictMatrixLengthOffset);
    const auto capacity = read_field<unsigned>(bytes, DistrictMatrixCapacityOffset);
    if (length != count * static_cast<unsigned>(policy.districts) || capacity < length ||
        capacity > MaxCityPlotRows * MaxEntries ||
        !readable(rows, count * PlotAnalysisRowSize, false) ||
        !readable(scores, length * sizeof(*scores), false))
        return {};
    return {rows, scores, count};
}

int* resolve_late_cell(const DistrictMatrix& matrix, unsigned rowIndex, const Entry& entry) {
    const auto* row = matrix.rows + rowIndex * PlotAnalysisRowSize;
    const int x = read_field<int>(row, RowXOffset);
    const int y = read_field<int>(row, RowYOffset);
    if (x < 0 || y < 0 || x >= policy.width || y >= policy.height ||
        y * policy.width + x != entry.plot || !row[RowDistrictEligibleOffset] || !row[RowOwnedOffset])
        return nullptr;
    const auto* reservation = read_field<const int*>(row, RowReservationOffset);
    auto* rowScores = matrix.scores + rowIndex * policy.districts;
    if (!readable(reservation, sizeof(*reservation), false) || *reservation != UnreservedPlot ||
        read_field<int*>(row, RowDistrictScoresOffset) != rowScores)
        return nullptr;
    auto* score = rowScores + entry.district;
    return readable(score, sizeof(*score), true) ? score : nullptr;
}

struct LateBatch {
    DistrictMatrix matrix;
    // Row zero is the city center, so zero marks an entry without an admission.
    // This bounded lookup allocates no heap in the worker.
    struct Candidate {
        uint8_t row{};
        bool matchedBefore{};
    };
    std::array<Candidate, MaxEntries> candidates{};
};

void capture_late_rows(void* object, const Observation& observation, LateBatch& batch) {
    std::shared_lock guard(policyMutex);
    if (!enabled || generation != observation.epoch || !policy.hasLateEntries)
        return;
    batch.matrix = read_matrix(object, observation.key);
    if (!batch.matrix.rows)
        return;
    for (int index = 0; index < policy.count; ++index) {
        const auto& entry = policy.entries[index];
        if (!entry.expectedBefore || entry.city != observation.key.city || entry.player != observation.key.player)
            continue;
        for (unsigned row = 1; row < batch.matrix.rowCount; ++row) {
            const auto* score = resolve_late_cell(batch.matrix, row, entry);
            if (score) {
                batch.candidates[index] = {static_cast<uint8_t>(row), *score == entry.expectedBefore};
                break;
            }
        }
    }
}

void apply_late_rows(void* object, const Observation& observation, const LateBatch& batch) {
    if (!batch.matrix.rows || !enabled || generation != observation.epoch ||
        read_matrix(object, observation.key) != batch.matrix)
        return;
    for (int index = 0; enabled && index < policy.count; ++index) {
        const auto candidate = batch.candidates[index];
        if (!candidate.row)
            continue;
        const auto& entry = policy.entries[index];
        auto* score = resolve_late_cell(batch.matrix, candidate.row, entry);
        if (!score)
            continue;
        if (!candidate.matchedBefore || *score != entry.expectedFinal) {
            record_trace(Late, entry, entry.district, entry.plot, *score, *score,
                candidate.matchedBefore ? LateFinalMismatch : LateInputMismatch, 1);
            continue;
        }
        ++matched;
        apply_score(Late, entry, score);
    }
}

// Keep the policy lock off the native call. Cache diagnostics follow the
// late write so they describe what downstream readers will see.
void calculate_with_late_policy(void* object, const Observation& observation) {
    LateBatch late;
    capture_late_rows(object, observation, late);
    original_cache(object);
    std::unique_lock guard(policyMutex);
    apply_late_rows(object, observation, late);
    observe_cache(object, observation, Cache, 0, 0);
}

void calculate_district_values(void* object, void* caller) {
    ActiveCall activity;
    const auto observation = observe_enter(object);
    if (observation.hasLateEntries && caller == Runtime::GetGameCoreGlobalAt<void*>(DistrictAggregationReturnRva)) {
        calculate_with_late_policy(object, observation);
        return;
    }
    original_cache(object);
    if (observation.accepted) {
        std::unique_lock guard(policyMutex);
        observe_cache(object, observation, Cache, 0, 0);
    }
}

void __cdecl hook_cache(void* object) {
    // The detour sees the original DoTurnActivate return address. Use the
    // intrinsic here, before forwarding through another C++ frame.
    calculate_district_values(object, _ReturnAddress());
}
// Only the ordinary joint selector is covered. Its stock body reads a bounded
// row vector, checks reservations and selects a winner. It does not rebuild
// rows or dispatch another city analysis. Restore the exact bytes before CE
// observes the winner or native district preparation consumes the cache.
class SupplierRowMask {
public:
    SupplierRowMask(void* object, const Observation& observation) {
        if (!observation.accepted)
            return;
        std::shared_lock guard(policyMutex);
        if (!enabled || generation != observation.epoch || !policy.exclusionCount)
            return;
        const auto first = policy.exclusions.begin();
        const auto last = first + policy.exclusionCount;
        const auto sameCity = [&](const SupplierExclusion& site) {
            return site.player == observation.key.player && site.city == observation.key.city;
        };
        if (std::none_of(first, last, sameCity))
            return;
        const auto matrix = read_matrix(object, observation.key);
        if (!matrix.rows || !readable(matrix.rows, matrix.rowCount * PlotAnalysisRowSize, true))
            return;
        for (unsigned index = 1; index < matrix.rowCount; ++index) {
            auto* row = matrix.rows + index * PlotAnalysisRowSize;
            const int x = read_field<int>(row, RowXOffset);
            const int y = read_field<int>(row, RowYOffset);
            auto* eligible = row + RowDistrictEligibleOffset;
            if (!*eligible || !row[RowOwnedOffset] || x < 0 || y < 0 || x >= policy.width || y >= policy.height ||
                read_field<int16_t>(row, RowOwnerOffset) != observation.key.player ||
                read_field<int>(row, RowCityIdOffset) != observation.key.city)
                continue;
            const int plot = y * policy.width + x;
            if (std::any_of(first, last, [&](const SupplierExclusion& site) {
                return sameCity(site) && site.plot == plot;
            }))
                saved[count++] = {eligible, *eligible};
        }
        for (unsigned index = 0; index < count; ++index)
            *saved[index].flag = 0;
        excludedSupplierRows.fetch_add(count, std::memory_order_relaxed);
    }

    ~SupplierRowMask() {
        for (unsigned index = 0; index < count; ++index)
            *saved[index].flag = saved[index].original;
    }
    SupplierRowMask(const SupplierRowMask&) = delete;
    SupplierRowMask& operator=(const SupplierRowMask&) = delete;

private:
    struct Saved { unsigned char* flag; unsigned char original; };
    std::array<Saved, MaxCityPlotRows> saved{};
    unsigned count{};
};

/* A -1 return is a legitimate no-winner result; coordinates are undefined then.
 * For a winner, read exactly two int32 coordinates and keep the district return
 * unchanged. Observing a winner does not establish a request or construction. */
int __cdecl hook_best(void* object, int* coordinates) {
    ActiveCall activity;
    Observation o = observe_enter(object, true);
    int result;
    {
        const SupplierRowMask mask(object, o);
        result = original_best(object, coordinates);
    }
    if (o.accepted) {
        std::unique_lock guard(policyMutex);
        if (enabled && generation == o.epoch) {
            if (result == -1)
                record_trace(Best, o.key, -1, -1, 0, 0, 0, 0);
            else if (result < 0 || result >= policy.districts || !readable(coordinates, 2 * sizeof(int), 0))
                native_fault();
            else {
                int x = coordinates[0], y = coordinates[1];
                if (x < 0 || y < 0 || x >= policy.width || y >= policy.height)
                    native_fault();
                else observe_cache(object, o, Best, result, y*policy.width + x);
            }
        }
    }
    return result;
}
/* Match the old observer's plot-specific scope. A null Plot* selects a different
 * native path/meaning and is intentionally not traced here. The original
 * routine must return its caller-owned out pointer; never substitute a pointer. */
int* __cdecl hook_present(void* object, int* out, int district, void* plot) {
    ActiveCall activity;
    Observation o = observe_enter(object);
    std::shared_lock guard(policyMutex);
    if (o.accepted && enabled && generation == o.epoch && district > 0 && district < policy.districts && readable(plot, PlotYOffset + sizeof(int16_t), 0)) {
        auto* p = static_cast<unsigned char*>(plot);
        int x = read_field<int16_t>(p, PlotXOffset), y = read_field<int16_t>(p, PlotYOffset);
        Entry* e = (x >= 0 && y >= 0 && x < policy.width && y < policy.height)?find_pair(o.key.player, o.key.city, district, y*policy.width + x) : nullptr;
        if (e)
            o.key = *e;
        else o.accepted = false;
    } else o.accepted = false;
    guard.unlock();
    int* returned = original_present(object, out, district, plot);
    if (o.accepted) {
        std::unique_lock guard(policyMutex);
        if (enabled && generation == o.epoch) {
            if (returned != out || !readable(out, sizeof(*out), 0))
                native_fault();
            else record_trace(PresentValue, o.key, district, o.key.plot, *out, *out, 0, 1);
        }
    }
    return returned;
}
} // namespace

/* Mutation is restricted to the ordinary caller returning to RVA 3e30be.
 * Other calls to NetValue retain native behavior. The shim supplies caller R12,
 * which is the plot-analysis row at this exact call site. Compare row coords,
 * owner/city IDs and native eligibility before copying an admitted Entry.
 *
 * After the original call, the copied generation must still match. Disable,
 * Begin, Commit or Reset may have retired it while the game routine ran. That
 * fence suppresses the OLD invocation's write; it does not make all rows in a
 * city-analysis pass use one policy or roll back saved cache/request influence. */

extern "C" int* AIDistrictScoreNet(int* out, unsigned district, int* time, int ready, int horizon,
    void* plot, void* city, unsigned char* row, void* caller) {
    Entry entry;
    bool found = false;
    long epoch = 0;
    ActiveCall activity;
    ++calls;
    if (enabled.load() && caller == Runtime::GetGameCoreGlobalAt<void*>(NetValueFromPlotAnalysisReturnRva)) {
        std::shared_lock guard(policyMutex);
        if (enabled && readable(city, CityOwnerOffset + sizeof(int), false) &&
            readable(plot, PlotYOffset + sizeof(int16_t), false) && readable(row, PlotAnalysisRowSize, false)) {
            const auto* cityBytes = static_cast<const unsigned char*>(city);
            const auto* plotBytes = static_cast<const unsigned char*>(plot);
            const int player = read_field<int>(cityBytes, CityOwnerOffset);
            const int cityId = read_field<int>(cityBytes, CityIdOffset);
            const int x = read_field<int16_t>(plotBytes, PlotXOffset);
            const int y = read_field<int16_t>(plotBytes, PlotYOffset);
            if (player >= 0 && player < MaxPlayers && policy.allowed[player] && x >= 0 && x < policy.width && y >= 0 && y < policy.height &&
                read_field<int>(row, RowXOffset) == x && read_field<int>(row, RowYOffset) == y && read_field<int16_t>(row, RowOwnerOffset) == player &&
                read_field<int>(row, RowCityIdOffset) == cityId && row[RowDistrictEligibleOffset] && (row[RowOwnedOffset] || row[RowFrontierOffset])) {
                Entry* e = find_pair(player, cityId, static_cast<int>(district), y*policy.width + x);
                if (e && !e->expectedBefore) {
                    entry = *e;
                    epoch = generation;
                    found = true;
                }
            }
        }
    }
    int* returned = original_net(out, district, time, ready, horizon, plot, city);
    if (found) {
        std::unique_lock guard(policyMutex);
        if (enabled && generation == epoch) {
            ++matched;
            if (returned != out || !readable(out, sizeof(*out), true))
                native_fault();
            else
                apply_score(Net, entry, out);
        }
    }
    return returned;
}

namespace { void observe_nomination_forecast(void* city, unsigned building); }
#include "native/builder_protection.inl"
#include "native/clear_plot_harvest.inl"

namespace {

int integer_result(lua_State* L, int value) {
    hks::pushinteger(L, value);
    return 1;
}
/* Convert every Lua argument BEFORE taking a lock: HKS checkinteger can longjmp
 * on an argument error. Consumers must catch errors and Disable explicitly.
 * Once conversion succeeds, Begin retires the old policy even if RANGE/version
 * validation fails. Argument errors before this point leave prior state intact. */
int l_begin(lua_State* L) {
    const int version = hks::checkinteger(L, 1);
    const int width = hks::checkinteger(L, 2);
    const int height = hks::checkinteger(L, 3);
    const int districtCount = hks::checkinteger(L, 4);
    bool ok;
    {
        std::unique_lock guard(policyMutex);
        enabled = false;
        generation++;
        ok = version > policy.version && width > 0 && width <= MaxMapDimension &&
            height > 0 && height <= MaxMapDimension && districtCount > 0 && districtCount <= MaxEntries;
        clear_policy(staging);
        staging_valid = ok;
        if (ok) {
            staging.version = version;
            staging.width = width;
            staging.height = height;
            staging.districts = districtCount;
        }
    }
    return integer_result(L, ok);
}
/* A rejected Allow/Entry poisons the staging batch. A later Commit cannot make a
 * partially validated table visible; the caller must start a new Begin. */
int l_allow(lua_State* L) {
    const int player = hks::checkinteger(L, 1);
    bool ok;
    {
        std::unique_lock guard(policyMutex);
        ok = staging_valid && player >= 0 && player < MaxPlayers;
        if (ok)
            staging.allowed[player] = true;
        else
            staging_valid = false;
    }
    return integer_result(L, ok);
}
/* City IDs are scoped to players; the full four-field key identifies a pair.
 * The caller owns original-human exclusion, legality and world-state freshness.
 * The native hook supplies additional identity/row guards, not an AI-role oracle. */
bool stage_entry(const Entry& entry, bool late) {
    std::unique_lock guard(policyMutex);
    bool ok = staging_valid && staging.count < MaxEntries && entry.player >= 0 && entry.player < MaxPlayers &&
        staging.allowed[entry.player] && entry.city >= 0 && entry.district > 0 && entry.district < staging.districts &&
        entry.plot >= 0 && entry.plot < staging.width * staging.height &&
        entry.delta >= MinDelta && entry.delta <= MaxDelta && entry.delta != 0;
    if (late) {
        const auto adjustedFinal = static_cast<int64_t>(entry.expectedFinal) + entry.delta;
        ok = ok && entry.expectedBefore > 0 && entry.expectedFinal >= entry.expectedBefore && entry.expectedFinal <= MaxScore &&
            adjustedFinal > 0 && adjustedFinal <= MaxScore;
    }
    for (int i = 0; ok && i < staging.count; ++i) {
        const auto& existing = staging.entries[i];
        if (existing.player == entry.player && existing.city == entry.city &&
            existing.district == entry.district && existing.plot == entry.plot)
            ok = false;
    }
    if (ok) {
        staging.entries[staging.count++] = entry;
        staging.hasLateEntries |= late;
    }
    else {
        staging_valid = false;
    }
    return ok;
}

int l_entry(lua_State* L) {
    const Entry entry{hks::checkinteger(L, 1), hks::checkinteger(L, 2), hks::checkinteger(L, 3),
        hks::checkinteger(L, 4), hks::checkinteger(L, 5)};
    return integer_result(L, stage_entry(entry, false));
}

bool stage_builder_allowance(const BuilderAllowance& entry) {
    bool ok;
    {
        std::unique_lock guard(policyMutex);
        ok = builder_ready && !builder_faults && staging_valid && staging.builderCount < MaxEntries &&
            entry.player >= 0 && entry.player < MaxPlayers && staging.allowed[entry.player] &&
            entry.city >= 0 && entry.plot >= 0 && entry.plot < staging.width * staging.height &&
            entry.improvement >= 0 && entry.deltaQ8 >= 0 && entry.deltaQ8 <= MaxBuilderBidQ8;
        if (ok)
            staging.builders[staging.builderCount++] = entry;
        else
            staging_valid = false;
    }
    return ok;
}

int l_builder_allow(lua_State* L) {
    const BuilderAllowance entry{hks::checkinteger(L, 1), hks::checkinteger(L, 2),
        hks::checkinteger(L, 3), hks::checkinteger(L, 4)};
    return integer_result(L, stage_builder_allowance(entry));
}

int l_builder_bid(lua_State* L) {
    const BuilderAllowance entry{hks::checkinteger(L, 1), hks::checkinteger(L, 2),
        hks::checkinteger(L, 3), hks::checkinteger(L, 4), hks::checkinteger(L, 5)};
    return integer_result(L, stage_builder_allowance(entry));
}

int l_builder_status(lua_State* L) {
    char output[400];
    {
        std::shared_lock guard(policyMutex);
        const auto& a = first_builder_change;
        const auto& b = first_builder_bid;
        snprintf(output, sizeof output,
            "{\"ready\":%d,\"entries\":%d,\"changed\":%ld,\"faults\":%ld,\"first\":[%d,%d,%d,%d,%d,%d],"
            "\"maxBidQ8\":%d,\"bids\":%ld,\"firstBid\":[%d,%d,%d,%d,%d,%d]}",
            static_cast<int>(builder_ready), policy.builderCount, builder_changed, builder_faults,
            a[0], a[1], a[2], a[3], a[4], a[5], MaxBuilderBidQ8, builder_bids,
            b[0], b[1], b[2], b[3], b[4], b[5]);
    }
    push_json(L, output, strlen(output));
    return 1;
}

int l_stage_harvest_protection(lua_State* L) {
    bool ok;
    {
        std::unique_lock guard(policyMutex);
        ok = harvest_ready && !harvest_faults && staging_valid && staging.builderCount > 0;
        staging.protectHarvests = ok;
        if (!ok)
            staging_valid = false;
    }
    return integer_result(L, ok);
}

int l_harvest_status(lua_State* L) {
    char output[256];
    {
        std::shared_lock guard(policyMutex);
        const auto& sample = first_harvest_rejection;
        snprintf(output, sizeof output,
            "{\"ready\":%d,\"protected\":%d,\"rejected\":%ld,\"faults\":%ld,\"first\":[%d,%d,%d]}",
            static_cast<int>(harvest_ready), static_cast<int>(policy.protectHarvests), harvest_rejected, harvest_faults,
            sample[0], sample[1], sample[2]);
    }
    push_json(L, output, strlen(output));
    return 1;
}

int l_stage_wonder_protection(lua_State* L) {
    bool ok;
    {
        std::unique_lock guard(policyMutex);
        ok = wonder_ready && !wonder_faults && staging_valid && staging.builderCount > 0;
        staging.protectWonders = ok;
        if (!ok)
            staging_valid = false;
    }
    return integer_result(L, ok);
}

int l_wonder_status(lua_State* L) {
    char output[400];
    {
        std::shared_lock guard(policyMutex);
        const auto& a = first_wonder_change;
        snprintf(output, sizeof output,
            "{\"ready\":%d,\"protected\":%d,\"changed\":%ld,\"faults\":%ld,\"first\":[%d,%d,%d,%d,%d,%d]}",
            static_cast<int>(wonder_ready), static_cast<int>(policy.protectWonders), wonder_changed, wonder_faults,
            a[0], a[1], a[2], a[3], a[4], a[5]);
    }
    push_json(L, output, strlen(output));
    return 1;
}

int l_late_entry(lua_State* L) {
    // Public argument order follows the calculation, then the delta.
    const int player = hks::checkinteger(L, 1);
    const int city = hks::checkinteger(L, 2);
    const int district = hks::checkinteger(L, 3);
    const int plot = hks::checkinteger(L, 4);
    const int before = hks::checkinteger(L, 5);
    const int final = hks::checkinteger(L, 6);
    const int delta = hks::checkinteger(L, 7);
    return integer_result(L, stage_entry({player, city, district, plot, delta, before, final}, true));
}

#include "native/building_nomination.inl"
#include "native/district_repair_admission.inl"

/* Only a ready, fault-free consumer may publish. Faults stay sticky until a
 * context reset. Clearing trace/samples here starts a new evidence window, so
 * callers must archive the retired window before committing a replacement. */
int l_supplier_exclusion(lua_State* L) {
    const SupplierExclusion site{hks::checkinteger(L, 1), hks::checkinteger(L, 2), hks::checkinteger(L, 3)};
    bool ok;
    {
        std::unique_lock guard(policyMutex);
        const auto end = staging.exclusions.begin() + staging.exclusionCount;
        ok = staging_valid && hooks_ready && staging.exclusionCount < MaxSupplierExclusions &&
            site.player >= 0 && site.player < MaxPlayers && staging.allowed[site.player] &&
            site.city >= 0 && site.plot >= 0 && site.plot < staging.width * staging.height &&
            std::none_of(staging.exclusions.begin(), end, [&](const auto& old) {
                return site.player == old.player && site.city == old.city && site.plot == old.plot;
            });
        if (ok)
            staging.exclusions[staging.exclusionCount++] = site;
    }
    return integer_result(L, ok);
}

int l_supplier_exclusion_status(lua_State* L) {
    char output[160];
    int length;
    {
        std::shared_lock guard(policyMutex);
        length = snprintf(output, sizeof output,
            "{\"ready\":%d,\"entries\":%d,\"capacity\":%d,\"masked\":%ld}",
            static_cast<int>(hooks_ready && !faults), policy.exclusionCount, MaxSupplierExclusions,
            excludedSupplierRows.load(std::memory_order_relaxed));
    }
    if (length > 0 && length < static_cast<int>(sizeof output))
        push_json(L, output, static_cast<size_t>(length));
    else
        push_json(L, "{}", 2);
    return 1;
}

int l_commit(lua_State* L) {
    bool ok;
    {
        std::unique_lock guard(policyMutex);
        ok = staging_valid && hooks_ready && !faults;
        if (ok) {
            const auto first = staging.builders.begin();
            const auto end = first + staging.builderCount;
            // Sort once. For shared suppliers keep the strongest bid on
            // an identical improvement. Stacking would exaggerate its value.
            std::sort(first, end, [](const auto& a, const auto& b) {
                if (builder_order(a, b)) return true;
                if (builder_order(b, a)) return false;
                return a.deltaQ8 > b.deltaQ8;
            });
            staging.builderCount = static_cast<int>(std::unique(first, end, [](const auto& a, const auto& b) {
                return !builder_order(a, b) && !builder_order(b, a);
            }) - first);
            finish_nominations(staging);
            finish_repairs(staging);
            policy = staging;
            nominations_published.store(policy.nominationCount > 0, std::memory_order_relaxed);
            repairs_published.store(policy.repairCount > 0, std::memory_order_relaxed);
            repair_considered = repair_admitted = repair_duplicates = repair_rejected = 0;
            first_repair = {};
            nomination_changed = 0;
            first_nomination_change = {};
            builder_changed = builder_bids = 0;
            first_builder_change = {};
            first_builder_bid = {};
            wonder_changed = 0;
            first_wonder_change = {};
            harvest_rejected = 0;
            first_harvest_rejection = {};
            changed = matched = sample_count = trace_count = 0;
            generation++;
            enabled = true;
        }
        staging_valid = false;
    }
    return integer_result(L, ok);
}
/* Retire without clearing the journal or published version. Advancing generation
 * prevents already admitted callbacks from appending/writing when they return.
 * They may still be executing their original game routine; this is not a drain. */
int l_disable(lua_State* L) {
    {
        std::unique_lock guard(policyMutex);
        enabled = false;
        staging_valid = false;
        generation++;
    }
    return integer_result(L, 1);
}
/* Serialize fixed-size numeric fields while holding a shared lock; release it
 * before touching HKS. 64 samples of six int32s fit comfortably in 16000 bytes.
 * Reassess that bound if adding variable strings or increasing sample count. */
int l_status(lua_State* L) {
    char output[16000];
    {
        std::shared_lock guard(policyMutex);
        int n = snprintf(
            output,
            sizeof output,
            "{\"abi\":1,\"ready\":%d,\"enabled\":%ld,\"generation\":%ld,\"version\":%d,\"entries\":%d,\"calls\":%ld,\"matched\":%ld,\"changed\":%ld,\"faults\":%ld,\"samples\":[",
            static_cast<int>(hooks_ready),
            static_cast<long>(enabled.load()),
            generation,
            policy.version,
            policy.count,
            calls.load(),
            matched,
            changed,
            faults
        );
        for (int i = 0; i < sample_count; i++) {
            Sample s = samples[i];
            n += snprintf(
                output + n,
                sizeof output - n,
                "%s[%d,%d,%d,%d,%d,%d]",
                i?",":"",
                s.key.player,
                s.key.city,
                s.key.district,
                s.key.plot,
                s.base,
                s.result
            );
        }
        snprintf(
            output + n,
            sizeof output - n,
            "],\"trace_count\":%d,\"trace_capacity\":%d,\"active\":%ld}",
            trace_count,
            TraceCapacity,
            active.load()
        );
    }
    push_json(L, output, strlen(output));
    return 1;
}
// Query the existing bounded journal for one published site before the next
// commit. Cache reads, exact score attempts, and stock winners are separate
// evidence. Only sparse callers should use this readback in normal play.
int l_district_receipt(lua_State* L) {
    const int version = hks::checkinteger(L, 1);
    const Entry key{hks::checkinteger(L, 2), hks::checkinteger(L, 3),
        hks::checkinteger(L, 4), hks::checkinteger(L, 5), 0};
    int staged = 0, scoreAttempts = 0, writes = 0, cacheReads = 0, cacheEligible = 0;
    int citySelections = 0, siteWins = 0, lastReason = -1;
    {
        std::shared_lock guard(policyMutex);
        staged = version == policy.version && find_pair(key.player, key.city, key.district, key.plot);
        if (staged) {
            for (int i = 0; i < trace_count; ++i) {
                const auto& record = trace_records[i];
                if (record.version != version || record.player != key.player || record.city != key.city)
                    continue;
                const bool sameSite = record.district == key.district && record.plot == key.plot;
                if (record.kind == Best) {
                    ++citySelections;
                    siteWins += sameSite;
                } else if (sameSite && (record.kind == Net || record.kind == Late)) {
                    ++scoreAttempts;
                    writes += record.base != record.result;
                    lastReason = record.reason;
                } else if (sameSite && record.kind == Cache) {
                    ++cacheReads;
                    cacheEligible = record.eligible;
                }
            }
        }
    }
    char output[320];
    const int length = snprintf(output, sizeof output,
        "{\"staged\":%d,\"scoreAttempts\":%d,\"writes\":%d,\"cacheReads\":%d,\"cacheEligible\":%d,"
        "\"citySelections\":%d,\"siteWins\":%d,\"lastReason\":%d}",
        staged, scoreAttempts, writes, cacheReads, cacheEligible, citySelections, siteWins, lastReason);
    if (length > 0 && length < static_cast<int>(sizeof output))
        push_json(L, output, static_cast<size_t>(length));
    else
        push_json(L, "{}", 2);
    return 1;
}

/* Non-consuming zero-based pages; expected generation prevents a caller from
 * accidentally joining records across a publication/context reset. Disable and
 * capture before replacement when a stable complete window is required. Disable
 * changes generation, so old callbacks may finish but cannot append or mutate. */
int l_trace_count(lua_State* L) {
    int count;
    {
        std::shared_lock guard(policyMutex);
        count = trace_count;
    }
    return integer_result(L, count);
}
/* Copy at most 64 records under the lock, then format/push outside it. Integer
 * field widths and fixed keys bound the JSON well below 32768 bytes. A stale
 * generation returns no records. While enabled, later callbacks may append
 * between pages; Disable first for a complete stable publication window. */
int l_trace_read(lua_State* L) {
    int offset = hks::checkinteger(L, 1), limit = hks::checkinteger(L, 2), expected = hks::checkinteger(L, 3);
    char output[32768];
    TraceRecord page[TracePageSize];
    int count = 0, total, epoch, version, stale;
    if (offset < 0 || offset > TraceCapacity || limit < 1 || limit > TracePageSize || expected < 0) {
        push_json(L, "{\"error\":\"invalid_trace_page\"}", strlen("{\"error\":\"invalid_trace_page\"}"));
        return 1;
    }
    {
        std::shared_lock guard(policyMutex);
        total = trace_count;
        epoch = generation;
        version = policy.version;
        stale = expected != epoch;
        if (!stale && offset < total) {
            count = total - offset;
            if (count > limit)
                count = limit;
            memcpy(page, trace_records.data() + offset, (size_t)count*sizeof *page);
        }
    }
    int n = snprintf(
        output,
        sizeof output,
        "{\"generation\":%d,\"version\":%d,\"total\":%d,\"offset\":%d,\"next\":%d,\"stale\":%s,\"records\":[",
        epoch,
        version,
        total,
        offset,
        offset + count,
        stale?"true":"false"
    );
    constexpr const char* kinds[] = {"net", "cache", "best", "pv", "late"};
    for (int i = 0; i < count; i++) {
        TraceRecord r = page[i];
        n += snprintf(
            output + n,
            sizeof output - n,
            "%s{\"k\":\"%s\",\"generation\":%d,\"version\":%d,\"player\":%d,\"city\":%d,\"district\":%d,\"plot\":%d,\"base\":%d,\"result\":%d,\"reason\":%d,\"eligible\":%d}",
            i?",":"",
            kinds[r.kind],
            r.generation,
            r.version,
            r.player,
            r.city,
            r.district,
            r.plot,
            r.base,
            r.result,
            r.reason,
            r.eligible
        );
    }
    snprintf(output + n, sizeof output - n, "]}");
    push_json(L, output, strlen(output));
    return 1;
}
// Each handle is owned by this qualification call. Release in reverse order;
// BCrypt's backing buffer must outlive the hash handle.
bool verify_file(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const std::unique_ptr<void, decltype(&CloseHandle)> ownedFile(file, CloseHandle);

    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;
    const std::unique_ptr<void, decltype([](void* handle) { BCryptCloseAlgorithmProvider(handle, 0); })>
        ownedAlgorithm(algorithm);

    DWORD objectSize{}, received{};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
        sizeof objectSize, &received, 0) < 0 || received != sizeof objectSize || !objectSize)
        return false;
    auto object = std::unique_ptr<UCHAR[]>(new (std::nothrow) UCHAR[objectSize]);
    if (!object)
        return false;
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(algorithm, &hash, object.get(), objectSize, nullptr, 0, 0) < 0)
        return false;
    const std::unique_ptr<void, decltype(&BCryptDestroyHash)> ownedHash(hash, BCryptDestroyHash);

    std::array<UCHAR, 65536> buffer;
    DWORD count{};
    do {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
            return false;
        if (count && BCryptHashData(hash, buffer.data(), count, 0) < 0)
            return false;
    } while (count);
    std::array<UCHAR, 32> digest;
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        return false;

    constexpr char digits[] = "0123456789abcdef";
    std::array<char, 64> hex;
    for (size_t i = 0; i < digest.size(); ++i) {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 0xf];
    }
    return std::equal(hex.begin(), hex.end(), GameCoreSha256);
}

/* Recovered function entries for the SHA-qualified Windows XP2 image. These exact
 * prefixes were checked by the previous four-hook experiment; mapped function
 * names alone are not signatures. A new GameCore build requires fresh ABI, field,
 * call-site and prefix qualification; do not merely update the file hash. */
const unsigned char fingerprint_cache[32] = {0x4c, 0x8b, 0xdc, 0x41, 0x55, 0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00, 0x83, 0xb9, 0xd8, 0x04, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0xe9, 0x0f, 0x84, 0xae, 0x03, 0x00, 0x00, 0x48, 0x8b, 0x91, 0xc8};
const unsigned char fingerprint_best[32] = {0x48, 0x89, 0x4c, 0x24, 0x08, 0x53, 0x55, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xec, 0x28, 0x4c, 0x8b, 0xea, 0x48, 0x8b, 0xe9, 0xe8, 0x73, 0xbe, 0x3e, 0x00, 0x48, 0x8b, 0xb5};
const unsigned char fingerprint_present[32] = {0x4d, 0x63, 0xd8, 0x4c, 0x8b, 0xd2, 0x48, 0x8b, 0xc1, 0x41, 0x83, 0xfb, 0xff, 0x75, 0x08, 0x33, 0xc9, 0x48, 0x8b, 0xc2, 0x89, 0x0a, 0xc3, 0x4d, 0x85, 0xc9, 0x74, 0x4d, 0x8b, 0x81, 0xd8, 0x04};
// Qualify all targets before CE's own registration hook changes the image.
// The registration prefix is an identity guard, not an additional hook.
bool install_hooks() {
    const auto* base = reinterpret_cast<const unsigned char*>(Runtime::GameCoreAddress);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    // An x64 CALL rel32 at DoTurnActivate must still target our aggregation hook.
    const auto* lateCall = base + DistrictAggregationCallRva;
    if (*lateCall != X64CallRel32 ||
        read_field<int32_t>(lateCall, 1) != static_cast<int32_t>(
            static_cast<intptr_t>(CalculateDistrictValuesRva) - static_cast<intptr_t>(DistrictAggregationReturnRva)))
        return false;
    if (pe->FileHeader.TimeDateStamp != GameCoreTimestamp || pe->OptionalHeader.SizeOfImage != GameCoreImageSize ||
        std::memcmp(base + RegisterScriptDataRva, fingerprint_register, sizeof fingerprint_register) ||
        std::memcmp(base + DistrictNetValueRva, fingerprint_net, sizeof fingerprint_net) ||
        std::memcmp(base + CalculateDistrictValuesRva, fingerprint_cache, sizeof fingerprint_cache) ||
        std::memcmp(base + GetBestDistrictRva, fingerprint_best, sizeof fingerprint_best) ||
        std::memcmp(base + DistrictPresentValueRva, fingerprint_present, sizeof fingerprint_present))
        return false;

    // Hooks and native originals are process-owned. Pin CE before installing
    // callbacks because live unload has no callback-drain protocol.
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&install_hooks), &pinned))
        return false;

    using namespace Runtime;
    const HookSpec hooks[] = {
        DescribeHook("District NetValue", GetGameCoreGlobalAt<NetFn>(DistrictNetValueRva), &AIDistrictScoreNetEntry, &original_net),
        DescribeHook("CalculateDistrictValues", GetGameCoreGlobalAt<CacheFn>(CalculateDistrictValuesRva), &hook_cache, &original_cache),
        DescribeHook("GetBestDistrict", GetGameCoreGlobalAt<BestFn>(GetBestDistrictRva), &hook_best, &original_best),
        DescribeHook("District PresentValueOf", GetGameCoreGlobalAt<PresentFn>(DistrictPresentValueRva), &hook_present, &original_present),
    };
    return InstallHookGroup(hooks);
}

/* Context boundary: retire admission before clearing policy/version/evidence.
 * Old callbacks keep only value copies and fail their generation check. Preserve
 * active/calls because old frames may still return and decrement active. This
 * does not undo engine-owned cached values or retained requests from the save. */
void reset_policy(void) {
    std::unique_lock guard(policyMutex);
    enabled = false;
    staging_valid = false;
    generation++;
    clear_policy(policy);
    changed = matched = sample_count = trace_count = faults = 0;
    builder_changed = builder_bids = builder_faults = wonder_changed = wonder_faults = 0;
    first_builder_bid = {};
    first_wonder_change = {};
    harvest_rejected = harvest_faults = 0;
    nominations_published.store(false, std::memory_order_relaxed);
    nomination_changed = nomination_faults = 0;
    repairs_published.store(false, std::memory_order_relaxed);
    repair_considered = repair_admitted = repair_duplicates = repair_rejected = repair_faults = 0;
    first_repair = {};
    first_nomination_change = {};
    first_harvest_rejection = {};
    first_builder_change = {};
}

#ifdef CE_AI_DISTRICT_SCORES_TEST
#include "native/tests/district_score_checks.inl"
#endif

} // namespace

#ifdef CE_AI_DISTRICT_SCORES_TEST
int SelfTest() { return run_self_test(); }
#endif

static bool has_lua_transport() {
    return hks::pushnamedcclosure && hks::setfield && hks::pushinteger &&
        hks::checkinteger && hks::pushfstring && hks::concat && hks::checkstack;
}

bool Create() {
    if (!has_lua_transport())
        return false;
    wchar_t path[32768];
    if (!Runtime::GameCore)
        return false;
    const auto length = GetModuleFileNameW(Runtime::GameCore, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path) || !verify_file(path))
        return false;
    hooks_ready = install_hooks();
    // The Builder boundary is optional. An unsupported prefix must not
    // disable the already qualified district interface.
    builder_ready = hooks_ready && install_builder_hook(reinterpret_cast<const unsigned char*>(Runtime::GameCoreAddress));
    wonder_ready = builder_ready && install_wonder_hook(reinterpret_cast<const unsigned char*>(Runtime::GameCoreAddress));
    harvest_ready = builder_ready && install_harvest_hook(reinterpret_cast<const unsigned char*>(Runtime::GameCoreAddress));
    nomination_ready = hooks_ready && install_nomination_hook(reinterpret_cast<const unsigned char*>(Runtime::GameCoreAddress));
    repair_ready = hooks_ready && install_repair_hook(reinterpret_cast<const unsigned char*>(Runtime::GameCoreAddress));
    return hooks_ready;
}

void RegisterLua(hks::lua_State* L) {
    if (!has_lua_transport())
        return;
    PushLuaMethod(L, l_begin, "CE_AIDistrictScoresBegin", hks::LUA_GLOBAL, "CE_AIDistrictScoresBegin");
    PushLuaMethod(L, l_allow, "CE_AIDistrictScoresAllow", hks::LUA_GLOBAL, "CE_AIDistrictScoresAllow");
    PushLuaMethod(L, l_entry, "CE_AIDistrictScoresEntry", hks::LUA_GLOBAL, "CE_AIDistrictScoresEntry");
    PushLuaMethod(L, l_builder_allow, "CE_AIBuilderProtectionAllow", hks::LUA_GLOBAL, "CE_AIBuilderProtectionAllow");
    PushLuaMethod(L, l_builder_status, "CE_AIBuilderProtectionStatus", hks::LUA_GLOBAL, "CE_AIBuilderProtectionStatus");
    PushLuaMethod(L, l_builder_bid, "CE_AIBuilderProtectionBid", hks::LUA_GLOBAL, "CE_AIBuilderProtectionBid");
    PushLuaMethod(L, l_stage_harvest_protection, "CE_AIBuilderHarvestProtectionStage", hks::LUA_GLOBAL, "CE_AIBuilderHarvestProtectionStage");
    PushLuaMethod(L, l_harvest_status, "CE_AIBuilderHarvestProtectionStatus", hks::LUA_GLOBAL, "CE_AIBuilderHarvestProtectionStatus");
    PushLuaMethod(L, l_stage_wonder_protection, "CE_AISupplierWonderProtectionStage", hks::LUA_GLOBAL, "CE_AISupplierWonderProtectionStage");
    PushLuaMethod(L, l_wonder_status, "CE_AISupplierWonderProtectionStatus", hks::LUA_GLOBAL, "CE_AISupplierWonderProtectionStatus");
    PushLuaMethod(L, l_late_entry, "CE_AIDistrictScoresLateEntry", hks::LUA_GLOBAL, "CE_AIDistrictScoresLateEntry");
    PushLuaMethod(L, l_nomination_bid, "CE_AIBuildingNominationBid", hks::LUA_GLOBAL, "CE_AIBuildingNominationBid");
    PushLuaMethod(L, l_nomination_status, "CE_AIBuildingNominationStatus", hks::LUA_GLOBAL, "CE_AIBuildingNominationStatus");
    PushLuaMethod(L, l_repair_stage, "CE_AIDistrictRepairStage", hks::LUA_GLOBAL, "CE_AIDistrictRepairStage");
    PushLuaMethod(L, l_repair_status, "CE_AIDistrictRepairStatus", hks::LUA_GLOBAL, "CE_AIDistrictRepairStatus");
    PushLuaMethod(L, l_supplier_exclusion, "CE_AISupplierExclusionStage", hks::LUA_GLOBAL, "CE_AISupplierExclusionStage");
    PushLuaMethod(L, l_supplier_exclusion_status, "CE_AISupplierExclusionStatus", hks::LUA_GLOBAL, "CE_AISupplierExclusionStatus");
    PushLuaMethod(L, l_commit, "CE_AIDistrictScoresCommit", hks::LUA_GLOBAL, "CE_AIDistrictScoresCommit");
    PushLuaMethod(L, l_disable, "CE_AIDistrictScoresDisable", hks::LUA_GLOBAL, "CE_AIDistrictScoresDisable");
    PushLuaMethod(L, l_status, "CE_AIDistrictScoresStatus", hks::LUA_GLOBAL, "CE_AIDistrictScoresStatus");
    PushLuaMethod(L, l_district_receipt, "CE_AIDistrictScoresReceipt", hks::LUA_GLOBAL, "CE_AIDistrictScoresReceipt");
    PushLuaMethod(L, l_nomination_receipt, "CE_AIBuildingNominationReceipt", hks::LUA_GLOBAL, "CE_AIBuildingNominationReceipt");
    PushLuaMethod(L, l_trace_count, "CE_AIDistrictScoresTraceCount", hks::LUA_GLOBAL, "CE_AIDistrictScoresTraceCount");
    PushLuaMethod(L, l_trace_read, "CE_AIDistrictScoresTraceRead", hks::LUA_GLOBAL, "CE_AIDistrictScoresTraceRead");
}

void Reset() {
    reset_policy();
}
} // namespace AI::DistrictScores
