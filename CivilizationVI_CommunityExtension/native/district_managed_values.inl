// Caller-valued district investments. Lua publishes a managed type's default
// and sparse complete site bids; native callbacks only read those numbers.
// Refresh AFTER aggregation and BEFORE each cache consumer. Reservations remain
// owned by Civ: a live claim is a threshold to beat, not a write prohibition.
// Existing district foundations/repairs are deliberately outside this contract.
// All helpers below require policyMutex, borrow current pointers only, and never
// invoke Lua. Every bound derives from the published table or native 37-row cap.
enum class ManagedReason { Applied, NoMatrix, Ineligible, WrongCity, ReservationUnreadable,
    ScoreAlias, ScoreUnwritable, NativeInvalid, ExistingDistrict, FootprintUnavailable };
struct ManagedCoordinates { int x, y; };
using DistrictAtFn = unsigned char* (__cdecl *)(void*, const ManagedCoordinates*);
using LocationFn = void* (__cdecl *)(void*, int, unsigned char);
using RequestUpdateFn = bool (__cdecl *)(void*, void*);
// Current setter body reads RCX(wrapper), RDX(value*) only. It overwrites R8B
// before any use; old call-site/decompiler third arguments are not ABI inputs.
using CountDistrictTypeFn = int (__cdecl *)(void*, int, bool, bool);
using AssignRequestValueFn = void (__cdecl *)(void*, const int*);
LocationFn original_location{};
RequestUpdateFn original_request_update{};
#ifdef CE_AI_DISTRICT_SCORES_TEST
AssignRequestValueFn test_request_assignment{};
CountDistrictTypeFn test_count_district_type{};
DistrictAtFn test_district_at{};
#endif

// Read the real city's district collection, not cached plot-analysis flags.
// Row+50 means improvement; row+51 excludes unfinished/pillaged districts.
// A foundation must remain exempt even after it removes its old improvement.
// The existing repair helper11e520 resolves coordinates and imposes neither
// completion nor pillage gates. This getter invokes no AI hook/Lua callback.
int managed_footprint_type(void* object, int plot) {
    const auto* build=static_cast<const unsigned char*>(object);
    auto* city=read_field<unsigned char*>(build,CityBuildCityOffset);
    if (plot<0 || plot>=policy.width*policy.height || !readable(city,ManagedCityDistrictsOffset+sizeof(void*),false)) return INT_MIN;
#ifdef CE_AI_DISTRICT_SCORES_TEST
    const auto lookup=test_district_at;
#else
    const auto lookup=Runtime::GetGameCoreGlobalAt<DistrictAtFn>(DistrictAtRva);
#endif
    if (!lookup) return INT_MIN;
    const ManagedCoordinates xy{plot%policy.width,plot/policy.width};
    const auto* district=lookup(city+ManagedCityDistrictsOffset,&xy);
    if (!district) return -1;
    if (!readable(district,DistrictTypeOffset+sizeof(int),false)) return INT_MIN;
    const int type=read_field<int>(district,DistrictTypeOffset);
    return type>=0 && type<policy.districts ? type : INT_MIN;
}

void managed_rejection(ManagedDistrictType& scope, const Entry& key, ManagedReason reason) {
    ++scope.failures;
    scope.lastCity = key.city; scope.lastPlot = key.plot;
    scope.lastReason = static_cast<int>(reason);
}
int* managed_cell(void* object, const DistrictMatrix& matrix, unsigned index, const Entry& key,
    ManagedDistrictType& scope) {
    const auto* row = matrix.rows + index * PlotAnalysisRowSize;
    scope.lastCity = key.city; scope.lastPlot = key.plot;
    if (read_field<int16_t>(row, RowOwnerOffset) != key.player || read_field<int>(row, RowCityIdOffset) != key.city) {
        managed_rejection(scope, key, ManagedReason::WrongCity); return nullptr;
    }
    const int footprint=managed_footprint_type(object,key.plot);
    if (footprint>=0 || footprint==INT_MIN) {
        managed_rejection(scope,key,footprint>=0 ? ManagedReason::ExistingDistrict : ManagedReason::FootprintUnavailable); return nullptr;
    }
    if (!row[RowDistrictEligibleOffset] || !(row[RowOwnedOffset] || row[RowFrontierOffset])) {
        managed_rejection(scope, key, ManagedReason::Ineligible); return nullptr;
    }
    const auto* reservation = read_field<const int*>(row, RowReservationOffset);
    if (!readable(reservation, sizeof(int), false)) {
        managed_rejection(scope, key, ManagedReason::ReservationUnreadable); return nullptr;
    }
    scope.lastReservation = *reservation; // ID alone does not establish a live claim.
    auto* expected = matrix.scores + index * policy.districts;
    if (read_field<int*>(row, RowDistrictScoresOffset) != expected) {
        managed_rejection(scope, key, ManagedReason::ScoreAlias); return nullptr;
    }
    auto* cell = expected + key.district;
    if (!readable(cell, sizeof(int), true)) {
        managed_rejection(scope, key, ManagedReason::ScoreUnwritable); return nullptr;
    }
    return cell;
}

