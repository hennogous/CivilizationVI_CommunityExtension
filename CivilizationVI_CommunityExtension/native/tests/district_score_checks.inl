/* Isolated native tests, excluded from normal CE builds. Synthetic buffers and
 * stub originals exercise contracts without loading Civ. They cannot validate
 * real ABI/worker timing, HKS lifetime, mod coexistence or context destruction. */
int test_base, test_bump_epoch, test_integer;
char test_json[32768], test_previous[32768];
int test_stack_checks, test_concat_fault;
int* __cdecl test_net(int* out, unsigned d, int* t, int r, int h, void *p, void *c) {
    (void)d;
    (void)t;
    (void)r;
    (void)h;
    (void)p;
    (void)c;
    *out = test_base;
    if (test_bump_epoch)
        ++generation;
    return out;
}
int* cacheTestScore{};
int cacheTestFinal{};
int* cacheTestReservation{};
void __cdecl test_cache(void*) {
    if (cacheTestScore)
        *cacheTestScore = cacheTestFinal;
    if (cacheTestReservation)
        *cacheTestReservation = 1;
    if (test_bump_epoch)
        ++generation;
}
int __cdecl test_best(void* object, int* coordinates) {
    (void)object;
    coordinates[0] = 0;
    coordinates[1] = 3;
    return 8;
}
// Synthetic selector reads the actual borrowed cells while the RAII mask is
// active. Live tests still have to prove the real native choice/request.
int __cdecl test_decline_best(void* object, int* coordinates) {
    auto* bytes = static_cast<unsigned char*>(object);
    auto* data = read_field<int*>(bytes, DistrictMatrixDataOffset);
    auto* row = read_field<unsigned char*>(bytes, CityBuildRowsOffset) + PlotAnalysisRowSize;
    const auto* reservation = read_field<const int*>(row, RowReservationOffset);
    // This fixture treats a non--1 reservation ID as manager-resolved. Ties
    // take the admitted branch deterministically; real selection randomizes.
    const int threshold = *reservation == UnreservedPlot ? InvalidScore : reservation[1];
    int winner = -1, best = InvalidScore;
    for (int district : {8, 9}) {
        const int value = data[10 + district];
        if (threshold < value && best <= value) {
            best = value; winner = district;
            coordinates[0] = 0; coordinates[1] = 3;
        }
    }
    return winner;
}
int* __cdecl test_present(void* object, int* out, int district, void* plot) {
    (void)object;
    (void)district;
    (void)plot;
    *out = 3579;
    return out;
}
int managedAssignmentCalls{}, managedExistingCount{};
bool managedRequestSuccess = true;
int __cdecl test_managed_count(void*, int, bool, bool) { return managedExistingCount; }
unsigned char managedTestPlot[PlotYOffset + sizeof(int16_t)]{};
void __cdecl test_managed_assignment(void* wrapper, const int* value) {
    std::memcpy(static_cast<unsigned char*>(wrapper) + RequestValueOffset - RequestValueWrapperOffset, value, sizeof(*value));
    ++managedAssignmentCalls;
}
int* __cdecl test_managed_present(void*, int* out, int, void* plot) {
    *out = plot ? *cacheTestScore : (std::max)(0, *cacheTestScore); return out;
}
void* __cdecl test_managed_location(void* object, int, unsigned char) {
    auto* row = read_field<unsigned char*>(static_cast<unsigned char*>(object), CityBuildRowsOffset) + PlotAnalysisRowSize;
    if (row[RowHasDistrictOffset]) return managedTestPlot;
    const auto* claim = read_field<const int*>(row, RowReservationOffset);
    if (*claim != UnreservedPlot) return nullptr; // Native location reader skips live claims.
    return row[RowDistrictEligibleOffset] && *cacheTestScore > InvalidScore ? managedTestPlot : nullptr;
}
bool __cdecl test_managed_request(void* contract, void* object) {
    auto* bytes = static_cast<unsigned char*>(contract);
    const int district = read_field<int>(bytes, RequestDistrictOffset);
    if (!managedRequestSuccess) return false;
    if (district >= 0 && read_field<int>(bytes,RequestUnitOffset)==-1 && read_field<int>(bytes,RequestBuildingOffset)==-1) {
        const int x=read_field<int>(bytes,RequestCoordinatesOffset);
        int value{}; hook_present(object, &value, district, x<0 ? nullptr : managedTestPlot);
        const int old = read_field<int>(bytes, RequestValueOffset);
        const int retained = (std::max)(old, value);
        std::memcpy(bytes + RequestValueOffset, &retained, sizeof(retained));
    }
    if (test_bump_epoch) ++generation;
    return 1;
}
int __cdecl test_check(lua_State* L, int index) {
    return reinterpret_cast<int*>(L)[index - 1];
}
void __cdecl test_push_integer(lua_State* L, int value) {
    (void)L;
    test_integer = value;
}
const char* __cdecl test_push_string(lua_State* L, const char* format, ...) {
    (void)L;
    strcpy_s(test_previous, test_json);
    va_list a;
    va_start(a, format);
    vsnprintf(test_json, 512, format, a);
    va_end(a);
    return test_json;
}
void __cdecl test_concat(lua_State* L, int count) {
    (void)L;
    if (count != 2)
        test_concat_fault = 1;
    strcat_s(test_previous, test_json);
    strcpy_s(test_json, test_previous);
}
void __cdecl test_stack(lua_State* L, int count, const char* message) {
    (void)L;
    if (count != 2 || !message)
        test_concat_fault = 1;
    test_stack_checks++;
}
template <typename Value>
void put_field(unsigned char* buffer, size_t offset, Value value) {
    std::memcpy(buffer + offset, &value, sizeof value);
}