void apply_managed_values(void* object, const Observation& o, ValuePhase phase, int onlyDistrict = -1) {
    if (!o.accepted || !enabled || generation != o.epoch || !policy.managedCount) return;
    const auto started = std::chrono::steady_clock::now();
    ++managed_passes;
    const auto matrix = read_matrix(object, o.key);
    for (int m = 0; enabled && m < policy.managedCount; ++m) {
        auto& scope = policy.managed[m];
        if (scope.player != o.key.player || (onlyDistrict >= 0 && scope.district != onlyDistrict)) continue;
        if (!matrix.rows) { managed_rejection(scope, o.key, ManagedReason::NoMatrix); continue; }
        for (unsigned i = 1; enabled && i < matrix.rowCount; ++i) {
            const auto* row = matrix.rows + i * PlotAnalysisRowSize;
            const int x = read_field<int>(row, RowXOffset), y = read_field<int>(row, RowYOffset);
            if (x < 0 || y < 0 || x >= policy.width || y >= policy.height) continue;
            Entry key{o.key.player, o.key.city, scope.district, y * policy.width + x, 0};
            ++scope.considered;
            auto* cell = managed_cell(object,matrix, i, key, scope);
            if (!cell) continue;
            if (*cell <= InvalidScore) { managed_rejection(scope, key, ManagedReason::NativeInvalid); continue; }
            const auto* entry = find_pair(key.player, key.city, key.district, key.plot);
            const int desired = entry && entry->finalReplacement ? entry->expectedFinal : scope.fallbackQ8;
            const int before = *cell;
            if (before != desired) {
                *cell = desired; ++managed_writes; ++scope.writes;
                if (scope.lastReservation != UnreservedPlot) ++scope.reservedWrites;
                if (!entry) ++scope.fallbackWrites;
                record_managed_trace(Final, key, before, desired, scope.lastReservation, phase);
            }
            ++matched; ++scope.applied;
            scope.lastReason = static_cast<int>(ManagedReason::Applied);
        }
    }
    managed_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
}

// Native GetBestDistrict can select a negative cell above its invalid floor.
// A nonpositive caller-owned investment means DEFER, so withhold only those
// managed columns for this comparison. Restore every byte before returning.
// The type-specific location reader uses the same transient mask. Existing
// districts bypass it so repair/progress never becomes a new-site refusal.
class ManagedDistrictMask {
public:
    ManagedDistrictMask(void* object, const Observation& o, int onlyDistrict = -1) {
        std::unique_lock guard(policyMutex);
        if (!o.accepted || !enabled || generation != o.epoch) return;
        const auto matrix = read_matrix(object, o.key);
        if (!matrix.rows) return;
        for (int m = 0; m < policy.managedCount; ++m) {
            auto& scope = policy.managed[m];
            if (scope.player != o.key.player || (onlyDistrict >= 0 && scope.district != onlyDistrict)) continue;
            for (unsigned i = 1; i < matrix.rowCount; ++i) {
                const auto* row = matrix.rows + i * PlotAnalysisRowSize;
                const int x = read_field<int>(row, RowXOffset), y = read_field<int>(row, RowYOffset);
                if (x < 0 || y < 0 || x >= policy.width || y >= policy.height) continue;
                Entry key{o.key.player, o.key.city, scope.district, y * policy.width + x, 0};
                auto* cell = managed_cell(object,matrix, i, key, scope);
                if (!cell) continue;
                const auto* entry = find_pair(key.player, key.city, key.district, key.plot);
                const bool acceptedPositive = entry && entry->finalReplacement && entry->expectedFinal > 0 && *cell == entry->expectedFinal;
                if (acceptedPositive) continue;
                if (count == saved.size()) { native_fault(); return; }
                saved[count++] = {cell, *cell}; *cell = InvalidScore - 1;
                ++scope.masked;
            }
        }
    }
    ~ManagedDistrictMask() { for (unsigned i = 0; i < count; ++i) *saved[i].cell = saved[i].value; }
    ManagedDistrictMask(const ManagedDistrictMask&) = delete;
    ManagedDistrictMask& operator=(const ManagedDistrictMask&) = delete;
private:
    struct Saved { int* cell; int value; };
    std::array<Saved, MaxCityPlotRows * MaxManagedTypesPerPlayer> saved{};
    unsigned count{};
};