unsigned char* ownershipTestPlot{};
unsigned char* ownershipTestAttributes{};
int ownershipTestAssignments{};
void* ownership_test_map() { return ownershipTestPlot; }
const unsigned char* ownership_test_plot(const void*, const OwnershipXY* xy) {
    return xy->x == 2 && xy->y == 3 ? ownershipTestPlot : nullptr;
}
const unsigned char* ownership_test_attributes(const void*) { return ownershipTestAttributes; }
void ownership_test_assignment(void* variable, const int* value) {
    ++ownershipTestAssignments;
    // Fake the tracked native setter's scalar result, not a production write.
    put_field<int>(static_cast<unsigned char*>(variable), 0x10, *value);
}
int run_self_test() {
    int n = 0, v, out = 0, xy[2], params[3];
#define CHECK(c) do { n++; if(!(c)) { printf("district native test %d failed: %s\n",n,#c);return -n; } } while(0)
    CHECK(adjusted(1000, 256, &v) && v == 1256);
    CHECK(adjusted(1000, -256, &v) && v == 744);
    CHECK(!adjusted(0, 256, &v) && v == 0);
    CHECK(!adjusted(InvalidScore, 256, &v) && v == InvalidScore);
    CHECK(!adjusted(INT_MAX, 256, &v) && v == INT_MAX);
    CHECK(adjusted(-1048500, -256, &v) && v == MinimumScore);
    CHECK(!adjusted(-1048577, -256, &v) && v == -1048577);
    alignas(8) unsigned char object[DistrictMatrixLengthOffset + sizeof(unsigned)]{};
    alignas(8) unsigned char city[ManagedCityDistrictsOffset + sizeof(void*)]{};
    alignas(8) unsigned char rows[2 * PlotAnalysisRowSize]{};
    alignas(8) unsigned char plot[PlotYOffset + sizeof(int16_t)]{};
    auto* row = rows;
    int scores[10] = {0};
    put_field<void*>(object, CityBuildCityOffset, city);
    put_field<void*>(object, CityBuildRowsOffset, row);
    put_field<unsigned>(object, CityBuildRowCountOffset, 1);
    put_field<int>(city, CityOwnerOffset, 1);
    put_field<int>(city, CityIdOffset, 7);
    put_field<int16_t>(plot, PlotXOffset, 0);
    put_field<int16_t>(plot, PlotYOffset, 3);
    put_field<int>(row, RowXOffset, 0);
    put_field<int>(row, RowYOffset, 3);
    put_field<int16_t>(row, RowOwnerOffset, 1);
    put_field<int>(row, RowCityIdOffset, 7);
    row[RowDistrictEligibleOffset] = row[RowOwnedOffset] = 1;
    put_field<void*>(row, RowDistrictScoresOffset, scores);
    scores[8] = 2468;
    clear_policy(policy);
    policy.version = 4;
    policy.width = policy.height = 10;
    policy.districts = 10;
    policy.count = 2;
    policy.allowed[1] = 1;
    policy.entries[0] = Entry{1, 7, 8, 30, -65536};
    policy.entries[1] = Entry{1, 7, 9, 31, 1048576};
    generation = 10;
    enabled = true;
    faults = changed = matched = sample_count = trace_count = 0;
    active = 0;
    original_net = test_net;
    original_cache = test_cache;
    original_best = test_best;
    original_present = test_present;
    Runtime::GameCoreAddress = 0;
    auto* netCaller = Runtime::GetGameCoreGlobalAt<void*>(NetValueFromPlotAnalysisReturnRva);
    test_base = 5000;
    CHECK(AIDistrictScoreNet(&out, 8, NULL, 0, 0, plot, city, row, netCaller) == &out && out == -60536);
    CHECK(changed == 1 && trace_count == 1 && trace_records[0].kind == 0 && trace_records[0].reason == 0);
    CHECK(trace_records[0].generation == 10 && trace_records[0].version == 4 && trace_records[0].player == 1 && active == 0);
    test_base = 0;
    AIDistrictScoreNet(&out, 8, NULL, 0, 0, plot, city, row, netCaller);
    CHECK(out == 0 && changed == 1 && trace_count == 2 && trace_records[1].reason == 2);
    test_base = InvalidScore;
    AIDistrictScoreNet(&out, 8, NULL, 0, 0, plot, city, row, netCaller);
    CHECK(out == InvalidScore && trace_records[2].reason == 2);
    test_base = -1000000;
    AIDistrictScoreNet(&out, 8, NULL, 0, 0, plot, city, row, netCaller);
    CHECK(out == MinimumScore && trace_records[3].reason == 0);
    put_field<int16_t>(plot, PlotXOffset, 1);
    put_field<int>(row, RowXOffset, 1);
    test_base = 100;
    AIDistrictScoreNet(&out, 9, NULL, 0, 0, plot, city, row, netCaller);
    CHECK(out == 1048676 && trace_records[4].plot == 31 && trace_records[4].district == 9);
    test_base = INT_MAX;
    AIDistrictScoreNet(&out, 9, NULL, 0, 0, plot, city, row, netCaller);
    CHECK(out == INT_MAX && trace_records[5].reason == 3 && trace_count == 6);
    int before = trace_count;
    test_base = 100;
    test_bump_epoch = 1;
    AIDistrictScoreNet(&out, 9, NULL, 0, 0, plot, city, row, netCaller);
    test_bump_epoch = 0;
    CHECK(out == 100 && trace_count == before && active == 0);
    put_field<int>(city, CityOwnerOffset, 0);
    put_field<int16_t>(row, RowOwnerOffset, 0);
    AIDistrictScoreNet(&out, 9, NULL, 0, 0, plot, city, row, netCaller);
    CHECK(out == 100 && trace_count == before);
    put_field<int>(city, CityOwnerOffset, 1);
    put_field<int16_t>(row, RowOwnerOffset, 1);
    put_field<int16_t>(plot, PlotXOffset, 0);
    put_field<int>(row, RowXOffset, 0);
    hook_cache(object);
    CHECK(trace_count == before + 1 && trace_records[before].kind == 1 && trace_records[before].base == 2468);
    CHECK(hook_best(object, xy) == 8 && xy[0] == 0 && xy[1] == 3);
    CHECK(trace_count == before + 2 && trace_records[before + 1].kind == Best && trace_records[before + 1].district == 8);
    CHECK(hook_present(object, &out, 8, plot) == &out && out == 3579);
    CHECK(trace_count == before + 3 && trace_records[before + 2].kind == 3 && trace_records[before + 2].base == 3579);
    before = trace_count;
    hook_present(object, &out, 8, NULL);
    CHECK(trace_count == before && active == 0);
    hks::checkinteger = test_check;
    hks::pushfstring = test_push_string;
    hks::pushinteger = test_push_integer;
    hks::concat = test_concat;
    hks::checkstack = test_stack;
    char long_json[1501];
    memset(long_json, 'x', 1500);
    long_json[0] = '{';
    long_json[1] = '%';
    long_json[1499] = '}';
    long_json[1500] = 0;
    push_json(NULL, long_json, 1500);
    CHECK(strlen(test_json) == 1500 && !memcmp(test_json, long_json, 1500));
    CHECK(test_stack_checks == 1 && !test_concat_fault);
    push_json(NULL, "", 0);
    CHECK(test_json[0] == 0);
    params[0] = 0;
    params[1] = 2;
    params[2] = generation;
    CHECK(l_trace_read(reinterpret_cast<lua_State*>(params)) == 1 && strstr(test_json, "\"stale\":false") && strstr(test_json, "\"next\":2"));
    CHECK(strstr(test_json, "\"k\":\"net\"") && strstr(test_json, "\"result\":-60536"));
    params[0] = 2;
    params[1] = 1;
    CHECK(l_trace_read(reinterpret_cast<lua_State*>(params)) == 1 && strstr(test_json, "\"offset\":2") && strstr(test_json, "\"next\":3"));
    params[2] = generation - 1;
    CHECK(l_trace_read(reinterpret_cast<lua_State*>(params)) == 1 && strstr(test_json, "\"stale\":true") && strstr(test_json, "\"records\":[]"));
    params[1] = 65;
    CHECK(l_trace_read(reinterpret_cast<lua_State*>(params)) == 1 && strstr(test_json, "invalid_trace_page"));
    CHECK(l_trace_count(reinterpret_cast<lua_State*>(params)) == 1 && test_integer == trace_count);
    int exactSite[] = {4, 1, 7, 8, 30};
    CHECK(l_district_receipt(reinterpret_cast<lua_State*>(exactSite)) == 1 && strstr(test_json, "\"staged\":1") &&
        strstr(test_json, "\"scoreAttempts\":") && strstr(test_json, "\"cacheReads\":1"));
    exactSite[0] = 3;
    CHECK(l_district_receipt(reinterpret_cast<lua_State*>(exactSite)) == 1 && strstr(test_json, "\"staged\":0") &&
        strstr(test_json, "\"scoreAttempts\":0"));

    for (int i = 0; i < 64; i++)
        samples[i] = Sample{{1, 7, 8, i, 256}, 1000, 1256};
    sample_count = 64;
    CHECK(l_status(NULL) == 1 && strlen(test_json) > 512 && strstr(test_json, "\"trace_count\":") && test_json[strlen(test_json) - 1] == '}');
    for (int i = 0; i < 64; i++)
        trace_records[i] = trace_records[0];
    trace_count = 64;
    params[0] = 0;
    params[1] = 64;
    params[2] = generation;
    CHECK(l_trace_read(reinterpret_cast<lua_State*>(params)) == 1 && strlen(test_json) > 512 && strstr(test_json, "\"next\":64") && test_json[strlen(test_json) - 1] == '}');
    trace_count = TraceCapacity;
    test_base = 100;
    AIDistrictScoreNet(&out, 8, NULL, 0, 0, plot, city, row, netCaller);
    CHECK(out == 100 && enabled == 0 && faults == 1 && active == 0);
    reset_policy();
    CHECK(enabled == 0 && policy.version == 0 && policy.count == 0 && trace_count == 0 && faults == 0 && active == 0);

    const int lateBefore = 77534;
    const int lateNative = 104621;
    const int lateDelta = -4398;
    int matrix[20]{};
    int reservation = UnreservedPlot;
    auto* lateRow = rows + PlotAnalysisRowSize;
    std::memcpy(lateRow, rows, PlotAnalysisRowSize);
    put_field(lateRow, RowReservationOffset, &reservation);
    put_field(lateRow, RowDistrictScoresOffset, matrix + 10);
    put_field(rows, RowDistrictScoresOffset, matrix);
    put_field(rows, RowXOffset, 1); // city center; excluded from late writes
    put_field(object, CityBuildRowCountOffset, 2u);
    put_field(object, DistrictMatrixDataOffset, matrix + 0);
    put_field(object, DistrictMatrixLengthOffset, 20u);
    put_field(object, DistrictMatrixCapacityOffset, 20u);
    auto* lateCaller = Runtime::GetGameCoreGlobalAt<void*>(DistrictAggregationReturnRva);
    auto* score = &matrix[18];
    cacheTestScore = score;
    cacheTestFinal = lateNative;
    hooks_ready = true;
    auto asLua = [](int* arguments) { return reinterpret_cast<lua_State*>(arguments); };
    int begin[] = {1, 10, 10, 10};
    int allow[] = {1};
    int entry[] = {1, 7, 8, 30, lateBefore, lateNative, lateDelta};
    l_begin(asLua(begin));
    l_allow(asLua(allow));
    l_late_entry(asLua(entry));
    CHECK(test_integer == 1 && staging.hasLateEntries);
    CHECK(!stage_entry(Entry{1, 7, 8, 30, 256}, false) && !staging_valid);
    l_begin(asLua(begin));
    l_allow(asLua(allow));
    l_late_entry(asLua(entry));
    l_commit(nullptr);
    CHECK(test_integer == 1 && policy.hasLateEntries && enabled);
    policy.exclusions[0] = {1, 7, 30};
    policy.exclusionCount = 1;
    lateRow[RowDistrictEligibleOffset] = 9;
    const auto maskObservation = observe_enter(object);
    CHECK(maskObservation.accepted);
    {
        const SupplierRowMask outer(object, maskObservation);
        CHECK(lateRow[RowDistrictEligibleOffset] == 0 && rows[RowDistrictEligibleOffset] == 1);
        {
            const SupplierRowMask nested(object, maskObservation);
            CHECK(lateRow[RowDistrictEligibleOffset] == 0);
        }
        CHECK(lateRow[RowDistrictEligibleOffset] == 0);
    }
    CHECK(lateRow[RowDistrictEligibleOffset] == 9 && *score == 0 && reservation == UnreservedPlot);
    const int savedEntries = policy.count;
    policy.count = 0;
    CHECK(!observe_enter(object).accepted);
    const auto exclusionOnly = observe_enter(object, true);
    CHECK(exclusionOnly.accepted && exclusionOnly.key.city == 7);
    {
        const SupplierRowMask mask(object, exclusionOnly);
        CHECK(lateRow[RowDistrictEligibleOffset] == 0);
    }
    CHECK(lateRow[RowDistrictEligibleOffset] == 9);
    policy.count = savedEntries;
    lateRow[RowDistrictEligibleOffset] = 1;
    policy.exclusionCount = 0;

    // Typed declines leave other columns, reservations and row flags intact.
    policy.declineCount = 1; policy.declines[0] = {1, 7, 8, 30};
    const auto declineObservation = observe_enter(object, true);
    reservation = 77; matrix[19] = 321;
    for (int value : {413460, 0, -10, InvalidScore, InvalidScore - 1, INT_MAX}) {
        *score = value;
        {
            const DistrictCandidateMask mask(object, declineObservation);
            CHECK(*score == (value >= InvalidScore && value <= MaxScore ? DeclinedScore : value));
            CHECK(matrix[19] == 321 && reservation == 77 && lateRow[RowDistrictEligibleOffset] == 1);
            {
                const DistrictCandidateMask nested(object, declineObservation);
                CHECK(*score == (value >= InvalidScore && value <= MaxScore ? DeclinedScore : value));
            }
            CHECK(*score == (value >= InvalidScore && value <= MaxScore ? DeclinedScore : value));
        }
        CHECK(*score == value);
    }
    lateRow[RowOwnedOffset] = 0; lateRow[RowFrontierOffset] = 1; *score = 123;
    {
        const DistrictCandidateMask frontier(object, declineObservation);
        CHECK(*score == DeclinedScore);
    }
    CHECK(*score == 123); lateRow[RowOwnedOffset] = 1; lateRow[RowFrontierOffset] = 0;
    const long originalEpoch = generation;
    {
        const DistrictCandidateMask retiring(object, declineObservation);
        CHECK(*score == DeclinedScore); ++generation;
    }
    CHECK(*score == 123); generation = originalEpoch;
    const int priorCount = policy.count; policy.count = 0;
    CHECK(observe_enter(object, true).accepted); // Decline-only publication.
    policy.count = priorCount;
    original_best = test_decline_best;
    reservation = UnreservedPlot; *score = 413460;
    CHECK(hook_best(object, xy) == 9 && *score == 413460 && matrix[19] == 321);
    *score = 0; matrix[19] = -1;
    CHECK(hook_best(object, xy) == 9 && *score == 0 && matrix[19] == -1);
    CHECK(policy.declines[0].masked > 0 && policy.declines[0].restored > 0);
    int lowPriorityReservation[]{77, InvalidScore - 10};
    put_field(lateRow, RowReservationOffset, lowPriorityReservation);
    matrix[19] = InvalidScore - 2;
    for (int value : {InvalidScore, InvalidScore + 1}) {
        *score = value;
        CHECK(test_decline_best(object, xy) == 8); // Original can admit this reserved cell.
        CHECK(hook_best(object, xy) == -1 && *score == value && matrix[19] == InvalidScore - 2);
    }
    put_field(lateRow, RowReservationOffset, &reservation);
    original_best = test_best;
    *score = 123;
    const auto rejectedBefore = policy.declines[0].rejected;
    put_field(lateRow, RowDistrictScoresOffset, matrix);
    {
        const DistrictCandidateMask detached(object, declineObservation);
        CHECK(*score == 123 && policy.declines[0].rejected == rejectedBefore + 1);
    }
    put_field(lateRow, RowDistrictScoresOffset, matrix + 10);
    policy.declineCount = 2; policy.declines[1] = {1, 7, 9, 30};
    *score = 10; matrix[19] = 20; trace_count = TraceCapacity - 1;
    const auto oldFaults = faults;
    {
        const DistrictCandidateMask noRoom(object, declineObservation);
        CHECK(*score == 10 && matrix[19] == 20); // No partial application.
    }
    CHECK(!enabled && faults == oldFaults + 1);
    faults = oldFaults; enabled = true; trace_count = TraceCapacity - 2;
    {
        const DistrictCandidateMask exactRoom(object, declineObservation);
        CHECK(*score == DeclinedScore && matrix[19] == DeclinedScore);
    }
    CHECK(*score == 10 && matrix[19] == 20 && trace_count == TraceCapacity);
    policy.declineCount = 0;
    trace_count = 0; // Preserve the following late-policy test's initial journal.
    matrix[19] = 0;

    test_base = lateBefore;
    AIDistrictScoreNet(&out, 8, nullptr, 0, 0, plot, city, rows, netCaller);
    CHECK(out == lateBefore && changed == 0);
    *score = lateBefore;
    calculate_district_values(object, lateCaller);
    CHECK(*score == lateNative + lateDelta && changed == 1 && active == 0);
    CHECK(trace_records[0].kind == Late && trace_records[0].base == lateNative &&
        trace_records[0].result == lateNative + lateDelta && trace_records[0].reason == Applied);
    CHECK(trace_records[1].kind == Cache && trace_records[1].base == lateNative + lateDelta);
    *score = lateBefore + 1;
    before = trace_count;
    calculate_district_values(object, lateCaller);
    CHECK(*score == lateNative && changed == 1 && trace_records[before].reason == LateInputMismatch);
    *score = lateBefore;
    cacheTestFinal = lateNative + 1;
    before = trace_count;
    calculate_district_values(object, lateCaller);
    CHECK(*score == lateNative + 1 && changed == 1 && trace_records[before].reason == LateFinalMismatch);
    cacheTestFinal = lateNative;
    *score = lateBefore;
    calculate_district_values(object, netCaller);
    CHECK(*score == lateNative && changed == 1);
    *score = lateBefore;
    reservation = 3;
    calculate_district_values(object, lateCaller);
    CHECK(*score == lateNative && changed == 1);
    *score = lateBefore;
    reservation = UnreservedPlot;
    cacheTestReservation = &reservation;
    calculate_district_values(object, lateCaller);
    CHECK(*score == lateNative && changed == 1 && reservation == 1);
    cacheTestReservation = nullptr;
    *score = lateBefore;
    reservation = UnreservedPlot;
    put_field(object, DistrictMatrixCapacityOffset, 19u);
    calculate_district_values(object, lateCaller);
    CHECK(*score == lateNative && changed == 1);
    put_field(object, DistrictMatrixCapacityOffset, 20u);
    *score = lateBefore;
    test_bump_epoch = true;
    calculate_district_values(object, lateCaller);
    CHECK(*score == lateNative && changed == 1 && active == 0);
    test_bump_epoch = false;
    cacheTestScore = nullptr;

    // A bid applies only to the exact compatible improvement. The other
    // improvements at that supplier still receive the normal protection.
    policy.builderCount = 1;
    policy.builders[0] = {3, 131073, 796, 5, MaxBuilderBidQ8};
    const auto pasture = builder_adjustment({3, 131073, 796, 5});
    const auto farm = builder_adjustment({3, 131073, 796, 1});
    const auto elsewhere = builder_adjustment({3, 131073, 795, 5});
    CHECK(!pasture.reject && pasture.deltaQ8 == 64 * Q8Units);
    CHECK(farm.reject && !farm.deltaQ8);
    CHECK(!elsewhere.reject && !elsewhere.deltaQ8);
    CHECK(adjusted_builder_score(29056, InvalidScore, pasture) == 45440);
    CHECK(adjusted_builder_score(0, InvalidScore, pasture) == 0);
    CHECK(adjusted_builder_score(InvalidScore, InvalidScore, pasture) == InvalidScore);
    CHECK(adjusted_builder_score(INT_MAX, InvalidScore, pasture) == INT_MAX);
    CHECK(adjusted_builder_score(38655, InvalidScore, farm) == InvalidScore);

    // The whole-function gate leaves cleanup to the original. Test the same
    // binding layout and failure contract against synthetic engine objects.
    unsigned char node[ClearSelectedWorker + sizeof(int)]{};
    unsigned char player[PlayerInstanceId + sizeof(int)]{};
    unsigned char targetVector[BindingCountOffset + sizeof(unsigned)]{};
    unsigned char harvestVector[BindingCountOffset + sizeof(unsigned)]{};
    unsigned char targetRows[ClearTargetStride]{};
    unsigned char harvestRows[ClearHarvestStride]{};
    put_field(targetVector, 0, targetRows + 0);
    put_field(targetVector, BindingCountOffset, 1u);
    put_field(harvestVector, 0, harvestRows + 0);
    put_field(harvestVector, BindingCountOffset, 1u);
    put_field(node, ClearTargetBindings, targetVector + 0);
    put_field(node, ClearHarvestBindings, harvestVector + 0);
    put_field(targetRows, 0, 56);
    put_field(targetRows, sizeof(int), 10);
    put_field(harvestRows, 0, static_cast<unsigned char>(1));
    put_field(player, PlayerInstanceId, 3);
    policy.width = 74;
    policy.height = 46;
    policy.allowed[3] = true;
    policy.protectHarvests = true;
    enabled = true;
    original_clear_plot_update = +[](unsigned char* node, const unsigned char*, void*) {
        put_field(node, ClearSelectedWorker, 2228225);
        if (test_bump_epoch)
            ++generation;
        return test_base;
    };
    const auto clear = [&] { return hook_clear_plot_update(node, player, nullptr); };
    test_base = ClearReady;
    CHECK(clear() == ClearFailed && read_field<int>(node, ClearSelectedWorker) == NoSelectedWorker);
    CHECK(harvest_rejected == 1 && first_harvest_rejection[2] == 2228225 && active == 0);
    test_base = 2;
    CHECK(clear() == 2 && read_field<int>(node, ClearSelectedWorker) == 2228225);
    test_base = ClearReady;
    put_field(harvestRows, 0, static_cast<unsigned char>(0));
    CHECK(clear() == ClearReady && harvest_rejected == 1);
    put_field(harvestRows, 0, static_cast<unsigned char>(1));
    put_field(targetRows, 0, 55);
    CHECK(clear() == ClearReady && harvest_rejected == 1);
    put_field(targetRows, 0, 56);
    put_field(node, ClearTargetIndex, 1u);
    CHECK(clear() == ClearReady && harvest_rejected == 1);
    put_field(node, ClearTargetIndex, 0u);
    policy.allowed[3] = false;
    CHECK(clear() == ClearReady && harvest_rejected == 1);
    policy.allowed[3] = true;
    test_bump_epoch = true;
    CHECK(clear() == ClearReady && read_field<int>(node, ClearSelectedWorker) == 2228225);
    test_bump_epoch = false;
    policy.protectHarvests = false;
    CHECK(clear() == ClearReady && harvest_rejected == 1);
    policy.builderCount = 2;
    policy.builders[1] = {3, 65536, 797, 1};
    CHECK(builder_order(policy.builders[0], policy.builders[1]));
    CHECK(protects_harvest(3, 796) && protects_harvest(3, 797) && !protects_harvest(4, 796));
    CHECK(builder_adjustment({3, 65536, 797, 1}).deltaQ8 == 0 &&
        !builder_adjustment({3, 65536, 797, 1}).reject);
    staging.nominationCount = 3;
    staging.nominations[0] = {3, 42, 19, 100};
    staging.nominations[1] = {1, 42, 19, 300};
    staging.nominations[2] = {3, 42, 19, 200};
    finish_nominations(staging);
    CHECK(staging.nominationCount == 2 && staging.nominations[0].player == 1 &&
        staging.nominations[1].deltaQ8 == 200);
    CHECK(adjusted_nomination_score(93359, 114688) == 208047);
    CHECK(adjusted_nomination_score(0, 100) == 0 && adjusted_nomination_score(-1, 100) == -1);
    CHECK(adjusted_nomination_score(INT_MAX - 1, 100) == INT_MAX - 1 &&
        adjusted_nomination_score(100, MaxNominationBidQ8 + 1) == 100);
    put_field<int>(city, CityOwnerOffset, 1);
    put_field<int>(city, CityIdOffset, 7);
    put_field<void*>(object, CityBuildCityOffset, city);
    enabled = true;
    policy.allowed[1] = true;
    nomination_ready = nominations_published = true;
    nomination_faults = nomination_changed = 0;
    policy.nominationCount = 1;
    policy.nominations[0] = {1, 7, 19, 100};
    observe_nomination_forecast(city, 18);
    observe_nomination_forecast(city, 19);
    const auto nominationCaller = Runtime::GetGameCoreGlobalAt<const void*>(OrdinaryBuildingNominationReturnRva);
    CHECK(AIBuildingNominationScore(object, 19, 0, nominationCaller) == 0);
    CHECK(AIBuildingNominationScore(object, 19, 500, nominationCaller) == 600);
    int exactBuilding[] = {policy.version, 1, 7, 19};
    CHECK(l_nomination_receipt(asLua(exactBuilding)) == 1 && strstr(test_json, "\"staged\":1") &&
        strstr(test_json, "\"forecasts\":1") && strstr(test_json, "\"comparisons\":2") &&
        strstr(test_json, "\"writes\":1"));
    ++exactBuilding[0];
    CHECK(l_nomination_receipt(asLua(exactBuilding)) == 1 && strstr(test_json, "\"staged\":0"));
    repair_ready = true;
    staging_valid = true;
    staging.width = 74;
    staging.height = 46;
    staging.districts = 60;
    staging.allowed[2] = true;
    staging.repairCount = 0;
    int repairArgs[]{2, 196610, 31, 2311};
    l_repair_stage(asLua(repairArgs));
    CHECK(test_integer == 1 && staging.repairCount == 1);
    repairArgs[3] = 2312;
    l_repair_stage(asLua(repairArgs));
    CHECK(test_integer == 0 && staging.repairCount == 1);
    repairArgs[1] = 65536;
    l_repair_stage(asLua(repairArgs));
    CHECK(test_integer == 1 && staging.repairCount == 2);
    finish_repairs(staging);
    CHECK(staging.repairs[0].city == 65536 && staging.repairs[1].city == 196610);
    repairArgs[1] = 7;
    repairArgs[3] = 74 * 46;
    l_repair_stage(asLua(repairArgs));
    CHECK(test_integer == 0 && staging.repairCount == 2);

    unsigned char repairManager[ActiveTreeListOffset + sizeof(void*)]{};
    unsigned char repairNode[TreeListNextOffset + sizeof(void*)]{};
    constexpr size_t analysisOffset = TreeAnalysisOffset + AutoVariableDataOffset;
    unsigned char repairTree[analysisOffset + AnalysisSize]{};
    CHECK(check_active_repairs(repairManager, city, plot, 31) == RepairResult::Admitted);
    put_field<const void*>(repairManager, ActiveTreeListOffset, repairNode);
    put_field<const void*>(repairNode, 0, repairTree);
    put_field<const void*>(repairTree, analysisOffset + AnalysisCityOffset, city);
    put_field<const void*>(repairTree, analysisOffset + AnalysisPlotOffset, plot);
    put_field<int>(repairTree, analysisOffset + AnalysisDistrictOffset, 31);
    CHECK(check_active_repairs(repairManager, city, plot, 31) == RepairResult::Duplicate);
    CHECK(check_active_repairs(repairManager, city, plot, 32) == RepairResult::Admitted);
    put_field<const char*>(repairTree, TreeDisplayStringOffset, RepairTreeLabel);
    CHECK(check_active_repairs(repairManager, city, plot, 32) == RepairResult::Duplicate);
    CHECK(check_active_repairs(repairManager, object, plot, 31) == RepairResult::Admitted);
    put_field<const void*>(repairNode, TreeListNextOffset, repairNode);
    CHECK(check_active_repairs(repairManager, object, plot, 31) == RepairResult::Fault);
    clear_policy(staging);
    CHECK(staging.repairCount == 0);
    // Exercise the new C++ implementation, not the earlier JS prototype.
    // Native calls are typed stubs; real ABI and reload are live-test gates.
    alignas(8) unsigned char ownTree[OwnershipTreePlot + sizeof(void*)]{};
    alignas(8) unsigned char ownContract[OwnershipContractXY + sizeof(OwnershipXY)]{};
    alignas(8) unsigned char ownAttributes[OwnershipAttributePlayer + 1]{};
    void* ownHandle = ownContract;
    ownershipTestPlot = plot; ownershipTestAttributes = ownAttributes;
    const OwnershipNativeCalls ownNative{ownership_test_map, ownership_test_plot, ownership_test_attributes, ownership_test_assignment};
    put_field<int>(city, CityOwnerOffset, 1); put_field<int>(city, CityIdOffset, 7);
    put_field<const void*>(ownTree, OwnershipTreeCity, city);
    put_field<const void*>(ownTree, OwnershipTreePlot, plot);
    put_field<int>(ownTree, OwnershipTreeDistrict, 8);
    put_field<int>(ownTree, TreeIdOffset, 55); put_field<int>(ownTree, TreeControllerIdOffset, 66);
    put_field<const void*>(ownContract, 0, Runtime::GetGameCoreGlobalAt<void*>(OwnershipPurchaseVtableRva));
    put_field<int>(ownContract, OwnershipContractState, 3);
    put_field<int>(ownContract, OwnershipContractId, 77); put_field<int>(ownContract, OwnershipContractPlayer, 1);
    put_field<int>(ownContract, OwnershipContractControl, 66);
    put_field<OwnershipXY>(ownContract, OwnershipContractXY, {2, 3});
    put_field<int16_t>(plot, PlotXOffset, 2); put_field<int16_t>(plot, PlotYOffset, 3);
    put_field<int8_t>(ownAttributes, OwnershipAttributePlayer, 1); put_field<int>(ownAttributes, OwnershipAttributeCity, 7);
    clear_policy(policy); policy.width = 74; policy.height = 46; policy.version = 9; policy.districts = 60;
    policy.allowed[1] = true; policy.ownershipCount = 1; policy.ownership[0] = {1, 7, 8};
    enabled = false; ownership_published = true; ownership_faults = 0; ownershipTestAssignments = 0;
    acknowledge_owned_purchase(&ownHandle, ownTree, ownNative);
    CHECK(ownershipTestAssignments == 0);
    enabled = true; policy.ownership[0].district = 9;
    acknowledge_owned_purchase(&ownHandle, ownTree, ownNative);
    CHECK(ownershipTestAssignments == 0);
    policy.ownership[0].district = 8; put_field<int8_t>(ownAttributes, OwnershipAttributePlayer, -1);
    acknowledge_owned_purchase(&ownHandle, ownTree, ownNative);
    CHECK(ownershipTestAssignments == 0 && policy.ownership[0].rejected == 1);
    put_field<int8_t>(ownAttributes, OwnershipAttributePlayer, 1); put_field<int>(ownAttributes, OwnershipAttributeCity, 99);
    acknowledge_owned_purchase(&ownHandle, ownTree, ownNative);
    CHECK(ownershipTestAssignments == 0 && policy.ownership[0].rejected == 2);
    put_field<int>(ownAttributes, OwnershipAttributeCity, 7); put_field<int>(ownTree, TreeControllerIdOffset, -1);
    put_field<int>(ownContract, OwnershipContractControl, -1);
    acknowledge_owned_purchase(&ownHandle, ownTree, ownNative);
    CHECK(ownershipTestAssignments == 0);
    put_field<int>(ownTree, TreeControllerIdOffset, 66); put_field<int>(ownContract, OwnershipContractControl, 66);
    acknowledge_owned_purchase(&ownHandle, ownTree, ownNative);
    CHECK(ownershipTestAssignments == 1 && read_field<int>(ownContract, OwnershipContractState) == 7);
    CHECK(policy.ownership[0].acknowledged == 1 && policy.ownership[0].lastContract == 77 && policy.ownership[0].lastPlot == 224);
    acknowledge_owned_purchase(&ownHandle, ownTree, ownNative);
    CHECK(ownershipTestAssignments == 1); // Already fulfilled: no repeated assignment.
    int ownReceipt[]{9, 1, 7, 8}; l_ownership_receipt(asLua(ownReceipt));
    CHECK(strstr(test_json, "\"acknowledged\":1") && strstr(test_json, "\"after\":7"));
    ownReceipt[0] = 10; l_ownership_receipt(asLua(ownReceipt));
    CHECK(strstr(test_json, "\"staged\":0"));
    clear_policy(staging); staging_valid = true; staging.width = 74; staging.height = 46;
    staging.districts = 60; staging.allowed[1] = true; ownership_ready = true;
    int ownArgs[]{1, 7, 8}; l_ownership_stage(asLua(ownArgs));
    CHECK(test_integer == 1 && staging.ownershipCount == 1);
    ownArgs[0] = 2; l_ownership_stage(asLua(ownArgs));
    CHECK(test_integer == 0 && !staging_valid); // Rejected scope poisons the batch.
    clear_policy(staging); staging_valid = true; staging.width = staging.height = 10;
    staging.districts = 10; staging.allowed[1] = true;
    int declineArgs[]{1, 7, 8, 30};
    l_decline_stage(asLua(declineArgs));
    CHECK(test_integer == 1 && staging.declineCount == 1);
    l_decline_stage(asLua(declineArgs)); finish_declines(staging);
    CHECK(test_integer == 1 && staging.declineCount == 1); // Deterministic duplicate collapse.
    policy.declineCount = 1; policy.declines[0] = staging.declines[0]; policy.version = 50;
    int declineReceipt[]{50, 1, 7, 8, 30};
    l_decline_receipt(asLua(declineReceipt));
    CHECK(strstr(test_json, "\"staged\":1"));
    declineReceipt[0] = 49; l_decline_receipt(asLua(declineReceipt));
    CHECK(strstr(test_json, "\"staged\":0"));
    declineArgs[2] = 10; l_decline_stage(asLua(declineArgs));
    CHECK(test_integer == 0 && !staging_valid);
    reset_policy();
    CHECK(policy.ownershipCount == 0 && !ownership_published && policy.declineCount == 0);

    // Complete-value replacement does not depend on captured score snapshots.
    reset_policy(); hooks_ready = true;
    put_field(city, CityOwnerOffset, 1); put_field(city, CityIdOffset, 7);
    put_field(object, CityBuildCityOffset, static_cast<void*>(city));
    put_field(object, CityBuildRowsOffset, rows); put_field(object, CityBuildRowCountOffset, 2u);
    put_field(object, DistrictMatrixDataOffset, matrix); put_field(object, DistrictMatrixLengthOffset, 20u);
    put_field(object, DistrictMatrixCapacityOffset, 20u);
    put_field(lateRow, RowXOffset, 0); put_field(lateRow, RowYOffset, 3);
    put_field(lateRow, RowDistrictScoresOffset, matrix + 10);
    put_field(lateRow, RowReservationOffset, &reservation);
    lateRow[RowDistrictEligibleOffset] = 1; lateRow[RowOwnedOffset] = 1;
    put_field(lateRow, RowOwnerOffset, static_cast<int16_t>(1)); put_field(lateRow, RowCityIdOffset, 7);
    cacheTestScore = &matrix[18]; cacheTestReservation = nullptr; original_cache = test_cache;
    int finalEntry[]{1, 7, 8, 30, 12345};
    ++begin[0]; l_begin(asLua(begin)); l_allow(asLua(allow)); l_final_entry(asLua(finalEntry));
    CHECK(test_integer == 1 && staging.entries[0].finalReplacement && staging.hasLateEntries);
    l_commit(nullptr); *cacheTestScore = 777; cacheTestFinal = 424242; reservation = UnreservedPlot;
    calculate_district_values(object, lateCaller);
    CHECK(*cacheTestScore == 12345 && changed == 1 && trace_records[0].kind == Final);
    CHECK(samples[0].base == 424242 && samples[0].result == 12345);
    *cacheTestScore = 99000; cacheTestFinal = 909090;
    calculate_district_values(object, lateCaller);
    CHECK(*cacheTestScore == 12345 && changed == 2); // Dynamic native inputs, same published bid.
    finalEntry[4] = -16384;
    ++begin[0]; l_begin(asLua(begin)); l_allow(asLua(allow)); l_final_entry(asLua(finalEntry)); l_commit(nullptr);
    *cacheTestScore = 777; cacheTestFinal = 424242;
    calculate_district_values(object, lateCaller);
    CHECK(*cacheTestScore == -16384 && changed == 1); // A genuinely unattractive legal investment.
    *cacheTestScore = 777; cacheTestFinal = InvalidScore;
    calculate_district_values(object, lateCaller);
    CHECK(*cacheTestScore == InvalidScore && changed == 1);
    *cacheTestScore = InvalidScore; cacheTestFinal = 424242;
    calculate_district_values(object, lateCaller);
    CHECK(*cacheTestScore == 424242 && changed == 1); // Do not resurrect an invalid entry.
    *cacheTestScore = 777; reservation = 17;
    calculate_district_values(object, lateCaller);
    CHECK(*cacheTestScore == -16384 && changed == 2); // Complete bids may price a reserved cell.
    reservation = UnreservedPlot; lateRow[RowFrontierOffset] = 0; lateRow[RowOwnedOffset] = 0; *cacheTestScore = 777;
    calculate_district_values(object, lateCaller);
    CHECK(*cacheTestScore == 424242 && changed == 2); // Frontier remains separate, explicit scope.
    lateRow[RowFrontierOffset] = 1; *cacheTestScore = 777;
    calculate_district_values(object, lateCaller);
    CHECK(*cacheTestScore == -16384 && changed == 3); // Explicit quoted frontier investment.
    put_field(lateRow, RowOwnerOffset, static_cast<int16_t>(2)); *cacheTestScore = 777;
    calculate_district_values(object, lateCaller);
    CHECK(*cacheTestScore == 424242 && changed == 3);
    put_field(lateRow, RowOwnerOffset, static_cast<int16_t>(1)); put_field(lateRow, RowCityIdOffset, 8); *cacheTestScore = 777;
    calculate_district_values(object, lateCaller);
    CHECK(*cacheTestScore == 424242 && changed == 3);
    put_field(lateRow, RowCityIdOffset, 7);
    lateRow[RowOwnedOffset] = 1; *cacheTestScore = 777; test_bump_epoch = true;
    calculate_district_values(object, lateCaller);
    CHECK(*cacheTestScore == 424242 && changed == 3);
    test_bump_epoch = false;
    ++begin[0]; l_begin(asLua(begin)); l_allow(asLua(allow)); l_final_entry(asLua(finalEntry));
    CHECK(!stage_entry(Entry{1, 7, 8, 30, 256}, false) && !staging_valid);
    finalEntry[4] = MaxDelta + 1;
    ++begin[0]; l_begin(asLua(begin)); l_allow(asLua(allow)); l_final_entry(asLua(finalEntry));
    CHECK(test_integer == 0 && !staging_valid);
    // Native consumer tests: reservation threshold, fresh/no-offer city,
    // cache rebuild before selection and sticky request priority. These read
    // the actual matrix in a selector stub; they do not claim a live-game ABI.
    reset_policy(); hooks_ready = true; test_bump_epoch = false;
    put_field(city, CityOwnerOffset, 1); put_field(city, CityIdOffset, 7);
    put_field(lateRow, RowOwnerOffset, static_cast<int16_t>(1)); put_field(lateRow, RowCityIdOffset, 7);
    lateRow[RowDistrictEligibleOffset] = lateRow[RowOwnedOffset] = 1; lateRow[RowHasDistrictOffset] = 0;
    put_field(lateRow, RowDistrictScoresOffset, matrix + 10);
    int managedClaim[]{17, 163 * Q8Units, 99}; put_field(lateRow, RowReservationOffset, managedClaim);
    cacheTestScore = &matrix[18]; cacheTestFinal = 444512; original_cache = test_cache;
    int managedArgs[]{1, 8, -1024 * Q8Units}; finalEntry[4] = 90000;
    ++begin[0]; l_begin(asLua(begin)); l_allow(asLua(allow)); l_managed_type(asLua(managedArgs));
    CHECK(test_integer == 1); l_final_entry(asLua(finalEntry)); l_commit(nullptr);
    CHECK(test_integer == 1 && policy.managedCount == 1);
    *cacheTestScore = 777; calculate_district_values(object, lateCaller);
    CHECK(*cacheTestScore == 90000 && policy.managed[0].reservedWrites == 1);
    CHECK(managedClaim[0] == 17 && managedClaim[1] == 163 * Q8Units && managedClaim[2] == 99);
    CHECK(trace_records[0].kind == Final && trace_records[0].reservation == 17 && trace_records[0].phase == 1);
    original_best = test_decline_best; matrix[19] = 100000; *cacheTestScore = 900000;
    int managedCoordinates[2]{};
    CHECK(hook_best(object, managedCoordinates) == 9); // Correct Quarter bid lands and loses fairly.
    CHECK(*cacheTestScore == 90000 && matrix[19] == 100000);
    finalEntry[4] = 130000;
    ++begin[0]; l_begin(asLua(begin)); l_allow(asLua(allow)); l_managed_type(asLua(managedArgs)); l_final_entry(asLua(finalEntry)); l_commit(nullptr);
    *cacheTestScore = 900000;
    CHECK(hook_best(object, managedCoordinates) == 8 && *cacheTestScore == 130000);
    CHECK(managedClaim[0] == 17 && managedClaim[1] == 163 * Q8Units);
    // No explicit site offer: new cities must not fall through to a positive
    // native building forecast while awaiting the next Lua snapshot.
    ++begin[0]; l_begin(asLua(begin)); l_allow(asLua(allow)); l_managed_type(asLua(managedArgs)); l_commit(nullptr);
    CHECK(test_integer == 1 && policy.count == 0);
    *cacheTestScore = 900000;
    CHECK(hook_best(object, managedCoordinates) == 9 && *cacheTestScore == -1024 * Q8Units);
    put_field(city, CityIdOffset, 8); put_field(lateRow, RowCityIdOffset, 8); *cacheTestScore = 900000;
    CHECK(hook_best(object, managedCoordinates) == 9 && *cacheTestScore == -1024 * Q8Units);
    put_field(city, CityIdOffset, 7); put_field(lateRow, RowCityIdOffset, 7);
    // Even a negative candidate could otherwise beat the native invalid floor.
    managedClaim[0] = UnreservedPlot; matrix[19] = InvalidScore; *cacheTestScore = 900000;
    CHECK(hook_best(object, managedCoordinates) == -1);
    CHECK(*cacheTestScore == -1024 * Q8Units); // Transient admission mask restored.
    lateRow[RowHasDistrictOffset] = 1; *cacheTestScore = 765432;
    { const auto o = observe_enter(object); std::unique_lock guard(policyMutex); apply_managed_values(object, o, ValuePhase::Aggregation); }
    CHECK(*cacheTestScore == 765432);
    // Existing foundations/repair footprints retain native value and eligibility.
    lateRow[RowHasDistrictOffset] = 0;
    finalEntry[4] = 90000;
    ++begin[0]; l_begin(asLua(begin)); l_allow(asLua(allow)); l_managed_type(asLua(managedArgs)); l_final_entry(asLua(finalEntry)); l_commit(nullptr);
    original_present = test_managed_present; original_request_update = test_managed_request;
    original_location = test_managed_location;
    put_field(managedTestPlot, PlotXOffset, static_cast<int16_t>(0)); put_field(managedTestPlot, PlotYOffset, static_cast<int16_t>(3));
    int managedValue{}; *cacheTestScore = 900000;
    CHECK(hook_present(object, &managedValue, 8, nullptr) == &managedValue && managedValue == 90000);
    *cacheTestScore = 900000;
    CHECK(hook_location(object, 8, 0) == managedTestPlot && *cacheTestScore == 90000);
    managedClaim[0] = 17;
    CHECK(hook_location(object, 8, 0) == nullptr && managedClaim[0] == 17);
    managedClaim[0] = UnreservedPlot;
    test_request_assignment = test_managed_assignment; test_count_district_type=test_managed_count;
    managedAssignmentCalls = managedExistingCount = 0; managedRequestSuccess=true;
    alignas(8) unsigned char managedRequest[RequestReadableBytes]{};
    put_field(managedRequest, RequestDistrictOffset, 8);
    put_field(managedRequest, RequestUnitOffset, -1); put_field(managedRequest, RequestBuildingOffset, -1);
    put_field(managedRequest, RequestCoordinatesOffset, 0); put_field(managedRequest, RequestCoordinatesOffset + sizeof(int), 3);
    put_field(managedTestPlot, PlotXOffset, static_cast<int16_t>(0)); put_field(managedTestPlot, PlotYOffset, static_cast<int16_t>(3));
    put_field(managedRequest, RequestValueOffset, 900000); *cacheTestScore = 900000;
    CHECK(hook_request_update(managedRequest, object) == 1);
    CHECK(read_field<int>(managedRequest, RequestValueOffset) == 90000 && managedAssignmentCalls == 1);
    CHECK(policy.managed[0].requestWrites == 1 && *cacheTestScore == 90000);
    CHECK(read_field<int>(managedRequest, RequestCoordinatesOffset) == 0 && managedClaim[0] == UnreservedPlot);
    put_field(managedRequest, RequestDistrictOffset, -1); put_field(managedRequest, RequestValueOffset, 777777);
    CHECK(hook_request_update(managedRequest, object) == 1 && read_field<int>(managedRequest, RequestValueOffset) == 777777);
    CHECK(managedAssignmentCalls == 1); // Non-district requests untouched.
    put_field(managedRequest, RequestDistrictOffset, 8); lateRow[RowHasDistrictOffset] = 1;
    *cacheTestScore = 888888; put_field(managedRequest, RequestValueOffset, 777777);
    CHECK(hook_request_update(managedRequest, object) == 1 && read_field<int>(managedRequest, RequestValueOffset) == 888888);
    CHECK(managedAssignmentCalls == 1); // Existing construction/repair is native.
    lateRow[RowHasDistrictOffset] = 0; test_bump_epoch = true; put_field(managedRequest, RequestValueOffset, 999999);
    CHECK(hook_request_update(managedRequest, object) == 1 && read_field<int>(managedRequest, RequestValueOffset) == 999999);
    CHECK(managedAssignmentCalls == 1); test_bump_epoch = false;
    ++begin[0]; l_begin(asLua(begin)); l_allow(asLua(allow)); l_managed_type(asLua(managedArgs)); l_commit(nullptr);
    put_field(managedRequest, RequestValueOffset, 444512); *cacheTestScore = 444512;
    CHECK(hook_request_update(managedRequest, object) == 1 && read_field<int>(managedRequest, RequestValueOffset) == -1024 * Q8Units);
    CHECK(managedAssignmentCalls == 2 && policy.managed[0].requestWrites == 1);
    CHECK(hook_location(object, 8, 0) == nullptr && *cacheTestScore == -1024 * Q8Units);
    CHECK(hook_present(object, &managedValue, 8, nullptr) == &managedValue && managedValue == 0);
    int managedReceipt[]{begin[0], 1, 8}; l_managed_receipt(asLua(managedReceipt));
    CHECK(strstr(test_json, "\"requestWrites\":1") && strstr(test_json, "\"staged\":1"));
    // Native AL failure and branch precedence cannot grant district ownership.
    managedRequestSuccess=false; put_field(managedRequest,RequestValueOffset,555555);
    CHECK(!hook_request_update(managedRequest,object) && read_field<int>(managedRequest,RequestValueOffset)==555555);
    CHECK(managedAssignmentCalls==2); managedRequestSuccess=true;
    put_field(managedRequest,RequestBuildingOffset,3);
    CHECK(hook_request_update(managedRequest,object) && read_field<int>(managedRequest,RequestValueOffset)==555555);
    put_field(managedRequest,RequestBuildingOffset,-1); put_field(managedRequest,RequestUnitOffset,4);
    CHECK(hook_request_update(managedRequest,object) && read_field<int>(managedRequest,RequestValueOffset)==555555);
    CHECK(managedAssignmentCalls==2); put_field(managedRequest,RequestUnitOffset,-1);
    // Locationless requests use the zero-floored column aggregate, not a
    // fabricated exact site. Already-present Quarters retain native repair.
    put_field(managedRequest,RequestCoordinatesOffset,-9999);
    put_field(managedRequest,RequestCoordinatesOffset+sizeof(int),-9999);
    CHECK(hook_request_update(managedRequest,object) && read_field<int>(managedRequest,RequestValueOffset)==0);
    CHECK(managedAssignmentCalls==3);
    managedExistingCount=1; put_field(managedRequest,RequestValueOffset,888888);
    CHECK(hook_request_update(managedRequest,object) && read_field<int>(managedRequest,RequestValueOffset)==888888);
    CHECK(managedAssignmentCalls==3); managedExistingCount=0;
    // Critical suppliers are excluded in both placement selectors. Existing
    // district repair lookup and every transient flag remain unchanged.
    finalEntry[4]=90000;
    ++begin[0];l_begin(asLua(begin));l_allow(asLua(allow));l_managed_type(asLua(managedArgs));l_final_entry(asLua(finalEntry));l_commit(nullptr);
    policy.exclusionCount=1;policy.exclusions[0]={1,7,30}; *cacheTestScore=900000;
    CHECK(hook_location(object,8,0)==nullptr && lateRow[RowDistrictEligibleOffset]==1);
    lateRow[RowHasDistrictOffset]=1;
    CHECK(hook_location(object,8,0)==managedTestPlot);
    lateRow[RowHasDistrictOffset]=0;policy.exclusionCount=0;
    // More repeated consumer writes than the legacy4096-write and16384-trace
    // caps must still land. Optional samples drop, counters keep incrementing,
    // and core Best comparisons retain their reserved journal capacity.
    for(int iteration=0;iteration<20000;++iteration) {
        *cacheTestScore=900000; hook_present(object,&managedValue,8,nullptr);
    }
    CHECK(enabled && faults==0 && changed==0 && managed_writes>=20000);
    CHECK(*cacheTestScore==90000 && managed_trace_dropped>0 && trace_count<=MaxManagedTraceRecords);
    matrix[19]=100000;
    CHECK(hook_best(object,managedCoordinates)==9 && enabled && faults==0);
    CHECK(trace_records[trace_count-1].kind==Best);
    ++begin[0]; l_begin(asLua(begin)); l_allow(asLua(allow)); managedArgs[2] = 1; l_managed_type(asLua(managedArgs));
    CHECK(test_integer == 0 && !staging_valid); managedArgs[2] = -1024 * Q8Units;
    ++begin[0]; l_begin(asLua(begin)); l_allow(asLua(allow)); l_managed_type(asLua(managedArgs)); l_managed_type(asLua(managedArgs));
    CHECK(test_integer == 0 && !staging_valid);
    reset_policy(); CHECK(policy.managedCount == 0); cacheTestScore = nullptr;
    CHECK(quote_times(0, 50, 1024) && !quote_times(50, 50, 1024) && !quote_times(0, 101, 1024));
    economy_pricing_ready = false;
    int quoteArgs[]{1,7,0,256,0,50,1024};
    CHECK(l_yield_stream_value(asLua(quoteArgs)) == 0);
    CHECK(l_yield_fixed_value(asLua(quoteArgs)) == 0);
    cacheTestScore = nullptr; reset_policy();

#undef CHECK
    return n;
}