void* select_protected_location(void*, const Observation&, int, unsigned char);

void* __cdecl hook_location(void* object, int district, unsigned char frontier) {
    ActiveCall activity;
    const Observation o = observe_enter(object);
    { std::unique_lock guard(policyMutex); apply_managed_values(object, o, ValuePhase::LocationSelection, district); }
    void* result;
    { const ManagedDistrictMask mask(object, o, district); result = select_protected_location(object, o, district, frontier); }
    if (result && o.accepted) {
        std::unique_lock guard(policyMutex);
        if (enabled && generation == o.epoch && district > 0 && district < policy.districts && readable(result, PlotYOffset + sizeof(int16_t), false)) {
            const auto* p = static_cast<const unsigned char*>(result);
            const int x = read_field<int16_t>(p, PlotXOffset), y = read_field<int16_t>(p, PlotYOffset);
            if (x >= 0 && y >= 0 && x < policy.width && y < policy.height)
                observe_cache(object, o, Location, district, y * policy.width + x);
        }
    }
    return result;
}

// Query the same native collection as DistrictLocation; do not infer an
// existing Quarter's type from the generic row.hasDistrict flag. Negative or
// unavailable answers leave retained requests native rather than pricing repair
// as a new investment. Test injection avoids calling any game address offline.
int managed_existing_count(void* object, int district) {
#ifdef CE_AI_DISTRICT_SCORES_TEST
    if (!test_count_district_type) return -1;
    const auto count = test_count_district_type;
#else
    const auto count = Runtime::GetGameCoreGlobalAt<CountDistrictTypeFn>(CountDistrictTypeRva);
#endif
    const auto* build = static_cast<const unsigned char*>(object);
    auto* city = read_field<unsigned char*>(build, CityBuildCityOffset);
    if (!readable(city, ManagedCityDistrictsOffset + sizeof(void*), false)) return -1;
    return count(city + ManagedCityDistrictsOffset, district, false, true);
}

bool __cdecl hook_request_update(void* contract, void* object) {
    ActiveCall activity;
    // Native Update returns AL (bool), not a full-width integer. It retains
    // max(previous bid,current cache). Rebase only validated caller-managed
    // district requests AFTER its original validation and ETA update. Building
    // and unit branches take precedence even if the district field is non--1.
    const auto o = observe_enter(object);
    const bool result = original_request_update(contract, object);
    std::unique_lock guard(policyMutex);
    if (!result || !o.accepted || !enabled || generation != o.epoch || !readable(contract, RequestReadableBytes, false)) return result;
    const auto* bytes = static_cast<const unsigned char*>(contract);
    if (read_field<int>(bytes, RequestBuildingOffset) != -1 || read_field<int>(bytes, RequestUnitOffset) != -1) return result;
    const int district = read_field<int>(bytes, RequestDistrictOffset);
    auto* scope = find_managed(o.key.player, district);
    if (!scope) return result;
    const int x = read_field<int>(bytes, RequestCoordinatesOffset), y = read_field<int>(bytes, RequestCoordinatesOffset + sizeof(int));
    const auto matrix = read_matrix(object, o.key);
    if (!matrix.rows) return result;
    Entry key{o.key.player, o.key.city, district, -1, 0};
    int desired{}, reservation = INT_MIN;
    if (x < 0 || y < 0 || x >= policy.width || y >= policy.height) {
        // Native map lookup returns null for unspecified/invalid coordinates;
        // PresentValue(null) then uses max(0,column). Match that meaning, not
        // the negative per-site default. Withhold already-existing Quarters
        // (including foundations/repairs) through the native collection first.
        if (managed_existing_count(object, district) != 0) return result;
        apply_managed_values(object, o, ValuePhase::RequestRebase, district);
        original_present(object, &desired, district, nullptr);
    } else {
        key.plot = y * policy.width + x;
        bool found = false;
        for (unsigned i = 1; i < matrix.rowCount; ++i) {
            const auto* row = matrix.rows + i * PlotAnalysisRowSize;
            if (read_field<int>(row, RowXOffset) != x || read_field<int>(row, RowYOffset) != y) continue;
            auto* cell = managed_cell(object,matrix, i, key, *scope);
            if (!cell || *cell <= InvalidScore) return result;
            const auto* entry = find_pair(key.player, key.city, district, key.plot);
            desired = entry && entry->finalReplacement ? entry->expectedFinal : scope->fallbackQ8;
            reservation = scope->lastReservation; found = true; break;
        }
        if (!found) return result;
    }
    const int before = read_field<int>(bytes, RequestValueOffset);
    if (before == desired || !readable(static_cast<unsigned char*>(contract) + RequestValueOffset, sizeof(int), true)) return result;
    // Verified FAutoVariable assignment also registers its owner for change
    // tracking; a plain scalar store would bypass that engine bookkeeping.
    // It is a leaf operation with no Lua/policy callback. Keep admission locked
    // so a retired generation cannot write; no reservation/queue/ETA changes.
#ifdef CE_AI_DISTRICT_SCORES_TEST
    const auto assign = test_request_assignment;
#else
    const auto assign = Runtime::GetGameCoreGlobalAt<AssignRequestValueFn>(AssignRequestValueRva);
#endif
    if (assign) {
        assign(static_cast<unsigned char*>(contract) + RequestValueWrapperOffset, &desired);
        if (read_field<int>(bytes, RequestValueOffset) != desired) { native_fault(); return result; }
        ++scope->requestWrites;
        record_managed_trace(RequestValue, key, before, desired, reservation, ValuePhase::RequestRebase);
    }
    return result;
}

int l_managed_type(lua_State* L) {
    const int player = hks::checkinteger(L, 1), district = hks::checkinteger(L, 2), fallback = hks::checkinteger(L, 3);
    bool ok;
    {
        std::unique_lock guard(policyMutex);
        int samePlayer = 0;
        bool duplicate = false;
        for (int i = 0; i < staging.managedCount; ++i) {
            samePlayer += staging.managed[i].player == player;
            duplicate |= staging.managed[i].player == player && staging.managed[i].district == district;
        }
        ok = staging_valid && player >= 0 && player < MaxPlayers && staging.allowed[player] && district > 0 && district < staging.districts &&
            fallback >= MinimumScore && fallback <= 0 && staging.managedCount < MaxManagedTypes && samePlayer < MaxManagedTypesPerPlayer && !duplicate;
        if (ok) { staging.managed[staging.managedCount++] = ManagedDistrictType{player, district, fallback}; staging.hasLateEntries = true; }
        else staging_valid = false;
    }
    return integer_result(L, ok);
}
int l_managed_receipt(lua_State* L) {
    const int version = hks::checkinteger(L, 1), player = hks::checkinteger(L, 2), district = hks::checkinteger(L, 3);
    ManagedDistrictType copy{}; bool staged = false;
    long long passes{}, micros{};
    { std::shared_lock guard(policyMutex); const auto* scope = find_managed(player, district);
      if (version == policy.version && scope) { copy = *scope; staged = true; }
      passes = managed_passes; micros = managed_microseconds; }
    char output[1024];
    const int n = snprintf(output, sizeof output,
        "{\"staged\":%d,\"considered\":%ld,\"applied\":%ld,\"writes\":%ld,\"reservedWrites\":%ld,\"fallbackWrites\":%ld,\"masked\":%ld,\"requestWrites\":%ld,\"protected\":%ld,\"lastCity\":%d,\"lastPlot\":%d,\"lastReason\":%d,\"reservationId\":%d,\"passes\":%lld,\"microseconds\":%lld}",
        staged, copy.considered, copy.applied, copy.writes, copy.reservedWrites, copy.fallbackWrites, copy.masked, copy.requestWrites, copy.failures,
        copy.lastCity, copy.lastPlot, copy.lastReason, copy.lastReservation, passes, micros);
    if (n > 0 && n < static_cast<int>(sizeof output)) push_json(L, output, n); else push_json(L, "{}", 2);
    return 1;
}
